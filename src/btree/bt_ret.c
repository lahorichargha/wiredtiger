/*-
 * Copyright (c) 2014-2017 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __key_return --
 *	Change the cursor to reference an internal return key.
 */
static inline int
__key_return(WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt)
{
	WT_CURSOR *cursor;
	WT_ITEM *tmp;
	WT_PAGE *page;
	WT_ROW *rip;

	page = cbt->ref->page;
	cursor = &cbt->iface;

	if (page->type == WT_PAGE_ROW_LEAF) {
		rip = &page->pg_row[cbt->slot];

		/*
		 * If the cursor references a WT_INSERT item, take its key.
		 * Else, if we have an exact match, we copied the key in the
		 * search function, take it from there.
		 * If we don't have an exact match, take the key from the
		 * original page.
		 */
		if (cbt->ins != NULL) {
			cursor->key.data = WT_INSERT_KEY(cbt->ins);
			cursor->key.size = WT_INSERT_KEY_SIZE(cbt->ins);
			return (0);
		}

		if (cbt->compare == 0) {
			/*
			 * If not in an insert list and there's an exact match,
			 * the row-store search function built the key we want
			 * to return in the cursor's temporary buffer. Swap the
			 * cursor's search-key and temporary buffers so we can
			 * return it (it's unsafe to return the temporary buffer
			 * itself because our caller might do another search in
			 * this table using the key we return, and we'd corrupt
			 * the search key during any subsequent search that used
			 * the temporary buffer.
			 */
			tmp = cbt->row_key;
			cbt->row_key = cbt->tmp;
			cbt->tmp = tmp;

			cursor->key.data = cbt->row_key->data;
			cursor->key.size = cbt->row_key->size;
			return (0);
		}
		return (__wt_row_leaf_key(
		    session, page, rip, &cursor->key, false));
	}

	/*
	 * WT_PAGE_COL_FIX, WT_PAGE_COL_VAR:
	 *	The interface cursor's record has usually been set, but that
	 * isn't universally true, specifically, cursor.search_near may call
	 * here without first setting the interface cursor.
	 */
	cursor->recno = cbt->recno;
	return (0);
}

/*
 * __value_return --
 *	Change the cursor to reference an internal original-page return value.
 */
static inline int
__value_return(WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt)
{
	WT_BTREE *btree;
	WT_CELL *cell;
	WT_CELL_UNPACK unpack;
	WT_CURSOR *cursor;
	WT_PAGE *page;
	WT_ROW *rip;
	uint8_t v;

	btree = S2BT(session);

	page = cbt->ref->page;
	cursor = &cbt->iface;

	if (page->type == WT_PAGE_ROW_LEAF) {
		rip = &page->pg_row[cbt->slot];

		/* Simple values have their location encoded in the WT_ROW. */
		if (__wt_row_leaf_value(page, rip, &cursor->value))
			return (0);

		/*
		 * Take the value from the original page cell (which may be
		 * empty).
		 */
		if ((cell =
		    __wt_row_leaf_value_cell(page, rip, NULL)) == NULL) {
			cursor->value.size = 0;
			return (0);
		}
		__wt_cell_unpack(cell, &unpack);
		return (__wt_page_cell_data_ref(
		    session, page, &unpack, &cursor->value));

	}

	if (page->type == WT_PAGE_COL_VAR) {
		/* Take the value from the original page cell. */
		cell = WT_COL_PTR(page, &page->pg_var[cbt->slot]);
		__wt_cell_unpack(cell, &unpack);
		return (__wt_page_cell_data_ref(
		    session, page, &unpack, &cursor->value));
	}

	/* WT_PAGE_COL_FIX: Take the value from the original page. */
	v = __bit_getv_recno(cbt->ref, cursor->recno, btree->bitcnt);
	return (__wt_buf_set(session, &cursor->value, &v, 1));
}

/*
 * __value_return_upd --
 *	Change the cursor to reference an internal update structure return
 * value.
 */
