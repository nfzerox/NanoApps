/*
 * program.c — see program.h.
 */

#include "program.h"
#include "measured.h"
#include "wavout.h"

/* ---- bands -------------------------------------------------------------- */

en_band_t en_band_of(double beat_hz)
{
    if (beat_hz < 4.0)  return EN_BAND_DELTA;
    if (beat_hz < 8.0)  return EN_BAND_THETA;
    if (beat_hz < 13.0) return EN_BAND_ALPHA;
    if (beat_hz < 30.0) return EN_BAND_BETA;
    return EN_BAND_GAMMA;
}

const char *en_band_name(en_band_t band)
{
    switch (band) {
    case EN_BAND_DELTA: return "Delta";
    case EN_BAND_THETA: return "Theta";
    case EN_BAND_ALPHA: return "Alpha";
    case EN_BAND_BETA:  return "Beta";
    case EN_BAND_GAMMA: return "Gamma";
    default:            return "?";
    }
}

uint32_t en_band_color(en_band_t band)
{
    switch (band) {
    case EN_BAND_DELTA: return 0x4C5BD4u;   /* indigo  */
    case EN_BAND_THETA: return 0x8B5CF6u;   /* violet  */
    case EN_BAND_ALPHA: return 0x14B8A6u;   /* teal    */
    case EN_BAND_BETA:  return 0xF59E0Bu;   /* amber   */
    case EN_BAND_GAMMA: return 0xEC4899u;   /* magenta */
    default:            return 0x8A8F98u;
    }
}

/* ---- loop planning ------------------------------------------------------ */

const uint32_t EN_RATES[] = { 11025u, 22050u, 44100u };
const int      EN_RATES_COUNT = (int)(sizeof EN_RATES / sizeof EN_RATES[0]);

static double dabs(double x) { return x < 0.0 ? -x : x; }

static uint32_t round_u32(double x)
{
    if (x <= 0.0) return 0;
    return (uint32_t)(x + 0.5);
}

/* Best loop for one rate. See the header for the rule this implements. */
static int plan_for_rate(double beat_hz, double carrier_hz, uint32_t sr,
                         uint32_t max_bytes, en_loop_plan_t *out)
{
    if (beat_hz <= 0.0 || sr == 0 || max_bytes <= EN_WAV_HEADER_BYTES)
        return 0;

    const uint32_t max_frames = (max_bytes - EN_WAV_HEADER_BYTES) / 4u;
    if (max_frames < 2) return 0;

    int      found = 0;
    double   best_err = 0.0;
    uint32_t best_n = 0, best_dn = 0;

    /* cycles_beat = dn. For each dn the ideal loop length is dn/beat seconds;
       round it to a whole number of samples and see how far the realised beat
       lands from the request. dn grows the loop, so stop as soon as the ideal
       length overflows the budget. */
    for (uint32_t dn = 1; dn < 1000000u; dn++) {
        double n_ideal = (double)dn * (double)sr / beat_hz;
        if (n_ideal > (double)max_frames + 0.5) break;

        uint32_t n = round_u32(n_ideal);
        if (n < 2 || n > max_frames) continue;

        double realised = (double)dn * (double)sr / (double)n;
        double err = dabs(realised - beat_hz);

        /* Prefer accuracy; among equally accurate options take the longest
           loop, because a longer loop means fewer seams to hide. Treat
           differences under a micro-hertz as equal — they are far below what
           any listener or any test can resolve. */
        if (!found || err < best_err - 1e-9 ||
            (err < best_err + 1e-9 && n > best_n)) {
            found = 1;
            best_err = err;
            best_n = n;
            best_dn = dn;
        }
    }

    if (!found) return 0;

    double t = (double)best_n / (double)sr;
    uint32_t cyc_l = round_u32(carrier_hz * t);
    if (cyc_l < 1) cyc_l = 1;

    out->sample_rate = sr;
    out->frames      = best_n;
    out->cycles_l    = cyc_l;
    out->cycles_beat = best_dn;
    out->t_seconds   = t;
    out->f_l         = (double)cyc_l / t;
    out->f_r         = (double)(cyc_l + best_dn) / t;
    out->beat_hz     = (double)best_dn / t;
    out->beat_error  = out->beat_hz - beat_hz;
    out->bytes       = en_wav_size(best_n, 2);
    return 1;
}

