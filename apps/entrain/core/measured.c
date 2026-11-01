/*
 * measured.c — programs measured from recordings. Generated; do not hand-edit.
 *
 * Produced by tools/analyse-layers.py and tools/port-measured.py. What was
 * taken from the recordings is a measurement: per carrier band, the carrier
 * frequency, the beat between the two channels, and how present that band is,
 * sampled every four seconds. Numbers and timings. Nothing of the recordings
 * themselves is here, and Entrain synthesises its own tones from these.
 *
 * The names are deliberately neutral and are not the originals. That follows
 * the same rule the imported suites follow, and the reason is in README.md
 * under Trademark: this app is not affiliated with, endorsed by or derived
 * from the organisation whose practice material was measured, and using its
 * exercise names would suggest otherwise.
 *
 * Every band that actually sounds is carried as its own layer, including the
 * stretches where its gain is zero - a layer that goes quiet for ten minutes
 * and returns is what the recording does, and flattening that would be
 * discarding a measurement.
 *
 * Regenerate with tools/gen-measured.sh.
 */

#include "measured.h"

/* Practice 1: measured from the recording, 1989s, 5 layers. */
const en_prog_seg_t en_meas_practice_1[] = {
    /*   0:00 */
    { .beat_start = 3.865, .beat_end = 3.865,
      .carrier_hz = 300.69, .seconds = 270, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 5, .layer = {
        {  300.69,  3.865,  3.865, 0.000, 0.000 },   /* high */
        {   60.63,  0.000,  0.000, 0.000, 0.000 },   /* sub */
        {  114.25,  0.001,  0.001, 0.000, 0.083 },   /* ref */
        {  107.19,  0.001,  0.001, 0.000, 0.072 },   /* main */
        {  179.99,  0.004,  0.004, 0.000, 0.000 },   /* mid */
      } },
    /*   4:30 */
    { .beat_start = 3.865, .beat_end = 3.865,
      .carrier_hz = 300.69, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 5, .layer = {
        {  300.69,  3.865,  3.865, 0.000, 0.000 },   /* high */
        {   60.63,  0.000,  0.000, 0.000, 0.000 },   /* sub */
        {  114.25,  0.001,  0.001, 0.083, 0.083 },   /* ref */
        {  107.19,  0.001,  0.001, 0.072, 0.000 },   /* main */
        {  179.99,  0.004,  0.004, 0.000, 0.000 },   /* mid */
      } },
    /*   5:00 */
    { .beat_start = 3.865, .beat_end = 3.864,
      .carrier_hz = 300.69, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 5, .layer = {
        {  300.69,  3.865,  3.864, 0.000, 0.749 },   /* high */
        {   60.63,  0.000,  0.000, 0.000, 0.000 },   /* sub */
        {  114.25,  0.001,  0.001, 0.083, 0.088 },   /* ref */
        {  107.19,  0.001,  0.001, 0.000, 0.000 },   /* main */
        {  179.99,  0.004,  0.004, 0.000, 0.000 },   /* mid */
      } },
    /*   5:30 */
    { .beat_start = 3.864, .beat_end = 3.866,
      .carrier_hz = 300.69, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 5, .layer = {
        {  300.69,  3.864,  3.866, 0.749, 0.736 },   /* high */
        {   60.63,  0.000,  0.000, 0.000, 0.000 },   /* sub */
        {  114.25,  0.001,  0.001, 0.088, 0.000 },   /* ref */
        {  107.19,  0.001,  0.001, 0.000, 0.000 },   /* main */
        {  179.99,  0.004,  0.004, 0.000, 0.000 },   /* mid */
      } },
    /*   6:00 */
    { .beat_start = 3.866, .beat_end = 3.850,
      .carrier_hz = 300.69, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 5, .layer = {
        {  300.69,  3.866,  3.850, 0.736, 0.744 },   /* high */
        {   60.63,  0.000,  0.000, 0.000, 0.000 },   /* sub */
        {  114.25,  0.001,  0.001, 0.000, 0.106 },   /* ref */
        {  107.19,  0.001,  0.000, 0.000, 0.141 },   /* main */
        {  179.99,  0.004,  0.004, 0.000, 0.000 },   /* mid */
      } },
    /*   6:30 */
    { .beat_start = 3.850, .beat_end = 3.864,
      .carrier_hz = 300.69, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 5, .layer = {
        {  300.69,  3.850,  3.864, 0.744, 0.743 },   /* high */
        {   60.63,  0.000,  0.000, 0.000, 0.000 },   /* sub */
        {  114.25,  0.001,  0.001, 0.106, 0.000 },   /* ref */
        {  107.19,  0.000,  0.000, 0.141, 0.000 },   /* main */
        {  179.99,  0.004,  0.004, 0.000, 0.000 },   /* mid */
      } },
    /*   7:00 */
    { .beat_start = 3.864, .beat_end = 3.857,
      .carrier_hz = 300.69, .seconds = 60, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 5, .layer = {
        {  300.69,  3.864,  3.857, 0.743, 0.593 },   /* high */
        {   60.63,  0.000,  0.000, 0.000, 0.000 },   /* sub */
        {  114.25,  0.001,  0.002, 0.000, 0.051 },   /* ref */
        {  107.19,  0.000,  0.000, 0.000, 0.000 },   /* main */
        {  179.99,  0.004,  0.004, 0.000, 0.000 },   /* mid */
      } },
    /*   8:00 */
    { .beat_start = 3.857, .beat_end = 3.856,
      .carrier_hz = 300.69, .seconds = 60, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 5, .layer = {
        {  300.69,  3.857,  3.856, 0.593, 0.353 },   /* high */
        {   60.63,  0.000,  0.000, 0.000, 0.000 },   /* sub */
        {  114.25,  0.002,  0.002, 0.051, 0.000 },   /* ref */
        {  107.19,  0.000,  0.000, 0.000, 0.000 },   /* main */
        {  179.99,  0.004,  0.004, 0.000, 0.000 },   /* mid */
      } },
    /*   9:00 */
    { .beat_start = 3.856, .beat_end = 3.863,
      .carrier_hz = 300.69, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 5, .layer = {
        {  300.69,  3.856,  3.863, 0.353, 0.317 },   /* high */
        {   60.63,  0.000,  0.001, 0.000, 0.231 },   /* sub */
        {  114.25,  0.002,  0.002, 0.000, 0.000 },   /* ref */
        {  107.19,  0.000,  0.000, 0.000, 0.000 },   /* main */
        {  179.99,  0.004,  0.004, 0.000, 0.000 },   /* mid */
      } },
    /*   9:30 */
    { .beat_start = 3.863, .beat_end = 3.860,
      .carrier_hz = 300.69, .seconds = 90, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 5, .layer = {
        {  300.69,  3.863,  3.860, 0.317, 0.307 },   /* high */
        {   60.63,  0.001,  0.001, 0.231, 0.195 },   /* sub */
        {  114.25,  0.002,  0.000, 0.000, 0.059 },   /* ref */
        {  107.19,  0.000,  0.000, 0.000, 0.000 },   /* main */
        {  179.99,  0.004,  0.004, 0.000, 0.000 },   /* mid */
      } },
    /*  11:00 */
    { .beat_start = 3.860, .beat_end = 3.855,
      .carrier_hz = 300.69, .seconds = 60, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 5, .layer = {
        {  300.69,  3.860,  3.855, 0.307, 0.306 },   /* high */
        {   60.63,  0.001,  0.000, 0.195, 0.266 },   /* sub */
        {  114.25,  0.000,  0.000, 0.059, 0.000 },   /* ref */
        {  107.19,  0.000,  0.000, 0.000, 0.000 },   /* main */
        {  179.99,  0.004,  0.004, 0.000, 0.000 },   /* mid */
      } },
    /*  12:00 */
    { .beat_start = 3.855, .beat_end = 3.854,
      .carrier_hz = 300.69, .seconds = 60, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 5, .layer = {
        {  300.69,  3.855,  3.854, 0.306, 0.309 },   /* high */
        {   60.63,  0.000,  0.001, 0.266, 0.261 },   /* sub */
        {  114.25,  0.000,  0.001, 0.000, 0.059 },   /* ref */
        {  107.19,  0.000,  0.000, 0.000, 0.000 },   /* main */
        {  179.99,  0.004,  0.004, 0.000, 0.142 },   /* mid */
      } },
    /*  13:00 */
    { .beat_start = 3.854, .beat_end = 3.853,
      .carrier_hz = 300.69, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 5, .layer = {
        {  300.69,  3.854,  3.853, 0.309, 0.275 },   /* high */
        {   60.63,  0.001,  0.000, 0.261, 0.272 },   /* sub */
        {  114.25,  0.001,  0.001, 0.059, 0.000 },   /* ref */
        {  107.19,  0.000,  0.000, 0.000, 0.000 },   /* main */
        {  179.99,  0.004,  0.004, 0.142, 0.000 },   /* mid */
      } },
    /*  13:30 */
    { .beat_start = 3.853, .beat_end = 3.842,
      .carrier_hz = 300.69, .seconds = 150, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 5, .layer = {
        {  300.69,  3.853,  3.842, 0.275, 0.272 },   /* high */
        {   60.63,  0.000,  0.000, 0.272, 0.000 },   /* sub */
        {  114.25,  0.001,  0.001, 0.000, 0.000 },   /* ref */
        {  107.19,  0.000,  0.000, 0.000, 0.000 },   /* main */
        {  179.99,  0.004,  0.004, 0.000, 0.000 },   /* mid */
      } },
    /*  16:00 */
    { .beat_start = 3.842, .beat_end = 3.843,
      .carrier_hz = 300.69, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 5, .layer = {
        {  300.69,  3.842,  3.843, 0.272, 0.271 },   /* high */
        {   60.63,  0.000,  0.000, 0.000, 0.000 },   /* sub */
        {  114.25,  0.001,  0.001, 0.000, 0.059 },   /* ref */
        {  107.19,  0.000,  0.000, 0.000, 0.000 },   /* main */
        {  179.99,  0.004,  0.004, 0.000, 0.000 },   /* mid */
      } },
    /*  16:30 */
    { .beat_start = 3.843, .beat_end = 3.840,
      .carrier_hz = 300.69, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 5, .layer = {
        {  300.69,  3.843,  3.840, 0.271, 0.272 },   /* high */
        {   60.63,  0.000,  0.000, 0.000, 0.000 },   /* sub */
        {  114.25,  0.001,  0.001, 0.059, 0.000 },   /* ref */
        {  107.19,  0.000,  0.000, 0.000, 0.000 },   /* main */
        {  179.99,  0.004,  0.004, 0.000, 0.000 },   /* mid */
      } },
    /*  17:00 */
    { .beat_start = 3.840, .beat_end = 3.836,
      .carrier_hz = 300.69, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 5, .layer = {
        {  300.69,  3.840,  3.836, 0.272, 0.277 },   /* high */
        {   60.63,  0.000,  0.000, 0.000, 0.000 },   /* sub */
        {  114.25,  0.001,  0.000, 0.000, 0.057 },   /* ref */
        {  107.19,  0.000,  0.000, 0.000, 0.000 },   /* main */
        {  179.99,  0.004,  0.004, 0.000, 0.000 },   /* mid */
      } },
    /*  17:30 */
    { .beat_start = 3.836, .beat_end = 3.757,
      .carrier_hz = 300.69, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 5, .layer = {
        {  300.69,  3.836,  3.757, 0.277, 0.273 },   /* high */
        {   60.63,  0.000,  0.000, 0.000, 0.000 },   /* sub */
        {  114.25,  0.000,  0.000, 0.057, 0.000 },   /* ref */
        {  107.19,  0.000,  0.000, 0.000, 0.000 },   /* main */
        {  179.99,  0.004,  0.004, 0.000, 0.000 },   /* mid */
      } },
    /*  18:00 */
    { .beat_start = 3.757, .beat_end = 3.701,
      .carrier_hz = 300.69, .seconds = 90, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 5, .layer = {
        {  300.69,  3.757,  3.701, 0.273, 0.272 },   /* high */
        {   60.63,  0.000,  0.000, 0.000, 0.000 },   /* sub */
        {  114.25,  0.000,  0.005, 0.000, 0.109 },   /* ref */
        {  107.19,  0.000,  0.001, 0.000, 0.101 },   /* main */
        {  179.99,  0.004,  0.004, 0.000, 0.000 },   /* mid */
      } },
    /*  19:30 */
    { .beat_start = 3.701, .beat_end = 3.692,
      .carrier_hz = 300.69, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 5, .layer = {
        {  300.69,  3.701,  3.692, 0.272, 0.194 },   /* high */
        {   60.63,  0.000,  0.000, 0.000, 0.000 },   /* sub */
        {  114.25,  0.005,  0.001, 0.109, 0.507 },   /* ref */
        {  107.19,  0.001,  0.000, 0.101, 0.357 },   /* main */
        {  179.99,  0.004,  0.004, 0.000, 0.000 },   /* mid */
      } },
    /*  20:00 */
    { .beat_start = 3.692, .beat_end = 3.688,
      .carrier_hz = 300.69, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 5, .layer = {
        {  300.69,  3.692,  3.688, 0.194, 0.142 },   /* high */
        {   60.63,  0.000,  0.000, 0.000, 0.000 },   /* sub */
        {  114.25,  0.001,  0.000, 0.507, 0.397 },   /* ref */
        {  107.19,  0.000,  0.000, 0.357, 0.573 },   /* main */
        {  179.99,  0.004,  0.004, 0.000, 0.000 },   /* mid */
      } },
    /*  20:30 */
    { .beat_start = 3.688, .beat_end = 0.001,
      .carrier_hz = 300.69, .seconds = 60, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 5, .layer = {
        {  300.69,  3.688,  0.001, 0.142, 0.069 },   /* high */
        {   60.63,  0.000,  0.000, 0.000, 0.000 },   /* sub */
        {  114.25,  0.000,  0.001, 0.397, 0.321 },   /* ref */
        {  107.19,  0.000,  0.001, 0.573, 0.495 },   /* main */
        {  179.99,  0.004,  0.004, 0.000, 0.000 },   /* mid */
      } },
    /*  21:30 */
    { .beat_start = 0.001, .beat_end = 3.833,
      .carrier_hz = 300.69, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 5, .layer = {
        {  300.69,  0.001,  3.833, 0.069, 0.048 },   /* high */
        {   60.63,  0.000,  0.000, 0.000, 0.000 },   /* sub */
        {  114.25,  0.001,  1.361, 0.321, 0.247 },   /* ref */
        {  107.19,  0.001,  0.001, 0.495, 0.000 },   /* main */
        {  179.99,  0.004,  0.004, 0.000, 0.000 },   /* mid */
      } },
    /*  22:00 */
    { .beat_start = 3.833, .beat_end = 3.683,
      .carrier_hz = 300.69, .seconds = 330, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 5, .layer = {
        {  300.69,  3.833,  3.683, 0.048, 0.077 },   /* high */
        {   60.63,  0.000,  1.208, 0.000, 0.160 },   /* sub */
        {  114.25,  1.361,  1.361, 0.247, 0.000 },   /* ref */
        {  107.19,  0.001,  0.001, 0.000, 0.000 },   /* main */
        {  179.99,  0.004,  0.004, 0.000, 0.000 },   /* mid */
      } },
    /*  27:30 */
    { .beat_start = 3.683, .beat_end = 3.683,
      .carrier_hz = 300.69, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 5, .layer = {
        {  300.69,  3.683,  3.683, 0.077, 0.078 },   /* high */
        {   60.63,  1.208,  1.204, 0.160, 0.212 },   /* sub */
        {  114.25,  1.361,  0.000, 0.000, 0.060 },   /* ref */
        {  107.19,  0.001,  0.001, 0.000, 0.000 },   /* main */
        {  179.99,  0.004,  0.004, 0.000, 0.000 },   /* mid */
      } },
    /*  28:00 */
    { .beat_start = 3.683, .beat_end = 3.717,
      .carrier_hz = 300.69, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 5, .layer = {
        {  300.69,  3.683,  3.717, 0.078, 0.081 },   /* high */
        {   60.63,  1.204,  1.203, 0.212, 0.253 },   /* sub */
        {  114.25,  0.000,  0.000, 0.060, 0.000 },   /* ref */
        {  107.19,  0.001,  0.001, 0.000, 0.000 },   /* main */
        {  179.99,  0.004,  0.004, 0.000, 0.000 },   /* mid */
      } },
    /*  28:30 */
    { .beat_start = 3.717, .beat_end = 3.751,
      .carrier_hz = 300.69, .seconds = 120, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 5, .layer = {
        {  300.69,  3.717,  3.751, 0.081, 0.080 },   /* high */
        {   60.63,  1.203,  1.202, 0.253, 0.259 },   /* sub */
        {  114.25,  0.000,  0.000, 0.000, 0.055 },   /* ref */
        {  107.19,  0.001,  0.001, 0.000, 0.000 },   /* main */
        {  179.99,  0.004,  0.004, 0.000, 0.000 },   /* mid */
      } },
    /*  30:30 */
    { .beat_start = 3.751, .beat_end = 3.759,
      .carrier_hz = 300.69, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 5, .layer = {
        {  300.69,  3.751,  3.759, 0.080, 0.031 },   /* high */
        {   60.63,  1.202,  1.202, 0.259, 0.000 },   /* sub */
        {  114.25,  0.000,  0.000, 0.055, 0.000 },   /* ref */
        {  107.19,  0.001,  0.001, 0.000, 0.000 },   /* main */
        {  179.99,  0.004,  0.004, 0.000, 0.000 },   /* mid */
      } },
    /*  31:00 */
    { .beat_start = 3.759, .beat_end = 3.759,
      .carrier_hz = 300.69, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 5, .layer = {
        {  300.69,  3.759,  3.759, 0.031, 0.000 },   /* high */
        {   60.63,  1.202,  1.202, 0.000, 0.000 },   /* sub */
        {  114.25,  0.000,  0.000, 0.000, 0.000 },   /* ref */
        {  107.19,  0.001,  0.001, 0.000, 0.000 },   /* main */
        {  179.99,  0.004,  0.004, 0.000, 0.000 },   /* mid */
      } },
    /*  31:30 */
    { .beat_start = 3.759, .beat_end = 8.000,
      .carrier_hz = 300.69, .seconds = 60, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 5, .layer = {
        {  300.69,  3.759,  8.000, 0.000, 0.001 },   /* high */
        {   60.63,  1.202,  0.001, 0.000, 0.001 },   /* sub */
        {  114.25,  0.000,  0.001, 0.000, 0.001 },   /* ref */
        {  107.19,  0.001,  0.017, 0.000, 0.000 },   /* main */
        {  179.99,  0.004,  0.000, 0.000, 0.001 },   /* mid */
      } },
    /*  32:30 */
    { .beat_start = 8.000, .beat_end = 8.000,
      .carrier_hz = 300.69, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 5, .layer = {
        {  300.69,  8.000,  8.000, 0.001, 0.001 },   /* high */
        {   60.63,  0.001,  0.001, 0.001, 0.001 },   /* sub */
        {  114.25,  0.001,  0.001, 0.001, 0.001 },   /* ref */
        {  107.19,  0.017,  0.017, 0.000, 0.000 },   /* main */
        {  179.99,  0.000,  0.000, 0.001, 0.001 },   /* mid */
      } },
};
/* 31 segments, layers: high, sub, ref, main, mid */

