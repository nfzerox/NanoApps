/*
 * progfile.h — multi-layer programs loaded from a file rather than compiled in.
 *
 * The measured programs are long: thirty-odd segments each, three to five
 * layers, and there are thirty-six of them. Compiled into the binary that is
 * about four hundred kilobytes of tables on top of a five hundred kilobyte
 * app, for material most people will never play all of. Held as a file it is
 * a hundred kilobytes on the volume and nothing in flash, and a new one is a
 * file drop rather than a rebuild.
 *
 * The format is line-based and scannable with a pointer - no allocation, no
 * stdio, no strtod - because that is what core/ is allowed to be:
 *
 *     # comments and blank lines are ignored
 *     @<name>|<total seconds>
 *     C<hz>,<hz>,...                       one carrier per layer
 *     S<seconds>|<b0>,<b1>,<g0>,<g1>|...   one group per layer, in C's order
 *     S...
 *
 * `b0`/`b1` are the beat at the start and end of the segment and `g0`/`g1` its
 * gain, so a layer fading in over a segment is written as it reads. A layer
 * that is silent through a segment is a group of zeros and is kept, not
 * omitted: the measurement says that layer was not sounding then, and dropping
 * it would make the layer order shift under the carrier list.
 */

#ifndef ENTRAIN_PROGFILE_H
#define ENTRAIN_PROGFILE_H

#include <stdbool.h>
#include <stdint.h>

#include "program.h"

/* The longest program the loader will hold. Thirty-six measured programs run
   to fifty-one segments at their longest; this is sized above the material
   with room, and a program past it is refused rather than truncated. */
#define EN_PROGFILE_MAX_SEGS 128
#define EN_PROGFILE_NAME_MAX 48

typedef struct {
    char          name[EN_PROGFILE_NAME_MAX + 1];
    uint32_t      seconds;        /* as recorded in the file, for the list */
    uint8_t       layers;
    int           n_segs;
    en_prog_seg_t segs[EN_PROGFILE_MAX_SEGS];
} en_progfile_t;

/* How many programs the buffer holds. Cheap: it counts '@' at line starts and
   parses nothing. */
int en_progfile_count(const char *text, uint32_t len);

/* Name and length of the `index`th, without parsing its segments - which is
   what building a list of thirty-six needs, and all it needs. */
bool en_progfile_head(const char *text, uint32_t len, int index,
                      char *name_out, int name_cap, uint32_t *seconds_out);

/* Parse the `index`th in full. Returns false on a malformed entry, one that
   needs more segments than EN_PROGFILE_MAX_SEGS, or more layers than
   EN_PROG_MAX_LAYERS - never a partial result. */
bool en_progfile_load(const char *text, uint32_t len, int index,
                      en_progfile_t *out);

#endif /* ENTRAIN_PROGFILE_H */