int en_plan_loop(double beat_hz, double carrier_hz,
                 const uint32_t *rates, int n_rates,
                 uint32_t max_bytes, en_loop_plan_t *out)
{
    en_loop_plan_t best;
    int have = 0;

    for (int i = 0; i < n_rates; i++) {
        en_loop_plan_t p;
        if (!plan_for_rate(beat_hz, carrier_hz, rates[i], max_bytes, &p))
            continue;
        double e = dabs(p.beat_error);
        double be = have ? dabs(best.beat_error) : 0.0;
        if (!have || e < be - 1e-9 ||
            (e < be + 1e-9 && p.t_seconds > best.t_seconds)) {
            best = p;
            have = 1;
        }
    }

    if (!have) return 0;
    *out = best;
    return 1;
}

void en_plan_to_segment(const en_loop_plan_t *plan, en_mode_t mode,
                        en_noise_kind_t noise, double tone_level,
                        double noise_level, en_segment_t *out)
{
    out->sample_rate = plan->sample_rate;
    out->mode        = mode;
    out->noise       = noise;
    out->frames      = plan->frames;
    out->fade_in_s   = 0.0;   /* a loop never fades; the player does that */
    out->fade_out_s  = 0.0;

    out->start.carrier_hz  = plan->f_l;
    out->start.beat_hz     = plan->beat_hz;
    out->start.tone_level  = tone_level;
    out->start.noise_level = noise_level;
    out->end = out->start;    /* steady by definition */
}

/* ---- presets ------------------------------------------------------------ */

static const en_preset_t s_presets[] = {
    { "Delta 2 Hz",   "Binaural • 100 Hz carrier",
      2.0,  100.0, EN_MODE_BINAURAL, EN_NOISE_NONE, 0.0 },
    { "Theta 6 Hz",   "Binaural • 150 Hz carrier",
      6.0,  150.0, EN_MODE_BINAURAL, EN_NOISE_NONE, 0.0 },
    { "Schumann",     "Binaural • 200 Hz carrier",
      7.83, 200.0, EN_MODE_BINAURAL, EN_NOISE_NONE, 0.0 },
    { "Alpha 10 Hz",  "Binaural • 200 Hz carrier",
      10.0, 200.0, EN_MODE_BINAURAL, EN_NOISE_NONE, 0.0 },
    { "Beta 16 Hz",   "Binaural • 250 Hz carrier",
      16.0, 250.0, EN_MODE_BINAURAL, EN_NOISE_NONE, 0.0 },
    { "Gamma 40 Hz",  "Binaural • 300 Hz carrier",
      40.0, 300.0, EN_MODE_BINAURAL, EN_NOISE_NONE, 0.0 },
    { "Alpha + Pink", "Binaural • 200 Hz • pink bed",
      10.0, 200.0, EN_MODE_BINAURAL, EN_NOISE_PINK, 0.18 },
    { "Theta Speaker","Isochronic • 150 Hz carrier",
      6.0,  150.0, EN_MODE_ISOCHRONIC, EN_NOISE_NONE, 0.0 },
    { "Delta Brown",  "Binaural • 100 Hz • brown bed",
      2.0,  100.0, EN_MODE_BINAURAL, EN_NOISE_BROWN, 0.22 },
};

const en_preset_t *en_presets(int *count)
{
    if (count) *count = (int)(sizeof s_presets / sizeof s_presets[0]);
    return s_presets;
}

/* ---- programs ----------------------------------------------------------- */

/* A plain one-carrier segment, which is what every hand-written program is.
   Designated, so that adding a field to en_prog_seg_t does not warn on every
   row here — and so a row that omits one gets the documented default rather
   than whatever the next field along happens to be. */
#define SEG(beat0, beat1, carrier, secs, noise_kind, nlevel)                  \
    { .beat_start = (beat0), .beat_end = (beat1), .carrier_hz = (carrier),    \
      .seconds = (secs), .noise = (noise_kind), .noise_level = (nlevel) }

static const en_prog_seg_t s_wind_down[] = {
    SEG(10.0, 6.0, 200.0, 1500, EN_NOISE_NONE, 0.0),  /* 25 min alpha -> theta */
    SEG( 6.0, 2.0, 150.0, 1200, EN_NOISE_NONE, 0.0),  /* 20 min theta -> delta */
};

static const en_prog_seg_t s_power_nap[] = {
    SEG( 8.0, 4.0, 180.0,  240, EN_NOISE_NONE, 0.0),  /*  4 min descent  */
    SEG( 4.0, 3.0, 150.0,  840, EN_NOISE_NONE, 0.0),  /* 14 min low hold */
    SEG( 3.0,12.0, 220.0,  120, EN_NOISE_NONE, 0.0),  /*  2 min wake ramp */
};

