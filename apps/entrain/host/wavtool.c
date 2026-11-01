/*
 * wavtool.c — render any preset, program or ad-hoc setting to a .wav.
 *
 * The point of keeping core/ free of LVGL and SDK dependencies is that you can
 * listen to the DSP on real speakers instead of inferring it from a test
 * assertion. This is that.
 *
 *     make -f Makefile.host wav
 *     ./build/entrain-wav --list
 *     ./build/entrain-wav --preset 2 -o schumann.wav
 *     ./build/entrain-wav --program 0 --minutes 2 -o winddown.wav
 *     ./build/entrain-wav --beat 7.83 --carrier 200 --mode isochronic -o t.wav
 *
 * A --preset render is the exact loop the device would play, byte for byte, so
 * you can also drop it straight onto the iPod as a sanity check.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/render.h"
#include "../core/program.h"
#include "../core/wavout.h"

static int write_wav(const char *path, const int16_t *pcm, uint32_t frames,
                     uint32_t sample_rate)
{
    uint8_t hdr[EN_WAV_HEADER_BYTES];
    uint32_t total = en_wav_header(hdr, sample_rate, 2, frames);

    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return 0; }
    if (fwrite(hdr, 1, sizeof hdr, f) != sizeof hdr ||
        fwrite(pcm, 1, (size_t)frames * 4, f) != (size_t)frames * 4) {
        perror(path);
        fclose(f);
        return 0;
    }
    fclose(f);
    printf("wrote %s — %u frames, %u Hz, %.3f s, %u bytes\n",
           path, frames, sample_rate, (double)frames / sample_rate, total);
    if (total >= EN_MAX_LOAD_BYTES)
        printf("  NOTE: %u bytes is at or over the iPod loader's 1 MiB limit;\n"
               "        this file would be rejected on the device.\n", total);
    return 1;
}

static void list_all(void)
{
    int n;
    const en_preset_t *ps = en_presets(&n);
    printf("presets:\n");
    for (int i = 0; i < n; i++) {
        en_loop_plan_t p;
        if (en_plan_loop(ps[i].beat_hz, ps[i].carrier_hz, EN_RATES,
                         EN_RATES_COUNT, EN_TARGET_BYTES, &p)) {
            printf("  %2d  %-14s %-6s %s\n"
                   "      realised %.6f Hz  %.2f / %.2f Hz  "
                   "%u Hz  %.3f s loop  %u B\n",
                   i, ps[i].name, en_band_name(en_band_of(ps[i].beat_hz)),
                   ps[i].detail, p.beat_hz, p.f_l, p.f_r, p.sample_rate,
                   p.t_seconds, p.bytes);
        } else {
            printf("  %2d  %-14s  (no loop fits the budget)\n", i, ps[i].name);
        }
    }

    const en_program_t *gs = en_programs(&n);
    printf("\nprograms:\n");
    for (int i = 0; i < n; i++) {
        uint32_t s = en_program_seconds(&gs[i]);
        printf("  %2d  %-20s %-6s %2u:%02u  %s\n", i, gs[i].name,
               en_band_name(en_program_band(&gs[i])), s / 60, s % 60,
               gs[i].detail);
    }
}

static en_mode_t parse_mode(const char *s, int *ok)
{
    *ok = 1;
    if (!strcmp(s, "binaural"))   return EN_MODE_BINAURAL;
    if (!strcmp(s, "isochronic")) return EN_MODE_ISOCHRONIC;
    if (!strcmp(s, "monaural"))   return EN_MODE_MONAURAL;
    *ok = 0;
    return EN_MODE_BINAURAL;
}

static en_noise_kind_t parse_noise(const char *s, int *ok)
{
    *ok = 1;
    if (!strcmp(s, "none"))  return EN_NOISE_NONE;
    if (!strcmp(s, "white")) return EN_NOISE_WHITE;
    if (!strcmp(s, "pink"))  return EN_NOISE_PINK;
    if (!strcmp(s, "brown")) return EN_NOISE_BROWN;
    *ok = 0;
    return EN_NOISE_NONE;
}

/* Validate a user-program file the way the device will read it. Worth having
   as a desktop step: the iPod's volume is mounted read-only, so a file with a
   typo in it cannot be fixed in place once it is over there. */
