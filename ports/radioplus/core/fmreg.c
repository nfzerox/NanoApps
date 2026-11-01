/*
 * fmreg.c — the FM_RDS_Command register map. See fmreg.h.
 *
 * Transcribed from the FM_RDS_Command specification rather than summarised.
 * Where the specification marks a read length TBD, so does this — a guessed
 * length is a malformed bus transaction, and the whole point of the table is
 * that nobody has to guess at the call site.
 */

#include "fmreg.h"

/* ---- shared bit and enum tables ------------------------------------------ */

static const en_fm_bit_t bits_system[] = {
    { 0x01, "FM on" },
    { 0x02, "RDS on" },
};

static const en_fm_bit_t bits_fm_ctrl[] = {
    { 0x01, "Band select" },
    { 0x02, "Stereo/mono auto select" },
    { 0x04, "Stereo/mono manual select" },
    { 0x08, "Stereo/mono blend/switch" },
    { 0x10, "Hi/Lo injection control" },
};

static const en_fm_bit_t bits_rds_ctrl[] = {
    { 0x01, "RBDS instead of RDS" },
    { 0x02, "Flush FIFO" },
};

static const en_fm_bit_t bits_audio_ctrl[] = {
    { 0x01, "RF mute" },
    { 0x02, "Manual mute" },
    { 0x04, "Zero-mute left" },
    { 0x08, "Zero-mute right" },
    { 0x10, "Route to DAC" },
    { 0x20, "Route to I2S" },
    { 0x40, "De-emphasis select" },
};

/* Used by both the flag register and its interrupt mask — the mask names the
   same conditions it enables. */
static const en_fm_bit_t bits_rds_flags[] = {
    { 0x0001, "Search/tune finished" },
    { 0x0002, "Search/tune failed" },
    { 0x0004, "RSSI low" },
    { 0x0008, "Carrier error high" },
    { 0x0010, "Audio pause" },
    { 0x0020, "Stereo detected" },
    { 0x0040, "Stereo active" },
    { 0x0200, "FIFO waterline" },
    { 0x0800, "Block B match" },
    { 0x1000, "Sync lost" },
    { 0x2000, "PI match" },
    { 0x4000, "RDS bit flag transition" },
};

static const en_fm_bit_t bits_audio_path[] = {
    { 0x01, "FM RX to audio FIFO" },
};

static const en_fm_bit_t bits_antenna_match[] = {
    { 0x01, "Auto config on" },
    { 0x02, "Start a tune" },
};

static const en_fm_enum_t vals_antenna[] = {
    { 0, "Internal" },
    { 1, "External" },
};

static const en_fm_enum_t vals_search_method[] = {
    { 0, "Normal" },
    { 1, "Preset" },
    { 2, "RSSI" },
};

uint8_t en_fm_ctrl_set_stereo(uint8_t ctrl, en_fm_stereo_t mode)
{
    /* Read-modify-write: bit 0 is the band and bit 4 is the injection side,
       and a stereo control that reset the band would be a very confusing
       stereo control. */
    ctrl &= (uint8_t)~(EN_FM_CTRL_AUTO | EN_FM_CTRL_MANUAL);
    switch (mode) {
    case EN_FM_STEREO_AUTO:   ctrl |= EN_FM_CTRL_AUTO;   break;
    case EN_FM_STEREO_STEREO: ctrl |= EN_FM_CTRL_MANUAL; break;
    case EN_FM_STEREO_MONO:   break;      /* manual, and manual means mono */
    }
    return ctrl;
}

en_fm_stereo_t en_fm_ctrl_stereo(uint8_t ctrl)
{
    if (ctrl & EN_FM_CTRL_AUTO) return EN_FM_STEREO_AUTO;
    return (ctrl & EN_FM_CTRL_MANUAL) ? EN_FM_STEREO_STEREO
                                      : EN_FM_STEREO_MONO;
}

/* ---- per-register field tables ------------------------------------------- */

#define WHOLE(w) ((uint8_t)((w) * 8u - 1u)), 0u

