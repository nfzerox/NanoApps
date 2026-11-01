/*
 * fmcmd.h — encode and decode FM_RDS_Command (HCI vendor opcode 0xFC15).
 *
 * Every tuner register on this chip is reached through one HCI command. The
 * command carries an I2C address, a read/write flag, and either a payload to
 * write or a length to read. So "support every feature of the tuner" is one
 * encoder plus a table of register descriptions — see fmreg.h — rather than
 * forty hand-written special cases.
 *
 * Pure C99, no allocation, no I/O, no dependencies. Both platform backends hand
 * the bytes this produces to a different transport: on Linux the UART HCI path,
 * on RetailOS the OS's own send. Neither difference reaches this file, which is
 * why it can be tested on a desktop with no tuner attached.
 *
 * The framing is not guessed. The RetailOS bring-up sequence walks a table of
 * commands that decode to HCI_Reset (0x0C03), Read_Local_Version_Information
 * (0x1001) and Read_BD_ADDR (0x1009) — three standard opcodes in the canonical
 * order — which pins the layout as:
 *
 *     01 <ocf_lo> <ogf_hi> <plen> <parameters...>
 *
 * and the same code validates its own reply byte by byte, which pins the event:
 *
 *     04 0E <plen> 01 <ocf_lo> <ogf_hi> <status> <return parameters...>
 *
 * The command's own return parameters are Status, I2C_Address and
 * Read_Write_Mode before the register value, so a register read lands at offset
 * 9 of the event. See RADIO_NOTES.md.
 */

#ifndef RADIOPLUS_FMCMD_H
#define RADIOPLUS_FMCMD_H

#include <stdbool.h>
#include <stdint.h>

/* HCI vendor opcode for FM_RDS_Command: OGF 0x3F, OCF 0x015. */
#define EN_FM_OPCODE 0xFC15u

/* H4 packet indicators. */
#define EN_H4_COMMAND 0x01u
#define EN_H4_EVENT   0x04u

#define EN_HCI_COMMAND_COMPLETE 0x0Eu

/* Longest command this can produce: H4 type, opcode, length, address, mode,
   and a payload. The largest write in the register table is 8 bytes. */
#define EN_FM_CMD_MAX 16u

/* A read can ask for up to 250 bytes (the RDS FIFO), plus the event header and
   the three leading return parameters. */
#define EN_FM_EVENT_MAX 260u

/* Where a register value starts in a command-complete event: past the H4 type,
   event code, parameter length, packet count, two opcode bytes, and then the
   command's own Status, I2C_Address and Read_Write_Mode. */
#define EN_FM_VALUE_OFFSET 9u

typedef enum {
    EN_FM_WRITE = 0,
    EN_FM_READ  = 1
} en_fm_mode_t;

/* A decoded command-complete for FM_RDS_Command. `value` points into the
   caller's event buffer and is valid only as long as that buffer is. */
typedef struct {
    uint8_t        status;        /* 0 is success; HCI error code otherwise */
    uint8_t        addr;          /* the I2C address the chip echoed back */
    en_fm_mode_t   mode;
    const uint8_t *value;         /* NULL for a write, or when length is 0 */
    uint8_t        value_len;
} en_fm_reply_t;

/* Build a register write.
 *
 * Returns the packet length, or 0 if it would not fit or the payload is too
 * long to describe. `out` must have room for EN_FM_CMD_MAX. */
uint8_t en_fm_build_write(uint8_t addr, const uint8_t *payload, uint8_t len,
                          uint8_t *out, uint8_t out_cap);

/* Build a register read.
 *
 * `read_len` is how many bytes to ask for. Most registers have a fixed length
 * — en_fm_read_len() in fmreg.h gives it — but the RDS FIFO and the preset list
 * are variable and the caller chooses. Returns the packet length, or 0. */
uint8_t en_fm_build_read(uint8_t addr, uint8_t read_len,
                         uint8_t *out, uint8_t out_cap);

/* Decode a command-complete event.
 *
 * Returns false unless this really is a command-complete for 0xFC15 and the
 * lengths are self-consistent — a short or truncated event is rejected rather
 * than read past. A non-zero `status` is still reported as true, because the
 * chip refusing a command is a valid answer that the caller needs to see. */
bool en_fm_parse_event(const uint8_t *evt, uint16_t len, en_fm_reply_t *out);

/* Multi-byte register values are big-endian on this chip: the datasheet-order
 * registers (frequency, the search boundaries, the RDS matchers) are written
 * most significant byte first, unlike HCI's own little-endian parameters.
 *
 * Flagged rather than buried, because it is the one part of the encoding not
 * confirmed from the firmware, and if it turns out to be wrong every 16-bit
 * register is wrong together in an obvious way — a frequency that lands
 * byte-swapped rather than slightly off. Changing these two functions changes
 * it everywhere. */
void     en_fm_put_u16(uint8_t *p, uint16_t v);
uint16_t en_fm_get_u16(const uint8_t *p);

#endif /* RADIOPLUS_FMCMD_H */