static const en_prog_seg_t s_deep_focus[] = {
    SEG(14.0,18.0, 250.0, 1800, EN_NOISE_PINK, 0.16), /* 30 min rise  */
    SEG(18.0,14.0, 250.0, 1800, EN_NOISE_PINK, 0.16), /* 30 min back  */
};

static const en_prog_seg_t s_creative_drift[] = {
    SEG( 7.5, 7.5, 180.0, 1800, EN_NOISE_NONE, 0.0),  /* 30 min steady */
};

static const en_prog_seg_t s_morning_lift[] = {
    SEG( 6.0,10.0, 200.0,  450, EN_NOISE_NONE, 0.0),  /* 7.5 min theta -> alpha */
    SEG(10.0,18.0, 260.0,  450, EN_NOISE_NONE, 0.0),  /* 7.5 min alpha -> beta  */
};

static const en_prog_seg_t s_meditation[] = {
    SEG(11.0, 8.0, 190.0,  900, EN_NOISE_NONE, 0.0),  /* 15 min slow alpha */
    SEG( 8.0, 5.5, 160.0,  900, EN_NOISE_NONE, 0.0),  /* 15 min into theta */
};

/* ---- imported suites -----------------------------------------------------
 *
 * Two multi-layer suites ported from a research archive of original synthesised
 * practice audio. What is ported is the SCHEDULE — beat breakpoints, layer
 * carriers, per-phase gains — read out of the generators' own metadata. No
 * recording was decoded and no third-party audio is involved. The stage names
 * are the archive's own deliberately neutral ones, and as everywhere else in
 * this app no effect of any kind is claimed; see README.md.
 *
 * Three conventions carry through from the source:
 *
 *  - Levels are normalised so the main carrier is 1.0. The archive works in
 *    absolute peak amplitude — 0.070 for the main carrier, 0.045 and 0.018 for
 *    the others, about -23 dBFS — which is right for a mastered hundred-minute
 *    file and twenty decibels below everything else in this library.
 *    Normalising keeps the balance between layers exactly while letting the
 *    segment's master level put the mix where the rest of the app sits.
 *
 *  - Glides are smoothstep, because these are chains of holds and glides and a
 *    linear glide arriving at a hold has a corner in it.
 *
 *  - Every layer of a stage shares one beat curve, which is how the source
 *    builds them: the quiet support layers carry the same beat as the main one
 *    at a different carrier. The hundred-minute piece is the exception and its
 *    second layer holds its own steady beat.
 *
 * The beds are specified as an RMS of 0.005 against that 0.070 primary, so 7.1%
 * of the tone. en_noise_next is roughly unit PEAK and pink has a crest factor
 * near 3.5, which is why these say 0.20 and not 0.071.
 */

#define LV_A(g) ((g) * (0.045 / 0.070))   /* low warm carrier, 104 Hz */
#define LV_B(g) ((g) * 1.0)               /* main training carrier, 300 Hz */
#define LV_C(g) ((g) * (0.018 / 0.070))   /* quiet upper support, 496 Hz */

/* One three-carrier stage segment: duration, the beat all three share, then a
   start and end gain for each layer in the source's own A/B/C order. */
#define STAGE_SEG(secs, beat0, beat1, ga0, ga1, gb0, gb1, gc0, gc1)           \
    { .beat_start = (beat0), .beat_end = (beat1), .carrier_hz = 300.0,        \
      .seconds = (secs), .noise = EN_NOISE_PINK, .noise_level = 0.20,         \
      .interp = EN_INTERP_SMOOTH, .layers = 3, .layer = {                     \
          { 300.0, (beat0), (beat1), LV_B(gb0), LV_B(gb1) },                  \
          { 104.0, (beat0), (beat1), LV_A(ga0), LV_A(ga1) },                  \
          { 496.0, (beat0), (beat1), LV_C(gc0), LV_C(gc1) } } }