static const en_fm_field_t f_system[] = {
    { "Enable", "Turn the FM and RDS blocks on.", 0, 1, WHOLE(1),
      EN_FMF_BITMAP, 0, 0xFF, bits_system, 2, 0, 0 },
};

static const en_fm_field_t f_fm_ctrl[] = {
    { "Control", "Band, stereo handling and injection side.", 0, 1, WHOLE(1),
      EN_FMF_BITMAP, 0, 0xFF, bits_fm_ctrl, 5, 0, 0 },
};

static const en_fm_field_t f_rds_ctrl[] = {
    { "Control", "RDS or RBDS, and FIFO flush.", 0, 1, WHOLE(1),
      EN_FMF_BITMAP, 0, 0xFF, bits_rds_ctrl, 2, 0, 0 },
};

static const en_fm_field_t f_audio_pause[] = {
    { "Pause duration", "How long to pause audio when needed.",
      0, 1, 7, 4, 0, 0, 15, 0, 0, 0, 0 },
    { "RSSI threshold", "Pause audio below this RSSI.",
      0, 1, 3, 0, 0, 0, 15, 0, 0, 0, 0 },
};

static const en_fm_field_t f_audio_ctrl[] = {
    { "Audio bandwidth", "Narrower trades bandwidth for signal-to-noise.",
      0, 2, 15, 7, 0, 0, 3, 0, 0, 0, 0 },
    { "Flags", "Muting, routing and de-emphasis.",
      0, 2, 6, 0, EN_FMF_BITMAP, 0, 0x7F, bits_audio_ctrl, 7, 0, 0 },
};

static const en_fm_field_t f_search_ctrl[] = {
    { "Search up", "Direction: set to search upwards.",
      0, 1, 7, 7, 0, 0, 1, 0, 0, 0, 0 },
    { "RSSI threshold", "Signal strength a station must reach, in dB.",
      0, 1, 6, 0, 0, 0, 127, 0, 0, 0, 0 },
};

static const en_fm_field_t f_search_snr[] = {
    { "SNR threshold", "Signal-to-noise a station must reach during auto search.",
      0, 1, WHOLE(1), EN_FMF_SIGNED, -128, 127, 0, 0, 0, 0 },
};

static const en_fm_field_t f_tune_mode[] = {
    { "Mode", "Selects the search or tune function; clearing it stops the "
              "state machine, which must be stopped before starting anything "
              "new.",
      0, 1, WHOLE(1), 0, 0, 3, 0, 0, 0, 0 },
};

static const en_fm_field_t f_freq[] = {
    { "Frequency", "Start frequency for a search, or the direct tune target. "
                   "Takes effect when the tuner state machine is started.",
      0, 2, WHOLE(2), 0, 0, 65535, 0, 0, 0, 0 },
};

static const en_fm_field_t f_af_freq[] = {
    { "AF frequency", "Alternate-frequency jump target. Takes effect when the "
                      "tuner state machine is started.",
      0, 2, WHOLE(2), 0, 0, 65535, 0, 0, 0, 0 },
};

static const en_fm_field_t f_carrier[] = {
    { "Carrier error", "Error from the nominal IF, in kHz.",
      0, 1, WHOLE(1), EN_FMF_SIGNED, -128, 127, 0, 0, 0, 0 },
};

static const en_fm_field_t f_rssi[] = {
    { "RSSI", "Signal strength of the tuned channel.",
      0, 1, WHOLE(1), 0, 0, 255, 0, 0, 0, 0 },
};

static const en_fm_field_t f_snr[] = {
    { "SNR", "Signal-to-noise of the tuned channel.",
      0, 1, WHOLE(1), EN_FMF_SIGNED, -128, 127, 0, 0, 0, 0 },
};

static const en_fm_field_t f_rds_flag[] = {
    { "Flags", "Status bits. Reading the register clears them, and clears the "
               "matching mask bits with them.",
      0, 2, WHOLE(2), EN_FMF_BITMAP, 0, 0xFFFF, bits_rds_flags, 12, 0, 0 },
};