static int check_program(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return 1; }
    static char text[8192];
    size_t n = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[n] = 0;

    en_user_program_t up;
    int line = 0;
    int rc = en_parse_program(text, (uint32_t)n, &up, &line);
    if (rc != EN_PARSE_OK) {
        fprintf(stderr, "%s: %s on line %d\n", path,
                en_parse_error_text(rc), line);
        return 1;
    }

    uint32_t total = 0;
    for (int i = 0; i < up.n_segs; i++) total += up.segs[i].seconds;
    printf("%s\n  name    %s\n  mode    %s\n  carrier %.2f Hz\n"
           "  noise   %s %.2f\n  %d segment(s), %u:%02u total\n",
           path, up.name, en_mode_name(up.mode), up.carrier_hz,
           en_noise_name(up.noise), up.noise_level, up.n_segs,
           total / 60u, total % 60u);
    for (int i = 0; i < up.n_segs; i++)
        printf("    %6.2f -> %6.2f Hz over %4u s   (%s)\n",
               up.segs[i].beat_start, up.segs[i].beat_end, up.segs[i].seconds,
               en_band_name(en_band_of(up.segs[i].beat_start)));
    return 0;
}

static void usage(void)
{
    printf(
      "entrain-wav — render Entrain audio to a .wav\n\n"
      "  --list                 show presets and programs\n"
      "  --check FILE...        validate user-program files and stop\n"
      "  --preset N             render preset N as its device loop\n"
      "  --program N            render program N (see --minutes)\n"
      "  --beat HZ              ad-hoc: beat frequency\n"
      "  --carrier HZ           ad-hoc: carrier (default 200)\n"
      "  --mode M               binaural | isochronic | monaural\n"
      "  --noise K [LEVEL]      none | white | pink | brown\n"
      "  --minutes M            cap a program render (default: the whole thing)\n"
      "  --rate HZ              force a sample rate for ad-hoc renders\n"
      "  -o FILE                output path (default out.wav)\n");
}