static const en_prog_seg_t s_stage_one[] = {
    STAGE_SEG(270, 10.0, 10.0, 0.00, 0.00, 0.60, 0.60, 0.00, 0.00),
    STAGE_SEG(120, 10.0, 10.0, 0.00, 0.00, 0.60, 0.50, 0.00, 0.00),
    STAGE_SEG( 90, 10.0, 10.0, 0.00, 0.00, 0.50, 0.50, 0.00, 0.00),
    STAGE_SEG(270, 10.0,  8.5, 0.00, 0.30, 0.50, 1.00, 0.00, 0.00),
    STAGE_SEG(480,  8.5,  7.0, 0.30, 0.55, 1.00, 0.90, 0.00, 0.00),
    STAGE_SEG( 72,  7.0,  7.0, 0.55, 0.55, 0.90, 0.90, 0.00, 0.00),
    STAGE_SEG(408,  7.0,  7.0, 0.55, 0.40, 0.90, 1.00, 0.00, 0.15),
    STAGE_SEG( 67,  7.0,  7.0, 0.40, 0.40, 1.00, 1.00, 0.15, 0.15),
    STAGE_SEG(383,  7.0, 12.0, 0.40, 0.00, 1.00, 0.85, 0.15, 0.05),
};

static const en_prog_seg_t s_stage_two[] = {
    STAGE_SEG(240, 10.0, 10.0, 0.00, 0.00, 0.70, 0.70, 0.00, 0.00),
    STAGE_SEG( 60, 10.0, 10.0, 0.00, 0.00, 0.70, 0.70, 0.00, 0.00),
    STAGE_SEG(240, 10.0,  8.0, 0.00, 0.30, 0.70, 1.00, 0.00, 0.00),
    STAGE_SEG(360,  8.0,  7.0, 0.30, 0.55, 1.00, 0.90, 0.00, 0.00),
    STAGE_SEG(420,  7.0,  4.0, 0.55, 0.50, 0.90, 1.00, 0.00, 0.00),
    STAGE_SEG( 60,  4.0,  4.0, 0.50, 0.50, 1.00, 1.00, 0.00, 0.00),
    STAGE_SEG(540,  4.0,  4.0, 0.50, 0.45, 1.00, 1.00, 0.00, 0.12),
    STAGE_SEG( 36,  4.0,  4.0, 0.45, 0.45, 1.00, 1.00, 0.12, 0.12),
    STAGE_SEG(324,  4.0, 12.0, 0.45, 0.00, 1.00, 0.85, 0.12, 0.05),
};

/* Stage three repeats a rise and a settle three times: the transition itself is
   what it exists to practise, so the repeats are the content and not padding. */
static const en_prog_seg_t s_stage_three[] = {
    STAGE_SEG(180, 10.0, 10.0, 0.00, 0.00, 0.70, 0.70, 0.00, 0.00),
    STAGE_SEG( 48, 10.0, 10.0, 0.00, 0.00, 0.70, 0.70, 0.00, 0.00),
    STAGE_SEG(192, 10.0,  8.0, 0.00, 0.30, 0.70, 1.00, 0.00, 0.00),
    STAGE_SEG(300,  8.0,  7.0, 0.30, 0.55, 1.00, 0.90, 0.00, 0.00),
    STAGE_SEG(300,  7.0,  4.0, 0.55, 0.50, 0.90, 1.00, 0.00, 0.00),
    STAGE_SEG( 42,  4.0,  4.0, 0.50, 0.50, 1.00, 1.00, 0.00, 0.00),
    STAGE_SEG(378,  4.0,  4.0, 0.50, 0.55, 1.00, 1.00, 0.00, 0.22),
    STAGE_SEG( 72,  4.0,  8.5, 0.55, 0.35, 1.00, 1.00, 0.22, 0.10),
    STAGE_SEG( 78,  8.5,  4.0, 0.35, 0.55, 1.00, 1.00, 0.10, 0.18),
    STAGE_SEG( 72,  4.0,  8.5, 0.55, 0.35, 1.00, 1.00, 0.18, 0.10),
    STAGE_SEG( 78,  8.5,  4.0, 0.35, 0.55, 1.00, 1.00, 0.10, 0.18),
    STAGE_SEG( 72,  4.0,  8.5, 0.55, 0.35, 1.00, 1.00, 0.18, 0.10),
    STAGE_SEG( 32,  8.5,  8.5, 0.35, 0.35, 1.00, 1.00, 0.10, 0.10),
    STAGE_SEG( 76,  8.5,  4.0, 0.35, 0.55, 1.00, 1.00, 0.10, 0.18),
    STAGE_SEG( 36,  4.0,  4.0, 0.55, 0.55, 1.00, 1.00, 0.18, 0.18),
    STAGE_SEG(324,  4.0, 12.0, 0.55, 0.00, 1.00, 0.85, 0.18, 0.05),
};

