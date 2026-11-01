/*
 * tuner_linux.c — tuner.h through the bcm2078-bt driver.
 *
 * The driver owns hci0 and sends FM_RDS_Command (vendor opcode 0xFC15) itself.
 * Everything here goes through its sysfs interface and nothing touches the UART
 * or builds an HCI packet, which is the driver's job. If the tuner needs
 * something this file cannot express, the answer is a driver patch, not a
 * shortcut around it.
 *
 * Attributes, from bcm2078-bt.c:
 *
 *   fm_power   W   "1" / "0"
 *   fm_tune    W   kHz, decimal
 *   fm_seek    W   "up" / "down"
 *   fm_rssi    R   "%u"
 *   fm_snr     R   "%u"
 *   fm_rds     RW  write "1"/"0"; read gives on=, poll=, groups=, flag=,
 *                  pi=, pty=, ps=, rt=
 *   fm_reg     RW  write "r <reg> <len>" or "w <reg> <hex>"; read gives
 *                  "0x%02x: %*ph"
 *   hci_cmd    RW  arbitrary command, not used here
 *
 * Mute is the exception that does not appear in that list, because it is not in
 * sysfs at all: bcm2078-bt exports it and the nano7-audio machine driver
 * publishes it as the "FM Tuner Mute" control on card 0. That is the proper
 * interface for it - a mixer control is how everything else on this card is
 * turned down - so this file reaches it through the card's control interface
 * rather than asking for a sysfs attribute that deliberately does not exist.
 *
 * Note what fm_reg buys beyond convenience: register 0x80 is the RDS FIFO, so
 * reading it gives raw tuples that this app can decode itself with core/rds.c
 * rather than settling for the driver's summary. The driver's own pi, ps and rt
 * are then an independent oracle for that decoder - and in particular for the
 * one unverified thing in it, the FIFO tuple framing. Both are read, and a
 * disagreement is worth surfacing rather than hiding.
 */

#include "tuner.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "mixer.h"

#include "../core/fmreg.h"

static char s_dir[256];          /* the sysfs directory holding fm_power */
static bool s_ready;
static char s_desc[416];   /* holds the sysfs path plus a note */
static int  s_hci_state;

/* ---- sysfs plumbing ------------------------------------------------------ */

static int attr_path(char *out, size_t n, const char *name)
{
    int k = snprintf(out, n, "%s/%s", s_dir, name);
    return (k > 0 && (size_t)k < n) ? 0 : -1;
}

static en_tuner_err_t attr_write(const char *name, const char *value)
{
    char path[320];
    if (!s_ready || attr_path(path, sizeof path, name)) return EN_TUNER_FAILED;

    int fd = open(path, O_WRONLY);
    if (fd < 0) return (errno == ENOENT) ? EN_TUNER_UNSUPPORTED : EN_TUNER_FAILED;

    size_t len = strlen(value);
    ssize_t w = write(fd, value, len);
    int err = errno;
    close(fd);

    if (w == (ssize_t)len) return EN_TUNER_OK;

    /* The driver returns ENETDOWN when hci0 is not up, which is the single
       most likely failure and deserves its own answer rather than a generic
       one - the fix is to bring Bluetooth up, not to retry. */
    if (err == ENETDOWN || err == ENODEV) return EN_TUNER_NOT_READY;
    return EN_TUNER_FAILED;
}

static en_tuner_err_t attr_read(const char *name, char *buf, size_t n)
{
    char path[320];
    if (!s_ready || attr_path(path, sizeof path, name)) return EN_TUNER_FAILED;

    int fd = open(path, O_RDONLY);
    if (fd < 0) return (errno == ENOENT) ? EN_TUNER_UNSUPPORTED : EN_TUNER_FAILED;

    ssize_t r = read(fd, buf, n - 1);
    int err = errno;
    close(fd);

    if (r < 0) {
        if (err == ENETDOWN || err == ENODEV) return EN_TUNER_NOT_READY;
        return EN_TUNER_FAILED;
    }
    buf[r] = 0;
    return EN_TUNER_OK;
}