/* Practice 2: measured from the recording, 2230s, 4 layers. */
const en_prog_seg_t en_meas_practice_2[] = {
    /*   0:00 */
    { .beat_start = 8.864, .beat_end = 8.864,
      .carrier_hz = 102.08, .seconds = 60, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.08,  8.864,  8.864, 0.000, 0.000 },   /* main */
        {  300.14,  0.039,  0.039, 0.000, 0.000 },   /* high */
        {  119.47,  0.004,  0.004, 0.000, 0.254 },   /* ref */
        {  162.18, 11.034, 11.034, 0.000, 0.000 },   /* mid */
      } },
    /*   1:00 */
    { .beat_start = 8.864, .beat_end = 8.864,
      .carrier_hz = 102.08, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.08,  8.864,  8.864, 0.000, 0.000 },   /* main */
        {  300.14,  0.039,  0.039, 0.000, 0.000 },   /* high */
        {  119.47,  0.004, 10.418, 0.254, 0.204 },   /* ref */
        {  162.18, 11.034, 11.034, 0.000, 0.000 },   /* mid */
      } },
    /*   1:30 */
    { .beat_start = 8.864, .beat_end = 8.864,
      .carrier_hz = 102.08, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.08,  8.864,  8.864, 0.000, 0.000 },   /* main */
        {  300.14,  0.039,  0.039, 0.000, 0.000 },   /* high */
        {  119.47, 10.418, 10.418, 0.204, 0.000 },   /* ref */
        {  162.18, 11.034, 11.034, 0.000, 0.000 },   /* mid */
      } },
    /*   2:00 */
    { .beat_start = 8.864, .beat_end = 8.864,
      .carrier_hz = 102.08, .seconds = 60, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.08,  8.864,  8.864, 0.000, 0.000 },   /* main */
        {  300.14,  0.039,  0.039, 0.000, 0.000 },   /* high */
        {  119.47, 10.418, 10.440, 0.000, 0.116 },   /* ref */
        {  162.18, 11.034, 11.034, 0.000, 0.000 },   /* mid */
      } },
    /*   3:00 */
    { .beat_start = 8.864, .beat_end = 8.864,
      .carrier_hz = 102.08, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.08,  8.864,  8.864, 0.000, 0.000 },   /* main */
        {  300.14,  0.039,  0.039, 0.000, 0.000 },   /* high */
        {  119.47, 10.440, 10.440, 0.116, 0.000 },   /* ref */
        {  162.18, 11.034, 11.034, 0.000, 0.000 },   /* mid */
      } },
    /*   3:30 */
    { .beat_start = 8.864, .beat_end = 8.864,
      .carrier_hz = 102.08, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.08,  8.864,  8.864, 0.000, 0.000 },   /* main */
        {  300.14,  0.039,  0.039, 0.000, 0.000 },   /* high */
        {  119.47, 10.440, 10.473, 0.000, 0.114 },   /* ref */
        {  162.18, 11.034, 11.034, 0.000, 0.000 },   /* mid */
      } },
    /*   4:00 */
    { .beat_start = 8.864, .beat_end = 8.864,
      .carrier_hz = 102.08, .seconds = 60, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.08,  8.864,  8.864, 0.000, 0.000 },   /* main */
        {  300.14,  0.039,  0.039, 0.000, 0.000 },   /* high */
        {  119.47, 10.473, 10.473, 0.114, 0.000 },   /* ref */
        {  162.18, 11.034, 11.034, 0.000, 0.000 },   /* mid */
      } },
    /*   5:00 */
    { .beat_start = 8.864, .beat_end = 4.175,
      .carrier_hz = 102.08, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.08,  8.864,  4.175, 0.000, 0.152 },   /* main */
        {  300.14,  0.039,  0.039, 0.000, 0.000 },   /* high */
        {  119.47, 10.473,  0.971, 0.000, 0.145 },   /* ref */
        {  162.18, 11.034, 11.034, 0.000, 0.000 },   /* mid */
      } },
    /*   5:30 */
    { .beat_start = 4.175, .beat_end = 0.032,
      .carrier_hz = 102.08, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.08,  4.175,  0.032, 0.152, 0.314 },   /* main */
        {  300.14,  0.039,  0.030, 0.000, 0.060 },   /* high */
        {  119.47,  0.971,  0.747, 0.145, 0.567 },   /* ref */
        {  162.18, 11.034, 11.034, 0.000, 0.000 },   /* mid */
      } },
    /*   6:00 */
    { .beat_start = 0.032, .beat_end = 0.361,
      .carrier_hz = 102.08, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.08,  0.032,  0.361, 0.314, 0.340 },   /* main */
        {  300.14,  0.030,  0.227, 0.060, 0.042 },   /* high */
        {  119.47,  0.747,  0.025, 0.567, 0.499 },   /* ref */
        {  162.18, 11.034, 11.034, 0.000, 0.000 },   /* mid */
      } },
    /*   6:30 */
    { .beat_start = 0.361, .beat_end = 4.084,
      .carrier_hz = 102.08, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.08,  0.361,  4.084, 0.340, 0.952 },   /* main */
        {  300.14,  0.227,  0.043, 0.042, 0.045 },   /* high */
        {  119.47,  0.025,  0.165, 0.499, 0.247 },   /* ref */
        {  162.18, 11.034, 11.034, 0.000, 0.000 },   /* mid */
      } },
    /*   7:00 */
    { .beat_start = 4.084, .beat_end = 4.089,
      .carrier_hz = 102.08, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.08,  4.084,  4.089, 0.952, 0.713 },   /* main */
        {  300.14,  0.043,  1.647, 0.045, 0.026 },   /* high */
        {  119.47,  0.165,  0.903, 0.247, 0.262 },   /* ref */
        {  162.18, 11.034, 11.034, 0.000, 0.000 },   /* mid */
      } },
    /*   7:30 */
    { .beat_start = 4.089, .beat_end = 4.105,
      .carrier_hz = 102.08, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.08,  4.089,  4.105, 0.713, 0.712 },   /* main */
        {  300.14,  1.647,  1.647, 0.026, 0.000 },   /* high */
        {  119.47,  0.903,  0.000, 0.262, 0.052 },   /* ref */
        {  162.18, 11.034, 11.034, 0.000, 0.000 },   /* mid */
      } },
    /*   8:00 */
    { .beat_start = 4.105, .beat_end = 4.114,
      .carrier_hz = 102.08, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.08,  4.105,  4.114, 0.712, 0.711 },   /* main */
        {  300.14,  1.647,  1.647, 0.000, 0.000 },   /* high */
        {  119.47,  0.000,  0.000, 0.052, 0.000 },   /* ref */
        {  162.18, 11.034, 11.034, 0.000, 0.000 },   /* mid */
      } },
    /*   8:30 */
    { .beat_start = 4.114, .beat_end = 4.127,
      .carrier_hz = 102.08, .seconds = 90, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.08,  4.114,  4.127, 0.711, 0.715 },   /* main */
        {  300.14,  1.647,  3.841, 0.000, 0.067 },   /* high */
        {  119.47,  0.000,  0.000, 0.000, 0.000 },   /* ref */
        {  162.18, 11.034, 11.034, 0.000, 0.000 },   /* mid */
      } },
    /*  10:00 */
    { .beat_start = 4.127, .beat_end = 4.126,
      .carrier_hz = 102.08, .seconds = 60, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.08,  4.127,  4.126, 0.715, 0.710 },   /* main */
        {  300.14,  3.841,  3.852, 0.067, 0.097 },   /* high */
        {  119.47,  0.000,  0.002, 0.000, 0.050 },   /* ref */
        {  162.18, 11.034, 11.034, 0.000, 0.000 },   /* mid */
      } },
    /*  11:00 */
    { .beat_start = 4.126, .beat_end = 4.129,
      .carrier_hz = 102.08, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.08,  4.126,  4.129, 0.710, 0.712 },   /* main */
        {  300.14,  3.852,  3.852, 0.097, 0.116 },   /* high */
        {  119.47,  0.002,  0.002, 0.050, 0.000 },   /* ref */
        {  162.18, 11.034, 11.034, 0.000, 0.000 },   /* mid */
      } },
    /*  11:30 */
    { .beat_start = 4.129, .beat_end = 4.126,
      .carrier_hz = 102.08, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.08,  4.129,  4.126, 0.712, 0.708 },   /* main */
        {  300.14,  3.852,  3.853, 0.116, 0.135 },   /* high */
        {  119.47,  0.002,  0.001, 0.000, 0.046 },   /* ref */
        {  162.18, 11.034, 11.034, 0.000, 0.000 },   /* mid */
      } },
    /*  12:00 */
    { .beat_start = 4.126, .beat_end = 4.127,
      .carrier_hz = 102.08, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.08,  4.126,  4.127, 0.708, 0.714 },   /* main */
        {  300.14,  3.853,  3.853, 0.135, 0.135 },   /* high */
        {  119.47,  0.001,  0.001, 0.046, 0.000 },   /* ref */
        {  162.18, 11.034, 11.034, 0.000, 0.000 },   /* mid */
      } },
    /*  12:30 */
    { .beat_start = 4.127, .beat_end = 4.108,
      .carrier_hz = 102.08, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.08,  4.127,  4.108, 0.714, 0.704 },   /* main */
        {  300.14,  3.853,  3.843, 0.135, 0.134 },   /* high */
        {  119.47,  0.001,  0.003, 0.000, 0.048 },   /* ref */
        {  162.18, 11.034, 11.034, 0.000, 0.000 },   /* mid */
      } },
    /*  13:00 */
    { .beat_start = 4.108, .beat_end = 4.103,
      .carrier_hz = 102.08, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.08,  4.108,  4.103, 0.704, 0.581 },   /* main */
        {  300.14,  3.843,  3.849, 0.134, 0.136 },   /* high */
        {  119.47,  0.003,  0.003, 0.048, 0.000 },   /* ref */
        {  162.18, 11.034, 11.034, 0.000, 0.000 },   /* mid */
      } },
    /*  13:30 */
    { .beat_start = 4.103, .beat_end = 4.108,
      .carrier_hz = 102.08, .seconds = 90, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.08,  4.103,  4.108, 0.581, 0.429 },   /* main */
        {  300.14,  3.849,  3.841, 0.136, 0.151 },   /* high */
        {  119.47,  0.003,  0.000, 0.000, 0.049 },   /* ref */
        {  162.18, 11.034, 11.034, 0.000, 0.000 },   /* mid */
      } },
    /*  15:00 */
    { .beat_start = 4.108, .beat_end = 4.105,
      .carrier_hz = 102.08, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.08,  4.108,  4.105, 0.429, 0.423 },   /* main */
        {  300.14,  3.841,  3.847, 0.151, 0.168 },   /* high */
        {  119.47,  0.000,  0.000, 0.049, 0.000 },   /* ref */
        {  162.18, 11.034, 11.034, 0.000, 0.000 },   /* mid */
      } },
    /*  15:30 */
    { .beat_start = 4.105, .beat_end = 4.096,
      .carrier_hz = 102.08, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.08,  4.105,  4.096, 0.423, 0.411 },   /* main */
        {  300.14,  3.847,  3.846, 0.168, 0.170 },   /* high */
        {  119.47,  0.000,  0.001, 0.000, 0.043 },   /* ref */
        {  162.18, 11.034, 11.034, 0.000, 0.000 },   /* mid */
      } },
    /*  16:00 */
    { .beat_start = 4.096, .beat_end = 4.074,
      .carrier_hz = 102.08, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.08,  4.096,  4.074, 0.411, 0.419 },   /* main */
        {  300.14,  3.846,  3.848, 0.170, 0.171 },   /* high */
        {  119.47,  0.001,  0.001, 0.043, 0.000 },   /* ref */
        {  162.18, 11.034, 11.034, 0.000, 0.000 },   /* mid */
      } },
    /*  16:30 */
    { .beat_start = 4.074, .beat_end = 4.068,
      .carrier_hz = 102.08, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.08,  4.074,  4.068, 0.419, 0.359 },   /* main */
        {  300.14,  3.848,  3.853, 0.171, 0.170 },   /* high */
        {  119.47,  0.001,  0.001, 0.000, 0.041 },   /* ref */
        {  162.18, 11.034, 11.034, 0.000, 0.000 },   /* mid */
      } },
    /*  17:00 */
    { .beat_start = 4.068, .beat_end = 4.046,
      .carrier_hz = 102.08, .seconds = 120, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.08,  4.068,  4.046, 0.359, 0.333 },   /* main */
        {  300.14,  3.853,  3.853, 0.170, 0.177 },   /* high */
        {  119.47,  0.001,  0.001, 0.041, 0.000 },   /* ref */
        {  162.18, 11.034, 11.034, 0.000, 0.000 },   /* mid */
      } },
    /*  19:00 */
    { .beat_start = 4.046, .beat_end = 4.047,
      .carrier_hz = 102.08, .seconds = 60, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.08,  4.046,  4.047, 0.333, 0.225 },   /* main */
        {  300.14,  3.853,  3.850, 0.177, 0.179 },   /* high */
        {  119.47,  0.001,  0.001, 0.000, 0.040 },   /* ref */
        {  162.18, 11.034, 11.034, 0.000, 0.000 },   /* mid */
      } },
    /*  20:00 */
    { .beat_start = 4.047, .beat_end = 4.039,
      .carrier_hz = 102.08, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.08,  4.047,  4.039, 0.225, 0.234 },   /* main */
        {  300.14,  3.850,  3.847, 0.179, 0.182 },   /* high */
        {  119.47,  0.001,  0.001, 0.040, 0.000 },   /* ref */
        {  162.18, 11.034, 11.034, 0.000, 0.000 },   /* mid */
      } },
    /*  20:30 */
    { .beat_start = 4.039, .beat_end = 4.032,
      .carrier_hz = 102.08, .seconds = 90, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.08,  4.039,  4.032, 0.234, 0.225 },   /* main */
        {  300.14,  3.847,  3.841, 0.182, 0.177 },   /* high */
        {  119.47,  0.001,  0.001, 0.000, 0.046 },   /* ref */
        {  162.18, 11.034, 11.034, 0.000, 0.000 },   /* mid */
      } },
    /*  22:00 */
    { .beat_start = 4.032, .beat_end = 4.027,
      .carrier_hz = 102.08, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.08,  4.032,  4.027, 0.225, 0.226 },   /* main */
        {  300.14,  3.841,  3.840, 0.177, 0.175 },   /* high */
        {  119.47,  0.001,  0.001, 0.046, 0.000 },   /* ref */
        {  162.18, 11.034, 11.034, 0.000, 0.000 },   /* mid */
      } },
    /*  22:30 */
    { .beat_start = 4.027, .beat_end = 4.011,
      .carrier_hz = 102.08, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.08,  4.027,  4.011, 0.226, 0.227 },   /* main */
        {  300.14,  3.840,  3.833, 0.175, 0.176 },   /* high */
        {  119.47,  0.001,  0.000, 0.000, 0.052 },   /* ref */
        {  162.18, 11.034, 11.034, 0.000, 0.000 },   /* mid */
      } },
    /*  23:00 */
    { .beat_start = 4.011, .beat_end = 4.009,
      .carrier_hz = 102.08, .seconds = 60, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.08,  4.011,  4.009, 0.227, 0.232 },   /* main */
        {  300.14,  3.833,  3.828, 0.176, 0.178 },   /* high */
        {  119.47,  0.000,  0.000, 0.052, 0.000 },   /* ref */
        {  162.18, 11.034, 11.034, 0.000, 0.000 },   /* mid */
      } },
    /*  24:00 */
    { .beat_start = 4.009, .beat_end = 4.001,
      .carrier_hz = 102.08, .seconds = 60, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.08,  4.009,  4.001, 0.232, 0.232 },   /* main */
        {  300.14,  3.828,  3.825, 0.178, 0.175 },   /* high */
        {  119.47,  0.000,  0.000, 0.000, 0.046 },   /* ref */
        {  162.18, 11.034, 11.034, 0.000, 0.000 },   /* mid */
      } },
    /*  25:00 */
    { .beat_start = 4.001, .beat_end = 3.992,
      .carrier_hz = 102.08, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.08,  4.001,  3.992, 0.232, 0.236 },   /* main */
        {  300.14,  3.825,  3.833, 0.175, 0.174 },   /* high */
        {  119.47,  0.000,  0.000, 0.046, 0.000 },   /* ref */
        {  162.18, 11.034, 11.034, 0.000, 0.000 },   /* mid */
      } },
    /*  25:30 */
    { .beat_start = 3.992, .beat_end = 3.977,
      .carrier_hz = 102.08, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.08,  3.992,  3.977, 0.236, 0.229 },   /* main */
        {  300.14,  3.833,  3.816, 0.174, 0.176 },   /* high */
        {  119.47,  0.000,  0.001, 0.000, 0.045 },   /* ref */
        {  162.18, 11.034, 11.034, 0.000, 0.000 },   /* mid */
      } },
    /*  26:00 */
    { .beat_start = 3.977, .beat_end = 3.957,
      .carrier_hz = 102.08, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.08,  3.977,  3.957, 0.229, 0.236 },   /* main */
        {  300.14,  3.816,  3.786, 0.176, 0.180 },   /* high */
        {  119.47,  0.001,  0.001, 0.045, 0.000 },   /* ref */
        {  162.18, 11.034, 11.034, 0.000, 0.000 },   /* mid */
      } },
    /*  26:30 */
    { .beat_start = 3.957, .beat_end = 3.893,
      .carrier_hz = 102.08, .seconds = 90, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.08,  3.957,  3.893, 0.236, 0.230 },   /* main */
        {  300.14,  3.786,  3.682, 0.180, 0.176 },   /* high */
        {  119.47,  0.001,  0.000, 0.000, 0.050 },   /* ref */
        {  162.18, 11.034, 11.034, 0.000, 0.000 },   /* mid */
      } },
    /*  28:00 */
    { .beat_start = 3.893, .beat_end = 3.880,
      .carrier_hz = 102.08, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.08,  3.893,  3.880, 0.230, 0.228 },   /* main */
        {  300.14,  3.682,  3.688, 0.176, 0.180 },   /* high */
        {  119.47,  0.000,  0.000, 0.050, 0.000 },   /* ref */
        {  162.18, 11.034, 11.034, 0.000, 0.000 },   /* mid */
      } },
    /*  28:30 */
    { .beat_start = 3.880, .beat_end = 3.890,
      .carrier_hz = 102.08, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.08,  3.880,  3.890, 0.228, 0.228 },   /* main */
        {  300.14,  3.688,  3.684, 0.180, 0.176 },   /* high */
        {  119.47,  0.000,  0.001, 0.000, 0.050 },   /* ref */
        {  162.18, 11.034, 11.034, 0.000, 0.000 },   /* mid */
      } },
    /*  29:00 */
    { .beat_start = 3.890, .beat_end = 3.894,
      .carrier_hz = 102.08, .seconds = 60, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.08,  3.890,  3.894, 0.228, 0.238 },   /* main */
        {  300.14,  3.684,  3.701, 0.176, 0.184 },   /* high */
        {  119.47,  0.001,  0.001, 0.050, 0.000 },   /* ref */
        {  162.18, 11.034, 11.034, 0.000, 0.000 },   /* mid */
      } },
    /*  30:00 */
    { .beat_start = 3.894, .beat_end = 3.888,
      .carrier_hz = 102.08, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.08,  3.894,  3.888, 0.238, 0.228 },   /* main */
        {  300.14,  3.701,  3.710, 0.184, 0.176 },   /* high */
        {  119.47,  0.001,  0.000, 0.000, 0.046 },   /* ref */
        {  162.18, 11.034, 11.034, 0.000, 0.000 },   /* mid */
      } },
    /*  30:30 */
    { .beat_start = 3.888, .beat_end = 3.871,
      .carrier_hz = 102.08, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.08,  3.888,  3.871, 0.228, 0.225 },   /* main */
        {  300.14,  3.710,  3.720, 0.176, 0.177 },   /* high */
        {  119.47,  0.000,  0.000, 0.046, 0.000 },   /* ref */
        {  162.18, 11.034, 11.034, 0.000, 0.000 },   /* mid */
      } },
    /*  31:00 */
    { .beat_start = 3.871, .beat_end = 3.864,
      .carrier_hz = 102.08, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.08,  3.871,  3.864, 0.225, 0.229 },   /* main */
        {  300.14,  3.720,  3.718, 0.177, 0.172 },   /* high */
        {  119.47,  0.000,  0.000, 0.000, 0.049 },   /* ref */
        {  162.18, 11.034, 11.034, 0.000, 0.000 },   /* mid */
      } },
    /*  31:30 */
    { .beat_start = 3.864, .beat_end = 3.864,
      .carrier_hz = 102.08, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.08,  3.864,  3.864, 0.229, 0.000 },   /* main */
        {  300.14,  3.718,  3.765, 0.172, 0.182 },   /* high */
        {  119.47,  0.000,  0.000, 0.049, 0.000 },   /* ref */
        {  162.18, 11.034, 11.034, 0.000, 0.000 },   /* mid */
      } },
    /*  32:00 */
    { .beat_start = 3.864, .beat_end = 3.864,
      .carrier_hz = 102.08, .seconds = 90, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.08,  3.864,  3.864, 0.000, 0.000 },   /* main */
        {  300.14,  3.765,  3.775, 0.182, 0.176 },   /* high */
        {  119.47,  0.000,  0.000, 0.000, 0.045 },   /* ref */
        {  162.18, 11.034, 11.034, 0.000, 0.000 },   /* mid */
      } },
    /*  33:30 */
    { .beat_start = 3.864, .beat_end = 3.864,
      .carrier_hz = 102.08, .seconds = 60, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.08,  3.864,  3.864, 0.000, 0.000 },   /* main */
        {  300.14,  3.775,  3.777, 0.176, 0.159 },   /* high */
        {  119.47,  0.000,  0.000, 0.045, 0.000 },   /* ref */
        {  162.18, 11.034, 11.040, 0.000, 0.093 },   /* mid */
      } },
    /*  34:30 */
    { .beat_start = 3.864, .beat_end = 3.864,
      .carrier_hz = 102.08, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.08,  3.864,  3.864, 0.000, 0.000 },   /* main */
        {  300.14,  3.777,  3.780, 0.159, 0.135 },   /* high */
        {  119.47,  0.000,  0.001, 0.000, 0.049 },   /* ref */
        {  162.18, 11.040, 11.039, 0.093, 0.165 },   /* mid */
      } },
    /*  35:00 */
    { .beat_start = 3.864, .beat_end = 3.864,
      .carrier_hz = 102.08, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.08,  3.864,  3.864, 0.000, 0.000 },   /* main */
        {  300.14,  3.780,  3.780, 0.135, 0.000 },   /* high */
        {  119.47,  0.001,  0.001, 0.049, 0.000 },   /* ref */
        {  162.18, 11.039, 11.046, 0.165, 0.271 },   /* mid */
      } },
    /*  35:30 */
    { .beat_start = 3.864, .beat_end = 0.007,
      .carrier_hz = 102.08, .seconds = 60, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.08,  3.864,  0.007, 0.000, 0.000 },   /* main */
        {  300.14,  3.780,  7.998, 0.000, 0.001 },   /* high */
        {  119.47,  0.001,  0.001, 0.000, 0.001 },   /* ref */
        {  162.18, 11.046,  0.000, 0.271, 0.001 },   /* mid */
      } },
    /*  36:30 */
    { .beat_start = 0.007, .beat_end = 0.007,
      .carrier_hz = 102.08, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.08,  0.007,  0.007, 0.000, 0.000 },   /* main */
        {  300.14,  7.998,  7.998, 0.001, 0.001 },   /* high */
        {  119.47,  0.001,  0.001, 0.001, 0.001 },   /* ref */
        {  162.18,  0.000,  0.000, 0.001, 0.001 },   /* mid */
      } },
};
/* 51 segments, layers: main, high, ref, mid */

