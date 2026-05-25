/*
 * hb_paged_list.c — trivial paging state.
 */

#include "hb_sdk.h"

void hb_paged_list_init(hb_paged_list_t *l, int total, int page_size)
{
    l->total        = total < 0 ? 0 : total;
    l->page_size    = page_size < 1 ? 1 : page_size;
    l->current_page = 0;
}

int hb_paged_list_page_count(const hb_paged_list_t *l)
{
    if (l->total <= 0) return 1;
    /* Subtract-based ceil(total/page_size) — no libgcc divmod. */
    int n = 0;
    int remaining = l->total;
    while (remaining > 0) { remaining -= l->page_size; n++; }
    return n;
}

int hb_paged_list_first(const hb_paged_list_t *l)
{
    int f = l->current_page * l->page_size;
    if (f >= l->total) f = (hb_paged_list_page_count(l) - 1) * l->page_size;
    if (f < 0) f = 0;
    return f;
}

int hb_paged_list_last(const hb_paged_list_t *l)
{
    int last = hb_paged_list_first(l) + l->page_size;
    if (last > l->total) last = l->total;
    return last;
}

void hb_paged_list_next(hb_paged_list_t *l)
{
    int max_page = hb_paged_list_page_count(l) - 1;
    if (l->current_page < max_page) l->current_page++;
}

void hb_paged_list_prev(hb_paged_list_t *l)
{
    if (l->current_page > 0) l->current_page--;
}

void hb_paged_list_set_total(hb_paged_list_t *l, int total)
{
    l->total        = total < 0 ? 0 : total;
    l->current_page = 0;
}