/* ---- bringing the controller up ------------------------------------------ */

/*
 * FM rides on hci0, and the driver answers ENETDOWN while it is down. The
 * device rootfs has no hciconfig, btmgmt or rfkill to bring it up with, so the
 * app does it directly - this is precisely what hciconfig hci0 up performs.
 *
 * Doing it here rather than in a launcher means the radio works when started by
 * hand, from a menu, or on boot, instead of only through one script.
 *
 * Two operations, and the difference between them is the whole of this file's
 * startup behaviour.
 *
 *   ASKING is an ioctl that returns immediately. HCIGETDEVINFO hands back the
 *   controller's flags and HCI_UP is bit zero of them.
 *
 *   RAISING is not. hci_bcm now carries HCI_QUIRK_NON_PERSISTENT_SETUP - it
 *   has to, or a controller left DOWN keeps a serdev open with its receiver
 *   enabled and buries the machine in interrupts - and the quirk means the
 *   kernel re-runs hdev->setup() on every open. That is a 31 KB patchram
 *   upload and a baud change, held under the device's request lock. HCIDEVUP
 *   blocks for seconds.
 *
 * These used to be one call, which is why this app reported "no Bluetooth"
 * while the boot script was still bringing the controller up: one attempt, at
 * the wrong instant, and the answer latched. Now the asking is cheap enough to
 * do several times a second and the raising happens on a thread of its own, so
 * the interface keeps drawing while the radio comes up underneath it.
 */
#define BT_AF_BLUETOOTH 31
#define BT_PROTO_HCI    1
#define BT_HCIDEVUP     _IOW('H', 201, int)
#define BT_HCIGETDEVINFO 0x800448d3u   /* _IOR('H', 211, int) as EABI packs it */

/*
 * Only the front of what the kernel returns is named. Everything past the
 * flags is left as padding rather than transcribed, because a field name
 * guessed wrong here would be worse than no name at all and nothing in this
 * app needs the rest.
 */
struct bt_dev_info {
    uint16_t dev_id;
    char     name[8];
    uint8_t  bdaddr[6];
    uint32_t flags;
    uint8_t  rest[220];
};

/* Why it could not be raised, for the message. These want different things
   done about them and lumping them together sends people to the wrong place. */
typedef enum {
    HCI_IS_UP = 0,
    HCI_NO_SOCKETS,      /* kernel has the device but no AF_BLUETOOTH */
    HCI_DOWN,            /* sockets exist; the controller is not running */
    HCI_ABSENT           /* no hci0 at all - the driver has not bound yet */
} hci_state_t;

/* One raise at a time, and never on the caller's thread. */
static pthread_mutex_t s_hci_lock = PTHREAD_MUTEX_INITIALIZER;
static bool s_raising;

/* 1 up, 0 down, negative if the question could not be put. */
static int hci_flags_up(int fd)
{
    struct bt_dev_info di;

    memset(&di, 0, sizeof di);
    di.dev_id = 0;
    if (ioctl(fd, BT_HCIGETDEVINFO, &di) < 0)
        return -1;
    return (di.flags & 1u) ? 1 : 0;
}

/* The cheap half. No side effects, safe to call as often as you like. */
static hci_state_t hci_state(void)
{
    int fd, up;

    if (access("/sys/class/bluetooth/hci0", F_OK) != 0)
        return HCI_ABSENT;

    fd = socket(BT_AF_BLUETOOTH, SOCK_RAW, BT_PROTO_HCI);
    if (fd < 0) {
        /* Observed on this device: hci0 is registered through serdev, but
           AF_BLUETOOTH is absent from /proc/net/protocols, so there is no
           socket to issue HCIDEVUP on and no userspace route to raise it. */
        return HCI_NO_SOCKETS;
    }

    up = hci_flags_up(fd);
    close(fd);

    /*
     * A controller that will not answer HCIGETDEVINFO is not up, whatever else
     * is wrong with it. Reported as DOWN rather than as its own state because
     * the thing to do about it is the same, and en_tuner_state will say more.
     */
    return up == 1 ? HCI_IS_UP : HCI_DOWN;
}

