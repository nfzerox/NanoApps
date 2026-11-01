/*
 * hb_silver_res.h — serve custom resources (labels, icons, layouts) to the
 * stock UI by id, and add custom home-screen items, at runtime.
 *
 * The stock UI resolves every label/image/layout by a 4-char type + numeric
 * id, looked up through a per-task chain of resource databases. These helpers
 * (a) parse a flattened resource-DB blob (the format the OS resource system reads
 * and writes), (b) splice it into that chain so its ids resolve everywhere,
 * and (c) register a home-screen item that references ids by number.
 *
 * All of these MUST run on the UI task (e.g. from a home-controller vtable
 * hook); they touch OS UI singletons and lists directly — must run on the UI task.
 *
 * Addresses are for iPod nano 7 OS 1.1.2.
 */
#ifndef HB_SILVER_RES_H
#define HB_SILVER_RES_H

#include "hb_sdk.h"

/* 4-char resource types (the tags that appear in the blob sections). */
#define HB_SILVER_RES_STRING 0x53747220u   /* 'Str ' — UI strings / labels */
#define HB_SILVER_RES_BITMAP 0x424d6170u   /* 'BMap' — bitmaps / icons     */

/* Scratch sizes the caller must provide as resident storage. */
#define HB_SILVER_RESDB_OBJ_SIZE 64u       /* >= the resource-db object     */
#define HB_SILVER_RESDB_NODE_SIZE 16u      /* one chain node: [next][prev][tag][db] */

/*
 * Parse a flattened resource-DB blob into a resource-db object built in
 * `obj_storage` (>= HB_SILVER_RESDB_OBJ_SIZE bytes, resident). `blob`/`size`
 * point at the flattened bytes (kept borrowed, not copied — keep them
 * resident). Returns the object pointer, or NULL.
 */
void *hb_silver_resdb_open(void *obj_storage, const void *blob, unsigned int size);

/*
 * Look up one resource by (4-char type, id) in a single resource-db.
 * Returns a pointer to the resource bytes (size via *out_size if non-NULL),
 * or NULL if absent. Useful to verify a blob parsed before publishing it.
 */
const void *hb_silver_resdb_get(void *resdb, unsigned int type4cc, int id,
                                unsigned int *out_size);

/*
 * Splice a resource-db into the current UI task's resolution chain so its ids
 * resolve for labels/images/layouts everywhere. `node_storage` is 16 bytes of
 * caller-owned RESIDENT memory that must outlive the registration; `tag4cc`
 * names the entry. Tail-insert (so chain trims that drop the head never touch
 * it). Returns 1 on success, 0 if the resource model wasn't available.
 */
int hb_silver_resdb_publish(void *resdb, unsigned int tag4cc, void *node_storage);

/*
 * Return 1 if a node tagged `tag4cc` is currently linked in the UI task's
 * resolution chain, else 0. The chain is rebuilt by the OS on every (re)boot,
 * so our node is absent after a reboot but present after a bare module re-push
 * — a reliable "do I need to (re)publish + re-register?" check that, unlike a
 * RAM marker, isn't fooled by DRAM surviving a warm panic-reboot.
 */
int hb_silver_resdb_has_tag(unsigned int tag4cc);
/* Unlink one caller-owned node before reusing its DB/blob during a refresh. */
void hb_silver_resdb_unpublish(void *node_storage);

/*
 * Add a home-screen item with id string `sbid`, resolving its label and icon
 * from resource ids `label_id` / `image_id` (pass -1 to leave a slot default),
 * and make it visible. Use ids served by a published resource-db (above) for a
 * fully custom item, or existing stock ids to borrow.
 */
void hb_silver_home_add_item(const char *sbid, int label_id, int image_id);
/* Stock hide-item operation; updates page positions and posts model notices. */
void hb_silver_home_hide_item(const char *sbid);

/*
 * Rebuild every springboard page cell from the model. Call ONCE after a batch of
 * hb_silver_home_add_item: the per-item show only realises a new page's cell
 * when the view passes its visibility gate (a batched add on the action task
 * does not), so items that needed a new page are in the model but off screen
 * until this reload. No-op if the springboard view isn't currently up.
 */
void hb_silver_home_reload(void);

/*
 * Pre-create enough empty springboard pages to hold `extra_items` new items,
 * so each following hb_silver_home_add_item lands on an already-present page
 * (the per-item show's own auto-grow-a-new-page path does not take effect from
 * a batched add on the action task). Call ONCE before the batch. Pair with
 * hb_silver_home_trim_pages afterwards to drop any page left empty.
 */
void hb_silver_home_ensure_capacity(int extra_items);

/* Drop springboard pages left empty (e.g. over-provisioned by ensure_capacity). */
void hb_silver_home_trim_pages(void);

#endif /* HB_SILVER_RES_H */