static const en_fm_field_t f_rds_mask[] = {
    { "Mask", "Which status bits are allowed to raise the FM interrupt.",
      0, 2, WHOLE(2), EN_FMF_BITMAP, 0, 0xFFFF, bits_rds_flags, 12, 0, 0 },
};

static const en_fm_field_t f_wline[] = {
    { "Waterline", "RDS FIFO depth, in tuples, that raises an interrupt.",
      0, 1, WHOLE(1), 0, 0, 255, 0, 0, 0, 0 },
};

static const en_fm_field_t f_blkb_match[] = {
    { "Block B match", "Value to match against RDS block B.",
      0, 2, WHOLE(2), 0, 0, 65535, 0, 0, 0, 0 },
};

static const en_fm_field_t f_blkb_mask[] = {
    { "Block B mask", "Which block B bits take part in the match. Zero "
                      "disables block B matching.",
      0, 2, WHOLE(2), 0, 0, 65535, 0, 0, 0, 0 },
};

static const en_fm_field_t f_pi_match[] = {
    { "PI match", "Programme identification code to match.",
      0, 2, WHOLE(2), 0, 0, 65535, 0, 0, 0, 0 },
};

static const en_fm_field_t f_pi_mask[] = {
    { "PI mask", "Which PI bits take part in the match. Zero disables PI "
                 "matching.",
      0, 2, WHOLE(2), 0, 0, 65535, 0, 0, 0, 0 },
};

static const en_fm_field_t f_slave[] = {
    { "Slave address", "Bits 0 to 6 are the address; the top bit enables I2C.",
      0, 1, WHOLE(1), 0, 0, 255, 0, 0, 0, 0 },
};

static const en_fm_field_t f_route_pcm[] = {
    { "Route to PCM", "With the top bit set, FM audio routes to PCM.",
      0, 1, WHOLE(1), 0, 0, 255, 0, 0, 0, 0 },
};

static const en_fm_field_t f_best_tune[] = {
    { "Best tune", "Bit 7 clear enables best tune, set disables it. On read, "
                   "bits 5 and 4 report an AF jump error: 1 RSSI low, "
                   "2 frequency offset high, 3 PI mismatch.",
      0, 1, WHOLE(1), 0, 0, 255, 0, 0, 0, 0 },
};

static const en_fm_field_t f_smute3[] = {
    { "Lowpass start SNR", "PSD estimate at which lowpass filtering begins.",
      0, 1, WHOLE(1), EN_FMF_SIGNED, -128, 127, 0, 0, 0, 0 },
    { "Lowpass start frequency", "Cutoff to start from once the threshold is "
                                 "reached.",
      1, 1, WHOLE(1), EN_FMF_SIGNED, -128, 127, 0, 0, 0, 0 },
    { "Lowpass step speed", "kHz the cutoff drops per step of PSD estimate.",
      2, 1, WHOLE(1), EN_FMF_SIGNED, -128, 127, 0, 0, 0, 0 },
    { "Lowpass 40 kHz SNR", "SNR at which the cutoff switches to 40 kHz.",
      3, 1, WHOLE(1), EN_FMF_SIGNED, -128, 127, 0, 0, 0, 0 },
    { "Lowpass 10 kHz SNR", "SNR at which the cutoff switches to 10 kHz.",
      4, 1, WHOLE(1), EN_FMF_SIGNED, -128, 127, 0, 0, 0, 0 },
};

static const en_fm_field_t f_features[] = {
    { "Features", "Bit map of FM features and their versions.",
      0, 4, WHOLE(4), 0, 0, 0x7FFFFFFF, 0, 0, 0, 0 },
};

static const en_fm_field_t f_prescan[] = {
    { "Prescan quality", "Carrier offset slope threshold.",
      0, 1, WHOLE(1), EN_FMF_SIGNED, -128, 127, 0, 0, 0, 0 },
};