static void *hci_raise_worker(void *arg)
{
    int fd;

    (void)arg;

    fd = socket(BT_AF_BLUETOOTH, SOCK_RAW, BT_PROTO_HCI);
    if (fd >= 0) {
        /*
         * The return is not trusted. HCIDEVUP starts a bring-up that can still
         * fail partway through, and EALREADY means somebody else got there
         * first, which is success. The next hci_state() is the answer either
         * way, so this simply asks and does not interpret.
         */
        (void)ioctl(fd, BT_HCIDEVUP, 0);
        close(fd);
    }

    pthread_mutex_lock(&s_hci_lock);
    s_raising = false;
    pthread_mutex_unlock(&s_hci_lock);
    return NULL;
}

/*
 * Start a raise if one is warranted and none is already running.
 *
 * Detached rather than joined: nothing waits for this. The caller polls
 * hci_state() and finds out when it finds out, which is what keeps a
 * multi-second patchram upload off the thread that is drawing the screen.
 */
static void hci_raise_async(void)
{
    pthread_t th;
    pthread_attr_t at;

    pthread_mutex_lock(&s_hci_lock);
    if (s_raising) {
        pthread_mutex_unlock(&s_hci_lock);
        return;
    }
    s_raising = true;
    pthread_mutex_unlock(&s_hci_lock);

    if (pthread_attr_init(&at) != 0)
        goto give_up;
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &at, hci_raise_worker, NULL) != 0) {
        pthread_attr_destroy(&at);
        goto give_up;
    }
    pthread_attr_destroy(&at);
    return;

give_up:
    /* No thread means no raise, and leaving the flag set would mean never
       trying again. Better to be slow than to be permanently stuck. */
    pthread_mutex_lock(&s_hci_lock);
    s_raising = false;
    pthread_mutex_unlock(&s_hci_lock);
}

/* ---- finding the device -------------------------------------------------- */

/* The driver binds under /sys/devices somewhere that depends on how the
   companion device is described, so the directory is found by looking for the
   attribute rather than by hard-coding a path that a device-tree change would
   silently break. */
static bool find_dir(const char *root, int depth)
{
    if (depth > 4) return false;

    DIR *d = opendir(root);
    if (!d) return false;

    char path[256];
    struct dirent *e;
    bool found = false;

    while (!found && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        int k = snprintf(path, sizeof path, "%s/%s", root, e->d_name);
        if (k <= 0 || (size_t)k >= sizeof path) continue;

        if (strcmp(e->d_name, "fm_power") == 0) {
            snprintf(s_dir, sizeof s_dir, "%s", root);
            found = true;
            break;
        }
        /* Symlinks are skipped deliberately: /sys is full of them and following
           them turns this walk into a cycle. */
        if (e->d_type == DT_DIR) found = find_dir(path, depth + 1);
    }
    closedir(d);
    return found;
}

/*
 * Safe to call repeatedly, and meant to be.
 *
 * This used to return EN_TUNER_OK for every call after the first, on the
 * strength of having found the sysfs directory once - so a caller that
 * retried was told the tuner was fine while hci0 was still DOWN, and a caller
 * that did not retry latched the first answer forever. Both were wrong in the
 * same way: whether this app can talk to the tuner is not a fact about
 * startup, it is a fact about right now.
 *
 * Two things are therefore separate. Finding the driver's sysfs directory is
 * done once, because it does not move. The controller's state is re-read on
 * every call, because it changes - the boot script raises hci0 at about the
 * same moment this app starts, and which of them wins is a race nobody should
 * be relying on.
 *
 * Raising is asynchronous, so this returns promptly whatever it finds. A
 * caller that gets EN_TUNER_NOT_READY should call again shortly; the model
 * does, until it either works or a long time has passed.
 */
