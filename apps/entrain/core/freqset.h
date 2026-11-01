/*
 * freqset.h — flat frequency programs loaded from a file.
 *
 * These are not entrainment timelines. Each one is a list of plain tones and a
 * dwell: play 273.435 Hz for three seconds, then 384.802 Hz for three seconds,
 * and so on to the end. There are several hundred of them and they are what
 * turns the device into a pocket signal generator with a list of presets —
 * which is the only claim made for them here. Nothing in this file, and
 * nothing in the shipped bundle, asserts that any of these frequencies does
 * anything to anybody.
 *
 * The format is one set per line, so a set can be found by counting newlines
 * and the whole bundle never has to be parsed to build a list:
 *
 *     # comments and blank lines are ignored
 *     <name>|<dwell seconds>|<hz>,<hz>,<hz>,...
 *
 * Held as a file rather than compiled in for the same reason as progfile: four
 * hundred sets of eighty steps is megabytes of tables for material most people
 * will never play, and a new set should be a line in a file rather than a
 * rebuild.
 */

#ifndef ENTRAIN_FREQSET_H
#define ENTRAIN_FREQSET_H

#include <stdbool.h>
#include <stdint.h>

#include "program.h"

/* The longest set the loader will expand. The shipped bundle's longest is
   eighty steps; this is sized above the material, and a set past it is
   refused rather than truncated - half a sweep is not the sweep. */
#define EN_FREQSET_MAX_STEPS 96
#define EN_FREQSET_NAME_MAX  63

/* How many sets the buffer holds. Counts lines and parses nothing. */
int en_freqset_count(const char *text, uint32_t len);

/* Name, step count and dwell of the `index`th, without reading its
   frequencies - which is what building a list of four hundred needs. */
bool en_freqset_head(const char *text, uint32_t len, int index,
                     char *name_out, int name_cap,
                     int *steps_out, uint32_t *dwell_out);

/* Expand the `index`th into one segment per step: carrier at the step's
   frequency, no beat, no ramp. Pass `dwell_s` non-zero to override the dwell
   recorded in the file. Returns segments written, or 0 if the entry is
   malformed or longer than `cap` - never a partial result.

   `nyquist_hz` is the output rate over two. Steps at or above it are dropped
   rather than rendered, because a tone above Nyquist does not come out as a
   quiet tone, it comes out as a different frequency entirely, and a signal
   generator that silently lies about what it is emitting is worse than one
   that skips a step. Pass 0 to disable the check. `dropped_out` is optional
   and reports how many were skipped so the caller can say so. */
int en_freqset_segs(const char *text, uint32_t len, int index,
                    uint32_t dwell_s, double nyquist_hz,
                    en_prog_seg_t *segs, int cap, int *dropped_out);

#endif /* ENTRAIN_FREQSET_H */
