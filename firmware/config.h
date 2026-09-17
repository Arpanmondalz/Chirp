#pragma once
#include <Arduino.h>

// =============================================================
// Chirp 1.0 - build-wide configuration
// Board: Seeed XIAO ESP32-S3
// Required Arduino board settings:
//   PSRAM: "OPI PSRAM"   USB CDC On Boot: "Enabled"
//   USB Mode: "USB-OTG (TinyUSB)"   CPU: 240MHz   Flash: QIO 80MHz
// =============================================================

// Set to 1 to boot into a serial ADC readout instead of the player.
// Needed to find BAT_CAL_GAIN; also useful to re-check the button ladder.
#define CHIRP_CALIBRATE 0

// ---- Pins (GPIO numbers, not D-numbers) ----
#define PIN_I2S_BCLK 1   // D0
#define PIN_I2S_LRC  2   // D1
#define PIN_I2S_DOUT 44  // D7
#define PIN_SD_CS    43  // D6
#define PIN_SD_SCK   7   // D8
#define PIN_SD_MOSI  8   // D9
#define PIN_SD_MISO  9   // D10
#define PIN_OLED_SDA 5   // D4
#define PIN_OLED_SCL 6   // D5
#define PIN_BTN_ADC  4   // D3  (10k physical pull-up to 3V3)
#define PIN_BAT_ADC  3   // D2  (10k/10k divider off switched BAT+)

// ---- Bus speeds ----
#define SD_SPI_HZ 20000000

// ---- Display ----
#define OLED_W 128
#define OLED_H 128
// Panel is mounted upright, so no rotation (U8G2_R0). Keep it that way: with the
// glass turned 90 deg, a text line crosses all 16 SH1107 pages instead of 2, which
// costs 8x the I2C transactions per marquee frame and couples noise into the DAC.
//
// The SH1107 init sequence sets contrast 0x2f (47), NOT 255 - so this is a
// fraction of 47, not of full scale. Raising it drives the charge pump harder,
// which couples audible noise into the DAC. Raise only if it is unreadable.
#define OLED_CONTRAST 33
// u8g2's table for this panel claims 400kHz is unreliable and uses 200kHz.
// Must be applied with setBusClock(), because begin() overrides Wire.setClock().
// Drop to 200000 if the display shows torn or corrupted rows.
#define I2C_HZ 400000

// ---- Timing ----
#define LONG_PRESS_MS    500
#define SCREEN_SLEEP_MS  8000
#define BTN_SAMPLE_MS    5
#define BTN_STABLE_N     3
#define MARQUEE_STEP_MS  40
#define MARQUEE_STEP_PX  1
#define MARQUEE_GAP_PX   24
#define TOAST_MS         1500

// ---- Button ladder (raw 12-bit ADC) ----
// Measured on hardware with only the physical 10k pull-up fitted (internal
// pull-up disabled). Thresholds are the midpoints between adjacent levels,
// which gives every level the widest possible drift margin. The binding gap
// is mid<->right (685 counts), so worst case is +-342 counts / ~0.25V.
#define BTN_LEVEL_LEFT  0
#define BTN_LEVEL_MID   1933
#define BTN_LEVEL_RIGHT 2618
#define BTN_LEVEL_IDLE  4095

#define BTN_LEFT_MAX  ((BTN_LEVEL_LEFT + BTN_LEVEL_MID) / 2)    // 966
#define BTN_MID_MAX   ((BTN_LEVEL_MID + BTN_LEVEL_RIGHT) / 2)   // 2275
#define BTN_RIGHT_MAX ((BTN_LEVEL_RIGHT + BTN_LEVEL_IDLE) / 2)  // 3356

// ---- Battery ----
#define BAT_DIVIDER     2.0f    // 10k/10k
#define BAT_CAL_GAIN    1.0f    // set from CHIRP_CALIBRATE: V_multimeter / V_reported
#define BAT_SAMPLES     32
#define BAT_EMA_ALPHA   0.10f
#define BAT_PERIOD_MS   2000
#define BAT_LOCKOUT_PCT 10      // below this, playback is blocked
#define BAT_FLASH_MS    500

// ---- Audio ----
#define VOL_MAX     21
#define VOL_DEFAULT 11

// ---- Library limits (PSRAM) ----
#define LIB_MAX_TRACKS  4000
#define LIB_MAX_FOLDERS 128
#define LIB_ARENA_BYTES (256 * 1024)
#define LIB_MAX_DEPTH   6

// ---- Sync mode (Screen 3) ----
#define SYNC_TMP_PATH      "/.chirptmp"
#define SYNC_LINE_MAX      512
#define SYNC_PATH_MAX      200
#define SYNC_CHUNK         4096
#define SYNC_RX_TIMEOUT_MS 5000