en_tuner_err_t en_tuner_init(void)
{
    hci_state_t st;
    const char *why;

    if (!s_ready) {
        if (!find_dir("/sys/devices", 0)) {
            /*
             * Not final either. The driver is built in and probes early, but
             * "early" is not "before this app", and a message that says it
             * did not bind is only true if it never does.
             */
            snprintf(s_desc, sizeof s_desc,
                     "waiting for bcm2078-bt (no fm_power in /sys yet)");
            return EN_TUNER_NO_DEVICE;
        }
        s_ready = true;
    }

    st = hci_state();

    /* Down and raiseable is the one case worth acting on. ABSENT means the
       Bluetooth driver has not bound, and NO_SOCKETS means there is no way to
       ask - neither is fixed by trying. */
    if (st == HCI_DOWN)
        hci_raise_async();

    switch (st) {
    case HCI_ABSENT:
        why = "  (no hci0 yet: the Bluetooth driver has not bound)";
        break;
    case HCI_NO_SOCKETS:
        why = "  (hci0 is down and this kernel has no AF_BLUETOOTH to raise it)";
        break;
    case HCI_DOWN:
        why = "  (hci0 is down; bringing it up)";
        break;
    default:
        why = "";
        break;
    }

    snprintf(s_desc, sizeof s_desc, "bcm2078-bt at %s%s", s_dir, why);

    s_hci_state = st;
    return st == HCI_IS_UP ? EN_TUNER_OK : EN_TUNER_NOT_READY;
}

void en_tuner_shutdown(void)
{
    if (s_ready) attr_write("fm_power", "0\n");
    s_ready = false;
}

const char *en_tuner_backend(void)
{
    return s_desc[0] ? s_desc : "bcm2078-bt (not probed)";
}

/* ---- basic control ------------------------------------------------------- */

en_tuner_err_t en_tuner_power(bool on)
{
    return attr_write("fm_power", on ? "1\n" : "0\n");
}

/* ---- mute ---------------------------------------------------------------- */

/*
 * "FM Tuner Mute" is a control on card 0 like any other, so this goes through
 * mixer.h rather than reaching for the card itself. Nothing is cached: a
 * control that is absent now can be present a moment later, and the card is
 * looked up by name every time for the same reason - an index shifts as
 * controls are added.
 *
 * Mute is pressed by hand or bracketed around a sweep, tens of times in a
 * session and not thousands, so the cost of asking each time is not worth
 * avoiding.
 */
en_tuner_err_t en_tuner_mute(bool on)
{
    /*
     * A card without the control is a machine driver that predates it, not a
     * failure of this app. Reported as unsupported so a caller can leave the
     * affordance off rather than showing one that does nothing.
     */
    if (!en_mix_present(EN_MIX_SW_FM_MUTE))
        return EN_TUNER_UNSUPPORTED;

    return en_mix_set_bool(EN_MIX_SW_FM_MUTE, on) ? EN_TUNER_OK
                                                  : EN_TUNER_FAILED;
}

int en_tuner_muted(void)
{
    return en_mix_get_bool(EN_MIX_SW_FM_MUTE);
}

/* ---- tuning -------------------------------------------------------------- */

en_tuner_err_t en_tuner_tune(uint32_t khz)
{
    char v[24];
    snprintf(v, sizeof v, "%u\n", khz);
    return attr_write("fm_tune", v);
}

en_tuner_err_t en_tuner_seek(bool up)
{
    return attr_write("fm_seek", up ? "up\n" : "down\n");
}

en_tuner_err_t en_tuner_state(en_tuner_state_t *out)
{
    if (!out) return EN_TUNER_FAILED;
    memset(out, 0, sizeof *out);

    char buf[64];
    en_tuner_err_t e = attr_read("fm_rssi", buf, sizeof buf);
    if (e != EN_TUNER_OK) return e;
    out->rssi = (uint8_t)strtoul(buf, 0, 10);

    if (attr_read("fm_snr", buf, sizeof buf) == EN_TUNER_OK)
        out->snr = (int8_t)strtol(buf, 0, 10);

    /* Stereo lives in the RDS flag register, which fm_rds reports. */
    if (attr_read("fm_rds", buf, sizeof buf) == EN_TUNER_OK) {
        const char *f = strstr(buf, "flag=0x");
        if (f) {
            unsigned long flag = strtoul(f + 7, 0, 16);
            out->stereo = (flag & 0x0040u) != 0;    /* STEREO ACTIVE */
        }
    }

    /* The driver has no attribute that reports the tuned frequency back, so
       the register is read directly rather than trusting a cached value that a
       seek would have invalidated. */
    uint8_t raw[2];
    if (en_tuner_reg_read(0x0A, raw, 2) == EN_TUNER_OK)
        out->khz = en_fm_reg_to_khz((uint16_t)((raw[0] << 8) | raw[1]));

    out->powered = true;
    return EN_TUNER_OK;
}

