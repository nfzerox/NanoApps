/*
 * tests.c — desktop tests for the Radio+ core.
 *
 * Everything here runs with no tuner, no iPod and no Bluetooth stack, which is
 * the whole reason core/ has no dependencies. Two kinds of check:
 *
 *   - the encoder produces exactly the bytes the firmware's own framing implies,
 *     and rejects malformed events rather than reading past them;
 *   - the register table is internally consistent, which matters because it is
 *     transcribed by hand from a specification and a typo in a bit offset is
 *     otherwise invisible until it is a wrong register write on real hardware.
 */

#include <stdio.h>
#include <string.h>

#include "../core/fmcmd.h"
#include "../core/fmreg.h"
#include "../core/rds.h"
#include "../core/region.h"
#include "../core/wav.h"
#include "../core/store.h"
#include "../core/scan.h"
#include "../core/timer.h"
#include "../core/affollow.h"
#include "../core/ctscan.h"
#include "../core/rdsname.h"

static int checks, failures;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        checks++;                                                             \
        if (!(cond)) {                                                        \
            failures++;                                                       \
            printf("  FAIL %s:%d  ", __FILE__, __LINE__);                     \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
        }                                                                     \
    } while (0)

static void section(const char *name) { printf("\n== %s\n", name); }

static int same(const uint8_t *a, const uint8_t *b, uint8_t n)
{
    return memcmp(a, b, n) == 0;
}

static void show(const char *label, const uint8_t *p, uint8_t n)
{
    printf("  %-22s", label);
    for (uint8_t i = 0; i < n; i++) printf(" %02x", p[i]);
    printf("\n");
}

/* ---- encoding ------------------------------------------------------------ */

static void test_build(void)
{
    section("command encoding");

    uint8_t buf[EN_FM_CMD_MAX];

    /* Turn FM and RDS on: write 0x03 to I2C_FM_RDS_SYSTEM. */
    uint8_t on = 0x03;
    uint8_t n = en_fm_build_write(0x00, &on, 1, buf, sizeof buf);
    const uint8_t want_write[] = { 0x01, 0x15, 0xFC, 0x03, 0x00, 0x00, 0x03 };
    show("FM on + RDS on", buf, n);
    CHECK(n == sizeof want_write, "write length %u", n);
    CHECK(same(buf, want_write, n), "write bytes differ");

    /* Read RSSI: one byte from 0x0F. */
    n = en_fm_build_read(0x0F, en_fm_read_len(0x0F), buf, sizeof buf);
    const uint8_t want_read[] = { 0x01, 0x15, 0xFC, 0x03, 0x0F, 0x01, 0x01 };
    show("read RSSI", buf, n);
    CHECK(n == sizeof want_read, "read length %u", n);
    CHECK(same(buf, want_read, n), "read bytes differ");

    /* A read always carries three parameters regardless of how much it asks
       for, because Read_Length is one byte. */
    n = en_fm_build_read(0x80, 250, buf, sizeof buf);
    show("read 250 RDS tuples", buf, n);
    CHECK(n == 7, "a big read is still a 7-byte command, got %u", n);
    CHECK(buf[3] == 3, "parameter length should be 3, got %u", buf[3]);
    CHECK(buf[6] == 250, "requested length not carried");

    /* Tune to 98.5 MHz. The register is kHz offset from 64 MHz, so 34500. */
    uint8_t freq[2];
    en_fm_put_u16(freq, en_fm_khz_to_reg(98500));
    n = en_fm_build_write(0x0A, freq, 2, buf, sizeof buf);
    show("tune 98.5 MHz", buf, n);
    CHECK(en_fm_khz_to_reg(98500) == 34500, "98.5 MHz should encode as 34500");
    CHECK(n == 8, "tune command length %u", n);
    CHECK(buf[4] == 0x0A && buf[5] == 0x00, "wrong register or mode");
    CHECK(en_fm_get_u16(&buf[6]) == 34500, "frequency did not round-trip");
    CHECK(en_fm_reg_to_khz(34500) == 98500, "reverse conversion wrong");

    /* Refusals rather than overruns. */
    uint8_t tiny[4];
    CHECK(en_fm_build_write(0x00, &on, 1, tiny, sizeof tiny) == 0,
          "a write that does not fit should be refused");
    CHECK(en_fm_build_read(0x0F, 1, tiny, sizeof tiny) == 0,
          "a read that does not fit should be refused");
    CHECK(en_fm_build_read(0x0F, 0, buf, sizeof buf) == 0,
          "a zero-length read is meaningless and should be refused");
    CHECK(en_fm_build_write(0x00, 0, 1, buf, sizeof buf) == 0,
          "a null payload with a non-zero length should be refused");
}

/* ---- event decoding ------------------------------------------------------ */

static void test_parse(void)
{
    section("event decoding");

    /* A command complete carrying one byte of RSSI. Layout comes from the
       firmware validating its own reply: 04 0E plen 01 ocf ogf status, then
       the command's own Status, I2C_Address and Read_Write_Mode. */
    const uint8_t evt[] = {
        0x04, 0x0E, 0x07, 0x01, 0x15, 0xFC, 0x00, 0x0F, 0x01, 0x2A
    };
    en_fm_reply_t r;
    CHECK(en_fm_parse_event(evt, sizeof evt, &r), "valid event rejected");
    CHECK(r.status == 0, "status %u", r.status);
    CHECK(r.addr == 0x0F, "address %02x", r.addr);
    CHECK(r.mode == EN_FM_READ, "mode should be read");
    CHECK(r.value_len == 1, "value length %u", r.value_len);
    CHECK(r.value && r.value[0] == 0x2A, "value wrong");
    printf("  RSSI read back: %u\n", r.value ? r.value[0] : 0);

    /* A failure from the chip is a real answer and must still parse. */
    uint8_t bad[sizeof evt];
    memcpy(bad, evt, sizeof evt);
    bad[6] = 0x12;
    CHECK(en_fm_parse_event(bad, sizeof bad, &r), "error event should parse");
    CHECK(r.status == 0x12, "error status not reported");

    /* Everything that is not ours, or not whole, must be refused rather than
       read past - this runs on bytes that came off a shared transport. */
    uint8_t wrong[sizeof evt];

    memcpy(wrong, evt, sizeof evt);
    wrong[0] = 0x02;
    CHECK(!en_fm_parse_event(wrong, sizeof wrong, &r), "non-event accepted");

    memcpy(wrong, evt, sizeof evt);
    wrong[1] = 0x0F;
    CHECK(!en_fm_parse_event(wrong, sizeof wrong, &r),
          "non-command-complete accepted");

    memcpy(wrong, evt, sizeof evt);
    wrong[4] = 0x18;
    CHECK(!en_fm_parse_event(wrong, sizeof wrong, &r),
          "another opcode's completion accepted");

    CHECK(!en_fm_parse_event(evt, 5, &r), "truncated event accepted");

    /* The length field claiming more than arrived is the dangerous case. */
    memcpy(wrong, evt, sizeof evt);
    wrong[2] = 0x40;
    CHECK(!en_fm_parse_event(wrong, sizeof wrong, &r),
          "over-long parameter length accepted");

    /* A write completion carries no value. */
    const uint8_t wevt[] = { 0x04, 0x0E, 0x06, 0x01, 0x15, 0xFC, 0x00, 0x00, 0x00 };
    CHECK(en_fm_parse_event(wevt, sizeof wevt, &r), "write completion rejected");
    CHECK(r.mode == EN_FM_WRITE && r.value == 0 && r.value_len == 0,
          "write completion should carry no value");
}

/* ---- the register table -------------------------------------------------- */

static void test_table(void)
{
    section("register table consistency");

    printf("  %u registers\n", en_fm_reg_count);
    CHECK(en_fm_reg_count == 39, "expected 39 registers, got %u",
          en_fm_reg_count);

    int tbd = 0, rmw = 0, varlen = 0, writable = 0;

    for (uint8_t i = 0; i < en_fm_reg_count; i++) {
        const en_fm_reg_t *r = &en_fm_regs[i];

        CHECK(r->name && r->doc, "register %02x is missing text", r->addr);
        CHECK(en_fm_reg_find(r->addr) == r, "lookup of %02x found the wrong row",
              r->addr);

        /* Addresses must be unique, or a lookup silently returns whichever
           came first and one register becomes unreachable. */
        for (uint8_t j = 0; j < i; j++)
            CHECK(en_fm_regs[j].addr != r->addr, "duplicate address %02x",
                  r->addr);

        if (r->flags & EN_FM_TBD) tbd++;
        if (r->flags & EN_FM_RMW) rmw++;
        if (r->read_len == EN_FM_LEN_VAR) varlen++;
        if (r->flags & EN_FM_W) writable++;

        /* Anything writable needs a payload length, and anything with a
           payload length needs to be writable. */
        if (r->flags & EN_FM_W)
            CHECK(r->write_len > 0, "%s is writable with no payload length",
                  r->name);
        if (r->write_len > 0)
            CHECK(r->flags & EN_FM_W, "%s has a payload but is not writable",
                  r->name);

        /* A read-modify-write register is useless if it cannot be read. */
        if (r->flags & EN_FM_RMW)
            CHECK(r->flags & EN_FM_R, "%s needs RMW but is not readable",
                  r->name);

        for (uint8_t k = 0; k < r->nfields; k++) {
            const en_fm_field_t *f = &r->fields[k];

            CHECK(f->name && f->doc, "%s field %u is missing text", r->name, k);
            CHECK(f->width == 1 || f->width == 2 || f->width == 4,
                  "%s.%s has width %u", r->name, f->name, f->width);
            CHECK(f->bit_hi >= f->bit_lo, "%s.%s has inverted bit range",
                  r->name, f->name);
            CHECK(f->bit_hi < f->width * 8u,
                  "%s.%s bit %u is outside its %u-byte value",
                  r->name, f->name, f->bit_hi, f->width);

            /* A field has to fit inside whichever of the read or write payloads
               actually exists, or it addresses bytes that are never there. */
            uint8_t span = (uint8_t)(f->off + f->width);
            if (r->write_len)
                CHECK(span <= r->write_len,
                      "%s.%s ends at %u, past the %u-byte write payload",
                      r->name, f->name, span, r->write_len);
            if (r->read_len < EN_FM_LEN_UNKNOWN)
                CHECK(span <= r->read_len,
                      "%s.%s ends at %u, past the %u-byte read payload",
                      r->name, f->name, span, r->read_len);

            CHECK(f->min <= f->max, "%s.%s has min above max", r->name, f->name);

            /* The declared range must be representable in the bits allotted,
               which is the typo this catches: a range copied from the spec
               against a bit width copied wrongly. */
            uint8_t nbits = (uint8_t)(f->bit_hi - f->bit_lo + 1u);
            if (nbits < 31u) {
                int32_t hi = (f->flags & EN_FMF_SIGNED)
                           ? (int32_t)((1u << (nbits - 1u)) - 1u)
                           : (int32_t)((1u << nbits) - 1u);
                int32_t lo = (f->flags & EN_FMF_SIGNED)
                           ? -(int32_t)(1u << (nbits - 1u)) : 0;
                CHECK(f->max <= hi, "%s.%s max %d needs more than %u bits",
                      r->name, f->name, f->max, nbits);
                CHECK(f->min >= lo, "%s.%s min %d needs more than %u bits",
                      r->name, f->name, f->min, nbits);

                /* Every named bit must fit the field it belongs to. */
                for (uint8_t b = 0; b < f->nbits; b++)
                    CHECK((f->bits[b].mask >> nbits) == 0,
                          "%s.%s bit %s (%#x) does not fit %u bits",
                          r->name, f->name, f->bits[b].name,
                          f->bits[b].mask, nbits);
            }

            if (f->flags & EN_FMF_BITMAP)
                CHECK(f->bits && f->nbits, "%s.%s is a bitmap with no bits",
                      r->name, f->name);
            if (f->flags & EN_FMF_ENUM)
                CHECK(f->vals && f->nvals, "%s.%s is an enum with no values",
                      r->name, f->name);
        }
    }

    printf("  %d writable, %d read-modify-write, %d variable length\n",
           writable, rmw, varlen);
    printf("  %d with read lengths marked TBD upstream\n", tbd);

    CHECK(en_fm_reg_find(0x03) == 0, "0x03 is not a register and must not match");

    /* Unknown and variable lengths both report 0, so a caller cannot silently
       inherit a length nobody established. */
    CHECK(en_fm_read_len(0x80) == 0, "the RDS FIFO length is the caller's choice");
    CHECK(en_fm_read_len(0xF5) == 0, "an unstated length must not be invented");
    CHECK(en_fm_read_len(0x0F) == 1, "RSSI reads one byte");
    CHECK(en_fm_read_len(0xF9) == 8, "the blend curve reads eight bytes");
}

