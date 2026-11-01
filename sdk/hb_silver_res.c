/*
 * hb_silver_res.c — runtime custom-resource serving + home-screen items.
 *
 */

#include "hb_silver_res.h"

#define FN_THUMB(addr) ((addr) | 1u)

#define ADDR_RESDB_OPEN  0x08006fdeu
#define ADDR_RESDB_GET   0x08409e86u
#define ADDR_RES_MODEL   0x084157c4u
#define RESDB_CHAIN_OFF  0x2cu
#define ADDR_SB_MODEL    0x084084b4u
#define ADDR_SB_REGISTER 0x08498e68u
#define ADDR_SB_SHOW     0x0849a3dcu
#define ADDR_SB_HIDE     0x08499e68u  /* paired show/hide virtual slots +0x134/+0x138 */
#define ADDR_STR_CTOR    0x0842d8bcu
#define ADDR_SB_VIEW     0x083f4dfcu   /* read the current springboard view (or NULL)         */
#define SBVIEW_TABLE_OFF 0x388u        /* springboard view: table-view field offset */
#define ADDR_TV_RELOAD   0x08400596u   /* reload a table view's data            */
#define ADDR_SB_ADDPAGE  0x08498cdau   /* append an empty home page                */
#define ADDR_SB_TRIMPAGE 0x0849968cu   /* drop unused home pages                   */
#define ADDR_SB_PAGES    0x084b2f32u   /* the home page list                       */
#define ADDR_SB_PERPAGE  0x084b2fe8u   /* items per home page                      */
#define ADDR_SB_GETMAXPAGES 0x084b2f38u /* current max page count                   */
#define ADDR_SB_SETMAXPAGES 0x08498cd6u /* set the max page count                   */

typedef void *(*resdb_open_t)(void *self, const void *data, unsigned int size, int a4);
typedef const void *(*resdb_get_t)(void *self, unsigned int type, int id, unsigned int *out_size);
typedef void *(*get_ptr_t)(void);
typedef void *(*str_ctor_t)(void *, const char *, void *);
typedef void  (*sb_register_t)(void *, void *, int, long, long, long, long);
typedef int   (*sb_show_t)(void *m, void *data);
typedef void  (*tv_reload_t)(void *table_view);
typedef int   (*int_meth_t)(void *self);          /* returns an int             */
typedef void  (*void_meth_t)(void *self);         /* void method(this)          */
typedef int   (*addpage_t)(void *self, int a2);    /* appends an empty home page      */

void *hb_silver_resdb_open(void *obj_storage, const void *blob, unsigned int size)
{
    if (!obj_storage || !blob)
        return 0;
    return ((resdb_open_t)FN_THUMB(ADDR_RESDB_OPEN))(obj_storage, blob, size, 0);
}

const void *hb_silver_resdb_get(void *resdb, unsigned int type4cc, int id,
                                unsigned int *out_size)
{
    unsigned int sz = 0;
    const void *p;
    if (!resdb)
        return 0;
    p = ((resdb_get_t)FN_THUMB(ADDR_RESDB_GET))(resdb, type4cc, id, &sz);
    if (out_size)
        *out_size = p ? sz : 0;
    return p;
}

int hb_silver_resdb_publish(void *resdb, unsigned int tag4cc, void *node_storage)
{
    void *model = ((get_ptr_t)FN_THUMB(ADDR_RES_MODEL))();
    uint32_t *sentinel, *last, *node;
    if (!model || !resdb || !node_storage)
        return 0;

    /* tail-insert node_storage into the chain whose sentinel is at model+0x2c */
    sentinel = *(uint32_t **)((char *)model + RESDB_CHAIN_OFF);
    if (!sentinel)
        return 0;
    last = (uint32_t *)(uintptr_t)sentinel[1];          /* sentinel->prev */
    node = (uint32_t *)node_storage;
    node[0] = (uint32_t)(uintptr_t)sentinel;            /* node->next = end   */
    node[1] = (uint32_t)(uintptr_t)last;                /* node->prev = last  */
    node[2] = tag4cc;                                   /* entry tag          */
    node[3] = (uint32_t)(uintptr_t)resdb;               /* entry db           */
    last[0]     = (uint32_t)(uintptr_t)node;            /* last->next = node  */
    sentinel[1] = (uint32_t)(uintptr_t)node;            /* sentinel->prev=node*/
    return 1;
}

int hb_silver_resdb_has_tag(unsigned int tag4cc)
{
    void *model = ((get_ptr_t)FN_THUMB(ADDR_RES_MODEL))();
    uint32_t *sentinel, *node;
    if (!model)
        return 0;
    sentinel = *(uint32_t **)((char *)model + RESDB_CHAIN_OFF);
    if (!sentinel)
        return 0;
    /* walk sentinel->next.. until we loop back; each node is [next][prev][tag][db] */
    for (node = (uint32_t *)(uintptr_t)sentinel[0];
         node && node != sentinel;
         node = (uint32_t *)(uintptr_t)node[0]) {
        if (node[2] == tag4cc)
            return 1;
    }
    return 0;
}