static const en_fm_field_t f_audio_path[] = {
    { "Additional path", "Audio paths beyond the DAC and I2S routing in the "
                         "audio control register.",
      0, 1, WHOLE(1), EN_FMF_BITMAP, 0, 0xFF, bits_audio_path, 1, 0, 0 },
};

static const en_fm_field_t f_antenna_match[] = {
    { "Antenna matching", "Automatic RX antenna matching, and starting a tune.",
      0, 1, WHOLE(1), EN_FMF_BITMAP, 0, 0xFF, bits_antenna_match, 2, 0, 0 },
};

static const en_fm_field_t f_volume[] = {
    { "Volume", "FM receive volume.",
      0, 2, WHOLE(2), 0, 0, 256, 0, 0, 0, 0 },
};

static const en_fm_field_t f_blend[] = {
    { "Stereo start SNR", "Above this SNR the chip goes stereo, if RSSI allows.",
      0, 1, WHOLE(1), 0, 0, 63, 0, 0, 0, 0 },
    { "Stereo stop SNR", "Below this SNR the chip goes mono.",
      1, 1, WHOLE(1), 0, 0, 63, 0, 0, 0, 0 },
    { "Blend start RSSI", "RSSI at which blending starts, once the start SNR "
                          "is exceeded.",
      2, 1, WHOLE(1), EN_FMF_SIGNED, -128, 127, 0, 0, 0, 0 },
    { "Blend stop RSSI", "RSSI at which blending stops, while the stop SNR is "
                         "not yet exceeded.",
      3, 1, WHOLE(1), EN_FMF_SIGNED, -128, 127, 0, 0, 0, 0 },
    { "Soft mute start SNR", "Below this SNR the chip mutes progressively "
                             "towards the attenuation below.",
      4, 1, WHOLE(1), 0, 0, 63, 0, 0, 0, 0 },
    { "Soft mute attenuation", "Ultimate mute attenuation.",
      5, 1, WHOLE(1), EN_FMF_SIGNED, -128, 127, 0, 0, 0, 0 },
    { "Soft mute rate", "How fast muting happens.",
      6, 1, WHOLE(1), 0, 0, 63, 0, 0, 0, 0 },
    { "SNR offset", "Added to the SNR reading to get the real value.",
      7, 1, WHOLE(1), EN_FMF_SIGNED, -128, 127, 0, 0, 0, 0 },
};

static const en_fm_field_t f_antenna_sel[] = {
    { "Antenna", "Which aerial to receive on.",
      0, 1, WHOLE(1), EN_FMF_ENUM, 0, 1, 0, 0, vals_antenna, 2 },
};

static const en_fm_field_t f_boundary[] = {
    { "Upper boundary", "Top of the search range. Must be programmed together "
                        "with the lower boundary.",
      0, 2, WHOLE(2), 0, 0, 65535, 0, 0, 0, 0 },
    { "Lower boundary", "Bottom of the search range. Must be programmed "
                        "together with the upper boundary.",
      2, 2, WHOLE(2), 0, 0, 65535, 0, 0, 0, 0 },
};

static const en_fm_field_t f_search_method[] = {
    { "Method", "How the search picks candidates.",
      0, 1, WHOLE(1), EN_FMF_ENUM, 0, 2, 0, 0, vals_search_method, 3 },
};

static const en_fm_field_t f_search_step[] = {
    { "Step", "Channel spacing used while searching.",
      0, 2, WHOLE(2), 0, 0, 65535, 0, 0, 0, 0 },
};

static const en_fm_field_t f_preset_max[] = {
    { "Preset count", "How many preset channels the chip holds.",
      0, 1, WHOLE(1), 0, 0, 255, 0, 0, 0, 0 },
};

/* ---- the table ----------------------------------------------------------- */

#define N(a) (uint8_t)(sizeof (a) / sizeof (a)[0])
#define NOFIELDS 0, 0