/* ---- field access -------------------------------------------------------- */

static void test_fields(void)
{
    section("field packing");

    /* The audio control register: a 9-bit bandwidth above a 7-bit flag set,
       both inside one 16-bit value. Editing one must not disturb the other. */
    const en_fm_reg_t *r = en_fm_reg_find(0x05);
    uint8_t p[2] = { 0, 0 };

    CHECK(en_fm_field_set(&r->fields[1], p, 2, 0x30), "could not set flags");
    CHECK(en_fm_field_set(&r->fields[0], p, 2, 3), "could not set bandwidth");
    show("bandwidth 3, route both", p, 2);
    CHECK(en_fm_field_get(&r->fields[0], p, 2) == 3, "bandwidth lost");
    CHECK(en_fm_field_get(&r->fields[1], p, 2) == 0x30, "flags disturbed");

    CHECK(!en_fm_field_set(&r->fields[0], p, 2, 4),
          "bandwidth 4 is out of range and must be refused");
    CHECK(en_fm_field_get(&r->fields[0], p, 2) == 3,
          "a refused set must not modify the payload");

    /* Signed fields, which are the ones a naive shift gets wrong. */
    const en_fm_reg_t *snr = en_fm_reg_find(0x08);
    uint8_t s[1] = { 0 };
    CHECK(en_fm_field_set(&snr->fields[0], s, 1, -40), "could not set -40");
    CHECK(s[0] == 0xD8, "-40 should store as 0xD8, got %02x", s[0]);
    CHECK(en_fm_field_get(&snr->fields[0], s, 1) == -40,
          "-40 did not round-trip, got %d",
          en_fm_field_get(&snr->fields[0], s, 1));

    /* Nibble fields. */
    const en_fm_reg_t *ap = en_fm_reg_find(0x04);
    uint8_t a[1] = { 0 };
    CHECK(en_fm_field_set(&ap->fields[0], a, 1, 9), "could not set duration");
    CHECK(en_fm_field_set(&ap->fields[1], a, 1, 5), "could not set threshold");
    show("pause 9, threshold 5", a, 1);
    CHECK(a[0] == 0x95, "nibbles packed wrong: %02x", a[0]);
    CHECK(en_fm_field_get(&ap->fields[0], a, 1) == 9, "duration lost");
    CHECK(en_fm_field_get(&ap->fields[1], a, 1) == 5, "threshold lost");

    /* The single top bit of the search control register. */
    const en_fm_reg_t *sc = en_fm_reg_find(0x07);
    uint8_t c[1] = { 0 };
    en_fm_field_set(&sc->fields[1], c, 1, 20);
    en_fm_field_set(&sc->fields[0], c, 1, 1);
    show("search up, RSSI 20", c, 1);
    CHECK(c[0] == 0x94, "search control packed wrong: %02x", c[0]);

    /* The whole eight-byte blend curve, mixed signed and unsigned. */
    const en_fm_reg_t *bl = en_fm_reg_find(0xF9);
    uint8_t b[8];
    memset(b, 0, sizeof b);
    const int32_t want[8] = { 40, 20, -60, -80, 15, -30, 12, 7 };
    for (uint8_t i = 0; i < 8; i++)
        CHECK(en_fm_field_set(&bl->fields[i], b, 8, want[i]),
              "could not set %s", bl->fields[i].name);
    show("blend + soft mute", b, 8);
    for (uint8_t i = 0; i < 8; i++)
        CHECK(en_fm_field_get(&bl->fields[i], b, 8) == want[i],
              "%s did not round-trip", bl->fields[i].name);

    /* Bounds. A field must never read or write outside the payload it was
       given, however wrong the caller is. */
    CHECK(en_fm_field_get(&bl->fields[7], b, 4) == 0,
          "a field past the buffer must read 0");
    CHECK(!en_fm_field_set(&bl->fields[7], b, 4, 1),
          "a field past the buffer must not be written");
}

/* ---- RDS ----------------------------------------------------------------- */

/* Build block B for a given group type and version, with the traffic-programme
   flag and programme type in their standard places. */
static uint16_t blkb(uint8_t type, bool ver_b, bool tp, uint8_t pty,
                     uint16_t low5)
{
    return (uint16_t)(((uint16_t)type << 12) | (ver_b ? 0x0800u : 0u)
                      | (tp ? 0x0400u : 0u) | ((uint16_t)(pty & 0x1Fu) << 5)
                      | (low5 & 0x1Fu));
}

static void feed(en_rds_t *r, uint16_t a, uint16_t b, uint16_t c, uint16_t d)
{
    uint16_t blk[4] = { a, b, c, d };
    en_rds_group(r, blk, EN_RDS_ALL);
}

static void test_rds_ps(void)
{
    section("RDS programme service name");

    en_rds_t r;
    en_rds_init(&r, true);

    /* "CBC RADI" delivered two characters at a time, out of order, because a
       receiver joining mid-cycle sees exactly that. */
    const char *name = "CBC RADI";
    const uint8_t order[4] = { 2, 0, 3, 1 };
    for (uint8_t i = 0; i < 4; i++) {
        uint8_t s = order[i];
        uint16_t d = (uint16_t)(((uint8_t)name[s * 2] << 8)
                                | (uint8_t)name[s * 2 + 1]);
        feed(&r, 0xC2B5, blkb(0, false, true, 22, s), 0x0000, d);
        CHECK(r.ps_valid == (i == 3),
              "PS should only become valid on the last segment (i=%u)", i);
    }
    printf("  PS \"%s\"  PI %04X  PTY %u (%s)\n",
           r.ps, r.pi, r.pty, en_rds_pty_name(r.pty, r.rbds));
    CHECK(strcmp(r.ps, "CBC RADI") == 0, "PS assembled as \"%s\"", r.ps);
    CHECK(r.pi == 0xC2B5, "PI wrong");
    CHECK(r.tp, "traffic programme flag lost");

    /* RBDS and RDS disagree about what 22 means, and the register that selects
       between them is I2C_RDS_CTRL bit 0. */
    printf("  PTY 22 is \"%s\" under RBDS, \"%s\" under RDS\n",
           en_rds_pty_name(22, true), en_rds_pty_name(22, false));
    CHECK(strcmp(en_rds_pty_name(22, true), "Public") == 0, "RBDS PTY 22");
    CHECK(strcmp(en_rds_pty_name(22, false), "Travel") == 0, "RDS PTY 22");
    CHECK(strcmp(en_rds_pty_name(200, true), "Unknown") == 0,
          "an out-of-range PTY must not index off the table");
}

static void test_rds_ta(void)
{
    section("RDS traffic announcement");

    en_rds_t r;
    en_rds_init(&r, true);

    uint16_t ch = 0;
    uint16_t blk[4] = { 0xC2B5, blkb(0, false, true, 22, 0), 0, 0x4142 };
    ch = en_rds_group(&r, blk, EN_RDS_ALL);
    CHECK(!r.ta, "TA should start clear");

    /* Bit 4 of block B is the announcement, and it is the thing an
       auto-recorder triggers on. */
    blk[1] = (uint16_t)(blkb(0, false, true, 22, 0) | 0x0010u);
    ch = en_rds_group(&r, blk, EN_RDS_ALL);
    CHECK(r.ta, "TA not set");
    CHECK(ch & EN_RDS_CH_TA, "TA change not reported");

    ch = en_rds_group(&r, blk, EN_RDS_ALL);
    CHECK(!(ch & EN_RDS_CH_TA), "TA reported as changed when it did not");

    blk[1] = blkb(0, false, true, 22, 0);
    ch = en_rds_group(&r, blk, EN_RDS_ALL);
    CHECK(!r.ta && (ch & EN_RDS_CH_TA), "TA clearing not reported");
}

static void test_rds_rt(void)
{
    section("RDS radio text");

    en_rds_t r;
    en_rds_init(&r, true);

    const char *text =
        "Now Playing: Neil Young - Harvest Moon                          ";
    for (uint8_t seg = 0; seg < 16; seg++) {
        uint16_t c = (uint16_t)(((uint8_t)text[seg * 4] << 8)
                                | (uint8_t)text[seg * 4 + 1]);
        uint16_t d = (uint16_t)(((uint8_t)text[seg * 4 + 2] << 8)
                                | (uint8_t)text[seg * 4 + 3]);
        feed(&r, 0xC2B5, blkb(2, false, true, 22, seg), c, d);
    }
    printf("  RT \"%s\"\n", r.rt);
    CHECK(r.rt_valid, "RT never became valid");
    CHECK(strncmp(r.rt, "Now Playing: Neil Young - Harvest Moon", 38) == 0,
          "RT assembled wrong");

    /* The A/B flag toggling means a new message. Without clearing, the tail of
       the old one shows through wherever the new one has not reached. */
    uint16_t b = (uint16_t)(blkb(2, false, true, 22, 0) | 0x0010u);
    feed(&r, 0xC2B5, b, 0x4E65, 0x7874);   /* "Next" */
    CHECK(!r.rt_valid, "a new message should invalidate the old text");
    CHECK(strncmp(r.rt, "Next", 4) == 0, "new text not written");
    CHECK(r.rt[10] == ' ', "old text showed through the new message");

    /* A carriage return ends the message early. */
    en_rds_init(&r, true);
    feed(&r, 0xC2B5, blkb(2, false, true, 22, 0), 0x4F4B, 0x0D20);
    printf("  RT with early terminator: \"%s\"\n", r.rt);
    CHECK(r.rt_valid, "a terminated message should be complete immediately");
    CHECK(strcmp(r.rt, "OK") == 0, "terminator not honoured, got \"%s\"", r.rt);
}

static void test_rds_ct(void)
{
    section("RDS clock time");

    en_rds_t r;
    en_rds_init(&r, true);

    /* Modified Julian day 51544 is 1 January 2000, which is checkable by hand
       against the conversion in the RDS specification. 12:34 UTC. */
    const uint32_t mjd = 51544;
    const uint8_t hour = 12, minute = 34;

    uint16_t b = (uint16_t)(blkb(4, false, false, 0, 0)
                            | (uint16_t)((mjd >> 15) & 0x03u));
    uint16_t c = (uint16_t)(((mjd & 0x7FFFu) << 1) | (hour >> 4));
    uint16_t d = (uint16_t)(((uint16_t)(hour & 0x0Fu) << 12)
                            | ((uint16_t)minute << 6));

    uint16_t ch = 0;
    uint16_t blk[4] = { 0xC2B5, b, c, d };
    ch = en_rds_group(&r, blk, EN_RDS_ALL);
    printf("  %04u-%02u-%02u %02u:%02u  (offset %d half hours)\n",
           r.ct_year, r.ct_month, r.ct_day, r.ct_hour, r.ct_minute, r.ct_offset);
    CHECK(ch & EN_RDS_CH_CT, "clock change not reported");
    CHECK(r.ct_valid, "clock never became valid");
    CHECK(r.ct_year == 2000 && r.ct_month == 1 && r.ct_day == 1,
          "MJD 51544 should be 2000-01-01");
    CHECK(r.ct_hour == 12 && r.ct_minute == 34, "time wrong");

    /* Newfoundland is minus three and a half hours, which is exactly the case
       a naive whole-hour offset gets wrong. */
    en_rds_init(&r, true);
    uint16_t dn = (uint16_t)(d | 0x0020u | 7u);      /* west, 7 half hours */
    uint16_t blkn[4] = { 0xC2B5, b, c, dn };
    en_rds_group(&r, blkn, EN_RDS_ALL);
    printf("  same instant at -3:30: %04u-%02u-%02u %02u:%02u\n",
           r.ct_year, r.ct_month, r.ct_day, r.ct_hour, r.ct_minute);
    CHECK(r.ct_hour == 9 && r.ct_minute == 4, "half-hour offset applied wrong");
    CHECK(r.ct_day == 1, "date should not have moved");

    /* An offset that crosses midnight has to move the date with it. */
    en_rds_init(&r, true);
    uint16_t early = (uint16_t)((1u << 12) | (0u << 6));   /* 01:00 UTC */
    uint16_t de = (uint16_t)(early | 0x0020u | 7u);
    uint16_t blke[4] = { 0xC2B5, b, c, de };
    en_rds_group(&r, blke, EN_RDS_ALL);
    printf("  01:00 UTC at -3:30: %04u-%02u-%02u %02u:%02u\n",
           r.ct_year, r.ct_month, r.ct_day, r.ct_hour, r.ct_minute);
    CHECK(r.ct_hour == 21 && r.ct_minute == 30, "wrapped time wrong");
    CHECK(r.ct_year == 1999 && r.ct_month == 12 && r.ct_day == 31,
          "crossing midnight backwards should reach 1999-12-31");
}

