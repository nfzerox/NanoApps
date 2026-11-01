/*
 * store.h — what Radio+ writes down: presets, and the RDS sidecar for a
 * recording.
 *
 * JSON, because these are files a person may well want to read, edit or copy
 * between devices, and because a recording outlives the app that made it. A
 * sidecar full of packed binary would be useless in five years; a sidecar full
 * of JSON is still readable with nothing but a text editor.
 *
 * The writer here is deliberately small - object, array, key, value, escape -
 * rather than a general JSON library. It emits into a caller-supplied buffer
 * and never allocates, so the same code serves the Linux build writing through
 * stdio and the RetailOS one writing through hb_fs_stream_write.
 *
 * The reader is equally small and only understands the shapes this file writes.
 * That is a deliberate limit and not a hidden one: en_preset_load() is
 * documented to be forgiving about whitespace and ordering and nothing else.
 *
 * The sidecar records raw groups alongside decoded fields. Raw groups mean a
 * recording can be decoded again later, by a better decoder than the one that
 * made it - which matters here specifically, because the FIFO framing in
 * core/rds.c is not yet confirmed and every recording made before it is
 * confirmed would otherwise be undecodable afterwards.
 */

#ifndef RADIOPLUS_STORE_H
#define RADIOPLUS_STORE_H

#include <stdbool.h>
#include <stdint.h>

#include "timer.h"

#include "rds.h"

/* ---- a very small JSON writer -------------------------------------------- */

typedef struct {
    char    *buf;
    uint32_t cap;
    uint32_t len;
    uint8_t  depth;
    bool     need_comma;
    bool     overflow;   /* sticky: once set, nothing more is written */
} en_json_t;

void en_json_init(en_json_t *j, char *buf, uint32_t cap);
void en_json_obj_open(en_json_t *j, const char *key);
void en_json_obj_close(en_json_t *j);
void en_json_arr_open(en_json_t *j, const char *key);
void en_json_arr_close(en_json_t *j);
void en_json_str(en_json_t *j, const char *key, const char *value);
void en_json_int(en_json_t *j, const char *key, int32_t value);
void en_json_uint(en_json_t *j, const char *key, uint32_t value);
void en_json_bool(en_json_t *j, const char *key, bool value);
void en_json_hex16(en_json_t *j, const char *key, uint16_t value);

/* Length written, or 0 if the buffer overflowed at any point. Callers must
   check: a truncated preset file is worse than none, because it looks valid. */
uint32_t en_json_done(en_json_t *j);

/* ---- presets ------------------------------------------------------------- */

#define EN_PRESET_MAX      40
#define EN_PRESET_NAME_LEN 24

typedef struct {
    uint32_t khz;
    char     name[EN_PRESET_NAME_LEN + 1];   /* usually the RDS station name */
    uint16_t pi;                             /* 0 when unknown */
    uint8_t  pty;
    bool     rbds;                           /* which table pty came from */

    /* Show this one on the simple screen, which holds a handful of big
       buttons and nothing else. Off by default, including for presets in a
       file written before this field existed: a simple screen that filled
       itself with every preset the moment you upgraded would not be simple,
       and the whole point is that the user picks the few. */
    bool     simple;
} en_preset_t;

/* How many presets the simple screen will show. Six 96 x 84 buttons in a
   2 x 3 grid is what fits at a size you can hit without looking. */
#define EN_SIMPLE_MAX 6

typedef struct {
    en_preset_t list[EN_PRESET_MAX];
    uint8_t     count;
    char        region[24];
} en_presets_t;

/* Presets flagged for the simple screen, in preset order, capped at
   EN_SIMPLE_MAX. Returns how many were written. */
uint8_t en_presets_simple(const en_presets_t *p, const en_preset_t **out,
                          uint8_t cap);

/* Flag or unflag one, refusing to go past EN_SIMPLE_MAX. Returns false when
   the grid is already full, which the caller should say out loud rather than
   silently ignoring the tap. */
bool en_preset_set_simple(en_presets_t *p, uint32_t khz, bool on);

void    en_presets_init(en_presets_t *p, const char *region);
int     en_preset_find(const en_presets_t *p, uint32_t khz);
bool    en_preset_add(en_presets_t *p, const en_preset_t *entry);
bool    en_preset_remove(en_presets_t *p, uint32_t khz);
void    en_preset_sort(en_presets_t *p);

uint32_t en_presets_save(const en_presets_t *p, char *buf, uint32_t cap);
bool     en_presets_load(en_presets_t *p, const char *json, uint32_t len);

/* ---- settings ------------------------------------------------------------ */

/*
 * What the app remembers between runs. Small on purpose: a settings file is a
 * thing users edit and copy between devices, and every field in it is one more
 * thing that can be stale or wrong.
 *
 * The region is stored by name rather than by index, because an index means a
 * settings file written by one build selects a different band in the next one
 * if the table ever gains a row.
 */