/* Practice 3: measured from the recording, 2170s, 4 layers. */
const en_prog_seg_t en_meas_practice_3[] = {
    /*   0:00 */
    { .beat_start = 0.196, .beat_end = 0.196,
      .carrier_hz = 102.44, .seconds = 210, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.44,  0.196,  0.196, 0.000, 0.349 },   /* main */
        {  301.08,  0.008,  0.008, 0.000, 0.000 },   /* high */
        {  162.20, 11.041, 11.041, 0.000, 0.000 },   /* mid */
        {  109.64,  0.424,  0.738, 0.000, 0.345 },   /* ref */
      } },
    /*   3:30 */
    { .beat_start = 0.196, .beat_end = 0.005,
      .carrier_hz = 102.44, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.44,  0.196,  0.005, 0.349, 0.389 },   /* main */
        {  301.08,  0.008,  0.009, 0.000, 0.105 },   /* high */
        {  162.20, 11.041, 11.041, 0.000, 0.000 },   /* mid */
        {  109.64,  0.738,  0.030, 0.345, 0.509 },   /* ref */
      } },
    /*   4:00 */
    { .beat_start = 0.005, .beat_end = 2.614,
      .carrier_hz = 102.44, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.44,  0.005,  2.614, 0.389, 0.496 },   /* main */
        {  301.08,  0.009,  0.581, 0.105, 0.114 },   /* high */
        {  162.20, 11.041, 11.041, 0.000, 0.000 },   /* mid */
        {  109.64,  0.030,  0.033, 0.509, 0.631 },   /* ref */
      } },
    /*   4:30 */
    { .beat_start = 2.614, .beat_end = 4.109,
      .carrier_hz = 102.44, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.44,  2.614,  4.109, 0.496, 0.838 },   /* main */
        {  301.08,  0.581,  0.022, 0.114, 0.125 },   /* high */
        {  162.20, 11.041, 11.041, 0.000, 0.000 },   /* mid */
        {  109.64,  0.033,  0.012, 0.631, 0.537 },   /* ref */
      } },
    /*   5:00 */
    { .beat_start = 4.109, .beat_end = 4.117,
      .carrier_hz = 102.44, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.44,  4.109,  4.117, 0.838, 0.836 },   /* main */
        {  301.08,  0.022,  0.022, 0.125, 0.000 },   /* high */
        {  162.20, 11.041, 11.041, 0.000, 0.000 },   /* mid */
        {  109.64,  0.012,  0.022, 0.537, 0.324 },   /* ref */
      } },
    /*   5:30 */
    { .beat_start = 4.117, .beat_end = 4.122,
      .carrier_hz = 102.44, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.44,  4.117,  4.122, 0.836, 0.845 },   /* main */
        {  301.08,  0.022,  0.022, 0.000, 0.000 },   /* high */
        {  162.20, 11.041, 11.041, 0.000, 0.000 },   /* mid */
        {  109.64,  0.022,  0.022, 0.324, 0.000 },   /* ref */
      } },
    /*   6:00 */
    { .beat_start = 4.122, .beat_end = 4.096,
      .carrier_hz = 102.44, .seconds = 450, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.44,  4.122,  4.096, 0.845, 0.620 },   /* main */
        {  301.08,  0.022,  0.022, 0.000, 0.000 },   /* high */
        {  162.20, 11.041, 11.041, 0.000, 0.000 },   /* mid */
        {  109.64,  0.022,  0.022, 0.000, 0.000 },   /* ref */
      } },
    /*  13:30 */
    { .beat_start = 4.096, .beat_end = 4.074,
      .carrier_hz = 102.44, .seconds = 90, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.44,  4.096,  4.074, 0.620, 0.483 },   /* main */
        {  301.08,  0.022,  0.022, 0.000, 0.000 },   /* high */
        {  162.20, 11.041, 11.041, 0.000, 0.000 },   /* mid */
        {  109.64,  0.022,  0.022, 0.000, 0.000 },   /* ref */
      } },
    /*  15:00 */
    { .beat_start = 4.074, .beat_end = 4.054,
      .carrier_hz = 102.44, .seconds = 90, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.44,  4.074,  4.054, 0.483, 0.418 },   /* main */
        {  301.08,  0.022,  3.843, 0.000, 0.149 },   /* high */
        {  162.20, 11.041, 11.041, 0.000, 0.000 },   /* mid */
        {  109.64,  0.022,  0.022, 0.000, 0.000 },   /* ref */
      } },
    /*  16:30 */
    { .beat_start = 4.054, .beat_end = 4.047,
      .carrier_hz = 102.44, .seconds = 60, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.44,  4.054,  4.047, 0.418, 0.281 },   /* main */
        {  301.08,  3.843,  3.856, 0.149, 0.338 },   /* high */
        {  162.20, 11.041, 11.041, 0.000, 0.000 },   /* mid */
        {  109.64,  0.022,  0.022, 0.000, 0.000 },   /* ref */
      } },
    /*  17:30 */
    { .beat_start = 4.047, .beat_end = 4.032,
      .carrier_hz = 102.44, .seconds = 90, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.44,  4.047,  4.032, 0.281, 0.094 },   /* main */
        {  301.08,  3.856,  3.866, 0.338, 0.511 },   /* high */
        {  162.20, 11.041, 11.041, 0.000, 0.000 },   /* mid */
        {  109.64,  0.022,  0.022, 0.000, 0.000 },   /* ref */
      } },
    /*  19:00 */
    { .beat_start = 4.032, .beat_end = 4.032,
      .carrier_hz = 102.44, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.44,  4.032,  4.032, 0.094, 0.000 },   /* main */
        {  301.08,  3.866,  3.868, 0.511, 0.541 },   /* high */
        {  162.20, 11.041, 11.041, 0.000, 0.000 },   /* mid */
        {  109.64,  0.022,  0.022, 0.000, 0.000 },   /* ref */
      } },
    /*  19:30 */
    { .beat_start = 4.032, .beat_end = 4.032,
      .carrier_hz = 102.44, .seconds = 150, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.44,  4.032,  4.032, 0.000, 0.000 },   /* main */
        {  301.08,  3.868,  3.871, 0.541, 0.352 },   /* high */
        {  162.20, 11.041, 11.041, 0.000, 0.000 },   /* mid */
        {  109.64,  0.022,  0.022, 0.000, 0.000 },   /* ref */
      } },
    /*  22:00 */
    { .beat_start = 4.032, .beat_end = 4.032,
      .carrier_hz = 102.44, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.44,  4.032,  4.032, 0.000, 0.000 },   /* main */
        {  301.08,  3.871,  3.871, 0.352, 0.000 },   /* high */
        {  162.20, 11.041, 11.040, 0.000, 0.364 },   /* mid */
        {  109.64,  0.022,  0.022, 0.000, 0.000 },   /* ref */
      } },
    /*  22:30 */
    { .beat_start = 4.032, .beat_end = 4.032,
      .carrier_hz = 102.44, .seconds = 60, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.44,  4.032,  4.032, 0.000, 0.000 },   /* main */
        {  301.08,  3.871,  3.871, 0.000, 0.000 },   /* high */
        {  162.20, 11.040, 11.043, 0.364, 0.512 },   /* mid */
        {  109.64,  0.022,  0.022, 0.000, 0.000 },   /* ref */
      } },
    /*  23:30 */
    { .beat_start = 4.032, .beat_end = 4.032,
      .carrier_hz = 102.44, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.44,  4.032,  4.032, 0.000, 0.000 },   /* main */
        {  301.08,  3.871,  3.871, 0.000, 0.000 },   /* high */
        {  162.20, 11.043, 11.043, 0.512, 0.000 },   /* mid */
        {  109.64,  0.022,  0.022, 0.000, 0.000 },   /* ref */
      } },
    /*  24:00 */
    { .beat_start = 4.032, .beat_end = 4.032,
      .carrier_hz = 102.44, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.44,  4.032,  4.032, 0.000, 0.000 },   /* main */
        {  301.08,  3.871,  3.853, 0.000, 0.272 },   /* high */
        {  162.20, 11.043, 11.043, 0.000, 0.000 },   /* mid */
        {  109.64,  0.022,  0.022, 0.000, 0.000 },   /* ref */
      } },
    /*  24:30 */
    { .beat_start = 4.032, .beat_end = 4.032,
      .carrier_hz = 102.44, .seconds = 570, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.44,  4.032,  4.032, 0.000, 0.000 },   /* main */
        {  301.08,  3.853,  3.853, 0.272, 0.000 },   /* high */
        {  162.20, 11.043, 11.075, 0.000, 0.257 },   /* mid */
        {  109.64,  0.022,  0.022, 0.000, 0.000 },   /* ref */
      } },
    /*  34:00 */
    { .beat_start = 4.032, .beat_end = 4.032,
      .carrier_hz = 102.44, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.44,  4.032,  4.032, 0.000, 0.000 },   /* main */
        {  301.08,  3.853,  3.853, 0.000, 0.000 },   /* high */
        {  162.20, 11.075, 11.086, 0.257, 0.697 },   /* mid */
        {  109.64,  0.022,  0.022, 0.000, 0.000 },   /* ref */
      } },
    /*  34:30 */
    { .beat_start = 4.032, .beat_end = 4.032,
      .carrier_hz = 102.44, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.44,  4.032,  4.032, 0.000, 0.000 },   /* main */
        {  301.08,  3.853,  3.853, 0.000, 0.000 },   /* high */
        {  162.20, 11.086, 11.086, 0.697, 0.000 },   /* mid */
        {  109.64,  0.022,  0.022, 0.000, 0.000 },   /* ref */
      } },
    /*  35:00 */
    { .beat_start = 4.032, .beat_end = 4.032,
      .carrier_hz = 102.44, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.44,  4.032,  4.032, 0.000, 0.000 },   /* main */
        {  301.08,  3.853,  3.853, 0.000, 0.000 },   /* high */
        {  162.20, 11.086,  0.004, 0.000, 0.004 },   /* mid */
        {  109.64,  0.022,  0.001, 0.000, 0.010 },   /* ref */
      } },
    /*  35:30 */
    { .beat_start = 4.032, .beat_end = 4.032,
      .carrier_hz = 102.44, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  102.44,  4.032,  4.032, 0.000, 0.000 },   /* main */
        {  301.08,  3.853,  3.853, 0.000, 0.000 },   /* high */
        {  162.20,  0.004,  0.004, 0.004, 0.004 },   /* mid */
        {  109.64,  0.001,  0.001, 0.010, 0.010 },   /* ref */
      } },
};
/* 22 segments, layers: main, high, mid, ref */