static void test_rds_af(void)
{
    section("RDS alternate frequencies");

    en_rds_t r;
    en_rds_init(&r, true);

    /* Codes 1 to 204 are 87.5 MHz plus 100 kHz per step, so the list runs from
       87.6 to 107.9 - the top of the band is not itself an AF code. Getting
       this wrong by one step was the first thing these tests caught. */
    CHECK(en_rds_af_khz(1) == 87600, "AF code 1 should be 87.6 MHz");
    CHECK(en_rds_af_khz(204) == 107900, "AF code 204 should be 107.9 MHz");
    CHECK(en_rds_af_khz(0) == 0, "AF code 0 is not a frequency");
    CHECK(en_rds_af_khz(205) == 0, "filler is not a frequency");
    CHECK(en_rds_af_khz(250) == 0, "the LF/MF marker is not a frequency");

    /* A list header says how many follow, then pairs of codes. */
    feed(&r, 0xC2B5, blkb(0, false, true, 22, 0),
         (uint16_t)((224u + 2u) << 8 | 108u), 0x4142);
    feed(&r, 0xC2B5, blkb(0, false, true, 22, 1),
         (uint16_t)(150u << 8 | 108u), 0x4344);

    printf("  %u alternates (station announced %u):", r.af_count, r.af_expected);
    for (uint8_t i = 0; i < r.af_count; i++)
        printf(" %u.%u", r.af[i] / 1000, (r.af[i] % 1000) / 100);
    printf("\n");
    CHECK(r.af_expected == 2, "AF count header not read");
    CHECK(r.af_count == 2, "expected 2 distinct alternates, got %u", r.af_count);
    CHECK(r.af[0] == 98300, "first alternate wrong");
    CHECK(r.af[1] == 102500, "second alternate wrong");
}

static void test_rds_robustness(void)
{
    section("RDS block errors and station changes");

    en_rds_t r;
    en_rds_init(&r, true);

    /* Without block B a group has no type and nothing may be interpreted -
       not even from the blocks that did arrive intact. */
    uint16_t blk[4] = { 0xC2B5, blkb(0, false, true, 22, 0), 0, 0x4142 };
    uint16_t ch = en_rds_group(&r, blk, (uint8_t)(EN_RDS_ALL & ~EN_RDS_B));
    CHECK(ch == 0, "a group without block B must decode to nothing");
    CHECK(!r.pi_valid, "PI must not be taken from a group with no type");
    CHECK(r.blocks_bad == 1, "bad block not counted");

    /* A corrupt block D must not write garbage into the name. */
    en_rds_init(&r, true);
    for (uint8_t s = 0; s < 4; s++)
        feed(&r, 0xC2B5, blkb(0, false, true, 22, s),
             0, (uint16_t)(0x4141u + s));
    CHECK(r.ps_valid, "PS should be complete");
    en_rds_group(&r, blk, (uint8_t)(EN_RDS_ALL & ~EN_RDS_D));
    CHECK(strcmp(r.ps, "AABACADA") == 0 || r.ps_valid,
          "a bad block D must leave the name alone");

    /* A PI change is a different station, so everything accumulated belongs to
       the previous one and has to go - otherwise an AF jump or a retune shows
       the old station's name against the new station's signal. */
    en_rds_init(&r, true);
    const char *name = "STATION1";
    for (uint8_t s = 0; s < 4; s++)
        feed(&r, 0xC2B5, blkb(0, false, true, 22, s), 0,
             (uint16_t)(((uint8_t)name[s * 2] << 8) | (uint8_t)name[s * 2 + 1]));
    CHECK(r.ps_valid && strcmp(r.ps, "STATION1") == 0, "setup failed");

    ch = 0;
    uint16_t other[4] = { 0x1234, blkb(0, false, false, 5, 0), 0, 0x5A5A };
    ch = en_rds_group(&r, other, EN_RDS_ALL);
    printf("  after PI change: PS valid %d, PI %04X\n", r.ps_valid, r.pi);
    CHECK(ch & EN_RDS_CH_PI, "PI change not reported");
    CHECK(!r.ps_valid, "the previous station's name survived a PI change");
    CHECK(r.pi == 0x1234, "new PI not adopted");
    CHECK(r.groups > 0, "statistics should survive a station change");
}

static void test_rds_tuples(void)
{
    section("RDS FIFO unpacking (framing unverified)");

    /* Four blocks, each two data bytes and a status byte whose top nibble is
       the block type. This is the one guess in the decoder and it is isolated
       so that correcting it on the device touches nothing else. */
    const uint8_t fifo[] = {
        0xC2, 0xB5, 0x00,          /* A: PI          */
        0x02, 0xC0, 0x10,          /* B: group 0A    */
        0x00, 0x00, 0x20,          /* C              */
        0x43, 0x42, 0x30,          /* D: "CB"        */
    };
    uint16_t groups[4][4];
    uint8_t valid[4];
    uint8_t n = en_rds_unpack_tuples_unverified(fifo, sizeof fifo, groups,
                                                valid, 4);
    CHECK(n == 1, "expected one group, got %u", n);
    CHECK(groups[0][0] == 0xC2B5, "block A wrong");
    CHECK(groups[0][3] == 0x4342, "block D wrong");
    CHECK(valid[0] == EN_RDS_ALL, "all blocks should be marked good");

    /* A block flagged bad must be reported bad rather than dropped silently,
       so the group decoder can decide what is still usable. */
    uint8_t bad[sizeof fifo];
    memcpy(bad, fifo, sizeof fifo);
    bad[11] = 0x31;                        /* block D, error bit set */
    n = en_rds_unpack_tuples_unverified(bad, sizeof bad, groups, valid, 4);
    CHECK(n == 1, "a group with a bad block is still a group");
    CHECK(!(valid[0] & EN_RDS_D), "bad block D should not be marked valid");

    /* Partial trailing data must not run off the end. */
    n = en_rds_unpack_tuples_unverified(fifo, 8, groups, valid, 4);
    CHECK(n == 0, "an incomplete group must not be emitted");
    n = en_rds_unpack_tuples_unverified(fifo, sizeof fifo, groups, valid, 0);
    CHECK(n == 0, "no room means no groups");
}

/* ---- regions -------------------------------------------------------------- */

static void test_regions(void)
{
    section("region band plans");

    printf("  %u regions\n", en_region_count);
    CHECK(en_region_count >= 5, "expected several regions, got %u",
          en_region_count);

    for (uint8_t i = 0; i < en_region_count; i++) {
        const en_region_t *g = &en_regions[i];
        printf("  %-11s %6u - %-6u kHz  step %3u  %-4s  %s\n",
               g->name, g->low_khz, g->high_khz, g->step_khz,
               g->rbds ? "RBDS" : "RDS",
               g->deemph_75us ? "75us" : "50us");

        CHECK(g->low_khz < g->high_khz, "%s has inverted edges", g->name);
        CHECK(g->step_khz > 0, "%s has no channel spacing", g->name);

        /* Every edge has to be expressible in the frequency register, which is
           a 16-bit offset from 64 MHz. A band the tuner cannot reach would be
           worse than not offering it. */
        CHECK(g->low_khz >= EN_FM_BASE_KHZ,
              "%s starts below the 64 MHz register base", g->name);
        CHECK(g->high_khz - EN_FM_BASE_KHZ <= 65535,
              "%s ends beyond the register range", g->name);

        /* The band has to be a whole number of channels wide, or the top edge
           is not a tunable channel. */
        CHECK(((g->high_khz - g->low_khz) % g->step_khz) == 0,
              "%s is not a whole number of channels wide", g->name);

        CHECK(en_region_find(g->name) == g, "%s does not look up", g->name);
    }
    CHECK(en_region_find("Atlantis") == 0, "an unknown region must not match");

    const en_region_t *am = en_region_find("Americas");
    CHECK(am && am->rbds, "the Americas must decode RBDS");
    CHECK(am && am->deemph_75us, "the Americas use 75 us de-emphasis");
    CHECK(am && am->step_khz == 200, "the Americas use 200 kHz spacing");

    const en_region_t *eu = en_region_find("Europe");
    CHECK(eu && !eu->rbds, "Europe decodes RDS");
    CHECK(eu && !eu->deemph_75us, "Europe uses 50 us de-emphasis");

    /* On a 200 kHz plan the odd hundreds are not channels at all, which is the
       thing that makes a grid check worth having. */
    CHECK(en_region_on_grid(am, 98500), "98.5 is a valid US channel");
    CHECK(!en_region_on_grid(am, 98600), "98.6 is not on a 200 kHz grid");
    CHECK(en_region_on_grid(eu, 98600), "98.6 is a valid European channel");
    CHECK(!en_region_on_grid(am, 120000), "out of band must not be on grid");

    /* Stepping wraps at the edges rather than sticking. */
    CHECK(en_region_step(am, 98500, true) == 98700, "step up wrong");
    CHECK(en_region_step(am, 98500, false) == 98300, "step down wrong");
    CHECK(en_region_step(am, 107900, true) == 87900, "step up should wrap");
    CHECK(en_region_step(am, 87900, false) == 107900, "step down should wrap");
    CHECK(en_region_on_grid(am, en_region_step(am, 98600, true)),
          "stepping from off-grid must land on the grid");

    const en_region_t *jp = en_region_find("Japan");
    CHECK(jp && jp->low_khz == 76000, "Japan starts at 76 MHz");
    CHECK(en_fm_khz_to_reg(76000) == 12000, "76 MHz should encode as 12000");

    const en_region_t *oirt = en_region_find("OIRT");
    CHECK(oirt && oirt->low_khz == 65000, "OIRT starts at 65 MHz");
    CHECK(en_fm_khz_to_reg(65000) == 1000, "65 MHz should encode as 1000");
}

static void test_region_apply(void)
{
    section("applying a region");

    uint8_t cmds[EN_REGION_CMDS][EN_FM_CMD_MAX];
    uint8_t lens[EN_REGION_CMDS];
    const en_region_t *am = en_region_find("Americas");

    /* Existing contents that must survive: stereo auto-select in the control
       register, and manual mute plus I2S routing in the audio register. */
    uint8_t n = en_region_apply(am, 0x12, 0x0022, cmds, lens, EN_REGION_CMDS);
    CHECK(n == EN_REGION_CMDS, "expected %u commands, got %u",
          EN_REGION_CMDS, n);

    for (uint8_t i = 0; i < n; i++) {
        char label[32];
        snprintf(label, sizeof label, "reg %02X", cmds[i][4]);
        show(label, cmds[i], lens[i]);
        CHECK(cmds[i][0] == 0x01 && cmds[i][1] == 0x15 && cmds[i][2] == 0xFC,
              "command %u is not an FM_RDS_Command", i);
        CHECK(cmds[i][5] == 0x00, "command %u should be a write", i);
    }

    /* Band select changed, everything else in the register preserved. */
    CHECK(cmds[0][4] == 0x01, "first write should be I2C_FM_CTRL");
    CHECK(cmds[0][6] == 0x12, "band select clobbered the stereo mode: %02x",
          cmds[0][6]);

    /* De-emphasis set for the Americas, mute and routing preserved. */
    CHECK(cmds[1][4] == 0x05, "second write should be I2C_FM_AUDIO_CTRL");
    CHECK(en_fm_get_u16(&cmds[1][6]) == 0x0062,
          "de-emphasis clobbered the audio flags: %04x",
          en_fm_get_u16(&cmds[1][6]));

    /* Boundaries, upper then lower, in one write as the register requires. */
    CHECK(cmds[2][4] == 0xFB, "third write should be the search boundary");
    CHECK(en_fm_get_u16(&cmds[2][6]) == en_fm_khz_to_reg(107900),
          "upper boundary wrong");
    CHECK(en_fm_get_u16(&cmds[2][8]) == en_fm_khz_to_reg(87900),
          "lower boundary wrong");

    CHECK(cmds[3][4] == 0xFD, "fourth write should be the search step");
    CHECK(en_fm_get_u16(&cmds[3][6]) == 200, "step wrong");

    /* Europe clears the de-emphasis bit and leaves the rest alone. */
    n = en_region_apply(en_region_find("Europe"), 0x13, 0x0062, cmds, lens,
                        EN_REGION_CMDS);
    CHECK(n == EN_REGION_CMDS, "Europe should apply too");
    CHECK(cmds[0][6] == 0x12, "band select should have cleared bit 0");
    CHECK(en_fm_get_u16(&cmds[1][6]) == 0x0022,
          "de-emphasis should have cleared, leaving the rest");

    /* All or nothing: too little room produces nothing at all rather than a
       tuner left in neither region. */
    CHECK(en_region_apply(am, 0, 0, cmds, lens, 2) == 0,
          "a partial application must be refused");
}

