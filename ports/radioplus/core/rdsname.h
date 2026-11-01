/*
 * rdsname.h — what the numbers in an RDS group actually mean.
 *
 * The decoder in rds.c turns groups into state. This turns that state into
 * words: which group type a count belongs to, what a programme identification
 * says about where a station is, which of the decoder-identification bits are
 * set, and what an Open Data Application identifier refers to.
 *
 * Kept apart from rds.c on purpose. That file has to be right about bits and
 * is tested against hand-built groups; this one is a pile of lookup tables
 * transcribed from IEC 62106 and NRSC-4-B, and a wrong string here is a
 * cosmetic bug where a wrong shift there is a corrupt decode. Different risk,
 * different file.
 *
 * Nothing here allocates, reads a file, or knows what a screen is. The RDS
 * inspector is the only caller today; the point of the separation is that it
 * does not have to be the only one.
 *
 * Pure C99.
 */

#ifndef RADIOPLUS_RDSNAME_H
#define RADIOPLUS_RDSNAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The name of a group type, as the specification names it.
 *
 * `type` is 0..15 and `version_b` selects between the A and B forms, which are
 * genuinely different applications for most types rather than variants of one:
 * 4A is clock-time and 4B is an Open Data Application, and a receiver that
 * confused them would be reading a paging message as a date.
 *
 * Always returns something printable, including for a type that carries
 * nothing standardised.
 */
const char *en_rds_group_name(uint8_t type, bool version_b);

/* "0A", "15B" - the label a group count is listed under. `out` needs 4 bytes. */
void en_rds_group_label(uint8_t type, bool version_b, char *out, size_t n);

/*
 * The three fields inside a programme identification.
 *
 * PI is not an opaque number. The top nibble is a country code, the next is
 * how far the programme reaches, and the bottom byte distinguishes programmes
 * within that. A listener sees a station; an engineer sees whether two
 * frequencies are carrying the same programme, which is exactly this.
 */
uint8_t en_rds_pi_country(uint16_t pi);
uint8_t en_rds_pi_coverage(uint16_t pi);
uint8_t en_rds_pi_reference(uint16_t pi);

/* "Local", "National", "Regional 3"... from the coverage nibble. */
const char *en_rds_coverage_name(uint8_t code);

/*
 * The North American call sign a PI encodes, if it encodes one.
 *
 * NRSC-4-B assigns each four-letter call sign a number: K stations fill
 * 0x1000 upward and W stations 0x54A8 upward, each range being exactly 26^3
 * wide, so the three letters after the prefix come straight back out by
 * division. Writes at most five characters plus a terminator and returns true
 * when it wrote one.
 *
 * Returns false for anything outside those two ranges, which covers every RDS
 * country that is not the United States - Canadian PI codes are allocated
 * centrally and do not encode the call - and also the legacy three-letter
 * calls, which are an exception table rather than an algorithm and are not
 * transcribed here. False means "this PI does not spell a call sign", not
 * "this station has none".
 */
bool en_rds_callsign(uint16_t pi, char *out, size_t n);

/*
 * The decoder identification bits, spelled out into `out`.
 *
 * Four bits arriving one per group 0, addressed by the segment: segment 0
 * carries d3 and segment 3 carries d0, so the bit numbering below is by
 * segment, which is what rds.c stores.
 *
 *   bit 0  (d3)  dynamic programme type, rather than static
 *   bit 1  (d2)  compressed
 *   bit 2  (d1)  artificial head
 *   bit 3  (d0)  stereo, rather than mono
 *
 * Writes something for every value, including "Mono" for zero - an empty
 * string would read as "not decoded yet" when it means the opposite.
 */
void en_rds_di_text(uint8_t di, char *out, size_t n);

/*
 * The name of an Open Data Application, from the identifier announced in
 * group 3A, or NULL when it is not one this knows.
 *
 * Deliberately short. The registry is long, most of it is proprietary, and an
 * inspector that prints the hex for an application it cannot name is more
 * honest than one that guesses.
 */
const char *en_rds_oda_name(uint16_t aid);

#endif /* RADIOPLUS_RDSNAME_H */