/*
 * A register the user has changed by hand.
 *
 * The tuner forgets everything when it is powered down, so a setting made in
 * the register explorer lasts exactly as long as the chip stays on. Keeping the
 * writes here and replaying them at start-up is what makes the explorer a
 * settings screen rather than a toy - a stereo blend curve tuned once should
 * still be there tomorrow.
 *
 * Replayed after the region, so a region change cannot silently undo a
 * deliberate override of one of the registers it touches.
 */
#define EN_OVERRIDE_MAX 16

typedef struct {
    uint8_t addr;
    uint8_t len;
    uint8_t data[8];
} en_override_t;

typedef struct {
    en_override_t list[EN_OVERRIDE_MAX];
    uint8_t       count;
} en_overrides_t;

/* Replaces an existing entry for the same register rather than appending, so
   editing one field twice leaves one override and not two. */
bool en_override_set(en_overrides_t *o, uint8_t addr, const uint8_t *data,
                     uint8_t len);
bool en_override_clear(en_overrides_t *o, uint8_t addr);
const en_override_t *en_override_find(const en_overrides_t *o, uint8_t addr);

typedef struct {
    char     region[24];
    uint32_t khz;            /* where to come back to */
    bool     rds_on;
    uint8_t  live_seconds;   /* how much live buffer to allocate */

    /* Off by default, deliberately. A radio that starts writing files on its
       own the first time a station announces traffic would be a surprise, and
       a surprise that fills a disk. */
    bool     ta_record;

    /* Auto, mono or stereo - en_fm_stereo_t. Persisted because forcing mono
       is a preference about a place, not about a session: somewhere with one
       weak station you want it every time. */
    uint8_t  stereo_mode;

    /* Follow the station onto another transmitter when this one fades. Off by
       default and deliberately so: the only thing that makes a candidate "the
       same station" is a matching PI, and PI is sixteen bits sent by whoever
       is transmitting. */
    bool     af_follow;

    /* Which optional screens are in the swipe order. Both default off: the
       swipe sequence a user learns should not grow a page because they
       upgraded. */
    bool     simple_screen;
    bool     wide_screen;

    /*
     * Where the audio goes, by name rather than by position.
     *
     * A name because the list of outputs can grow: storing "the second one"
     * means adding a device silently moves somebody's choice to a different
     * one, and the failure would be a radio that comes back playing to
     * Bluetooth for no reason anybody could explain. An empty name means
     * whatever the platform's first output is.
     */
    char     output[16];

    /*
     * Volume, 0..100, on whichever output that is - and the codec's own
     * analog level, 0..88, which is a different thing.
     *
     * The first is a preference and is restored on every start. The second is
     * a calibration against the physical output: the top of the codec's range
     * clips outright, so the default is deliberately below it and is worth
     * persisting only because somebody who has measured their own unit should
     * not have to do it twice.
     */
    uint8_t  volume;
    uint8_t  hp_level;

    /* Stop a recording after so long, and start one at a time of day. See
       timer.h for why those are two fields and not one feature. */
    en_rectimer_t rectimer;

    en_overrides_t overrides;
} en_settings_t;

void     en_settings_default(en_settings_t *s);
uint32_t en_settings_save(const en_settings_t *s, char *buf, uint32_t cap);

/* Missing fields keep their defaults rather than becoming zero, so a settings
   file written by an older build still loads. */
bool     en_settings_load(en_settings_t *s, const char *json, uint32_t len);

/* ---- the RDS sidecar ----------------------------------------------------- */

/*
 * Written alongside a recording as name.rds.json. Groups are appended as they
 * arrive with the offset into the recording, so a player can show what was on
 * the display at any point rather than only what was there at the end.
 *
 * Streaming rather than assembled: a recording can run for an hour, so the
 * sidecar is opened, appended to, and closed, and each append is a complete
 * line that stands on its own if the file is truncated.
 */
typedef struct {
    en_json_t j;
    uint32_t  groups;
    bool      open;
} en_sidecar_t;

/* Write the header - everything known about the station at the start. */
uint32_t en_sidecar_begin(en_sidecar_t *s, char *buf, uint32_t cap,
                          uint32_t khz, const char *region, bool rbds,
                          const en_rds_t *r);

/* One group, with its offset into the recording in milliseconds. Returns the
   bytes written into the buffer, which the caller then flushes and reuses. */
uint32_t en_sidecar_group(en_sidecar_t *s, char *buf, uint32_t cap,
                          uint32_t ms, const uint16_t blk[4], uint8_t valid);

/* Close the array and the object, and write the final decoded state. */
uint32_t en_sidecar_end(en_sidecar_t *s, char *buf, uint32_t cap,
                        uint32_t duration_ms, const en_rds_t *r);

#endif /* RADIOPLUS_STORE_H */