/*
 * The hundred-minute piece. Two layers: a 200 Hz primary carrying the whole
 * beat schedule, and a 260 Hz secondary holding a steady 7 Hz through the
 * middle third. The secondary is what makes the layered block a layered block
 * — two beats at once rather than one — so it is a second layer and not a
 * second program.
 */
#define XP_SEC_MAIN (0.0145 / 0.070)   /* 13.7 dB below the primary */
#define XP_SEC_EMPH (0.031  / 0.070)   /* emphasised during the return prep */
#define XP_PRI_DIP  0.62               /* primary reduced while it is */

#define XP_SEG(secs, pb0, pb1, pl0, pl1, sl0, sl1)                            \
    { .beat_start = (pb0), .beat_end = (pb1), .carrier_hz = 200.0,            \
      .seconds = (secs), .noise = EN_NOISE_PINK, .noise_level = 0.20,         \
      .interp = EN_INTERP_SMOOTH, .layers = 2, .layer = {                     \
          { 200.0, (pb0), (pb1), (pl0), (pl1) },                              \
          { 260.0,   7.0,   7.0, (sl0), (sl1) } } }

static const en_prog_seg_t s_extended[] = {
    XP_SEG( 300, 10.0, 10.0,       1.0,        1.0, 0.0,         0.0),
    XP_SEG( 600, 10.0,  7.0,       1.0,        1.0, 0.0,         0.0),
    XP_SEG( 900,  7.0,  4.0,       1.0,        1.0, 0.0,         0.0),
    XP_SEG( 420,  4.0,  4.0,       1.0,        1.0, 0.0,         0.0),
    XP_SEG( 180,  4.0,  7.0,       1.0,        1.0, 0.0,         0.0),
    XP_SEG( 300,  7.0,  4.0,       1.0,        1.0, 0.0, XP_SEC_MAIN),
    XP_SEG(1980,  4.0,  4.0,       1.0,        1.0, XP_SEC_MAIN, XP_SEC_MAIN),
    XP_SEG( 420,  4.0,  7.0,       1.0, XP_PRI_DIP, XP_SEC_MAIN, XP_SEC_EMPH),
    /* 85:00 to 87:30. The source curve has amplitude breakpoints here that the
       beat schedule does not, so this splits a glide the source runs whole:
       7 Hz to 10 Hz over 85:00-95:00. 7.469 is that curve's real value at the
       split, so the two halves land where the one span would have. Each half
       is smoothstepped in its own right, which flattens the join by a hair —
       under a tenth of a hertz across two and a half minutes. */
    XP_SEG( 150,  7.0, 7.469, XP_PRI_DIP,      1.0, XP_SEC_EMPH, 0.0),
    XP_SEG( 450, 7.469, 10.0,      1.0,        1.0, 0.0,         0.0),
    XP_SEG( 240, 10.0, 12.0,       1.0,        1.0, 0.0,         0.0),
    XP_SEG(  60, 12.0, 12.0,       1.0,        1.0, 0.0,         0.0),
};

#define ROW(nm, det, md, arr, grp)                                            \
    { .name = (nm), .detail = (det), .mode = (md), .segs = (arr),             \
      .n_segs = (int)(sizeof arr / sizeof arr[0]), .group = (grp) }

#define PROGRAM(nm, det, md, arr) ROW(nm, det, md, arr, EN_GROUP_PROGRAM)
#define SUITE(nm, det, md, arr)   ROW(nm, det, md, arr, EN_GROUP_SUITE)

/* The same shelf as the suites - to a listener they are the same kind of thing
   - but these tables live in measured.c and are declared extern, so the macro
   is spelled out rather than reusing ROW. */
#define MEASURED(nm, det, arr)                                                \
    { .name = (nm), .detail = (det), .mode = EN_MODE_BINAURAL,                \
      .segs = (arr), .n_segs = (int)(sizeof arr / sizeof arr[0]),             \
      .group = EN_GROUP_SUITE }