const en_fm_reg_t en_fm_regs[] = {
{ 0x00, "I2C_FM_RDS_SYSTEM", "Enable or disable the FM and RDS functions.",
  0, 1, 1, EN_FM_R | EN_FM_W, f_system, N(f_system) },

{ 0x01, "I2C_FM_CTRL", "Radio and FM control parameters.",
  0, 1, 1, EN_FM_R | EN_FM_W, f_fm_ctrl, N(f_fm_ctrl) },

{ 0x02, "I2C_RDS_CTRL", "RDS control parameters.",
  0, 1, 1, EN_FM_R | EN_FM_W, f_rds_ctrl, N(f_rds_ctrl) },

{ 0x04, "I2C_FM_AUDIO_PAUSE", "Audio pause duration and its RSSI threshold.",
  0, 1, 1, EN_FM_R | EN_FM_W, f_audio_pause, N(f_audio_pause) },

{ 0x05, "I2C_FM_AUDIO_CTRL", "Audio bandwidth, muting, routing, de-emphasis.",
  0, 2, 2, EN_FM_R | EN_FM_W, f_audio_ctrl, N(f_audio_ctrl) },

{ 0x07, "I2C_FM_SEARCH_CTRL", "Search direction and RSSI threshold.",
  0, 1, 1, EN_FM_R | EN_FM_W, f_search_ctrl, N(f_search_ctrl) },

{ 0x08, "I2C_FM_SEARCH_CTRL1", "SNR threshold for auto search.",
  0, 1, 1, EN_FM_R | EN_FM_W, f_search_snr, N(f_search_snr) },

{ 0x09, "I2C_FM_SEARCH_TUNE_MODE", "Start or stop the search/tune state machine.",
  0, 1, 1, EN_FM_R | EN_FM_W, f_tune_mode, N(f_tune_mode) },

{ 0x0A, "I2C_FM_FREQ", "Tune or search start frequency.",
  "kHz offset from 64 MHz", 2, 2, EN_FM_R | EN_FM_W, f_freq, N(f_freq) },

{ 0x0C, "I2C_FM_AF_FREQ", "Alternate-frequency jump target.",
  "kHz offset from 64 MHz", 2, 2, EN_FM_R | EN_FM_W, f_af_freq, N(f_af_freq) },

{ 0x0E, "I2C_FM_CARRIER", "Carrier frequency error from the baseband PLL.",
  "kHz", 1, 0, EN_FM_R, f_carrier, N(f_carrier) },

{ 0x0F, "I2C_FM_RSSI", "Signal strength of the tuned channel.",
  0, 1, 0, EN_FM_R, f_rssi, N(f_rssi) },

{ 0x10, "I2C_FM_RDS_MASK", "Which status bits raise the FM interrupt.",
  0, 2, 2, EN_FM_R | EN_FM_W, f_rds_mask, N(f_rds_mask) },

{ 0x12, "I2C_FM_RDS_FLAG", "Status bits. Reading clears them.",
  0, 2, 0, EN_FM_R, f_rds_flag, N(f_rds_flag) },

{ 0x14, "I2C_RDS_WLINE", "RDS FIFO interrupt waterline.",
  "RDS tuples", 1, 1, EN_FM_R | EN_FM_W, f_wline, N(f_wline) },

{ 0x16, "I2C_RDS_BLKB_MATCH", "Block B matching value.",
  0, 2, 2, EN_FM_R | EN_FM_W, f_blkb_match, N(f_blkb_match) },

{ 0x18, "I2C_RDS_BLKB_MASK", "Block B matching mask; zero disables it.",
  0, 2, 2, EN_FM_R | EN_FM_W, f_blkb_mask, N(f_blkb_mask) },

{ 0x1A, "I2C_RDS_PI_MATCH", "Block A and C-prime PI matching value.",
  0, 2, 2, EN_FM_R | EN_FM_W, f_pi_match, N(f_pi_match) },

{ 0x1C, "I2C_RDS_PI_MASK", "PI matching mask; zero disables it.",
  0, 2, 2, EN_FM_R | EN_FM_W, f_pi_mask, N(f_pi_mask) },

{ 0x1E, "I2C_FM_RDS_BOOT", "Undocumented; read length unverified upstream.",
  0, 1, 0, EN_FM_R | EN_FM_TBD, NOFIELDS },

{ 0x1F, "I2C_FM_RDS_TEST", "Undocumented; read length unverified upstream.",
  0, 1, 0, EN_FM_R | EN_FM_TBD, NOFIELDS },

{ 0x29, "I2C_SLAVE_CONFIG", "I2C slave address and enable.",
  0, 1, 1, EN_FM_R | EN_FM_W | EN_FM_TBD, f_slave, N(f_slave) },

{ 0x4D, "I2C_FM_ROUTE_PCM", "Route FM audio to PCM.",
  0, 1, 1, EN_FM_R | EN_FM_W | EN_FM_RMW | EN_FM_TBD,
  f_route_pcm, N(f_route_pcm) },

/* Variable by design: the caller asks for as many tuples as it wants, up to
   250. This is the RDS FIFO, and the whole RDS feature set is built on it. */
{ 0x80, "I2C_RDS_DATA", "RDS tuples from the FIFO.",
  0, EN_FM_LEN_VAR, 0, EN_FM_R, NOFIELDS },

{ 0x90, "I2C_FM_BEST_TUNE_MODE", "Enable or disable best tune.",
  0, 1, 1, EN_FM_R | EN_FM_W | EN_FM_RMW | EN_FM_TBD,
  f_best_tune, N(f_best_tune) },

{ 0xDA, "I2C_FM_SMUTE_VERSION3", "Lowpass filter behaviour against signal quality.",
  0, 5, 5, EN_FM_R | EN_FM_W, f_smute3, N(f_smute3) },

{ 0xDB, "I2C_FM_FEATURES", "Bit map of FM features and their versions.",
  0, 4, 0, EN_FM_R | EN_FM_TBD, f_features, N(f_features) },

{ 0xDE, "I2C_FM_PRESCAN_QUALITY", "Carrier offset slope threshold.",
  0, 1, 1, EN_FM_R | EN_FM_W | EN_FM_TBD, f_prescan, N(f_prescan) },

{ 0xDF, "I2C_FM_SNR", "Signal-to-noise of the tuned channel.",
  0, 1, 0, EN_FM_R | EN_FM_TBD, f_snr, N(f_snr) },

/* Absent from the specification's read-length table entirely, so the length is
   not merely unverified but unstated. */
{ 0xF5, "FM_I2C_ADDITIONAL_AUDIO_PATH", "Audio paths beyond DAC and I2S.",
  0, EN_FM_LEN_UNKNOWN, 1, EN_FM_R | EN_FM_W | EN_FM_TBD,
  f_audio_path, N(f_audio_path) },

{ 0xF6, "I2C_FM_ANTENNA_MATCHING", "RX antenna matching auto config, and tune.",
  0, 1, 1, EN_FM_R | EN_FM_W, f_antenna_match, N(f_antenna_match) },

{ 0xF8, "I2C_FM_VOLUME_CTRL", "FM receive volume.",
  0, 2, 2, EN_FM_R | EN_FM_W | EN_FM_TBD, f_volume, N(f_volume) },

{ 0xF9, "I2C_FM_STEREO_BLEND_SOFT_MUTE", "Stereo blending and soft mute curve.",
  0, 8, 8, EN_FM_R | EN_FM_W | EN_FM_TBD, f_blend, N(f_blend) },

{ 0xFA, "I2C_FM_ANTENNA_SELECTION", "Internal or external aerial.",
  0, 1, 1, EN_FM_R | EN_FM_W | EN_FM_TBD, f_antenna_sel, N(f_antenna_sel) },

{ 0xFB, "I2C_FM_SEARCH_BOUNDARY", "Upper and lower search bounds.",
  "kHz offset from 64 MHz", 4, 4, EN_FM_R | EN_FM_W | EN_FM_TBD,
  f_boundary, N(f_boundary) },

{ 0xFC, "I2C_FM_SEARCH_METHOD", "Normal, preset or RSSI search.",
  0, 1, 1, EN_FM_R | EN_FM_W | EN_FM_TBD,
  f_search_method, N(f_search_method) },

{ 0xFD, "I2C_FM_SEARCH_STEP", "Channel spacing used while searching.",
  0, 2, 2, EN_FM_R | EN_FM_W | EN_FM_TBD, f_search_step, N(f_search_step) },

{ 0xFE, "I2C_FM_PRESET_MAX_CHANNEL", "How many presets the chip holds.",
  0, 1, 1, EN_FM_R | EN_FM_W | EN_FM_TBD, f_preset_max, N(f_preset_max) },

{ 0xFF, "I2C_FM_PRESET_CHANNEL", "The preset channel list.",
  0, EN_FM_LEN_VAR, 0, EN_FM_R, NOFIELDS },
};