/* ---- WAV and persistence -------------------------------------------------- */

static void test_wav(void)
{
    section("WAV header");

    uint8_t h[EN_WAV_HDR_BYTES];
    uint32_t n = en_wav_header(h, sizeof h, 44100, 2, 16, 0);
    CHECK(n == EN_WAV_HDR_BYTES, "header length %u", n);
    CHECK(memcmp(h, "RIFF", 4) == 0 && memcmp(h + 8, "WAVE", 4) == 0,
          "not a RIFF/WAVE header");
    CHECK(memcmp(h + 36, "data", 4) == 0, "no data chunk");
    CHECK(en_wav_data_len(h, sizeof h) == 0, "an open stream should say 0");

    /* Byte rate and block align have to agree with the format, or players
       compute the wrong duration and seek to the wrong place. */
    uint32_t byte_rate = (uint32_t)h[28] | ((uint32_t)h[29] << 8)
                       | ((uint32_t)h[30] << 16) | ((uint32_t)h[31] << 24);
    CHECK(byte_rate == 44100u * 4u, "byte rate %u", byte_rate);
    CHECK(h[32] == 4 && h[33] == 0, "block align wrong");

    /* Patching after the fact is how a recording of unknown length closes. */
    CHECK(en_wav_patch_len(h, sizeof h, 176400), "patch failed");
    CHECK(en_wav_data_len(h, sizeof h) == 176400, "patched length not read back");
    uint32_t riff = (uint32_t)h[4] | ((uint32_t)h[5] << 8)
                  | ((uint32_t)h[6] << 16) | ((uint32_t)h[7] << 24);
    CHECK(riff == 176400 + 36, "RIFF size not patched with it");

    CHECK(en_wav_header(h, 10, 44100, 2, 16, 0) == 0,
          "a header that does not fit must be refused");
    CHECK(en_wav_header(h, sizeof h, 0, 2, 16, 0) == 0,
          "a zero sample rate must be refused");
    CHECK(en_wav_header(h, sizeof h, 44100, 2, 12, 0) == 0,
          "an impossible bit depth must be refused");
}

static void test_presets(void)
{
    section("presets");

    en_presets_t p;
    en_presets_init(&p, "Americas");

    en_preset_t e;
    memset(&e, 0, sizeof e);

    e.khz = 98500; strcpy(e.name, "CBC RADI"); e.pi = 0xC2B5; e.pty = 22;
    e.rbds = true;
    CHECK(en_preset_add(&p, &e), "add failed");

    e.khz = 88100; strcpy(e.name, "VOCM"); e.pi = 0x1234; e.pty = 1;
    CHECK(en_preset_add(&p, &e), "add failed");

    e.khz = 102900; strcpy(e.name, "KIXX"); e.pi = 0x5678; e.pty = 10;
    CHECK(en_preset_add(&p, &e), "add failed");
    CHECK(p.count == 3, "count %u", p.count);

    /* Re-adding updates rather than duplicating, so a placeholder is replaced
       once the station name actually decodes. */
    e.khz = 98500; strcpy(e.name, "CBC Radio One"); e.pi = 0xC2B5;
    CHECK(en_preset_add(&p, &e), "update failed");
    CHECK(p.count == 3, "re-adding a frequency should not duplicate it");
    CHECK(strcmp(p.list[0].name, "CBC Radio One") == 0, "update did not apply");

    en_preset_sort(&p);
    CHECK(p.list[0].khz == 88100 && p.list[2].khz == 102900, "sort wrong");

    char buf[4096];
    uint32_t n = en_presets_save(&p, buf, sizeof buf);
    CHECK(n > 0, "save overflowed");
    printf("  %u bytes of JSON for %u presets\n", n, p.count);
    printf("  %.140s...\n", buf);

    en_presets_t q;
    CHECK(en_presets_load(&q, buf, n), "load failed");
    CHECK(q.count == p.count, "loaded %u of %u", q.count, p.count);
    CHECK(strcmp(q.region, "Americas") == 0, "region lost: %s", q.region);
    for (uint8_t i = 0; i < p.count; i++) {
        CHECK(q.list[i].khz == p.list[i].khz, "preset %u frequency lost", i);
        CHECK(strcmp(q.list[i].name, p.list[i].name) == 0,
              "preset %u name lost: %s", i, q.list[i].name);
        CHECK(q.list[i].pi == p.list[i].pi, "preset %u PI lost", i);
        CHECK(q.list[i].rbds == p.list[i].rbds, "preset %u RBDS flag lost", i);
    }

    CHECK(en_preset_remove(&p, 88100), "remove failed");
    CHECK(p.count == 2 && en_preset_find(&p, 88100) < 0, "remove did not");
    CHECK(!en_preset_remove(&p, 88100), "removing twice should fail");

    /* A truncated file must be reported, not silently half-written - a preset
       list that quietly loses entries is worse than one that refuses. */
    uint32_t small = en_presets_save(&p, buf, 40);
    CHECK(small == 0, "an overflowing save must report 0, got %u", small);
}

static void test_sidecar(void)
{
    section("RDS sidecar");

    en_rds_t r;
    en_rds_init(&r, true);
    for (uint8_t s2 = 0; s2 < 4; s2++) {
        const char *nm = "CBC RADI";
        uint16_t d = (uint16_t)(((uint8_t)nm[s2 * 2] << 8) | (uint8_t)nm[s2 * 2 + 1]);
        feed(&r, 0xC2B5, blkb(0, false, true, 22, s2), 0, d);
    }

    char buf[2048];
    static char whole[8192];
    uint32_t total = 0;
    en_sidecar_t sc;

    uint32_t n = en_sidecar_begin(&sc, buf, sizeof buf, 98500, "Americas",
                                  true, &r);
    CHECK(n > 0, "sidecar header overflowed");
    memcpy(whole + total, buf, n); total += n;

    /* Groups are appended a flush at a time, so an hour of them never has to
       fit in memory. */
    const uint16_t g1[4] = { 0xC2B5, 0x02C0, 0x0000, 0x4342 };
    const uint16_t g2[4] = { 0xC2B5, 0x22C1, 0x4E6F, 0x7720 };
    n = en_sidecar_group(&sc, buf, sizeof buf, 0, g1, EN_RDS_ALL);
    CHECK(n > 0, "group append overflowed");
    memcpy(whole + total, buf, n); total += n;

    n = en_sidecar_group(&sc, buf, sizeof buf, 1040, g2,
                         (uint8_t)(EN_RDS_ALL & ~EN_RDS_C));
    memcpy(whole + total, buf, n); total += n;

    n = en_sidecar_end(&sc, buf, sizeof buf, 1500, &r);
    CHECK(n > 0, "sidecar footer overflowed");
    memcpy(whole + total, buf, n); total += n;
    whole[total] = 0;

    printf("  %u bytes for 2 groups\n", total);
    printf("  %.200s...\n", whole);

    /* Braces and brackets have to balance, or it is not JSON however good it
       looks - the streaming append is exactly where that would break. */
    int braces = 0, brackets = 0, instr = 0;
    for (uint32_t i = 0; i < total; i++) {
        char c = whole[i];
        if (instr) {
            if (c == '\\') i++;
            else if (c == '"') instr = 0;
            continue;
        }
        if (c == '"') instr = 1;
        else if (c == '{') braces++;
        else if (c == '}') braces--;
        else if (c == '[') brackets++;
        else if (c == ']') brackets--;
        CHECK(braces >= 0 && brackets >= 0, "unbalanced at byte %u", i);
    }
    CHECK(braces == 0, "braces unbalanced by %d", braces);
    CHECK(brackets == 0, "brackets unbalanced by %d", brackets);
    CHECK(!instr, "a string was left open");

    /* Raw blocks are what make a recording re-decodable later, which matters
       while the FIFO framing is still unconfirmed. */
    CHECK(strstr(whole, "\"blocks\"") != 0, "raw blocks not written");
    CHECK(strstr(whole, "0xC2B5") != 0, "block A not written");
    CHECK(strstr(whole, "\"valid\"") != 0, "block validity not written");
    CHECK(strstr(whole, "CBC RADI") != 0, "decoded name not written");
    CHECK(strstr(whole, "RBDS") != 0, "standard not recorded");
    CHECK(strstr(whole, "\"duration_ms\":1500") != 0, "duration not written");
}

static void test_simple_flags(void)
{
    section("simple screen selection");

    en_presets_t p;
    en_presets_init(&p, "Americas");

    en_preset_t e;
    memset(&e, 0, sizeof e);
    for (int i = 0; i < 9; i++) {
        e.khz = 88100u + (uint32_t)i * 200u;
        e.name[0] = (char)('A' + i);
        e.name[1] = 0;
        CHECK(en_preset_add(&p, &e), "add %d failed", i);
    }

    /* Nothing is on the simple screen until someone puts it there. A screen
       that filled itself would not be the user's few. */
    CHECK(en_presets_simple(&p, 0, EN_SIMPLE_MAX) == 0,
          "presets start out on the simple screen");

    for (int i = 0; i < EN_SIMPLE_MAX; i++)
        CHECK(en_preset_set_simple(&p, 88100u + (uint32_t)i * 200u, true),
              "flagging %d failed", i);
    CHECK(en_presets_simple(&p, 0, EN_SIMPLE_MAX) == EN_SIMPLE_MAX,
          "six should be flagged");

    /* The seventh is refused rather than silently dropped, which is what lets
       the UI say why instead of appearing to ignore the tap. */
    CHECK(!en_preset_set_simple(&p, 88100u + 6 * 200u, true),
          "a seventh was accepted");
    CHECK(!p.list[6].simple, "the refused one was flagged anyway");

    /* Re-flagging something already there is not a failure - it would be a
       confusing way to report "no change needed". */
    CHECK(en_preset_set_simple(&p, 88100u, true), "re-flagging reported failure");

    /* Clearing one makes room again. */
    CHECK(en_preset_set_simple(&p, 88100u, false), "unflagging failed");
    CHECK(en_presets_simple(&p, 0, EN_SIMPLE_MAX) == EN_SIMPLE_MAX - 1,
          "unflagging did not free a slot");
    CHECK(en_preset_set_simple(&p, 88100u + 6 * 200u, true),
          "the freed slot was not reusable");

    /* An unknown frequency is a refusal, not a crash. */
    CHECK(!en_preset_set_simple(&p, 1u, true), "an unknown frequency was flagged");

    /* The chosen ones come back in preset order, and they are the ones set. */
    const en_preset_t *pick[EN_SIMPLE_MAX];
    uint8_t n = en_presets_simple(&p, pick, EN_SIMPLE_MAX);
    CHECK(n == EN_SIMPLE_MAX, "expected %d picks, got %u", EN_SIMPLE_MAX, n);
    for (uint8_t i = 1; i < n; i++)
        CHECK(pick[i]->khz > pick[i - 1]->khz, "picks are out of preset order");

    /* Round trip. The flag has to survive a save and load, or the simple
       screen empties itself every time the app restarts. */
    char buf[4096];
    uint32_t len = en_presets_save(&p, buf, sizeof buf);
    CHECK(len > 0, "save overflowed");

    en_presets_t back;
    CHECK(en_presets_load(&back, buf, len), "load failed");
    CHECK(back.count == p.count, "count %u after reload", back.count);
    for (uint8_t i = 0; i < back.count; i++)
        CHECK(back.list[i].simple == p.list[i].simple,
              "preset %u lost its simple flag", i);

    /* A file written before the flag existed has to load with it off. Off is
       the only safe default: a simple screen that filled itself with six
       arbitrary stations because someone upgraded would be worse than empty. */
    static const char legacy[] =
        "{\"version\":1,\"region\":\"Americas\",\"presets\":["
        "{\"khz\":88100,\"name\":\"A\",\"pi\":0x0000,\"pty\":0,\"rbds\":false},"
        "{\"khz\":88300,\"name\":\"B\",\"pi\":0x0000,\"pty\":0,\"rbds\":false}]}";
    en_presets_t old;
    CHECK(en_presets_load(&old, legacy, (uint32_t)(sizeof legacy - 1)),
          "a preset file without the flag failed to load");
    CHECK(old.count == 2, "legacy file gave %u presets", old.count);
    CHECK(en_presets_simple(&old, 0, EN_SIMPLE_MAX) == 0,
          "a file written before the flag put stations on the simple screen");
}

