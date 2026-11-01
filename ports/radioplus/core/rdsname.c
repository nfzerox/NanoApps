/*
 * rdsname.c — the lookup tables behind rdsname.h.
 *
 * Transcribed from IEC 62106 and NRSC-4-B. No decoding happens here.
 */

#include "rdsname.h"

/* ---- group types ---------------------------------------------------------- */

/*
 * Every one of the thirty-two, because a table with holes in it is a table
 * somebody has to check against the standard again.
 *
 * The pattern worth knowing while reading this: nearly every B-form slot, and
 * most of the A-form slots above 4, are Open Data Applications. A station
 * announces in group 3A which application it has put in which slot, so the
 * name here is what the slot is FOR rather than what any given station is
 * using it for - which is why the inspector lists the announced identifiers
 * separately.
 */
static const char *const GROUPS[32] = {
    /* 0A */ "Tuning, and alternate frequencies",
    /* 0B */ "Tuning, no alternate frequencies",
    /* 1A */ "Programme item number, slow labelling",
    /* 1B */ "Programme item number",
    /* 2A */ "RadioText, 64 characters",
    /* 2B */ "RadioText, 32 characters",
    /* 3A */ "Open Data Application identification",
    /* 3B */ "Open Data Application",
    /* 4A */ "Clock time and date",
    /* 4B */ "Open Data Application",
    /* 5A */ "Transparent data channel",
    /* 5B */ "Transparent data channel",
    /* 6A */ "In-house application",
    /* 6B */ "In-house application",
    /* 7A */ "Radio paging",
    /* 7B */ "Open Data Application",
    /* 8A */ "Traffic Message Channel",
    /* 8B */ "Open Data Application",
    /* 9A */ "Emergency warning",
    /* 9B */ "Open Data Application",
    /* 10A */ "Programme type name",
    /* 10B */ "Open Data Application",
    /* 11A */ "Open Data Application",
    /* 11B */ "Open Data Application",
    /* 12A */ "Open Data Application",
    /* 12B */ "Open Data Application",
    /* 13A */ "Enhanced radio paging",
    /* 13B */ "Open Data Application",
    /* 14A */ "Enhanced Other Networks",
    /* 14B */ "Enhanced Other Networks",
    /* 15A */ "Fast programme service name",
    /* 15B */ "Fast tuning and switching",
};

const char *en_rds_group_name(uint8_t type, bool version_b)
{
    uint8_t i;

    if (type > 15) return "?";
    i = (uint8_t)((type << 1) | (version_b ? 1u : 0u));
    return GROUPS[i];
}

void en_rds_group_label(uint8_t type, bool version_b, char *out, size_t n)
{
    if (!out || n < 4) return;

    if (type > 15) { out[0] = '?'; out[1] = 0; return; }

    if (type < 10) {
        out[0] = (char)('0' + type);
        out[1] = version_b ? 'B' : 'A';
        out[2] = 0;
    } else {
        out[0] = '1';
        out[1] = (char)('0' + (type - 10));
        out[2] = version_b ? 'B' : 'A';
        out[3] = 0;
    }
}

/* ---- programme identification --------------------------------------------- */

uint8_t en_rds_pi_country(uint16_t pi)   { return (uint8_t)(pi >> 12); }
uint8_t en_rds_pi_coverage(uint16_t pi)  { return (uint8_t)((pi >> 8) & 0x0Fu); }
uint8_t en_rds_pi_reference(uint16_t pi) { return (uint8_t)(pi & 0xFFu); }

const char *en_rds_coverage_name(uint8_t code)
{
    /* Codes 4 to 15 are the twelve regional areas, numbered from one, which is
       why they are generated rather than listed. */
    static const char *const REGIONAL[12] = {
        "Regional 1",  "Regional 2",  "Regional 3",  "Regional 4",
        "Regional 5",  "Regional 6",  "Regional 7",  "Regional 8",
        "Regional 9",  "Regional 10", "Regional 11", "Regional 12",
    };

    switch (code) {
    case 0:  return "Local";
    case 1:  return "International";
    case 2:  return "National";
    case 3:  return "Supra-regional";
    default: break;
    }
    if (code >= 4 && code <= 15) return REGIONAL[code - 4];
    return "?";
}

/* ---- call signs ----------------------------------------------------------- */

/*
 * Two contiguous ranges, each exactly 26^3 = 17576 wide, so the arithmetic is
 * a base-26 conversion and nothing else. 0x1000 + 17576 == 0x54A8, which is
 * where the W range begins, and it ends at 0x994F.
 */
#define CALL_K_BASE 0x1000u
#define CALL_W_BASE 0x54A8u
#define CALL_SPAN   17576u          /* 26 * 26 * 26 */

bool en_rds_callsign(uint16_t pi, char *out, size_t n)
{
    uint32_t v = pi;
    char prefix;

    if (!out || n < 6) return false;

    if (v >= CALL_K_BASE && v < CALL_K_BASE + CALL_SPAN) {
        prefix = 'K';
        v -= CALL_K_BASE;
    } else if (v >= CALL_W_BASE && v < CALL_W_BASE + CALL_SPAN) {
        prefix = 'W';
        v -= CALL_W_BASE;
    } else {
        return false;
    }

    out[0] = prefix;
    out[1] = (char)('A' + (v / 676u));
    out[2] = (char)('A' + ((v / 26u) % 26u));
    out[3] = (char)('A' + (v % 26u));
    out[4] = 0;
    return true;
}

/* ---- decoder identification ------------------------------------------------ */

void en_rds_di_text(uint8_t di, char *out, size_t n)
{
    /* Indexed by segment, which is how the bit arrives and how rds.c stores
       it: segment 0 carries d3 and segment 3 carries d0. Getting this the
       wrong way round would report every mono station as stereo, so the order
       is written out rather than left to a reader's memory. */
    static const char *const SET[4] = {
        "Dynamic PTY",      /* bit 0, segment 0, d3 */
        "Compressed",       /* bit 1, segment 1, d2 */
        "Artificial head",  /* bit 2, segment 2, d1 */
        "Stereo",           /* bit 3, segment 3, d0 */
    };

    size_t at = 0;
    uint8_t i;

    if (!out || !n) return;
    out[0] = 0;

    for (i = 0; i < 4; i++) {
        const char *s;
        size_t k;

        if (!(di & (1u << i))) continue;

        s = SET[i];
        if (at) {
            if (at + 2 >= n) break;
            out[at++] = ',';
            out[at++] = ' ';
        }
        for (k = 0; s[k] && at + 1 < n; k++) out[at++] = s[k];
        out[at] = 0;
    }

    /* Nothing set is a real answer - mono, static programme type, no
       processing - and saying so beats a blank that reads as "not yet". */
    if (!at) {
        const char *s = "Mono, static PTY";
        for (at = 0; s[at] && at + 1 < n; at++) out[at] = s[at];
        out[at] = 0;
    }
}

/* ---- open data applications ------------------------------------------------ */

const char *en_rds_oda_name(uint16_t aid)
{
    switch (aid) {
    case 0x4BD7u: return "RadioText+";
    case 0x6552u: return "Enhanced RadioText";
    case 0xCD46u: return "Traffic Message Channel (Alert-C)";
    case 0xCD47u: return "Traffic Message Channel (Alert-C, encrypted)";
    case 0x0093u: return "Cross-reference to DAB";
    case 0x4400u: return "RASANT";
    default:      return 0;
    }
}