void hb_silver_home_add_item(const char *sbid, int label_id, int image_id)
{
    void *model = ((get_ptr_t)FN_THUMB(ADDR_SB_MODEL))();
    uint8_t  id_obj[32] = {0};
    uint32_t extra = 0;
    void *id_str;
    uint8_t show[160] = {0};
    int i;

    if (!model || !sbid)
        return;

    id_str = ((str_ctor_t)FN_THUMB(ADDR_STR_CTOR))(id_obj, sbid, &extra);
    ((sb_register_t)FN_THUMB(ADDR_SB_REGISTER))(model, id_str, 0,
                                                (long)label_id, (long)image_id, -1, -1);

    for (i = 0; sbid[i] && i < 120; i++)
        show[2 + i] = (uint8_t)sbid[i];
    ((sb_show_t)FN_THUMB(ADDR_SB_SHOW))(model, show);
}

void hb_silver_home_hide_item(const char *sbid)
{
    void *model = ((get_ptr_t)FN_THUMB(ADDR_SB_MODEL))();
    uint8_t data[160] = {0};
    int i;
    if (!model || !sbid) return;
    for (i = 0; sbid[i] && i < 120; i++) data[2 + i] = (uint8_t)sbid[i];
    ((sb_show_t)FN_THUMB(ADDR_SB_HIDE))(model, data);
}

void hb_silver_resdb_unpublish(void *node_storage)
{
    void *model = ((get_ptr_t)FN_THUMB(ADDR_RES_MODEL))();
    uint32_t *end, *n, *target = node_storage;
    if (!model || !target) return;
    end = *(uint32_t **)((char *)model + RESDB_CHAIN_OFF);
    if (!end) return;
    for (n = (uint32_t *)(uintptr_t)end[0]; n && n != end;
         n = (uint32_t *)(uintptr_t)n[0]) {
        if (n == target) {
            uint32_t *next = (uint32_t *)(uintptr_t)n[0];
            uint32_t *prev = (uint32_t *)(uintptr_t)n[1];
            next[1] = n[1]; prev[0] = n[0];
            n[0] = n[1] = 0;
            return;
        }
    }
}

void hb_silver_home_reload(void)
{
    /* Rebuild every page cell from the model. The per-item show above only
     * inserts a brand-new page's cell when the springboard view passes its
     * visibility gate, which a batched add does not — so items that needed a
     * new page land in the model but not on screen. One reload after the batch
     * realises them (the same call the OS makes when leaving edit mode). */
    void *view = ((get_ptr_t)FN_THUMB(ADDR_SB_VIEW))();
    void *table;

    if (!view)
        return;
    table = *(void **)((char *)view + SBVIEW_TABLE_OFF);
    if (table)
        ((tv_reload_t)FN_THUMB(ADDR_TV_RELOAD))(table);
}

void hb_silver_home_ensure_capacity(int extra_items)
{
    /* Pre-create enough empty pages to hold `extra_items` new items, so each
     * subsequent add lands via the model's "fill an existing page" path. The
     * auto-grow path inside the per-item show does not take effect from a batched 
     * add on the action task — it leaves the but a page WE add directly grows the 
     * model cleanly, and a slot on an already-present page is filled reliably. 
     * Over-provision (the existing pages' free slots are unknown); hb_silver_home_trim_pages 
     * drops any page left empty afterwards. */
    void *model = ((get_ptr_t)FN_THUMB(ADDR_SB_MODEL))();
    int per, want, have, need, i;

    if (!model || extra_items <= 0)
        return;
    per = ((int_meth_t)FN_THUMB(ADDR_SB_PERPAGE))(model);
    if (per <= 0)
        per = 6;
    want = (extra_items + per - 1) / per;     /* pages needed for the new items */
    have = ((int_meth_t)FN_THUMB(ADDR_SB_PAGES))(model);    /* page-list query */
    need = have + want;
    /* Raise the OS page cap if our items would exceed it. The springboard caps
     * at a max-pages constant of 10 by default; append empty page + the first-free-slot search
     * honour the DYNAMIC max-pages query, so lifting it here lets all our items land
     * (the hardcoded max-pages constant only bounds the OS's own default-order build). */
    if (((int_meth_t)FN_THUMB(ADDR_SB_GETMAXPAGES))(model) < need)
        ((addpage_t)FN_THUMB(ADDR_SB_SETMAXPAGES))(model, need + 2);
    for (i = 0; i < want; i++)
        ((addpage_t)FN_THUMB(ADDR_SB_ADDPAGE))(model, 0);   /* no will-insert msg */
}

void hb_silver_home_trim_pages(void)
{
    void *model = ((get_ptr_t)FN_THUMB(ADDR_SB_MODEL))();
    if (model)
        ((void_meth_t)FN_THUMB(ADDR_SB_TRIMPAGE))(model);   /* drop empty pages */
}