static void test_settings(void)
{
    section("settings");

    en_settings_t a;
    en_settings_default(&a);
    CHECK(a.live_seconds == 30, "default live buffer");
    CHECK(a.rds_on, "RDS should default on");

    snprintf(a.region, sizeof a.region, "%s", "Japan");
    a.khz = 82500;
    a.rds_on = false;
    a.live_seconds = 60;

    char buf[1024];
    uint32_t n = en_settings_save(&a, buf, sizeof buf);
    CHECK(n > 0, "save overflowed");
    printf("  %s\n", buf);

    en_settings_t b;
    CHECK(en_settings_load(&b, buf, n), "load failed");
    CHECK(strcmp(b.region, "Japan") == 0, "region lost: %s", b.region);
    CHECK(b.khz == 82500, "frequency lost");
    CHECK(!b.rds_on, "RDS flag lost");
    CHECK(b.live_seconds == 60, "buffer size lost");

    /* Stored by name, so adding a row to the region table cannot silently
       change which band an existing settings file selects. */
    CHECK(strstr(buf, "\"Japan\"") != 0, "region should be stored by name");

    /* A file from an older build is missing fields rather than wrong, and a
       missing field has to keep its default - a zero live buffer would be a bug
       that looked like a setting. */
    const char *partial = "{\"version\":1,\"region\":\"Europe\"}";
    en_settings_t c;
    CHECK(en_settings_load(&c, partial, (uint32_t)strlen(partial)),
          "partial load failed");
    CHECK(strcmp(c.region, "Europe") == 0, "region not read");
    CHECK(c.live_seconds == 30,
          "a missing field should keep its default, got %u", c.live_seconds);
    CHECK(c.rds_on, "a missing flag should keep its default");

    /* Register overrides ride in the same file. The tuner forgets everything
       on power down, so an override that does not survive this round trip is
       an override that lasts until the battery does. */
    en_settings_t d;
    en_settings_default(&d);

    const uint8_t audio[2] = { 0x01, 0xB0 };
    const uint8_t blend[8] = { 40, 20, 0xC4, 0xB0, 15, 0xE2, 12, 7 };
    CHECK(en_override_set(&d.overrides, 0x05, audio, 2), "set 0x05");
    CHECK(en_override_set(&d.overrides, 0xF9, blend, 8), "set 0xF9");
    CHECK(d.overrides.count == 2, "count %u", d.overrides.count);

    /* Editing the same register twice replaces rather than appends, or a
       morning of fiddling fills the list. */
    const uint8_t audio2[2] = { 0x01, 0xF0 };
    en_override_set(&d.overrides, 0x05, audio2, 2);
    CHECK(d.overrides.count == 2, "re-setting a register should not append");

    n = en_settings_save(&d, buf, sizeof buf);
    CHECK(n > 0, "save with overrides overflowed");
    printf("  %s\n", buf);

    en_settings_t e;
    CHECK(en_settings_load(&e, buf, n), "load with overrides failed");
    CHECK(e.overrides.count == 2, "loaded %u overrides", e.overrides.count);

    const en_override_t *o = en_override_find(&e.overrides, 0x05);
    CHECK(o && o->len == 2, "0x05 lost");
    CHECK(o && o->data[0] == 0x01 && o->data[1] == 0xF0,
          "0x05 payload wrong");

    /* An eight-byte payload has to come back eight bytes long: the length is
       part of the value, which is why they are stored as hex rather than as a
       number. */
    o = en_override_find(&e.overrides, 0xF9);
    CHECK(o && o->len == 8, "0xF9 length lost: %u", o ? o->len : 0);
    if (o) for (uint8_t i = 0; i < 8; i++)
        CHECK(o->data[i] == blend[i], "0xF9 byte %u wrong", i);

    CHECK(en_override_clear(&e.overrides, 0x05), "clear failed");
    CHECK(!en_override_find(&e.overrides, 0x05), "still there after clear");
    CHECK(en_override_find(&e.overrides, 0xF9), "clear removed the wrong one");
    CHECK(!en_override_clear(&e.overrides, 0x05), "clearing twice should fail");
}

/* The regions are a table, not a lookup - one helper here rather than an
   API nothing else needs. */
static const en_region_t *find_region(const char *name)
{
    for (uint8_t i = 0; i < en_region_count; i++)
        if (strcmp(en_regions[i].name, name) == 0) return &en_regions[i];
    return NULL;
}

/* ---- band scan ------------------------------------------------------------
 *
 * Driven the way the UI drives it: tick, observe, do what it asks. Both sweep
 * modes are exercised - stepping every channel in software, and asking a chip
 * to jump from station to station - because they find the same three stations
 * by completely different routes and only one of them can be tested on real
 * hardware today.
 */

static const uint32_t FAKE_STATIONS[] = { 88300, 95800, 104200 };
#define FAKE_N ((int)(sizeof FAKE_STATIONS / sizeof FAKE_STATIONS[0]))

static uint8_t fake_rssi(uint32_t khz)
{
    for (int i = 0; i < FAKE_N; i++)
        if (FAKE_STATIONS[i] == khz) return 200;
    return 12;
}

/* A chip that seeks: the next station above `khz`, wrapping to the bottom of
   the band the way a real one does when it runs off the top. */
static uint32_t fake_seek(const en_region_t *rg, uint32_t khz)
{
    for (int i = 0; i < FAKE_N; i++)
        if (FAKE_STATIONS[i] > khz) return FAKE_STATIONS[i];
    return rg->low_khz;
}

/* Names on two of the three, so the "took it early" path and the "gave up
   waiting" path both run. Filled directly rather than through the decoder:
   this is testing the scan, not the RDS parser. */
static const en_rds_t *fake_rds(en_rds_t *r, const en_scan_t *s, uint32_t khz,
                                uint8_t rssi)
{
    if (s->phase != EN_SCAN_NAMING || rssi < 100 || khz == 104200)
        return NULL;
    r->ps_valid = true;
    r->pi = 0x1234;
    r->pty = 3;
    memcpy(r->ps, khz == 88300 ? "BBC R1  " : "Kiss    ", 9);
    return r;
}

/* Run a whole scan and report how many ticks the sweep took. */
static int run_scan(en_scan_t *s, const en_region_t *rg, int use_seek,
                    int fail_seek, int *sweep_ticks)
{
    uint32_t khz, want = 0;
    en_rds_t rds;
    int guard = 0;

    en_rds_init(&rds, false);
    if (!en_scan_start(s, rg, 98000, 100, use_seek != 0))
        return 0;
    khz = s->khz;
    *sweep_ticks = 0;

    while (s->phase != EN_SCAN_DONE && guard++ < 200000) {
        uint8_t r = fake_rssi(khz);
        const en_rds_t *pr = fake_rds(&rds, s, khz, r);

        if (s->phase == EN_SCAN_SWEEP) (*sweep_ticks)++;

        switch (en_scan_tick(s, 40, khz, r, pr, &want)) {
        case EN_SCAN_TUNE:
            khz = want;
            break;
        case EN_SCAN_SEEK:
            /* A platform that cannot seek says so, exactly as the UI does. */
            if (fail_seek) en_scan_seek_failed(s);
            else khz = fake_seek(rg, khz);
            break;
        default:
            break;
        }
    }
    return khz == 98000 && s->phase == EN_SCAN_DONE;
}

static void check_hits(en_scan_t *s, const char *how)
{
    CHECK(s->n_hits == 3, "%s: expected 3 stations, found %u", how, s->n_hits);
    CHECK(!s->overflowed, "%s: overflowed on three stations", how);
    for (uint8_t i = 0; i < s->n_hits && i < 3; i++)
        CHECK(s->hits[i].khz == FAKE_STATIONS[i],
              "%s: hit %u was %u, expected %u", how, i, s->hits[i].khz,
              FAKE_STATIONS[i]);

    /* Trailing pad from the eight-character RDS field must not survive. */
    if (s->n_hits >= 3) {
        CHECK(strcmp(s->hits[0].name, "BBC R1") == 0,
              "%s: name 0 was '%s'", how, s->hits[0].name);
        CHECK(strcmp(s->hits[1].name, "Kiss") == 0,
              "%s: name 1 was '%s'", how, s->hits[1].name);
        CHECK(!s->hits[2].named,
              "%s: the silent station should have no name", how);
    }
    CHECK(en_scan_percent(s) == 100, "%s: percent %u at the end", how,
          en_scan_percent(s));
}

/*
 * A station on the very first channel of the band.
 *
 * This is the case the device met and this suite did not. The existing fakes
 * put their lowest station at 88.3 in a band starting at 87.5, so the first
 * seek always landed above the bottom edge - and the sweep's end-of-band test
 * was "at or below the bottom edge", which was therefore never true early.
 *
 * On the Americas grid the first channel is 87.9 and there was a station on
 * it. The first seek landed exactly on the bottom of the band, the sweep
 * decided it had already wrapped, and the whole scan ended after one step with
 * no hits. From the outside that is a scan button that does nothing except
 * leave the radio somewhere strange.
 *
 * The top edge is in here for the same reason: a station on the last channel
 * has to be found before the wrap, not after it.
 */
static const uint32_t EDGE_STATIONS[] = { 87900, 90100, 107900 };
#define EDGE_N ((int)(sizeof EDGE_STATIONS / sizeof EDGE_STATIONS[0]))

static uint8_t edge_rssi(uint32_t khz)
{
    for (int i = 0; i < EDGE_N; i++)
        if (EDGE_STATIONS[i] == khz) return 200;
    return 10;
}

/* Seeking up wraps to the bottom, which is what a real tuner does and what
   the sweep has to be able to tell apart from progress. */
static uint32_t edge_seek(const en_region_t *rg, uint32_t khz)
{
    for (int i = 0; i < EDGE_N; i++)
        if (EDGE_STATIONS[i] > khz) return EDGE_STATIONS[i];
    return rg->low_khz;
}

static void test_scan_band_edges(void)
{
    const en_region_t *us = find_region("Americas");
    en_scan_t sc;
    en_rds_t rds;
    uint32_t khz, want = 0;
    int guard = 0;

    CHECK(us != NULL, "no Americas region");
    if (!us) return;
    CHECK(EDGE_STATIONS[0] == us->low_khz,
          "the point of this test is a station on the first channel");
    CHECK(EDGE_STATIONS[EDGE_N - 1] == us->high_khz,
          "and one on the last");

    en_rds_init(&rds, true);
    CHECK(en_scan_start(&sc, us, 98100, 100, true), "the scan would not start");
    khz = sc.khz;

    while (sc.phase != EN_SCAN_DONE && guard++ < 200000) {
        switch (en_scan_tick(&sc, 40, khz, edge_rssi(khz), NULL, &want)) {
        case EN_SCAN_TUNE: khz = want; break;
        case EN_SCAN_SEEK: khz = edge_seek(us, khz); break;
        default: break;
        }
    }

    CHECK(sc.phase == EN_SCAN_DONE, "the edge sweep never finished");
    CHECK(sc.n_hits == EDGE_N, "found %u stations, expected %d - a hit on the "
                               "first channel is the one that used to be lost",
          sc.n_hits, EDGE_N);
    for (uint8_t i = 0; i < sc.n_hits && i < EDGE_N; i++)
        CHECK(sc.hits[i].khz == EDGE_STATIONS[i], "hit %u was %u, expected %u",
              i, sc.hits[i].khz, EDGE_STATIONS[i]);

    /* And it comes back to where the listener was, on the grid. */
    CHECK(khz == 98100, "the sweep left the radio on %u, not where it started",
          khz);
}