static const en_program_t s_programs[] = {
    PROGRAM("Wind Down",
            "10-6-2 Hz • 200 Hz carrier",
            EN_MODE_BINAURAL, s_wind_down),
    PROGRAM("Power Nap",
            "8-3 Hz, then a wake ramp",
            EN_MODE_BINAURAL, s_power_nap),
    PROGRAM("Deep Focus",
            "14-18-14 Hz • pink bed",
            EN_MODE_BINAURAL, s_deep_focus),
    PROGRAM("Creative Drift",
            "Steady 7.5 Hz • 180 Hz",
            EN_MODE_BINAURAL, s_creative_drift),
    PROGRAM("Morning Lift",
            "6-18 Hz • carrier 200-260 Hz",
            EN_MODE_BINAURAL, s_morning_lift),
    PROGRAM("Meditation Descent",
            "11-5.5 Hz • long fades",
            EN_MODE_BINAURAL, s_meditation),

    SUITE("Extended Practice",
          "200 + 260 Hz • two layers",
          EN_MODE_BINAURAL, s_extended),
    SUITE("Stage 1",
          "Orientation • 10-7-12 Hz",
          EN_MODE_BINAURAL, s_stage_one),
    SUITE("Stage 2",
          "Body still • 10-4-12 Hz",
          EN_MODE_BINAURAL, s_stage_two),
    SUITE("Stage 3",
          "Field practice • rise/settle x3",
          EN_MODE_BINAURAL, s_stage_three),

    /* Measured from recordings rather than read out of a generator's metadata.
       See core/measured.c for what was taken and what was not. Their layer
       counts are what the analysis found - three to five concurrent carrier
       families - not a number chosen in advance. */
    MEASURED("Practice 1", "Orientation, 60/102/114/300 Hz", en_meas_practice_1),
    MEASURED("Practice 2", "10 to 4 Hz, 102 + 300 Hz", en_meas_practice_2),
    MEASURED("Practice 3", "4 Hz held, 102/162/300 Hz", en_meas_practice_3),
    MEASURED("Practice 4", "4 Hz, then a return, 102 Hz", en_meas_practice_4),
    MEASURED("Practice 5", "4 to 1.4 Hz, 60/102/114/300 Hz", en_meas_practice_5),
    MEASURED("Practice 6", "Steady 3.8 Hz, 102 + 300 Hz", en_meas_practice_6),
};

int en_programs_in_group(en_group_t group, int *out, int cap)
{
    const int total = (int)(sizeof s_programs / sizeof s_programs[0]);
    int found = 0;
    for (int i = 0; i < total; i++) {
        if (s_programs[i].group != group) continue;
        if (out && found < cap) out[found] = i;
        found++;
    }
    return found;
}

const en_program_t *en_programs(int *count)
{
    if (count) *count = (int)(sizeof s_programs / sizeof s_programs[0]);
    return s_programs;
}

uint32_t en_program_seconds(const en_program_t *p)
{
    uint32_t t = 0;
    for (int i = 0; i < p->n_segs; i++) t += p->segs[i].seconds;
    return t;
}

en_band_t en_program_band(const en_program_t *p)
{
    uint32_t longest = 0;
    double beat = 10.0;
    for (int i = 0; i < p->n_segs; i++) {
        if (p->segs[i].seconds > longest) {
            longest = p->segs[i].seconds;
            beat = 0.5 * (p->segs[i].beat_start + p->segs[i].beat_end);
        }
    }
    return en_band_of(beat);
}

int en_program_seg_at(const en_program_t *p, double t_seconds)
{
    double acc = 0.0;
    for (int i = 0; i < p->n_segs; i++) {
        double next = acc + (double)p->segs[i].seconds;
        if (t_seconds < next) return i;
        acc = next;
    }
    return -1;
}

/* Smoothstep: 3u^2 - 2u^3. Zero slope at both ends, so a glide meeting a hold
   has no corner in it. Cheap enough to run per control block. */
static double smoothstep(double u)
{
    if (u <= 0.0) return 0.0;
    if (u >= 1.0) return 1.0;
    return u * u * (3.0 - 2.0 * u);
}

static double seg_ramp(const en_prog_seg_t *s, double u)
{
    return s->interp == EN_INTERP_SMOOTH ? smoothstep(u) : u;
}

