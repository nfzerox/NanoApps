/*
 * tuner.h — what Radio+ needs from a tuner, on any platform.
 *
 * Two implementations sit behind this:
 *
 *   Linux   talks to the bcm2078-bt driver through its sysfs interface. The
 *           driver owns hci0 and sends FM_RDS_Command itself; nothing above it
 *           goes near the UART, which is the driver's job and not ours.
 *
 *   RetailOS talks to the OS's own tuner, and for now only through operations
 *           the SDK can offer safely. Raw register access is deliberately not
 *           part of the RetailOS build: issuing HCI alongside a running OS
 *           Bluetooth stack means our command completions and the OS's share
 *           one event stream, and that race is unresolved. Writes might be
 *           fine; reads are not established. Until it is settled on hardware,
 *           the RetailOS backend reports raw access unavailable rather than
 *           doing something clever with somebody else's transport.
 *
 * So en_tuner_can_raw() is not a capability flag in the ordinary sense. It is
 * the boundary between what is proven and what is not, and the UI is expected
 * to respect it by hiding the register explorer rather than letting it fail.
 */

#ifndef RADIOPLUS_TUNER_H
#define RADIOPLUS_TUNER_H

#include <stdbool.h>
#include <stdint.h>

#include "../core/rds.h"
#include "../core/region.h"

typedef enum {
    EN_TUNER_OK = 0,
    EN_TUNER_NO_DEVICE,     /* the driver did not bind, or there is no tuner */
    EN_TUNER_NOT_READY,     /* present, but the transport is down */
    EN_TUNER_UNSUPPORTED,   /* this platform cannot do that operation */
    EN_TUNER_FAILED
} en_tuner_err_t;

typedef struct {
    uint32_t khz;
    uint8_t  rssi;
    int8_t   snr;
    bool     stereo;
    bool     powered;
} en_tuner_state_t;

/* Bring the backend up. Safe to call twice. */
en_tuner_err_t en_tuner_init(void);
void           en_tuner_shutdown(void);

/* A short human-readable description of the backend, for the settings screen -
   which driver, which device, so a support question has an answer. */
const char *en_tuner_backend(void);

en_tuner_err_t en_tuner_power(bool on);

/*
 * Squelch, at the tuner rather than anywhere downstream.
 *
 * MANUAL_MUTE is the chip's own mute bit in I2C_FM_AUDIO_CTRL, and it is the
 * right place for this: the IIS2 side has no gain stage of its own, and muting
 * by closing the capture PCM would stop the port's clock and take RDS with it -
 * so the one thing worth keeping during a sweep is exactly the thing that
 * method throws away.
 *
 * There is deliberately no volume here. No FM volume register appears anywhere
 * in the recovered FM_RDS_Command map; level on this path is the codec's
 * playback volume once the audio reaches the headphones, which is already a
 * control on this card. A software gain would be a second volume corresponding
 * to nothing.
 *
 * en_tuner_muted() returns 1 or 0, or negative if the question cannot be put -
 * which is not the same as unmuted and should not be drawn as though it were.
 */
en_tuner_err_t en_tuner_mute(bool on);
int            en_tuner_muted(void);

en_tuner_err_t en_tuner_tune(uint32_t khz);
en_tuner_err_t en_tuner_seek(bool up);
en_tuner_err_t en_tuner_state(en_tuner_state_t *out);

/* Apply a region: band edges, spacing, de-emphasis. The RDS/RBDS half of a
   region is a decoder setting rather than a tuner one and is applied by the
   caller to its en_rds_t. */
en_tuner_err_t en_tuner_set_region(const en_region_t *rg);

/* Turn the RDS decoder on, and drain whatever has arrived into `r`.
   en_tuner_rds_poll() returns the number of groups fed, which is worth showing:
   groups arriving but nothing decoding is a different fault from no signal. */
en_tuner_err_t en_tuner_rds_enable(bool on);

/*
 * Drain whatever RDS has arrived into `r`, and optionally hand back the raw
 * groups as well.
 *
 * `groups`, `valid` and `max` may be NULL/0 when the caller only wants the
 * decoded state. A recorder wants the raw blocks too: they are what lets a
 * recording be decoded again later by a better decoder, which matters while
 * the FIFO framing is still unconfirmed.
 *
 * Returns the number of groups fed, which is worth showing on its own - groups
 * arriving but nothing decoding is a different fault from no signal.
 */
int en_tuner_rds_poll(en_rds_t *r, uint16_t (*groups)[4], uint8_t *valid,
                      uint8_t max);

/*
 * Raw register access, for the advanced screens.
 *
 * Available only where issuing commands is known to be safe - see the note at
 * the top. Callers must check en_tuner_can_raw() and present the register
 * explorer only when it is true.
 */
bool           en_tuner_can_raw(void);
en_tuner_err_t en_tuner_reg_read(uint8_t addr, uint8_t *buf, uint8_t len);
en_tuner_err_t en_tuner_reg_write(uint8_t addr, const uint8_t *buf, uint8_t len);

const char *en_tuner_strerror(en_tuner_err_t e);

#endif /* RADIOPLUS_TUNER_H */