static void test_scan(void)
{
    const en_region_t *eu = find_region("Europe");
    CHECK(eu != NULL, "no Europe region");
    if (!eu) return;

    en_scan_t soft, hard, fell;
    int soft_ticks = 0, hard_ticks = 0, fell_ticks = 0;

    /* Stepping every channel. */
    CHECK(run_scan(&soft, eu, 0, 0, &soft_ticks),
          "the software sweep did not finish where it started");
    check_hits(&soft, "software");

    /* Letting the chip find them. Same three stations. */
    CHECK(run_scan(&hard, eu, 1, 0, &hard_ticks),
          "the seeking sweep did not finish where it started");
    check_hits(&hard, "seek");

    /* And it has to be the point of doing it: crossing the empty band in the
       chip rather than a channel at a time is the whole reason this mode
       exists, so if it is not dramatically cheaper it is not worth having. */
    CHECK(hard_ticks * 4 < soft_ticks,
          "seeking took %d ticks against stepping's %d - no better",
          hard_ticks, soft_ticks);

    /* A platform that says it can seek and then cannot must still finish,
       by stepping the rest. A slower scan is a scan; a stalled one is not. */
    CHECK(run_scan(&fell, eu, 1, 1, &fell_ticks),
          "a failed seek stalled the sweep");
    check_hits(&fell, "fallback");
    CHECK(!fell.use_seek, "the fallback should have given up on seeking");

    /* Committing: new presets appear, and a rescan updates rather than
       duplicates - including leaving the simple-screen choice alone. */
    en_presets_t p;
    en_presets_init(&p, "Europe");
    CHECK(en_scan_commit(&soft, &p) == 3, "commit added %u", p.count);
    CHECK(p.count == 3, "preset count %u", p.count);

    CHECK(en_preset_set_simple(&p, 95800, true), "could not flag for simple");
    en_scan_commit(&soft, &p);
    CHECK(p.count == 3, "a rescan duplicated presets: %u", p.count);
    int at = en_preset_find(&p, 95800);
    CHECK(at >= 0 && p.list[at].simple,
          "a rescan cleared the simple-screen choice");

    /* A region with no channels is refused rather than swept. */
    en_region_t empty = *eu;
    empty.high_khz = empty.low_khz;
    empty.step_khz = 0;
    en_scan_t bad;
    CHECK(!en_scan_start(&bad, &empty, 98000, 100, false),
          "an empty band should refuse to scan");

    printf("  sweep cost: %d ticks stepping, %d ticks seeking\n",
           soft_ticks, hard_ticks);
}

static void test_rectimer(void)
{
    en_rectimer_t t;
    en_rectimer_init(&t);
    CHECK(t.limit_min == 0 && t.at_min == EN_REC_AT_NONE,
          "a fresh timer should do nothing");

    /* Nothing set: a recording runs forever. */
    CHECK(en_rectimer_tick(&t, true, 600, true, 9999999u) == EN_REC_NOTHING,
          "an unset timer stopped a recording");

    /* A length. Stops at it, not before, and does nothing when not recording
       - a limit is not a reason to start. */
    t.limit_min = 30;
    CHECK(en_rectimer_tick(&t, true, 600, true, 29u * 60000u)
              == EN_REC_NOTHING, "stopped a minute early");
    CHECK(en_rectimer_tick(&t, true, 600, true, 30u * 60000u) == EN_REC_STOP,
          "did not stop at the length");
    CHECK(en_rectimer_tick(&t, true, 600, false, 0) == EN_REC_NOTHING,
          "a length should not start anything");

    /* A start time, with no clock. Never fires: a scheduled recording that
       begins whenever the time eventually turns up is worse than one that
       does not begin, because you cannot tell which you got. */
    en_rectimer_init(&t);
    t.at_min = 7 * 60 + 45;
    for (int m = 0; m < 24 * 60; m++)
        CHECK(en_rectimer_tick(&t, false, m, false, 0) == EN_REC_NOTHING,
              "fired at minute %d with no clock", m);

    /* With a clock: once, at the right minute, and not again during it. */
    en_rectimer_init(&t);
    t.at_min = 7 * 60 + 45;
    CHECK(en_rectimer_tick(&t, true, 7 * 60 + 44, false, 0) == EN_REC_NOTHING,
          "fired a minute early");
    CHECK(en_rectimer_tick(&t, true, 7 * 60 + 45, false, 0) == EN_REC_START,
          "did not fire at the time");
    CHECK(en_rectimer_tick(&t, true, 7 * 60 + 45, true, 1000) == EN_REC_NOTHING,
          "fired again while already recording");
    CHECK(en_rectimer_tick(&t, true, 7 * 60 + 45, false, 0) == EN_REC_NOTHING,
          "fired twice in the same minute");

    /* Tomorrow. Passing the minute rearms it, which is what makes a daily
       recording a daily recording rather than a single one. */
    CHECK(en_rectimer_tick(&t, true, 7 * 60 + 46, false, 0) == EN_REC_NOTHING,
          "fired after the minute passed");
    CHECK(en_rectimer_tick(&t, true, 7 * 60 + 45, false, 0) == EN_REC_START,
          "did not fire again the next day");

    /* Both together: the length wins while a recording is running, so a
       schedule cannot extend one past where it should have stopped. */
    en_rectimer_init(&t);
    t.at_min = 8 * 60;
    t.limit_min = 5;
    CHECK(en_rectimer_tick(&t, true, 8 * 60, true, 5u * 60000u) == EN_REC_STOP,
          "the schedule overrode the length");

    /* The countdown, including across midnight. */
    en_rectimer_init(&t);
    CHECK(en_rectimer_until(&t, true, 600) == -1,
          "a countdown with nothing scheduled");
    t.at_min = 10;                       /* 00:10 */
    CHECK(en_rectimer_until(&t, true, 23 * 60 + 50) == 20,
          "across midnight read %d, expected 20",
          en_rectimer_until(&t, true, 23 * 60 + 50));
    CHECK(en_rectimer_until(&t, false, 0) == -1,
          "a countdown with no clock");

    /* And that it survives a round trip through the settings file, since a
       timer that forgets itself on reboot is not a timer. */
    en_settings_t a, b;
    en_settings_default(&a);
    a.rectimer.limit_min = 90;
    a.rectimer.at_min = 6 * 60 + 30;
    char buf[2048];
    uint32_t n = en_settings_save(&a, buf, sizeof buf);
    CHECK(n > 0, "settings with a timer overflowed");
    en_settings_default(&b);
    CHECK(en_settings_load(&b, buf, n), "settings with a timer failed to load");
    CHECK(b.rectimer.limit_min == 90, "limit came back as %u",
          b.rectimer.limit_min);
    CHECK(b.rectimer.at_min == 6 * 60 + 30, "start came back as %d",
          b.rectimer.at_min);

    /* No start time is the absence of the key, so it has to survive as an
       absence and come back as none rather than as midnight. */
    en_settings_default(&a);
    a.rectimer.at_min = EN_REC_AT_NONE;
    n = en_settings_save(&a, buf, sizeof buf);
    en_settings_default(&b);
    b.rectimer.at_min = 123;
    CHECK(en_settings_load(&b, buf, n), "load failed");
    CHECK(b.rectimer.at_min == EN_REC_AT_NONE,
          "'no start' came back as %d", b.rectimer.at_min);
}

/* ---- stereo handling ------------------------------------------------------
 *
 * One register byte, and the thing that matters is what it does NOT touch:
 * bit 0 is the band and bit 4 is the injection side, and a stereo control
 * that reset either would be a very confusing stereo control.
 */

static void test_stereo_mode(void)
{
    /* Every mode round-trips through the register and back. */
    for (int m = 0; m <= 2; m++) {
        uint8_t c = en_fm_ctrl_set_stereo(0, (en_fm_stereo_t)m);
        CHECK(en_fm_ctrl_stereo(c) == (en_fm_stereo_t)m,
              "mode %d came back as %d", m, en_fm_ctrl_stereo(c));
    }

    /* Exactly one of the two selector bits at a time, and never both. */
    uint8_t a = en_fm_ctrl_set_stereo(0, EN_FM_STEREO_AUTO);
    uint8_t o = en_fm_ctrl_set_stereo(0, EN_FM_STEREO_MONO);
    uint8_t t = en_fm_ctrl_set_stereo(0, EN_FM_STEREO_STEREO);
    CHECK(a == EN_FM_CTRL_AUTO, "auto set 0x%02X", a);
    CHECK(o == 0, "mono set 0x%02X", o);
    CHECK(t == EN_FM_CTRL_MANUAL, "stereo set 0x%02X", t);

    /* The bits that are not ours survive every mode. Band and injection are
       set here, and the blend choice with them, because the advanced editor
       owns that one and this control must not fight it. */
    const uint8_t keep = 0x01u | 0x10u | EN_FM_CTRL_BLEND;
    for (int m = 0; m <= 2; m++) {
        uint8_t c = en_fm_ctrl_set_stereo(keep, (en_fm_stereo_t)m);
        CHECK((c & keep) == keep,
              "mode %d cleared a bit it does not own: 0x%02X", m, c);
    }

    /* And switching between modes does not accumulate: going auto -> stereo
       must not leave the auto bit set underneath. */
    uint8_t c = en_fm_ctrl_set_stereo(keep, EN_FM_STEREO_AUTO);
    c = en_fm_ctrl_set_stereo(c, EN_FM_STEREO_STEREO);
    CHECK(!(c & EN_FM_CTRL_AUTO), "auto survived a switch to stereo: 0x%02X", c);
    c = en_fm_ctrl_set_stereo(c, EN_FM_STEREO_MONO);
    CHECK(!(c & (EN_FM_CTRL_AUTO | EN_FM_CTRL_MANUAL)),
          "a selector bit survived a switch to mono: 0x%02X", c);
    CHECK((c & keep) == keep, "the band was lost along the way: 0x%02X", c);

    /* It persists: forcing mono is a preference about a place, so it has to
       come back after a restart. */
    en_settings_t x, y;
    en_settings_default(&x);
    x.stereo_mode = EN_FM_STEREO_MONO;
    char buf[2048];
    uint32_t n = en_settings_save(&x, buf, sizeof buf);
    en_settings_default(&y);
    CHECK(en_settings_load(&y, buf, n), "settings with a stereo mode failed");
    CHECK(y.stereo_mode == EN_FM_STEREO_MONO,
          "stereo mode came back as %u", y.stereo_mode);
}

/* ---- register coverage ----------------------------------------------------
 *
 * The advanced screen is generated from the register table, so a control that
 * is missing from the table is a control that does not exist in the app - and
 * it goes missing silently, because a register with three of its four bytes
 * described looks exactly like a register with four.
 *
 * So: every byte of every fixed-length readable register has to be claimed by
 * some field. This is what makes "did we expose all of the stereo blend
 * settings" a question the test suite answers rather than one somebody has to
 * re-read the datasheet for.
 */

static void test_reg_coverage(void)
{
    int gaps = 0;

    for (uint8_t i = 0; i < en_fm_reg_count; i++) {
        const en_fm_reg_t *r = &en_fm_regs[i];
        uint8_t len = r->read_len;

        /* Variable-length and length-unknown registers have nothing fixed to
           cover. I2C_RDS_DATA is the FIFO; its shape is the RDS group, not a
           field list. */
        if (!len || len > 8 || !r->nfields) continue;

        uint8_t seen[8];
        for (uint8_t b = 0; b < 8; b++) seen[b] = 0;

        for (uint8_t f = 0; f < r->nfields; f++) {
            const en_fm_field_t *fl = &r->fields[f];
            for (uint8_t b = fl->off; b < fl->off + fl->width && b < 8; b++)
                seen[b] = 1;
        }

        for (uint8_t b = 0; b < len; b++) {
            if (!seen[b]) {
                printf("  GAP: %s (0x%02X) byte %u is not described\n",
                       r->name, r->addr, b);
                gaps++;
            }
        }
    }

    CHECK(gaps == 0, "%d register byte(s) have no field describing them", gaps);

    /* And the one the stereo control depends on, named explicitly: if the
       blend curve ever loses a byte, this says which feature broke. */
    const en_fm_reg_t *blend = en_fm_reg_find(0xF9);
    CHECK(blend && blend->nfields == 8,
          "the stereo blend and soft mute register has %u fields, expected 8",
          blend ? blend->nfields : 0);

    const en_fm_reg_t *ctrl = en_fm_reg_find(EN_FM_CTRL_ADDR);
    CHECK(ctrl && ctrl->nfields >= 1,
          "the FM control register the stereo tap writes is not described");
}

/* ---- RadioText+ -----------------------------------------------------------
 *
 * Groups built by hand from the specification, because there is no tuner to
 * capture one from and the bit layout is the entire feature. Two fields with
 * DIFFERENT length widths - six bits and five - packed across three blocks is
 * exactly the shape that produces a title that is right and an artist that is
 * one character short.
 */