en_tuner_err_t en_tuner_set_region(const en_region_t *rg)
{
    if (!rg) return EN_TUNER_FAILED;

    /* The driver has no region attribute, so this is the register path: read
       the two registers that carry unrelated bits, let core/region build the
       writes, and send them. Doing it here rather than in the UI keeps the
       read-modify-write in one place. */
    uint8_t fm_ctrl = 0;
    uint16_t audio_ctrl = 0;
    uint8_t raw[2];

    if (en_tuner_reg_read(0x01, raw, 1) == EN_TUNER_OK) fm_ctrl = raw[0];
    if (en_tuner_reg_read(0x05, raw, 2) == EN_TUNER_OK)
        audio_ctrl = (uint16_t)((raw[0] << 8) | raw[1]);

    uint8_t cmds[EN_REGION_CMDS][EN_FM_CMD_MAX];
    uint8_t lens[EN_REGION_CMDS];
    uint8_t n = en_region_apply(rg, fm_ctrl, audio_ctrl, cmds, lens,
                                EN_REGION_CMDS);
    if (!n) return EN_TUNER_FAILED;

    /* en_region_apply builds whole HCI packets; the driver wants register
       writes, so the payload is lifted back out. Building the packet and then
       unpacking it looks redundant, but it keeps one description of what a
       region write is, and the RetailOS backend sends those same packets
       whole. */
    for (uint8_t i = 0; i < n; i++) {
        uint8_t addr = cmds[i][4];
        const uint8_t *payload = &cmds[i][6];
        uint8_t plen = (uint8_t)(lens[i] - 6);
        en_tuner_err_t e = en_tuner_reg_write(addr, payload, plen);
        if (e != EN_TUNER_OK) return e;
    }
    return EN_TUNER_OK;
}

/* ---- RDS ----------------------------------------------------------------- */

en_tuner_err_t en_tuner_rds_enable(bool on)
{
    return attr_write("fm_rds", on ? "1\n" : "0\n");
}

int en_tuner_rds_poll(en_rds_t *r, uint16_t (*out)[4], uint8_t *out_valid,
                      uint8_t out_max)
{
    if (!r) return -1;

    /* Reading fm_rds drains the FIFO into the driver's own decoder, so it has
       to happen before the raw read or there is nothing left to fetch. It also
       gives the cross-check below. */
    char summary[512];
    bool have_summary = attr_read("fm_rds", summary, sizeof summary) == EN_TUNER_OK;

    /* Register 0x80 is the RDS FIFO. Ask for a sensible bite rather than the
       250-byte maximum: at three bytes a tuple and four tuples a group, 96
       bytes is eight groups, which is more than arrives between polls. */
    uint8_t fifo[96];
    if (en_tuner_reg_read(0x80, fifo, sizeof fifo) != EN_TUNER_OK)
        return have_summary ? 0 : -1;

    uint16_t groups[8][4];
    uint8_t valid[8];
    uint8_t n = en_rds_unpack_tuples_unverified(fifo, sizeof fifo, groups,
                                                valid, 8);
    for (uint8_t i = 0; i < n; i++) {
        en_rds_group(r, groups[i], valid[i]);

        /* Copied out before decoding changes anything, so a recorder gets what
           arrived rather than what was made of it. */
        if (out && out_valid && i < out_max) {
            for (uint8_t k = 0; k < 4; k++) out[i][k] = groups[i][k];
            out_valid[i] = valid[i];
        }
    }

    /*
     * The driver decodes RDS too, and its answer is an independent check on
     * ours - most usefully on the FIFO tuple framing, which is the one thing in
     * core/rds.c that was guessed rather than read out of a specification.
     *
     * If the driver has a PI and we do not, or the two disagree, the framing is
     * wrong and everything decoded here is noise. That is worth knowing loudly
     * during bring-up rather than discovering through a station name that never
     * quite appears.
     */
    if (have_summary) {
        const char *p = strstr(summary, "pi=0x");
        if (p) {
            uint16_t driver_pi = (uint16_t)strtoul(p + 5, 0, 16);
            if (driver_pi && r->pi_valid && r->pi != driver_pi)
                fprintf(stderr,
                        "radioplus: RDS framing disagrees with the driver "
                        "(ours %04X, driver %04X) - "
                        "en_rds_unpack_tuples_unverified is wrong\n",
                        r->pi, driver_pi);
        }
    }
    return n;
}