/* Practice 4: measured from the recording, 2161s, 3 layers. */
const en_prog_seg_t en_meas_practice_4[] = {
    /*   0:00 */
    { .beat_start = 4.104, .beat_end = 4.111,
      .carrier_hz = 101.49, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 3, .layer = {
        {  101.49,  4.104,  4.111, 0.000, 0.265 },   /* main */
        {  119.52,  0.004,  0.003, 0.000, 0.344 },   /* ref */
        {  316.02,  0.035,  0.035, 0.000, 0.000 },   /* high */
      } },
    /*   0:30 */
    { .beat_start = 4.111, .beat_end = 4.103,
      .carrier_hz = 101.49, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 3, .layer = {
        {  101.49,  4.111,  4.103, 0.265, 0.449 },   /* main */
        {  119.52,  0.003,  0.000, 0.344, 0.101 },   /* ref */
        {  316.02,  0.035,  0.035, 0.000, 0.000 },   /* high */
      } },
    /*   1:00 */
    { .beat_start = 4.103, .beat_end = 4.064,
      .carrier_hz = 101.49, .seconds = 150, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 3, .layer = {
        {  101.49,  4.103,  4.064, 0.449, 0.498 },   /* main */
        {  119.52,  0.000,  0.000, 0.101, 0.000 },   /* ref */
        {  316.02,  0.035,  0.035, 0.000, 0.000 },   /* high */
      } },
    /*   3:30 */
    { .beat_start = 4.064, .beat_end = 4.059,
      .carrier_hz = 101.49, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 3, .layer = {
        {  101.49,  4.064,  4.059, 0.498, 0.497 },   /* main */
        {  119.52,  0.000,  0.002, 0.000, 0.098 },   /* ref */
        {  316.02,  0.035,  0.035, 0.000, 0.000 },   /* high */
      } },
    /*   4:00 */
    { .beat_start = 4.059, .beat_end = 4.051,
      .carrier_hz = 101.49, .seconds = 60, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 3, .layer = {
        {  101.49,  4.059,  4.051, 0.497, 0.495 },   /* main */
        {  119.52,  0.002,  0.029, 0.098, 0.394 },   /* ref */
        {  316.02,  0.035,  0.035, 0.000, 0.094 },   /* high */
      } },
    /*   5:00 */
    { .beat_start = 4.051, .beat_end = 4.038,
      .carrier_hz = 101.49, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 3, .layer = {
        {  101.49,  4.051,  4.038, 0.495, 0.482 },   /* main */
        {  119.52,  0.029,  0.011, 0.394, 0.320 },   /* ref */
        {  316.02,  0.035,  3.526, 0.094, 0.063 },   /* high */
      } },
    /*   5:30 */
    { .beat_start = 4.038, .beat_end = 4.009,
      .carrier_hz = 101.49, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 3, .layer = {
        {  101.49,  4.038,  4.009, 0.482, 0.495 },   /* main */
        {  119.52,  0.011,  0.013, 0.320, 0.296 },   /* ref */
        {  316.02,  3.526,  3.526, 0.063, 0.000 },   /* high */
      } },
    /*   6:00 */
    { .beat_start = 4.009, .beat_end = 4.003,
      .carrier_hz = 101.49, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 3, .layer = {
        {  101.49,  4.009,  4.003, 0.495, 0.497 },   /* main */
        {  119.52,  0.013,  0.002, 0.296, 0.101 },   /* ref */
        {  316.02,  3.526,  3.526, 0.000, 0.000 },   /* high */
      } },
    /*   6:30 */
    { .beat_start = 4.003, .beat_end = 3.956,
      .carrier_hz = 101.49, .seconds = 180, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 3, .layer = {
        {  101.49,  4.003,  3.956, 0.497, 0.344 },   /* main */
        {  119.52,  0.002,  0.000, 0.101, 0.079 },   /* ref */
        {  316.02,  3.526,  3.526, 0.000, 0.000 },   /* high */
      } },
    /*   9:30 */
    { .beat_start = 3.956, .beat_end = 3.928,
      .carrier_hz = 101.49, .seconds = 60, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 3, .layer = {
        {  101.49,  3.956,  3.928, 0.344, 0.355 },   /* main */
        {  119.52,  0.000,  0.000, 0.079, 0.000 },   /* ref */
        {  316.02,  3.526,  3.526, 0.000, 0.000 },   /* high */
      } },
    /*  10:30 */
    { .beat_start = 3.928, .beat_end = 3.931,
      .carrier_hz = 101.49, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 3, .layer = {
        {  101.49,  3.928,  3.931, 0.355, 0.345 },   /* main */
        {  119.52,  0.000,  0.000, 0.000, 0.091 },   /* ref */
        {  316.02,  3.526,  3.526, 0.000, 0.000 },   /* high */
      } },
    /*  11:00 */
    { .beat_start = 3.931, .beat_end = 3.934,
      .carrier_hz = 101.49, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 3, .layer = {
        {  101.49,  3.931,  3.934, 0.345, 0.339 },   /* main */
        {  119.52,  0.000,  0.000, 0.091, 0.000 },   /* ref */
        {  316.02,  3.526,  3.526, 0.000, 0.000 },   /* high */
      } },
    /*  11:30 */
    { .beat_start = 3.934, .beat_end = 3.920,
      .carrier_hz = 101.49, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 3, .layer = {
        {  101.49,  3.934,  3.920, 0.339, 0.328 },   /* main */
        {  119.52,  0.000,  0.001, 0.000, 0.090 },   /* ref */
        {  316.02,  3.526,  3.526, 0.000, 0.000 },   /* high */
      } },
    /*  12:00 */
    { .beat_start = 3.920, .beat_end = 3.865,
      .carrier_hz = 101.49, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 3, .layer = {
        {  101.49,  3.920,  3.865, 0.328, 0.346 },   /* main */
        {  119.52,  0.001,  0.001, 0.090, 0.000 },   /* ref */
        {  316.02,  3.526,  3.526, 0.000, 0.000 },   /* high */
      } },
    /*  12:30 */
    { .beat_start = 3.865, .beat_end = 3.880,
      .carrier_hz = 101.49, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 3, .layer = {
        {  101.49,  3.865,  3.880, 0.346, 0.350 },   /* main */
        {  119.52,  0.001,  0.000, 0.000, 0.088 },   /* ref */
        {  316.02,  3.526,  3.526, 0.000, 0.000 },   /* high */
      } },
    /*  13:00 */
    { .beat_start = 3.880, .beat_end = 3.907,
      .carrier_hz = 101.49, .seconds = 60, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 3, .layer = {
        {  101.49,  3.880,  3.907, 0.350, 0.344 },   /* main */
        {  119.52,  0.000,  0.000, 0.088, 0.000 },   /* ref */
        {  316.02,  3.526,  3.526, 0.000, 0.000 },   /* high */
      } },
    /*  14:00 */
    { .beat_start = 3.907, .beat_end = 3.870,
      .carrier_hz = 101.49, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 3, .layer = {
        {  101.49,  3.907,  3.870, 0.344, 0.347 },   /* main */
        {  119.52,  0.000,  0.000, 0.000, 0.095 },   /* ref */
        {  316.02,  3.526,  3.526, 0.000, 0.000 },   /* high */
      } },
    /*  14:30 */
    { .beat_start = 3.870, .beat_end = 3.864,
      .carrier_hz = 101.49, .seconds = 60, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 3, .layer = {
        {  101.49,  3.870,  3.864, 0.347, 0.357 },   /* main */
        {  119.52,  0.000,  0.000, 0.095, 0.000 },   /* ref */
        {  316.02,  3.526,  3.526, 0.000, 0.000 },   /* high */
      } },
    /*  15:30 */
    { .beat_start = 3.864, .beat_end = 3.859,
      .carrier_hz = 101.49, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 3, .layer = {
        {  101.49,  3.864,  3.859, 0.357, 0.347 },   /* main */
        {  119.52,  0.000,  0.000, 0.000, 0.085 },   /* ref */
        {  316.02,  3.526,  3.526, 0.000, 0.000 },   /* high */
      } },
    /*  16:00 */
    { .beat_start = 3.859, .beat_end = 3.811,
      .carrier_hz = 101.49, .seconds = 210, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 3, .layer = {
        {  101.49,  3.859,  3.811, 0.347, 0.344 },   /* main */
        {  119.52,  0.000,  0.000, 0.085, 0.000 },   /* ref */
        {  316.02,  3.526,  3.526, 0.000, 0.000 },   /* high */
      } },
    /*  19:30 */
    { .beat_start = 3.811, .beat_end = 3.828,
      .carrier_hz = 101.49, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 3, .layer = {
        {  101.49,  3.811,  3.828, 0.344, 0.342 },   /* main */
        {  119.52,  0.000,  0.000, 0.000, 0.090 },   /* ref */
        {  316.02,  3.526,  3.526, 0.000, 0.000 },   /* high */
      } },
    /*  20:00 */
    { .beat_start = 3.828, .beat_end = 3.890,
      .carrier_hz = 101.49, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 3, .layer = {
        {  101.49,  3.828,  3.890, 0.342, 0.371 },   /* main */
        {  119.52,  0.000,  0.000, 0.090, 0.000 },   /* ref */
        {  316.02,  3.526,  3.526, 0.000, 0.000 },   /* high */
      } },
    /*  20:30 */
    { .beat_start = 3.890, .beat_end = 3.900,
      .carrier_hz = 101.49, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 3, .layer = {
        {  101.49,  3.890,  3.900, 0.371, 0.346 },   /* main */
        {  119.52,  0.000,  0.000, 0.000, 0.087 },   /* ref */
        {  316.02,  3.526,  3.526, 0.000, 0.000 },   /* high */
      } },
    /*  21:00 */
    { .beat_start = 3.900, .beat_end = 3.879,
      .carrier_hz = 101.49, .seconds = 90, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 3, .layer = {
        {  101.49,  3.900,  3.879, 0.346, 0.340 },   /* main */
        {  119.52,  0.000,  0.000, 0.087, 0.000 },   /* ref */
        {  316.02,  3.526,  3.526, 0.000, 0.000 },   /* high */
      } },
    /*  22:30 */
    { .beat_start = 3.879, .beat_end = 3.876,
      .carrier_hz = 101.49, .seconds = 90, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 3, .layer = {
        {  101.49,  3.879,  3.876, 0.340, 0.350 },   /* main */
        {  119.52,  0.000,  0.001, 0.000, 0.100 },   /* ref */
        {  316.02,  3.526,  3.526, 0.000, 0.000 },   /* high */
      } },
    /*  24:00 */
    { .beat_start = 3.876, .beat_end = 3.868,
      .carrier_hz = 101.49, .seconds = 60, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 3, .layer = {
        {  101.49,  3.876,  3.868, 0.350, 0.351 },   /* main */
        {  119.52,  0.001,  0.001, 0.100, 0.000 },   /* ref */
        {  316.02,  3.526,  3.526, 0.000, 0.000 },   /* high */
      } },
    /*  25:00 */
    { .beat_start = 3.868, .beat_end = 3.873,
      .carrier_hz = 101.49, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 3, .layer = {
        {  101.49,  3.868,  3.873, 0.351, 0.343 },   /* main */
        {  119.52,  0.001,  0.000, 0.000, 0.088 },   /* ref */
        {  316.02,  3.526,  3.526, 0.000, 0.000 },   /* high */
      } },
    /*  25:30 */
    { .beat_start = 3.873, .beat_end = 3.873,
      .carrier_hz = 101.49, .seconds = 120, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 3, .layer = {
        {  101.49,  3.873,  3.873, 0.343, 0.000 },   /* main */
        {  119.52,  0.000,  0.000, 0.088, 0.000 },   /* ref */
        {  316.02,  3.526,  3.526, 0.000, 0.000 },   /* high */
      } },
    /*  27:30 */
    { .beat_start = 3.873, .beat_end = 3.875,
      .carrier_hz = 101.49, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 3, .layer = {
        {  101.49,  3.873,  3.875, 0.000, 0.342 },   /* main */
        {  119.52,  0.000,  0.001, 0.000, 0.080 },   /* ref */
        {  316.02,  3.526,  3.526, 0.000, 0.000 },   /* high */
      } },
    /*  28:00 */
    { .beat_start = 3.875, .beat_end = 3.869,
      .carrier_hz = 101.49, .seconds = 60, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 3, .layer = {
        {  101.49,  3.875,  3.869, 0.342, 0.345 },   /* main */
        {  119.52,  0.001,  0.001, 0.080, 0.000 },   /* ref */
        {  316.02,  3.526,  3.526, 0.000, 0.000 },   /* high */
      } },
    /*  29:00 */
    { .beat_start = 3.869, .beat_end = 3.863,
      .carrier_hz = 101.49, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 3, .layer = {
        {  101.49,  3.869,  3.863, 0.345, 0.342 },   /* main */
        {  119.52,  0.001,  1.378, 0.000, 0.245 },   /* ref */
        {  316.02,  3.526,  3.526, 0.000, 0.000 },   /* high */
      } },
    /*  29:30 */
    { .beat_start = 3.863, .beat_end = 3.866,
      .carrier_hz = 101.49, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 3, .layer = {
        {  101.49,  3.863,  3.866, 0.342, 0.156 },   /* main */
        {  119.52,  1.378,  1.383, 0.245, 0.323 },   /* ref */
        {  316.02,  3.526,  3.526, 0.000, 0.000 },   /* high */
      } },
    /*  30:00 */
    { .beat_start = 3.866, .beat_end = 3.866,
      .carrier_hz = 101.49, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 3, .layer = {
        {  101.49,  3.866,  3.866, 0.156, 0.000 },   /* main */
        {  119.52,  1.383,  1.375, 0.323, 0.331 },   /* ref */
        {  316.02,  3.526,  3.526, 0.000, 0.000 },   /* high */
      } },
    /*  30:30 */
    { .beat_start = 3.866, .beat_end = 3.866,
      .carrier_hz = 101.49, .seconds = 120, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 3, .layer = {
        {  101.49,  3.866,  3.866, 0.000, 0.000 },   /* main */
        {  119.52,  1.375,  3.157, 0.331, 0.299 },   /* ref */
        {  316.02,  3.526,  3.526, 0.000, 0.000 },   /* high */
      } },
    /*  32:30 */
    { .beat_start = 3.866, .beat_end = 3.866,
      .carrier_hz = 101.49, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 3, .layer = {
        {  101.49,  3.866,  3.866, 0.000, 0.000 },   /* main */
        {  119.52,  3.157,  3.157, 0.299, 0.000 },   /* ref */
        {  316.02,  3.526,  3.526, 0.000, 0.000 },   /* high */
      } },
    /*  33:00 */
    { .beat_start = 3.866, .beat_end = 3.866,
      .carrier_hz = 101.49, .seconds = 90, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 3, .layer = {
        {  101.49,  3.866,  3.866, 0.000, 0.000 },   /* main */
        {  119.52,  3.157,  0.000, 0.000, 0.015 },   /* ref */
        {  316.02,  3.526,  0.006, 0.000, 0.003 },   /* high */
      } },
    /*  34:30 */
    { .beat_start = 3.866, .beat_end = 3.866,
      .carrier_hz = 101.49, .seconds = 60, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 3, .layer = {
        {  101.49,  3.866,  3.866, 0.000, 0.000 },   /* main */
        {  119.52,  0.000,  0.022, 0.015, 0.000 },   /* ref */
        {  316.02,  0.006,  0.006, 0.003, 0.000 },   /* high */
      } },
    /*  35:30 */
    { .beat_start = 3.866, .beat_end = 3.866,
      .carrier_hz = 101.49, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 3, .layer = {
        {  101.49,  3.866,  3.866, 0.000, 0.000 },   /* main */
        {  119.52,  0.022,  0.022, 0.000, 0.000 },   /* ref */
        {  316.02,  0.006,  0.006, 0.000, 0.000 },   /* high */
      } },
};
/* 38 segments, layers: main, ref, high */