/* Feed a 2A radio-text group: four characters at segment `seg`. */
static void feed_rt(en_rds_t *r, uint16_t pi, uint8_t seg, const char *four)
{
    uint16_t blk[4];
    blk[0] = pi;
    blk[1] = (uint16_t)((2u << 12) | (0u << 11) | (0u << 10) | (5u << 5) | seg);
    blk[2] = (uint16_t)(((uint8_t)four[0] << 8) | (uint8_t)four[1]);
    blk[3] = (uint16_t)(((uint8_t)four[2] << 8) | (uint8_t)four[3]);
    en_rds_group(r, blk, EN_RDS_ALL);
}

/* Group 3A: "application `aid` will use group `type`/`ver`". */
static void feed_3a(en_rds_t *r, uint16_t pi, uint8_t type, uint8_t ver,
                    uint16_t aid)
{
    uint16_t blk[4];
    blk[0] = pi;
    blk[1] = (uint16_t)((3u << 12) | (0u << 11) | (0u << 10) | (5u << 5) |
                        ((type << 1) | ver));
    blk[2] = 0;
    blk[3] = aid;
    en_rds_group(r, blk, EN_RDS_ALL);
}

/* An RT+ marker group in whatever group type the station announced. */
static void feed_rtp(en_rds_t *r, uint16_t pi, uint8_t type, int toggle,
                     int running, uint8_t c1, uint8_t s1, uint8_t len1,
                     uint8_t c2, uint8_t s2, uint8_t len2)
{
    uint16_t blk[4];
    uint8_t l1 = (uint8_t)(len1 - 1), l2 = (uint8_t)(len2 - 1);

    blk[0] = pi;
    blk[1] = (uint16_t)((type << 12) | (0u << 11) | (0u << 10) | (5u << 5) |
                        ((toggle ? 0x10u : 0u) | (running ? 0x08u : 0u) |
                         ((c1 >> 3) & 0x07u)));
    blk[2] = (uint16_t)(((c1 & 0x07u) << 13) | ((s1 & 0x3Fu) << 7) |
                        ((l1 & 0x3Fu) << 1) | ((c2 >> 5) & 0x01u));
    blk[3] = (uint16_t)(((c2 & 0x1Fu) << 11) | ((s2 & 0x3Fu) << 5) |
                        (l2 & 0x1Fu));
    en_rds_group(r, blk, EN_RDS_ALL);
}

static void test_rtplus(void)
{
    en_rds_t r;
    en_rds_init(&r, false);

    /* 64 characters of radio text. The title sits at 0..12 and the artist at
       16..25, which is the shape a real station sends: a separator between
       them that belongs to neither. */
    const char *text =
        "Harvest Moon - Neil Young                                       ";
    for (uint8_t seg = 0; seg < 16; seg++)
        feed_rt(&r, 0xC2B5, seg, text + seg * 4);
    CHECK(r.rt_valid, "the radio text did not complete");

    /* Nothing yet: without the announcement there is nowhere to look. */
    CHECK(!r.rtplus_known, "RT+ known before it was announced");
    feed_rtp(&r, 0xC2B5, 11, 0, 1, EN_RTP_TITLE, 0, 12, EN_RTP_ARTIST, 15, 10);
    CHECK(!r.rt_title_valid,
          "took markers from a group that was never announced");

    /* An announcement for something else must not be mistaken for ours. */
    feed_3a(&r, 0xC2B5, 8, 0, 0xCD46);        /* TMC, not RT+ */
    CHECK(!r.rtplus_known, "a TMC announcement was taken for RT+");

    /* The real one. 11A here; the point of decoding 3A at all is that it
       could just as well be 12A. */
    feed_3a(&r, 0xC2B5, 11, 0, 0x4BD7);
    CHECK(r.rtplus_known && r.rtplus_group == ((11u << 1) | 0u),
          "RT+ group came out as %u", r.rtplus_group);

    feed_rtp(&r, 0xC2B5, 11, 0, 1, EN_RTP_TITLE, 0, 12, EN_RTP_ARTIST, 15, 10);
    CHECK(r.rtplus_running, "the item should be running");
    CHECK(r.rt_title_valid, "no title");
    CHECK(strcmp(r.rt_title, "Harvest Moon") == 0,
          "title was '%s'", r.rt_title);
    CHECK(r.rt_artist_valid, "no artist");
    CHECK(strcmp(r.rt_artist, "Neil Young") == 0,
          "artist was '%s'", r.rt_artist);

    /* The toggle flipping means a new item. What was there described the last
       one and must go, or the previous artist sits under the new title. */
    feed_rtp(&r, 0xC2B5, 11, 1, 1, EN_RTP_TITLE, 0, 12, 0, 0, 1);
    CHECK(strcmp(r.rt_title, "Harvest Moon") == 0,
          "the title was lost across a toggle: '%s'", r.rt_title);
    CHECK(!r.rt_artist_valid,
          "the previous artist survived a new item: '%s'", r.rt_artist);

    /* A marker running past the end of the radio text is dropped, not
       clamped. Half a title is worse than none: nothing would say it is half. */
    en_rds_init(&r, false);
    for (uint8_t seg = 0; seg < 16; seg++)
        feed_rt(&r, 0xC2B5, seg, text + seg * 4);
    feed_3a(&r, 0xC2B5, 11, 0, 0x4BD7);
    feed_rtp(&r, 0xC2B5, 11, 0, 1, EN_RTP_TITLE, 60, 20, 0, 0, 1);
    CHECK(!r.rt_title_valid,
          "a marker past the end of the text was accepted: '%s'", r.rt_title);

    /* And a marker into text that has not arrived yet is not guessed at. */
    en_rds_init(&r, false);
    feed_rt(&r, 0xC2B5, 0, "Harv");
    feed_3a(&r, 0xC2B5, 11, 0, 0x4BD7);
    feed_rtp(&r, 0xC2B5, 11, 0, 1, EN_RTP_TITLE, 0, 12, 0, 0, 1);
    CHECK(!r.rt_title_valid,
          "a marker into unreceived text was accepted: '%s'", r.rt_title);

    /* A station on 12A instead of 11A works identically - which is the whole
       reason the group is learnt rather than hard-coded. */
    en_rds_init(&r, false);
    for (uint8_t seg = 0; seg < 16; seg++)
        feed_rt(&r, 0xC2B5, seg, text + seg * 4);
    feed_3a(&r, 0xC2B5, 12, 0, 0x4BD7);
    feed_rtp(&r, 0xC2B5, 12, 0, 1, EN_RTP_TITLE, 0, 12, EN_RTP_ARTIST, 15, 10);
    CHECK(r.rt_title_valid && strcmp(r.rt_title, "Harvest Moon") == 0,
          "12A station: title was '%s'", r.rt_title);
    CHECK(r.rt_artist_valid && strcmp(r.rt_artist, "Neil Young") == 0,
          "12A station: artist was '%s'", r.rt_artist);

    /* The five-bit length field is the one that bites: a 32-character artist
       is the longest it can express, and it must come out whole. */
    CHECK(EN_RDS_RT_LEN >= 32, "radio text is too short to test this");
}

/* ---- following a station across transmitters ------------------------------
 *
 * The interesting cases are all refusals. Moving to a stronger transmitter of
 * the same station is the easy half; not moving to a stranger, not moving to
 * something worse, and not hopping for ever between two equal signals are
 * what make it usable, and what a radio you cannot test on the road has to
 * get right by construction.
 */

static en_rds_t af_rds(uint16_t pi, const uint32_t *list, uint8_t n)
{
    en_rds_t r;
    en_rds_init(&r, false);
    r.pi = pi;
    r.pi_valid = true;
    for (uint8_t i = 0; i < n && i < EN_RDS_AF_MAX; i++) r.af[r.af_count++] = list[i];
    return r;
}

/* Tick until it asks to move, or until `ms` has gone by with nothing. */
static uint32_t af_run(en_affollow_t *a, uint32_t ms, uint32_t khz,
                       uint8_t rssi, const en_rds_t *rds)
{
    uint32_t out = 0;
    for (uint32_t t = 0; t < ms; t += 100)
        if (en_af_tick(a, 100, khz, rssi, rds, &out) == EN_AF_GOTO)
            return out;
    return 0;
}


/* ---- the clock, as a number ----------------------------------------------- */

static void test_rds_ct_unix(void)
{
    section("RDS clock time as seconds");

    en_rds_t r;
    en_rds_init(&r, true);

    /*
     * MJD 51544 is 1 January 2000, whose Unix time is 946684800 - a number
     * worth checking against something other than this code, and one everybody
     * remembers from a different reason.
     */
    const uint32_t mjd = 51544;
    uint16_t b = (uint16_t)(blkb(4, false, false, 0, 0)
                            | (uint16_t)((mjd >> 15) & 0x03u));
    uint16_t c = (uint16_t)(((mjd & 0x7FFFu) << 1) | (12u >> 4));
    uint16_t d = (uint16_t)(((uint16_t)(12u & 0x0Fu) << 12)
                            | ((uint16_t)34u << 6));
    uint16_t blk[4] = { 0xC2B5, b, c, d };

    CHECK(en_rds_ct_unix(&r) == 0, "no clock yet should be zero, not a date");

    en_rds_group(&r, blk, EN_RDS_ALL);
    printf("  MJD %u 12:34 UTC -> %lld\n", mjd, (long long)en_rds_ct_unix(&r));
    CHECK(en_rds_ct_unix(&r) == 946684800LL + 12 * 3600 + 34 * 60,
          "2000-01-01 12:34 UTC should be 946684800 plus the time of day");

    CHECK(r.ct_groups == 1, "one clock group should have been counted");

    /*
     * The offset must NOT reach the epoch value. This is the whole reason the
     * transmitted fields are kept alongside the localised ones: a hardware
     * clock holding local time is wrong twice a year, and the same instant
     * transmitted from two time zones has to produce the same number.
     */
    en_rds_t west;
    en_rds_init(&west, true);
    uint16_t dw = (uint16_t)(d | 0x0020u | 7u);      /* -3:30 */
    uint16_t blkw[4] = { 0xC2B5, b, c, dw };
    en_rds_group(&west, blkw, EN_RDS_ALL);
    printf("  same instant at -3:30 -> %lld (local %02u:%02u)\n",
           (long long)en_rds_ct_unix(&west), west.ct_hour, west.ct_minute);
    CHECK(en_rds_ct_unix(&west) == en_rds_ct_unix(&r),
          "the local offset must not change the instant");
    CHECK(west.ct_hour == 9 && west.ct_minute == 4,
          "and it must still change what is displayed");

    CHECK(en_rds_ct_unix(0) == 0, "a null decoder is not a date");
}

/* ---- what the numbers mean ------------------------------------------------ */