static inline int
__value_return_upd(
    WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt, WT_UPDATE *upd)
{
	WT_CURSOR *cursor;
	WT_DECL_RET;
	WT_UPDATE **listp, *list[WT_MAX_MODIFY_UPDATE];
	u_int i;
	size_t allocated_bytes;

	cursor = &cbt->iface;
	allocated_bytes = 0;

	/*
	 * We're passed a "standard" or "modified"  update that's visible to us.
	 * Our caller should have already checked for deleted items (we're too
	 * far down the call stack to return not-found).
	 *
	 * Fast path if it's a standard item, assert our caller's behavior.
	 */
	if (upd->type == WT_UPDATE_STANDARD) {
		cursor->value.data = WT_UPDATE_DATA(upd);
		cursor->value.size = upd->size;
		return (0);
	}
	WT_ASSERT(session, upd->type == WT_UPDATE_MODIFIED);

	/*
	 * Find a complete update that's visible to us, tracking modifications
	 * that are visible to us.
	 */
	for (i = 0, listp = list; upd != NULL; upd = upd->next) {
		if (!__wt_txn_upd_visible(session, upd))
			continue;

		if (WT_UPDATE_DATA_VALUE(upd))
			break;

		if (upd->type == WT_UPDATE_MODIFIED) {
			/*
			 * Update lists are expected to be short, but it's not
			 * guaranteed. There's sufficient room on the stack to
			 * avoid memory allocation in normal cases, but we have
			 * to handle the edge cases too.
			 */
			if (i >= WT_MAX_MODIFY_UPDATE) {
				if (i == WT_MAX_MODIFY_UPDATE)
					listp = NULL;
				WT_ERR(__wt_realloc_def(
				    session, &allocated_bytes, i + 1, &listp));
				if (i == WT_MAX_MODIFY_UPDATE)
					memcpy(listp, list, sizeof(list));
			}
			listp[i++] = upd;
		}
	}

	/*
	 * If we hit the end of the chain, roll forward from the update item we
	 * found, otherwise, from the original page's value.
	 */
	if (upd == NULL) {
		/*
		 * Callers of this function set the cursor slot to an impossible
		 * value to check we're not trying to return on-page values when
		 * the update list should have been sufficient (which happens,
		 * for example, if an update list was truncated, deleting some
		 * standard update required by a previous modify update). Assert
		 * the case.
		 */
		WT_ASSERT(session, cbt->slot != UINT32_MAX);

		WT_ERR(__value_return(session, cbt));
	} else if (upd->type == WT_UPDATE_DELETED)
		WT_ERR(__wt_buf_set(session, &cursor->value, "", 0));
	else
		WT_ERR(__wt_buf_set(session,
		    &cursor->value, WT_UPDATE_DATA(upd), upd->size));

	while (i > 0)
		WT_ERR(__wt_modify_apply(
		    session, &cursor->value, WT_UPDATE_DATA(listp[--i])));

err:	if (allocated_bytes)
		__wt_free(session, listp);
	return (ret);
}

/*
 * __wt_key_return --
 *	Change the cursor to reference an internal return key.
 */
int
__wt_key_return(WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt)
{
	WT_CURSOR *cursor;

	cursor = &cbt->iface;

	/*
	 * We may already have an internal key and the cursor may not be set up
	 * to get another copy, so we have to leave it alone. Consider a cursor
	 * search followed by an update: the update doesn't repeat the search,
	 * it simply updates the currently referenced key's value. We will end
	 * up here with the correct internal key, but we can't "return" the key
	 * again even if we wanted to do the additional work, the cursor isn't
	 * set up for that because we didn't just complete a search.
	 */
	F_CLR(cursor, WT_CURSTD_KEY_EXT);
	if (!F_ISSET(cursor, WT_CURSTD_KEY_INT)) {
		WT_RET(__key_return(session, cbt));
		F_SET(cursor, WT_CURSTD_KEY_INT);
	}
	return (0);
}

/*
 * __wt_value_return --
 *	Change the cursor to reference an internal return value.
 */
int
__wt_value_return(
    WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt, WT_UPDATE *upd)
{
	WT_CURSOR *cursor;

	cursor = &cbt->iface;

	F_CLR(cursor, WT_CURSTD_VALUE_EXT);
	if (upd == NULL)
		WT_RET(__value_return(session, cbt));
	else
		WT_RET(__value_return_upd(session, cbt, upd));
	F_SET(cursor, WT_CURSTD_VALUE_INT);
	return (0);
}