/* Practice 5: measured from the recording, 2224s, 4 layers. */
const en_prog_seg_t en_meas_practice_5[] = {
    /*   0:00 */
    { .beat_start = 4.069, .beat_end = 4.069,
      .carrier_hz = 101.71, .seconds = 60, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  101.71,  4.069,  4.069, 0.000, 0.346 },   /* main */
        {  300.21,  0.035,  0.035, 0.000, 0.000 },   /* high */
        {  114.11,  1.699,  1.699, 0.000, 0.000 },   /* ref */
        {   60.28,  1.173,  1.173, 0.000, 0.000 },   /* sub */
      } },
    /*   1:00 */
    { .beat_start = 4.069, .beat_end = 4.069,
      .carrier_hz = 101.71, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  101.71,  4.069,  4.069, 0.346, 0.000 },   /* main */
        {  300.21,  0.035,  0.035, 0.000, 0.000 },   /* high */
        {  114.11,  1.699,  1.699, 0.000, 0.000 },   /* ref */
        {   60.28,  1.173,  1.173, 0.000, 0.000 },   /* sub */
      } },
    /*   1:30 */
    { .beat_start = 4.069, .beat_end = 4.087,
      .carrier_hz = 101.71, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  101.71,  4.069,  4.087, 0.000, 0.424 },   /* main */
        {  300.21,  0.035,  0.035, 0.000, 0.000 },   /* high */
        {  114.11,  1.699,  1.699, 0.000, 0.000 },   /* ref */
        {   60.28,  1.173,  1.173, 0.000, 0.000 },   /* sub */
      } },
    /*   2:00 */
    { .beat_start = 4.087, .beat_end = 4.107,
      .carrier_hz = 101.71, .seconds = 150, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  101.71,  4.087,  4.107, 0.424, 0.502 },   /* main */
        {  300.21,  0.035,  0.035, 0.000, 0.000 },   /* high */
        {  114.11,  1.699,  0.067, 0.000, 0.439 },   /* ref */
        {   60.28,  1.173,  1.173, 0.000, 0.000 },   /* sub */
      } },
    /*   4:30 */
    { .beat_start = 4.107, .beat_end = 4.111,
      .carrier_hz = 101.71, .seconds = 90, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  101.71,  4.107,  4.111, 0.502, 0.506 },   /* main */
        {  300.21,  0.035,  0.035, 0.000, 0.121 },   /* high */
        {  114.11,  0.067,  0.011, 0.439, 0.200 },   /* ref */
        {   60.28,  1.173,  1.173, 0.000, 0.000 },   /* sub */
      } },
    /*   6:00 */
    { .beat_start = 4.111, .beat_end = 4.064,
      .carrier_hz = 101.71, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  101.71,  4.111,  4.064, 0.506, 0.806 },   /* main */
        {  300.21,  0.035,  0.035, 0.121, 0.000 },   /* high */
        {  114.11,  0.011,  0.019, 0.200, 0.505 },   /* ref */
        {   60.28,  1.173,  1.173, 0.000, 0.000 },   /* sub */
      } },
    /*   6:30 */
    { .beat_start = 4.064, .beat_end = 4.059,
      .carrier_hz = 101.71, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  101.71,  4.064,  4.059, 0.806, 0.905 },   /* main */
        {  300.21,  0.035,  3.849, 0.000, 0.480 },   /* high */
        {  114.11,  0.019,  0.019, 0.505, 0.000 },   /* ref */
        {   60.28,  1.173,  1.173, 0.000, 0.000 },   /* sub */
      } },
    /*   7:00 */
    { .beat_start = 4.059, .beat_end = 4.055,
      .carrier_hz = 101.71, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  101.71,  4.059,  4.055, 0.905, 0.910 },   /* main */
        {  300.21,  3.849,  3.855, 0.480, 0.603 },   /* high */
        {  114.11,  0.019,  0.019, 0.000, 0.000 },   /* ref */
        {   60.28,  1.173,  1.173, 0.000, 0.000 },   /* sub */
      } },
    /*   7:30 */
    { .beat_start = 4.055, .beat_end = 4.054,
      .carrier_hz = 101.71, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  101.71,  4.055,  4.054, 0.910, 0.900 },   /* main */
        {  300.21,  3.855,  3.857, 0.603, 0.803 },   /* high */
        {  114.11,  0.019,  0.019, 0.000, 0.000 },   /* ref */
        {   60.28,  1.173,  1.173, 0.000, 0.000 },   /* sub */
      } },
    /*   8:00 */
    { .beat_start = 4.054, .beat_end = 4.047,
      .carrier_hz = 101.71, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  101.71,  4.054,  4.047, 0.900, 0.910 },   /* main */
        {  300.21,  3.857,  3.844, 0.803, 0.981 },   /* high */
        {  114.11,  0.019,  0.019, 0.000, 0.000 },   /* ref */
        {   60.28,  1.173,  1.173, 0.000, 0.000 },   /* sub */
      } },
    /*   8:30 */
    { .beat_start = 4.047, .beat_end = 4.037,
      .carrier_hz = 101.71, .seconds = 90, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  101.71,  4.047,  4.037, 0.910, 0.509 },   /* main */
        {  300.21,  3.844,  3.850, 0.981, 0.982 },   /* high */
        {  114.11,  0.019,  0.019, 0.000, 0.000 },   /* ref */
        {   60.28,  1.173,  1.173, 0.000, 0.000 },   /* sub */
      } },
    /*  10:00 */
    { .beat_start = 4.037, .beat_end = 4.036,
      .carrier_hz = 101.71, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  101.71,  4.037,  4.036, 0.509, 0.437 },   /* main */
        {  300.21,  3.850,  3.845, 0.982, 0.663 },   /* high */
        {  114.11,  0.019,  0.019, 0.000, 0.000 },   /* ref */
        {   60.28,  1.173,  1.173, 0.000, 0.000 },   /* sub */
      } },
    /*  10:30 */
    { .beat_start = 4.036, .beat_end = 3.891,
      .carrier_hz = 101.71, .seconds = 690, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  101.71,  4.036,  3.891, 0.437, 0.425 },   /* main */
        {  300.21,  3.845,  3.781, 0.663, 0.519 },   /* high */
        {  114.11,  0.019,  0.019, 0.000, 0.000 },   /* ref */
        {   60.28,  1.173,  1.173, 0.000, 0.000 },   /* sub */
      } },
    /*  22:00 */
    { .beat_start = 3.891, .beat_end = 3.886,
      .carrier_hz = 101.71, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  101.71,  3.891,  3.886, 0.425, 0.430 },   /* main */
        {  300.21,  3.781,  3.781, 0.519, 0.000 },   /* high */
        {  114.11,  0.019,  0.019, 0.000, 0.000 },   /* ref */
        {   60.28,  1.173,  1.173, 0.000, 0.000 },   /* sub */
      } },
    /*  22:30 */
    { .beat_start = 3.886, .beat_end = 3.886,
      .carrier_hz = 101.71, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  101.71,  3.886,  3.886, 0.430, 0.000 },   /* main */
        {  300.21,  3.781,  3.781, 0.000, 0.000 },   /* high */
        {  114.11,  0.019,  1.361, 0.000, 0.526 },   /* ref */
        {   60.28,  1.173,  1.173, 0.000, 0.000 },   /* sub */
      } },
    /*  23:00 */
    { .beat_start = 3.886, .beat_end = 3.886,
      .carrier_hz = 101.71, .seconds = 60, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  101.71,  3.886,  3.886, 0.000, 0.000 },   /* main */
        {  300.21,  3.781,  3.781, 0.000, 0.000 },   /* high */
        {  114.11,  1.361,  1.370, 0.526, 0.560 },   /* ref */
        {   60.28,  1.173,  1.180, 0.000, 0.374 },   /* sub */
      } },
    /*  24:00 */
    { .beat_start = 3.886, .beat_end = 3.886,
      .carrier_hz = 101.71, .seconds = 660, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  101.71,  3.886,  3.886, 0.000, 0.000 },   /* main */
        {  300.21,  3.781,  3.781, 0.000, 0.000 },   /* high */
        {  114.11,  1.370,  1.369, 0.560, 0.347 },   /* ref */
        {   60.28,  1.180,  1.182, 0.374, 0.264 },   /* sub */
      } },
    /*  35:00 */
    { .beat_start = 3.886, .beat_end = 3.886,
      .carrier_hz = 101.71, .seconds = 60, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  101.71,  3.886,  3.886, 0.000, 0.000 },   /* main */
        {  300.21,  3.781,  3.781, 0.000, 0.000 },   /* high */
        {  114.11,  1.369,  1.376, 0.347, 0.213 },   /* ref */
        {   60.28,  1.182,  1.184, 0.264, 0.154 },   /* sub */
      } },
    /*  36:00 */
    { .beat_start = 3.886, .beat_end = 3.886,
      .carrier_hz = 101.71, .seconds = 60, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 4, .layer = {
        {  101.71,  3.886,  3.886, 0.000, 0.000 },   /* main */
        {  300.21,  3.781,  3.781, 0.000, 0.000 },   /* high */
        {  114.11,  1.376,  1.376, 0.213, 0.213 },   /* ref */
        {   60.28,  1.184,  1.184, 0.154, 0.154 },   /* sub */
      } },
};
/* 19 segments, layers: main, high, ref, sub */