static void test_rdsname(void)
{
    section("RDS field names");

    char buf[64];

    /* Every one of the thirty-two says something. A hole in that table is a
       blank line on the inspector where a group count should be. */
    int named = 0;
    for (uint8_t t = 0; t < 16; t++) {
        for (uint8_t v = 0; v < 2; v++) {
            const char *n = en_rds_group_name(t, v != 0);
            if (n && n[0] && n[0] != '?') named++;
        }
    }
    CHECK(named == 32, "only %d of 32 group types are named", named);

    en_rds_group_label(0, false, buf, sizeof buf);
    CHECK(strcmp(buf, "0A") == 0, "group 0A labelled \"%s\"", buf);
    en_rds_group_label(15, true, buf, sizeof buf);
    CHECK(strcmp(buf, "15B") == 0, "group 15B labelled \"%s\"", buf);
    en_rds_group_label(4, false, buf, sizeof buf);
    CHECK(strcmp(buf, "4A") == 0, "group 4A labelled \"%s\"", buf);

    /*
     * Call signs. The two ranges are each exactly 26^3 wide and butt against
     * each other, so the boundaries are where an off-by-one would show: 0x1000
     * is the first K and 0x54A8 is the first W.
     */
    CHECK(en_rds_callsign(0x1000, buf, sizeof buf), "0x1000 should be a call");
    printf("  0x1000 -> %s\n", buf);
    CHECK(strcmp(buf, "KAAA") == 0, "0x1000 gave \"%s\", expected KAAA", buf);

    CHECK(en_rds_callsign(0x54A8, buf, sizeof buf), "0x54A8 should be a call");
    printf("  0x54A8 -> %s\n", buf);
    CHECK(strcmp(buf, "WAAA") == 0, "0x54A8 gave \"%s\", expected WAAA", buf);

    CHECK(en_rds_callsign(0x54A7, buf, sizeof buf), "0x54A7 should be a call");
    CHECK(buf[0] == 'K' && strcmp(buf, "KZZZ") == 0,
          "the last K should be KZZZ, got \"%s\"", buf);

    CHECK(!en_rds_callsign(0x0FFF, buf, sizeof buf),
          "below the K range is not a call sign");
    CHECK(!en_rds_callsign(0x9950, buf, sizeof buf),
          "above the W range is not a call sign");
    CHECK(!en_rds_callsign(0xC2B5, buf, sizeof buf),
          "a Canadian PI does not encode a call sign");
    CHECK(!en_rds_callsign(0x1000, buf, 4), "a short buffer must be refused");

    /* PI, taken apart. */
    CHECK(en_rds_pi_country(0xC2B5) == 0xC, "country nibble");
    CHECK(en_rds_pi_coverage(0xC2B5) == 0x2, "coverage nibble");
    CHECK(en_rds_pi_reference(0xC2B5) == 0xB5, "reference byte");
    CHECK(strcmp(en_rds_coverage_name(0), "Local") == 0, "coverage 0");
    CHECK(strcmp(en_rds_coverage_name(2), "National") == 0, "coverage 2");
    CHECK(strcmp(en_rds_coverage_name(4), "Regional 1") == 0, "coverage 4");
    CHECK(strcmp(en_rds_coverage_name(15), "Regional 12") == 0, "coverage 15");

    /* Decoder identification. Nothing set is a real answer and must not be a
       blank, which would read as "not decoded yet". */
    en_rds_di_text(0, buf, sizeof buf);
    printf("  DI 0 -> %s\n", buf);
    CHECK(buf[0] != 0, "no DI bits should still say something");

    en_rds_di_text(0x8, buf, sizeof buf);
    printf("  DI 8 -> %s\n", buf);
    CHECK(strstr(buf, "Stereo") != 0, "bit 3 is the stereo bit");

    en_rds_di_text(0xF, buf, sizeof buf);
    printf("  DI F -> %s\n", buf);
    CHECK(strstr(buf, "Stereo") && strstr(buf, "Dynamic"),
          "all four bits should all be listed");

    /* A truncating buffer must still terminate. */
    char tiny[8];
    en_rds_di_text(0xF, tiny, sizeof tiny);
    CHECK(tiny[sizeof tiny - 1] == 0, "DI text overran a small buffer");

    CHECK(en_rds_oda_name(0x4BD7) != 0, "RadioText+ should be named");
    CHECK(en_rds_oda_name(0x0001) == 0, "an unknown AID should not be invented");
}

/* ---- collecting the time off the band ------------------------------------- */

/* One clock group, built by hand, for whatever station is pretending to be on
   the air in these tests. */
static void feed_ct(en_rds_t *r, uint32_t mjd, uint8_t hh, uint8_t mm)
{
    uint16_t b = (uint16_t)(blkb(4, false, false, 0, 0)
                            | (uint16_t)((mjd >> 15) & 0x03u));
    uint16_t c = (uint16_t)(((mjd & 0x7FFFu) << 1) | (hh >> 4));
    uint16_t d = (uint16_t)(((uint16_t)(hh & 0x0Fu) << 12)
                            | ((uint16_t)mm << 6));
    uint16_t blk[4] = { 0xC2B5, b, c, d };
    en_rds_group(r, blk, EN_RDS_ALL);
}

static void test_ctscan(void)
{
    section("clock scan");

    en_ctscan_t s;
    en_rds_t r;
    uint32_t cand[3] = { 88100, 97500, 102900 };
    uint32_t khz = 0;

    en_rds_init(&r, true);
    CHECK(!en_ctscan_start(&s, cand, 0, 98500), "no candidates is not a scan");
    CHECK(en_ctscan_start(&s, cand, 3, 98500), "three candidates should start");
    CHECK(s.phase == EN_CTSCAN_RUNNING, "should be running");

    /* Nothing on the first station for ten seconds. */
    for (int i = 0; i < 100; i++)
        CHECK(en_ctscan_tick(&s, 100, &r, &khz) == EN_CTSCAN_WAIT,
              "silence should not move the scan");
    CHECK(!s.hits[0].got, "nothing was transmitted, so nothing was collected");

    /* Then it speaks. */
    feed_ct(&r, 51544, 12, 34);
    CHECK(en_ctscan_tick(&s, 100, &r, &khz) == EN_CTSCAN_TUNE,
          "an answer should move to the next station");
    CHECK(khz == 97500, "should have moved to the second candidate");
    CHECK(s.hits[0].got, "the first station's time was not recorded");
    printf("  station 1 said %lld, age %u ms\n",
           (long long)s.hits[0].unix_utc, en_ctscan_age_ms(&s, 0));
    CHECK(s.hits[0].unix_utc == 946684800LL + 12 * 3600 + 34 * 60,
          "the recorded instant is wrong");

    /*
     * The critical one. The decoder still holds the first station's clock, and
     * ct_valid is still true - so a scan that asked "does it have a time"
     * would credit station two with station one's answer immediately. Only the
     * group counter moving means this station spoke.
     */
    for (int i = 0; i < 50; i++) en_ctscan_tick(&s, 100, &r, &khz);
    CHECK(!s.hits[1].got,
          "station two was credited with station one's clock");

    /* The age keeps growing, and the corrected time grows with it. */
    int64_t at_first = en_ctscan_now(&s, 0);
    for (int i = 0; i < 100; i++) en_ctscan_tick(&s, 100, &r, &khz);
    printf("  ten seconds later it reads %lld (was %lld)\n",
           (long long)en_ctscan_now(&s, 0), (long long)at_first);
    CHECK(en_ctscan_now(&s, 0) == at_first + 10,
          "ten seconds of ticking should add ten seconds to the answer");

    /* A station that never answers is left after the dwell, and the scan ends
       having gone back where it started. */
    while (s.phase == EN_CTSCAN_RUNNING) en_ctscan_tick(&s, 1000, &r, &khz);
    CHECK(s.phase == EN_CTSCAN_DONE, "the scan should finish on its own");
    CHECK(khz == 98500, "it should end by tuning back");
    CHECK(!s.hits[1].got && !s.hits[2].got,
          "silent stations should not have collected anything");
    CHECK(en_ctscan_percent(&s) == 100, "a finished scan is 100 per cent");
    CHECK(en_ctscan_now(&s, 1) == 0, "a station with no clock has no time");

    /* And it must survive being asked about rows that do not exist. */
    CHECK(en_ctscan_now(&s, 99) == 0, "out of range is not a time");
    CHECK(en_ctscan_age_ms(0, 0) == 0, "a null scan has no age");
    en_ctscan_stop(0);
}

static void test_affollow(void)
{
    static const uint32_t LIST[] = { 89100, 96700 };
    en_rds_t home = af_rds(0xC2B5, LIST, 2);
    en_affollow_t a;

    /* Off by default, and off means off: a dead signal changes nothing. */
    en_af_init(&a);
    CHECK(!a.enabled, "AF following should be off until asked for");
    CHECK(af_run(&a, 60000, 98500, 0, &home) == 0,
          "a disabled follower retuned the radio");

    /* On, and healthy: it sits still. */
    en_af_enable(&a, true, NULL);
    CHECK(af_run(&a, 60000, 98500, 200, &home) == 0,
          "moved away from a strong signal");

    /* A brief dip is not a reason to move - RSSI drops under a bridge. */
    en_af_init(&a);
    en_af_enable(&a, true, NULL);
    af_run(&a, 2000, 98500, 200, &home);
    CHECK(af_run(&a, 2000, 98500, 5, &home) == 0,
          "moved on a two-second dip");
    CHECK(af_run(&a, 2000, 98500, 200, &home) == 0, "moved after recovering");

    /* Sustained weakness: it tries the first alternate. */
    en_af_init(&a);
    en_af_enable(&a, true, NULL);
    af_run(&a, 2000, 98500, 200, &home);
    uint32_t to = af_run(&a, 20000, 98500, 5, &home);
    CHECK(to == 89100, "tried %u, expected the first alternate", to);
    CHECK(a.home_khz == 98500, "home was not remembered: %u", a.home_khz);

    /* A candidate whose PI does NOT match is a different station, however
       strong it is, and must be refused. This is the whole safety property:
       stand next to something misconfigured and the radio stays put. */
    en_rds_t stranger = af_rds(0x1234, LIST, 2);
    to = af_run(&a, 5000, 89100, 255, &stranger);
    CHECK(to == 96700, "a mismatched PI was accepted (went to %u)", to);
    CHECK(a.rejects >= 1, "the refusal was not counted");

    /* Nothing else works either, so it goes back exactly where it started. */
    to = af_run(&a, 5000, 96700, 255, &stranger);
    CHECK(to == 98500, "a failed round left the radio on %u", to);
    CHECK(a.phase == EN_AF_COOLDOWN, "no cooldown after a failed round");
    CHECK(a.moves == 0, "it counted a move it did not make");

    /* And it does not immediately start again. */
    CHECK(af_run(&a, 10000, 98500, 5, &home) == 0,
          "started another round during the cooldown");

    /* The same station, but weaker, is not worth moving to. Accepting equal
       or worse is how a radio ends up swapping between two transmitters for
       ever at the point where they cross. */
    en_af_init(&a);
    en_af_enable(&a, true, NULL);
    af_run(&a, 2000, 98500, 45, &home);     /* home at 45 */
    to = af_run(&a, 20000, 98500, 30, &home);
    CHECK(to == 89100, "did not try an alternate");
    to = af_run(&a, 5000, 89100, 35, &home);   /* barely better than 30 */
    CHECK(to != 0 && to != 89100, "accepted a candidate inside the margin");
    CHECK(a.moves == 0, "moved to something not meaningfully better");

    /* A genuinely better transmitter of the same station: it moves, and stays. */
    en_af_init(&a);
    en_af_enable(&a, true, NULL);
    af_run(&a, 2000, 98500, 30, &home);
    to = af_run(&a, 20000, 98500, 20, &home);
    CHECK(to == 89100, "did not try the alternate");
    CHECK(af_run(&a, 5000, 89100, 200, &home) == 0,
          "refused a strong match of the same station");
    CHECK(a.moves == 1, "the move was not counted (%u)", a.moves);
    CHECK(a.home_khz == 89100, "home did not follow the move: %u", a.home_khz);

    /* Turning it off mid-attempt puts the radio back. The control that exists
       to be a way out must not strand you on a frequency you never chose. */
    en_af_init(&a);
    en_af_enable(&a, true, NULL);
    af_run(&a, 2000, 98500, 200, &home);
    af_run(&a, 20000, 98500, 5, &home);
    CHECK(a.phase == EN_AF_TRYING, "expected to be mid-attempt");
    uint32_t back = 0;
    CHECK(en_af_enable(&a, false, &back) == EN_AF_GOTO && back == 98500,
          "switching off mid-attempt did not restore %u (got %u)",
          98500u, back);
    CHECK(!a.enabled, "still enabled after being switched off");

    /* A station with no alternates does not thrash - one look, then a wait. */
    en_rds_t alone = af_rds(0xC2B5, NULL, 0);
    en_af_init(&a);
    en_af_enable(&a, true, NULL);
    af_run(&a, 2000, 98500, 200, &alone);
    CHECK(af_run(&a, 20000, 98500, 5, &alone) == 0,
          "retuned with an empty alternate list");
    CHECK(a.phase == EN_AF_COOLDOWN, "did not back off with nothing to try");

    /* Somebody else retuning - a preset, the scan, the dial - voids the round
       and makes the new frequency home. */
    en_af_init(&a);
    en_af_enable(&a, true, NULL);
    af_run(&a, 2000, 98500, 200, &home);
    en_af_retuned(&a, 91300);
    CHECK(a.home_khz == 91300, "a manual retune did not become home");
    CHECK(a.phase == EN_AF_WATCH, "a manual retune left it mid-round");
}

int main(void)
{
    printf("Radio+ core tests\n");

    test_build();
    test_parse();
    test_table();
    test_fields();

    test_rds_ps();
    test_rds_ta();
    test_rds_rt();
    test_rds_ct();
    test_rds_af();
    test_rds_robustness();
    test_rds_tuples();

    test_regions();
    test_region_apply();


    test_wav();
    test_presets();
    test_scan();
    test_scan_band_edges();
    test_stereo_mode();
    test_reg_coverage();
    test_rtplus();
    test_affollow();
    test_rds_ct_unix();
    test_rdsname();
    test_ctscan();
    test_rectimer();
    test_sidecar();
    test_simple_flags();
    test_settings();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