const uint8_t en_fm_reg_count = (uint8_t)(sizeof en_fm_regs / sizeof en_fm_regs[0]);

/* ---- lookups and field access -------------------------------------------- */

const en_fm_reg_t *en_fm_reg_find(uint8_t addr)
{
    for (uint8_t i = 0; i < en_fm_reg_count; i++)
        if (en_fm_regs[i].addr == addr) return &en_fm_regs[i];
    return 0;
}

uint8_t en_fm_read_len(uint8_t addr)
{
    const en_fm_reg_t *r = en_fm_reg_find(addr);
    if (!r) return 0;
    if (r->read_len == EN_FM_LEN_VAR || r->read_len == EN_FM_LEN_UNKNOWN)
        return 0;
    return r->read_len;
}

/* Assemble the big-endian integer a field lives inside. */
static uint32_t host_value(const en_fm_field_t *f, const uint8_t *p)
{
    uint32_t v = 0;
    for (uint8_t i = 0; i < f->width; i++)
        v = (v << 8) | p[f->off + i];
    return v;
}

static uint32_t field_mask(const en_fm_field_t *f)
{
    uint8_t nbits = (uint8_t)(f->bit_hi - f->bit_lo + 1u);
    if (nbits >= 32u) return 0xFFFFFFFFu;
    return ((1u << nbits) - 1u) << f->bit_lo;
}