uint8_t en_segs_layers_at(const en_prog_seg_t *segs, int n_segs, double t,
                          en_mode_t mode, en_layer_t *out,
                          en_noise_kind_t *noise, double *noise_level)
{
    if (!segs || n_segs <= 0) {
        out[0].mode = mode;
        out[0].carrier_hz = 200.0;
        out[0].beat_hz = 10.0;
        out[0].level = 1.0;
        if (noise) *noise = EN_NOISE_NONE;
        if (noise_level) *noise_level = 0.0;
        return 1;
    }

    if (t < 0.0) t = 0.0;

    const en_prog_seg_t *s = &segs[n_segs - 1];
    double u = 1.0;

    double acc = 0.0;
    for (int i = 0; i < n_segs; i++) {
        double dur = (double)segs[i].seconds;
        if (t < acc + dur || i == n_segs - 1) {
            s = &segs[i];
            u = dur > 0.0 ? (t - acc) / dur : 0.0;
            if (u < 0.0) u = 0.0;
            if (u > 1.0) u = 1.0;
            break;
        }
        acc += dur;
    }

    if (noise) *noise = s->noise;
    if (noise_level) *noise_level = s->noise_level;

    const double w = seg_ramp(s, u);

    if (!s->layers) {
        out[0].mode = mode;
        out[0].carrier_hz = s->carrier_hz;
        out[0].beat_hz = s->beat_start + (s->beat_end - s->beat_start) * w;
        out[0].level = 1.0;
        return 1;
    }

    uint8_t n = s->layers;
    if (n > EN_PROG_MAX_LAYERS) n = EN_PROG_MAX_LAYERS;
    for (uint8_t j = 0; j < n; j++) {
        const en_prog_layer_t *pl = &s->layer[j];
        out[j].mode = mode;
        out[j].carrier_hz = pl->carrier_hz;
        out[j].beat_hz = pl->beat_start + (pl->beat_end - pl->beat_start) * w;
        out[j].level = pl->level_start + (pl->level_end - pl->level_start) * w;
    }
    return n;
}

double en_program_beat_at(const en_program_t *p, double t_seconds)
{
    if (p->n_segs == 0) return 0.0;
    if (t_seconds < 0.0) t_seconds = 0.0;

    double acc = 0.0;
    for (int i = 0; i < p->n_segs; i++) {
        double dur = (double)p->segs[i].seconds;
        if (t_seconds < acc + dur) {
            double u = dur > 0.0 ? (t_seconds - acc) / dur : 0.0;
            if (u < 0.0) u = 0.0;
            if (u > 1.0) u = 1.0;
            /* Through the same ramp the renderer uses, or the readout would
               disagree with what is sounding for most of a smooth glide. */
            return p->segs[i].beat_start
                 + (p->segs[i].beat_end - p->segs[i].beat_start)
                   * seg_ramp(&p->segs[i], u);
        }
        acc += dur;
    }
    return p->segs[p->n_segs - 1].beat_end;
}

/* ---- user program parser ------------------------------------------------ */
/* Freestanding: no stdio, no strtod, no locale. Just a scanner over a buffer. */

static int is_space(char c)   { return c == ' ' || c == '\t' || c == '\r'; }
static int is_digit(char c)   { return c >= '0' && c <= '9'; }

static void skip_space(const char **p, const char *end)
{
    while (*p < end && is_space(**p)) (*p)++;
}

/* Read a bare word into `dst`. Returns its length. */
static int read_word(const char **p, const char *end, char *dst, int cap)
{
    skip_space(p, end);
    int n = 0;
    while (*p < end && !is_space(**p) && **p != '\n') {
        if (n < cap - 1) dst[n] = **p;
        n++;
        (*p)++;
    }
    dst[n < cap ? n : cap - 1] = 0;
    return n;
}

/* Read a decimal number, optionally signed, optionally fractional. */
static int read_num(const char **p, const char *end, double *out)
{
    skip_space(p, end);
    const char *s = *p;
    int neg = 0;
    if (s < end && (*s == '-' || *s == '+')) { neg = (*s == '-'); s++; }

    int any = 0;
    double v = 0.0;
    while (s < end && is_digit(*s)) { v = v * 10.0 + (*s - '0'); s++; any = 1; }
    if (s < end && *s == '.') {
        s++;
        double scale = 0.1;
        while (s < end && is_digit(*s)) {
            v += (*s - '0') * scale;
            scale *= 0.1;
            s++;
            any = 1;
        }
    }
    if (!any) return 0;
    *p = s;
    *out = neg ? -v : v;
    return 1;
}