int main(int argc, char **argv)
{
    const char *out = "out.wav";
    int preset = -1, program = -1, ok = 1;
    double beat = 0.0, carrier = 200.0, noise_level = 0.15, minutes = 0.0;
    uint32_t rate = 0;
    en_mode_t mode = EN_MODE_BINAURAL;
    en_noise_kind_t noise = EN_NOISE_NONE;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        int has_next = (i + 1 < argc);
        if (!strcmp(a, "--list")) { list_all(); return 0; }
        else if (!strcmp(a, "--check")) {
            int bad = 0;
            while (i + 1 < argc && argv[i + 1][0] != '-')
                bad |= check_program(argv[++i]);
            return bad;
        }
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(); return 0; }
        else if (!strcmp(a, "--preset") && has_next)  preset = atoi(argv[++i]);
        else if (!strcmp(a, "--program") && has_next) program = atoi(argv[++i]);
        else if (!strcmp(a, "--beat") && has_next)    beat = atof(argv[++i]);
        else if (!strcmp(a, "--carrier") && has_next) carrier = atof(argv[++i]);
        else if (!strcmp(a, "--minutes") && has_next) minutes = atof(argv[++i]);
        else if (!strcmp(a, "--rate") && has_next)    rate = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(a, "-o") && has_next)        out = argv[++i];
        else if (!strcmp(a, "--mode") && has_next) {
            mode = parse_mode(argv[++i], &ok);
            if (!ok) { fprintf(stderr, "bad --mode\n"); return 2; }
        } else if (!strcmp(a, "--noise") && has_next) {
            noise = parse_noise(argv[++i], &ok);
            if (!ok) { fprintf(stderr, "bad --noise\n"); return 2; }
            if (i + 1 < argc && argv[i + 1][0] != '-')
                noise_level = atof(argv[++i]);
        } else {
            fprintf(stderr, "unknown argument: %s\n", a);
            usage();
            return 2;
        }
    }

    /* ---- a preset: render exactly the loop the device would play ---- */
    if (preset >= 0) {
        int n;
        const en_preset_t *ps = en_presets(&n);
        if (preset >= n) { fprintf(stderr, "no preset %d\n", preset); return 2; }
        const en_preset_t *p = &ps[preset];

        en_loop_plan_t plan;
        if (!en_plan_loop(p->beat_hz, p->carrier_hz, EN_RATES, EN_RATES_COUNT,
                          EN_TARGET_BYTES, &plan)) {
            fprintf(stderr, "no loop fits for %s\n", p->name);
            return 1;
        }
        printf("%s — requested %.4f Hz, realised %.6f Hz "
               "(%.3f / %.3f Hz carriers), %.3f s loop at %u Hz\n",
               p->name, p->beat_hz, plan.beat_hz, plan.f_l, plan.f_r,
               plan.t_seconds, plan.sample_rate);

        en_segment_t seg;
        en_plan_to_segment(&plan, p->mode, p->noise, 0.8, p->noise_level, &seg);

        int16_t *pcm = malloc((size_t)plan.frames * 4);
        if (!pcm) { fprintf(stderr, "out of memory\n"); return 1; }
        en_render_t r;
        en_render_init(&r, plan.sample_rate, p->noise, 1);
        en_render_loop(&r, &seg, pcm);
        int rc = write_wav(out, pcm, plan.frames, plan.sample_rate);
        free(pcm);
        return rc ? 0 : 1;
    }

    /* ---- a program: render its timeline as one continuous file ---- */
    if (program >= 0) {
        int n;
        const en_program_t *gs = en_programs(&n);
        if (program >= n) { fprintf(stderr, "no program %d\n", program); return 2; }
        const en_program_t *g = &gs[program];

        uint32_t sr = rate ? rate : EN_RATES[0];
        uint32_t total_s = en_program_seconds(g);
        uint32_t cap_s = minutes > 0.0 ? (uint32_t)(minutes * 60.0) : total_s;
        printf("%s — %s\n", g->name, g->detail);

        en_render_t r;
        en_render_init(&r, sr, g->segs[0].noise, 1);

        /* One buffer, rendered segment by segment through one renderer, so the
           joins are continuous exactly as they are on the device. */
        uint32_t want = cap_s < total_s ? cap_s : total_s;
        int16_t *pcm = malloc((size_t)want * (size_t)sr * 4);
        if (!pcm) { fprintf(stderr, "out of memory\n"); return 1; }

        uint32_t done = 0;
        for (int s = 0; s < g->n_segs && done < want * sr; s++) {
            uint32_t frames = g->segs[s].seconds * sr;
            if (done + frames > want * sr) frames = want * sr - done;

            en_segment_t seg;
            memset(&seg, 0, sizeof seg);
            seg.sample_rate = sr;
            seg.mode = g->mode;
            seg.noise = g->segs[s].noise;
            seg.frames = frames;
            seg.start.carrier_hz  = g->segs[s].carrier_hz;
            seg.start.beat_hz     = g->segs[s].beat_start;
            seg.start.tone_level  = 0.8;
            seg.start.noise_level = g->segs[s].noise_level;
            seg.end = seg.start;
            seg.end.beat_hz = g->segs[s].beat_end;
            /* Fade in on the first segment and out on the last, nowhere else —
               an interior fade would be a hole in the middle of the program. */
            seg.fade_in_s  = (s == 0) ? 2.0 : 0.0;
            seg.fade_out_s = (s == g->n_segs - 1) ? 2.0 : 0.0;

            en_render_segment(&r, &seg, pcm + (size_t)done * 2);
            done += frames;
            printf("  seg %d: %.2f -> %.2f Hz over %u s\n", s,
                   g->segs[s].beat_start, g->segs[s].beat_end, frames / sr);
        }

        int rc = write_wav(out, pcm, done, sr);
        free(pcm);
        return rc ? 0 : 1;
    }

    /* ---- ad hoc ---- */
    if (beat > 0.0) {
        en_loop_plan_t plan;
        uint32_t rates[1];
        const uint32_t *rlist = EN_RATES;
        int rcount = EN_RATES_COUNT;
        if (rate) { rates[0] = rate; rlist = rates; rcount = 1; }

        if (!en_plan_loop(beat, carrier, rlist, rcount, EN_TARGET_BYTES, &plan)) {
            fprintf(stderr, "no loop fits for %.4f Hz\n", beat);
            return 1;
        }
        printf("requested %.4f Hz, realised %.6f Hz "
               "(%.3f / %.3f Hz), %.3f s loop at %u Hz\n",
               beat, plan.beat_hz, plan.f_l, plan.f_r,
               plan.t_seconds, plan.sample_rate);

        en_segment_t seg;
        en_plan_to_segment(&plan, mode, noise, 0.8,
                           noise == EN_NOISE_NONE ? 0.0 : noise_level, &seg);

        int16_t *pcm = malloc((size_t)plan.frames * 4);
        if (!pcm) { fprintf(stderr, "out of memory\n"); return 1; }
        en_render_t r;
        en_render_init(&r, plan.sample_rate, noise, 1);
        en_render_loop(&r, &seg, pcm);
        int rc = write_wav(out, pcm, plan.frames, plan.sample_rate);
        free(pcm);
        return rc ? 0 : 1;
    }

    usage();
    return 2;
}