/* ---- raw register access ------------------------------------------------- */

bool en_tuner_can_raw(void) { return true; }

en_tuner_err_t en_tuner_reg_read(uint8_t addr, uint8_t *buf, uint8_t len)
{
    if (!buf || !len) return EN_TUNER_FAILED;

    char req[32];
    snprintf(req, sizeof req, "r %02x %u\n", addr, len);
    en_tuner_err_t e = attr_write("fm_reg", req);
    if (e != EN_TUNER_OK) return e;

    /* The driver answers as "0x%02x: %*ph", which is space-separated hex. */
    char out[1024];
    e = attr_read("fm_reg", out, sizeof out);
    if (e != EN_TUNER_OK) return e;

    const char *p = strchr(out, ':');
    if (!p) return EN_TUNER_FAILED;      /* "no read pending" */
    p++;

    uint8_t got = 0;
    while (got < len) {
        while (*p == ' ') p++;
        if (!*p || *p == '\n') break;

        char *end;
        unsigned long v = strtoul(p, &end, 16);
        if (end == p) break;
        buf[got++] = (uint8_t)v;
        p = end;
    }
    return got == len ? EN_TUNER_OK : EN_TUNER_FAILED;
}

en_tuner_err_t en_tuner_reg_write(uint8_t addr, const uint8_t *buf, uint8_t len)
{
    if (!buf || !len || len > 8) return EN_TUNER_FAILED;

    /* The driver takes the value as one hex number, so the payload is written
       big-endian to match how the register table describes multi-byte values. */
    char req[64];
    int k = snprintf(req, sizeof req, "w %02x ", addr);
    for (uint8_t i = 0; i < len && k > 0 && (size_t)k < sizeof req - 3; i++)
        k += snprintf(req + k, sizeof req - (size_t)k, "%02x", buf[i]);
    snprintf(req + k, sizeof req - (size_t)k, "\n");

    return attr_write("fm_reg", req);
}

const char *en_tuner_strerror(en_tuner_err_t e)
{
    switch (e) {
    case EN_TUNER_OK:          return "ok";
    case EN_TUNER_NO_DEVICE:   return "no tuner found";
    case EN_TUNER_NOT_READY:
        /* The fix differs per case, so the message does too. This compared
           against a bare 1 before, which happened to be HCI_NO_SOCKETS and
           read as though it meant something else. */
        switch ((hci_state_t)s_hci_state) {
        case HCI_NO_SOCKETS:
            return "hci0 is down and this kernel has no Bluetooth sockets to "
                   "raise it. FM rides on hci0, so the tuner cannot answer "
                   "until the driver brings it up or the kernel gains "
                   "AF_BLUETOOTH.";
        case HCI_ABSENT:
            return "waiting for the Bluetooth driver; FM rides on hci0";
        default:
            return "bringing up hci0; FM rides on it";
        }
    case EN_TUNER_UNSUPPORTED: return "not supported on this platform";
    default:                   return "failed";
    }
}