int32_t en_fm_field_get(const en_fm_field_t *f, const uint8_t *payload,
                        uint8_t len)
{
    if (!f || !payload) return 0;
    if ((uint32_t)f->off + f->width > len) return 0;

    uint32_t raw = (host_value(f, payload) & field_mask(f)) >> f->bit_lo;

    if (f->flags & EN_FMF_SIGNED) {
        uint8_t nbits = (uint8_t)(f->bit_hi - f->bit_lo + 1u);
        if (nbits < 32u && (raw & (1u << (nbits - 1u))))
            return (int32_t)(raw | ~((1u << nbits) - 1u));
    }
    return (int32_t)raw;
}

bool en_fm_field_set(const en_fm_field_t *f, uint8_t *payload, uint8_t len,
                     int32_t value)
{
    if (!f || !payload) return false;
    if ((uint32_t)f->off + f->width > len) return false;
    if (value < f->min || value > f->max) return false;

    uint32_t mask = field_mask(f);
    uint8_t nbits = (uint8_t)(f->bit_hi - f->bit_lo + 1u);
    uint32_t room = (nbits >= 32u) ? 0xFFFFFFFFu : ((1u << nbits) - 1u);

    /* Signed values are stored two's complement in the field's own width, so a
       negative has to be truncated to that width rather than sign-extended. */
    uint32_t raw = ((uint32_t)value) & room;

    /* Read, replace, write: every bit outside the field survives untouched,
       which is what makes editing one control of a packed register safe. */
    uint32_t v = host_value(f, payload);
    v = (v & ~mask) | ((raw << f->bit_lo) & mask);

    for (uint8_t i = 0; i < f->width; i++)
        payload[f->off + i] = (uint8_t)(v >> (8u * (f->width - 1u - i)));
    return true;
}