/* Practice 6: measured from the recording, 2018s, 3 layers. */
const en_prog_seg_t en_meas_practice_6[] = {
    /*   0:00 */
    { .beat_start = 3.871, .beat_end = 3.871,
      .carrier_hz = 300.50, .seconds = 90, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 3, .layer = {
        {  300.50,  3.871,  3.871, 0.000, 0.000 },   /* high */
        {  101.95,  4.088,  4.088, 0.000, 0.163 },   /* main */
        {  111.09,  0.023,  0.023, 0.000, 0.000 },   /* ref */
      } },
    /*   1:30 */
    { .beat_start = 3.871, .beat_end = 3.871,
      .carrier_hz = 300.50, .seconds = 180, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 3, .layer = {
        {  300.50,  3.871,  3.871, 0.000, 0.000 },   /* high */
        {  101.95,  4.088,  4.128, 0.163, 0.221 },   /* main */
        {  111.09,  0.023,  0.023, 0.000, 0.153 },   /* ref */
      } },
    /*   4:30 */
    { .beat_start = 3.871, .beat_end = 3.871,
      .carrier_hz = 300.50, .seconds = 60, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 3, .layer = {
        {  300.50,  3.871,  3.871, 0.000, 0.000 },   /* high */
        {  101.95,  4.128,  4.106, 0.221, 0.439 },   /* main */
        {  111.09,  0.023,  0.485, 0.153, 0.221 },   /* ref */
      } },
    /*   5:30 */
    { .beat_start = 3.871, .beat_end = 3.871,
      .carrier_hz = 300.50, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 3, .layer = {
        {  300.50,  3.871,  3.871, 0.000, 0.000 },   /* high */
        {  101.95,  4.106,  4.092, 0.439, 0.438 },   /* main */
        {  111.09,  0.485,  1.247, 0.221, 0.099 },   /* ref */
      } },
    /*   6:00 */
    { .beat_start = 3.871, .beat_end = 3.863,
      .carrier_hz = 300.50, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 3, .layer = {
        {  300.50,  3.871,  3.863, 0.000, 0.146 },   /* high */
        {  101.95,  4.092,  4.075, 0.438, 0.435 },   /* main */
        {  111.09,  1.247,  1.247, 0.099, 0.000 },   /* ref */
      } },
    /*   6:30 */
    { .beat_start = 3.863, .beat_end = 3.863,
      .carrier_hz = 300.50, .seconds = 60, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 3, .layer = {
        {  300.50,  3.863,  3.863, 0.146, 0.392 },   /* high */
        {  101.95,  4.075,  4.064, 0.435, 0.310 },   /* main */
        {  111.09,  1.247,  1.247, 0.000, 0.000 },   /* ref */
      } },
    /*   7:30 */
    { .beat_start = 3.863, .beat_end = 3.860,
      .carrier_hz = 300.50, .seconds = 270, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 3, .layer = {
        {  300.50,  3.863,  3.860, 0.392, 0.260 },   /* high */
        {  101.95,  4.064,  4.029, 0.310, 0.200 },   /* main */
        {  111.09,  1.247,  1.247, 0.000, 0.000 },   /* ref */
      } },
    /*  12:00 */
    { .beat_start = 3.860, .beat_end = 3.743,
      .carrier_hz = 300.50, .seconds = 630, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 3, .layer = {
        {  300.50,  3.860,  3.743, 0.260, 0.271 },   /* high */
        {  101.95,  4.029,  4.029, 0.200, 0.000 },   /* main */
        {  111.09,  1.247,  1.247, 0.000, 0.000 },   /* ref */
      } },
    /*  22:30 */
    { .beat_start = 3.743, .beat_end = 15.470,
      .carrier_hz = 300.50, .seconds = 570, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 3, .layer = {
        {  300.50,  3.743, 15.470, 0.271, 0.079 },   /* high */
        {  101.95,  4.029,  4.029, 0.000, 0.000 },   /* main */
        {  111.09,  1.247,  1.247, 0.000, 0.000 },   /* ref */
      } },
    /*  32:00 */
    { .beat_start = 15.470, .beat_end = 15.382,
      .carrier_hz = 300.50, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 3, .layer = {
        {  300.50, 15.470, 15.382, 0.079, 0.305 },   /* high */
        {  101.95,  4.029,  4.029, 0.000, 0.000 },   /* main */
        {  111.09,  1.247,  1.247, 0.000, 0.000 },   /* ref */
      } },
    /*  32:30 */
    { .beat_start = 15.382, .beat_end = 15.363,
      .carrier_hz = 300.50, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 3, .layer = {
        {  300.50, 15.382, 15.363, 0.305, 0.760 },   /* high */
        {  101.95,  4.029,  4.029, 0.000, 0.000 },   /* main */
        {  111.09,  1.247,  0.001, 0.000, 0.004 },   /* ref */
      } },
    /*  33:00 */
    { .beat_start = 15.363, .beat_end = 15.363,
      .carrier_hz = 300.50, .seconds = 30, .noise = EN_NOISE_NONE,
      .noise_level = 0.0, .interp = EN_INTERP_SMOOTH,
      .layers = 3, .layer = {
        {  300.50, 15.363, 15.363, 0.760, 0.760 },   /* high */
        {  101.95,  4.029,  4.029, 0.000, 0.000 },   /* main */
        {  111.09,  0.001,  0.001, 0.004, 0.004 },   /* ref */
      } },
};
/* 12 segments, layers: high, main, ref */