static int word_eq(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

static void copy_rest_of_line(const char **p, const char *end,
                              char *dst, int cap)
{
    skip_space(p, end);
    int n = 0;
    while (*p < end && **p != '\n') {
        if (n < cap - 1) dst[n] = **p;
        n++;
        (*p)++;
    }
    /* trim trailing space */
    if (n > cap - 1) n = cap - 1;
    while (n > 0 && is_space(dst[n - 1])) n--;
    dst[n] = 0;
}

const char *en_parse_error_text(int code)
{
    switch (code) {
    case EN_PARSE_OK:             return "ok";
    case EN_PARSE_BAD_KEY:        return "unknown keyword";
    case EN_PARSE_BAD_VALUE:      return "bad value";
    case EN_PARSE_TOO_MANY_SEGS:  return "too many seg lines";
    case EN_PARSE_NO_SEGS:        return "no seg lines";
    default:                      return "error";
    }
}

int en_parse_program(const char *text, uint32_t len,
                     en_user_program_t *out, int *err_line)
{
    const char *p = text;
    const char *end = text + len;
    int line = 0;

    out->name[0] = 0;
    out->mode = EN_MODE_BINAURAL;
    out->carrier_hz = 200.0;
    out->noise = EN_NOISE_NONE;
    out->noise_level = 0.0;
    out->n_segs = 0;

    while (p < end) {
        line++;
        const char *line_end = p;
        while (line_end < end && *line_end != '\n') line_end++;

        const char *q = p;
        char key[16];
        skip_space(&q, line_end);

        if (q < line_end && *q != '#') {
            read_word(&q, line_end, key, sizeof key);

            if (word_eq(key, "name")) {
                copy_rest_of_line(&q, line_end, out->name, EN_NAME_MAX);

            } else if (word_eq(key, "mode")) {
                char v[16];
                read_word(&q, line_end, v, sizeof v);
                if      (word_eq(v, "binaural"))   out->mode = EN_MODE_BINAURAL;
                else if (word_eq(v, "isochronic")) out->mode = EN_MODE_ISOCHRONIC;
                else if (word_eq(v, "monaural"))   out->mode = EN_MODE_MONAURAL;
                else { if (err_line) *err_line = line; return EN_PARSE_BAD_VALUE; }

            } else if (word_eq(key, "carrier")) {
                double v;
                if (!read_num(&q, line_end, &v) || v < 20.0 || v > 1000.0) {
                    if (err_line) *err_line = line;
                    return EN_PARSE_BAD_VALUE;
                }
                out->carrier_hz = v;

            } else if (word_eq(key, "noise")) {
                char v[16];
                read_word(&q, line_end, v, sizeof v);
                if      (word_eq(v, "none"))  out->noise = EN_NOISE_NONE;
                else if (word_eq(v, "white")) out->noise = EN_NOISE_WHITE;
                else if (word_eq(v, "pink"))  out->noise = EN_NOISE_PINK;
                else if (word_eq(v, "brown")) out->noise = EN_NOISE_BROWN;
                else { if (err_line) *err_line = line; return EN_PARSE_BAD_VALUE; }
                double lv;
                out->noise_level = read_num(&q, line_end, &lv) ? lv : 0.15;
                if (out->noise_level < 0.0) out->noise_level = 0.0;
                if (out->noise_level > 1.0) out->noise_level = 1.0;

            } else if (word_eq(key, "seg")) {
                if (out->n_segs >= EN_USER_MAX_SEGS) {
                    if (err_line) *err_line = line;
                    return EN_PARSE_TOO_MANY_SEGS;
                }
                double b0, b1, secs;
                if (!read_num(&q, line_end, &b0) ||
                    !read_num(&q, line_end, &b1) ||
                    !read_num(&q, line_end, &secs) ||
                    b0 <= 0.0 || b1 <= 0.0 || b0 > 100.0 || b1 > 100.0 ||
                    secs < 1.0) {
                    if (err_line) *err_line = line;
                    return EN_PARSE_BAD_VALUE;
                }
                en_prog_seg_t *s = &out->segs[out->n_segs++];
                s->beat_start  = b0;
                s->beat_end    = b1;
                s->carrier_hz  = out->carrier_hz;
                s->seconds     = (uint32_t)secs;
                s->noise       = out->noise;
                s->noise_level = out->noise_level;

            } else {
                if (err_line) *err_line = line;
                return EN_PARSE_BAD_KEY;
            }
        }

        p = line_end < end ? line_end + 1 : end;
    }

    if (out->n_segs == 0) {
        if (err_line) *err_line = line;
        return EN_PARSE_NO_SEGS;
    }
    if (out->name[0] == 0) {
        const char *d = "Untitled";
        int i = 0;
        while (d[i] && i < EN_NAME_MAX - 1) { out->name[i] = d[i]; i++; }
        out->name[i] = 0;
    }
    return EN_PARSE_OK;
}
