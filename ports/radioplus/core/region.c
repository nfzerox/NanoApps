/*
 * region.c — see region.h.
 */

#include "region.h"
#include "fmreg.h"

/*
 * The frequency register is a 16-bit offset from 64 MHz, so anything from
 * 64.000 to 129.535 MHz is expressible. Every band below fits, including the
 * Japanese and OIRT plans that sit under the usual 87.5 MHz floor - which is
 * worth stating, because a receiver that silently cannot reach half its own
 * band list would be worse than one that offers fewer regions.
 */
const en_region_t en_regions[] = {
/* 87.9 to 107.9, not 87.5 to 108. The North American grid is 200 kHz wide and
   the nominal band edges are not themselves channels - 87.5 is half a channel
   below the first one. Written as 87.5 to 108 first, and caught by the test
   that the band be a whole number of channels wide. */
{ "Americas", "United States, Canada, Mexico. RBDS, 200 kHz spacing.",
  87900, 107900, 200, true, true, false },

{ "Europe", "Also Africa, most of Asia and the Middle East.",
  87500, 108000, 100, false, false, false },

{ "Australia", "87.5 to 108 MHz on a 100 kHz grid.",
  87500, 108000, 100, false, false, false },

{ "Japan", "76 to 95 MHz, the original Japanese plan.",
  76000, 95000, 100, false, false, true },

{ "Japan wide", "76 to 108 MHz, for receivers that cover both.",
  76000, 108000, 100, false, false, true },

/* Legacy Eastern European plan. Still in use in a few places, and reachable
   here only because the register is an offset from 64 MHz rather than from
   87.5 - a detail that decides whether this row can exist at all. */
{ "OIRT", "65 to 74 MHz, legacy Eastern European band.",
  65000, 74000, 30, false, false, true },
};

const uint8_t en_region_count =
    (uint8_t)(sizeof en_regions / sizeof en_regions[0]);

const en_region_t *en_region_find(const char *name)
{
    if (!name) return 0;
    for (uint8_t i = 0; i < en_region_count; i++) {
        const char *a = en_regions[i].name, *b = name;
        while (*a && *a == *b) { a++; b++; }
        if (!*a && !*b) return &en_regions[i];
    }
    return 0;
}

bool en_region_on_grid(const en_region_t *rg, uint32_t khz)
{
    if (!rg || !rg->step_khz) return false;
    if (khz < rg->low_khz || khz > rg->high_khz) return false;
    return ((khz - rg->low_khz) % rg->step_khz) == 0;
}

uint32_t en_region_step(const en_region_t *rg, uint32_t khz, bool up)
{
    if (!rg || !rg->step_khz) return khz;

    /* Snap onto the grid first, so stepping from an off-grid frequency lands
       somewhere legal rather than carrying the error along forever. */
    if (khz < rg->low_khz) khz = rg->low_khz;
    if (khz > rg->high_khz) khz = rg->high_khz;
    uint32_t off = (khz - rg->low_khz) / rg->step_khz;

    if (up) {
        uint32_t next = rg->low_khz + (off + 1u) * rg->step_khz;
        return (next > rg->high_khz) ? rg->low_khz : next;
    }
    if (off == 0) {
        uint32_t span = (rg->high_khz - rg->low_khz) / rg->step_khz;
        return rg->low_khz + span * rg->step_khz;
    }
    return rg->low_khz + (off - 1u) * rg->step_khz;
}

uint8_t en_region_apply(const en_region_t *rg,
                        uint8_t cur_fm_ctrl, uint16_t cur_audio_ctrl,
                        uint8_t cmds[][EN_FM_CMD_MAX], uint8_t *lens,
                        uint8_t max)
{
    if (!rg || !cmds || !lens) return 0;

    /* All or nothing. A half-applied region is a tuner that is in neither, and
       the caller has no way to know which writes landed. */
    if (max < EN_REGION_CMDS) return 0;

    uint8_t n = 0;
    uint8_t payload[4];

    /* Band select, bit 0 of I2C_FM_CTRL. Everything else in that register -
       the stereo mode and the injection side - is preserved. */
    uint8_t fm_ctrl = (uint8_t)((cur_fm_ctrl & ~0x01u)
                                | (rg->band_high ? 0x01u : 0x00u));
    payload[0] = fm_ctrl;
    lens[n] = en_fm_build_write(0x01, payload, 1, cmds[n], EN_FM_CMD_MAX);
    if (!lens[n]) return 0;
    n++;

    /* De-emphasis, bit 6 of I2C_FM_AUDIO_CTRL flags. The mute and routing bits
       beside it are preserved.

       Which polarity means 75 microseconds is not stated anywhere available -
       the specification calls the bit "De-emphasis select" and stops. Set here
       for the Americas and clear elsewhere, which is the conventional reading;
       if it turns out inverted the symptom is unmistakable, since every station
       sounds wrong in the same direction, and it is one line. */
    uint16_t audio = (uint16_t)((cur_audio_ctrl & ~0x0040u)
                                | (rg->deemph_75us ? 0x0040u : 0x0000u));
    en_fm_put_u16(payload, audio);
    lens[n] = en_fm_build_write(0x05, payload, 2, cmds[n], EN_FM_CMD_MAX);
    if (!lens[n]) return 0;
    n++;

    /* Search boundaries, upper then lower, in the same write - the register
       documentation is explicit that the pair has to be programmed together. */
    en_fm_put_u16(&payload[0], en_fm_khz_to_reg(rg->high_khz));
    en_fm_put_u16(&payload[2], en_fm_khz_to_reg(rg->low_khz));
    lens[n] = en_fm_build_write(0xFB, payload, 4, cmds[n], EN_FM_CMD_MAX);
    if (!lens[n]) return 0;
    n++;

    /* Channel spacing. */
    en_fm_put_u16(payload, (uint16_t)rg->step_khz);
    lens[n] = en_fm_build_write(0xFD, payload, 2, cmds[n], EN_FM_CMD_MAX);
    if (!lens[n]) return 0;
    n++;

    return n;
}
