/*
 * region.h — band plans, channel spacing, de-emphasis and RDS variant.
 *
 * The world does not agree about FM. Band edges differ, channel spacing
 * differs, the de-emphasis time constant differs, and North America decodes
 * RBDS where everyone else decodes RDS. All four are settable on this chip, and
 * all four are wrong by default for somebody.
 *
 * A region is a small record. Applying one is a handful of register writes, and
 * this file builds them so the same decision is not re-derived in two different
 * platform backends.
 *
 * Two of those writes touch registers that carry unrelated bits, so applying a
 * region needs the current value of each rather than a blind write — see
 * en_region_apply(). That is not incidental: I2C_FM_CTRL also holds the stereo
 * mode, and I2C_FM_AUDIO_CTRL holds the mute and routing flags, and clobbering
 * either while changing band would be a real bug.
 *
 * Pure C99, no allocation, no I/O.
 */

#ifndef RADIOPLUS_REGION_H
#define RADIOPLUS_REGION_H

#include <stdbool.h>
#include <stdint.h>

#include "fmcmd.h"

typedef struct {
    const char *name;
    const char *note;

    uint32_t low_khz;    /* inclusive band edges, in kHz */
    uint32_t high_khz;
    uint32_t step_khz;   /* channel spacing */

    bool rbds;           /* decode programme types as RBDS rather than RDS */
    bool deemph_75us;    /* 75 microseconds in the Americas, 50 elsewhere */
    bool band_high;      /* I2C_FM_CTRL band select bit */
} en_region_t;

extern const en_region_t en_regions[];
extern const uint8_t     en_region_count;

/* NULL if the name is not a known region. */
const en_region_t *en_region_find(const char *name);

/* True when a frequency in kHz sits on the region's channel grid. Worth asking
   before tuning: a station off the grid is almost always a typo, and on a
   200 kHz plan the odd hundreds are not valid channels at all. */
bool en_region_on_grid(const en_region_t *rg, uint32_t khz);

/* Step to the next or previous channel, wrapping at the band edges. */
uint32_t en_region_step(const en_region_t *rg, uint32_t khz, bool up);

/*
 * Build the commands that put the tuner into a region.
 *
 * `cur_fm_ctrl` and `cur_audio_ctrl` must be the registers' current contents,
 * because both carry bits this has no business changing — the stereo mode lives
 * beside the band select, and the mute and routing flags live beside the
 * de-emphasis select. Read them, pass them in, and only the bits that belong to
 * the region move.
 *
 * Writes into `cmds`, one packet each, with lengths in `lens`. Returns how many
 * were produced, or 0 if `max` is too small to hold all of them — partial
 * application would leave the tuner in a state that is neither region.
 */
uint8_t en_region_apply(const en_region_t *rg,
                        uint8_t cur_fm_ctrl, uint16_t cur_audio_ctrl,
                        uint8_t cmds[][EN_FM_CMD_MAX], uint8_t *lens,
                        uint8_t max);

/* How many commands en_region_apply() will produce. */
#define EN_REGION_CMDS 4

#endif /* RADIOPLUS_REGION_H */
