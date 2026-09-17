#include "ui.h"
#include "config.h"
#include "buttons.h"
#include "battery.h"
#include "library.h"
#include "player.h"
#include "sync.h"
#include "chirp_bitmaps.h"
#include <Wire.h>
#include <U8g2lib.h>

static U8G2_SH1107_SEEED_128X128_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

#define FONT_TITLE u8g2_font_helvB10_tf
#define FONT_BODY u8g2_font_helvR08_tf

// Layout (bands are 8px-aligned so they can be flushed as whole tile rows)
#define TOPBAR_TILES 3
#define DIVIDER_Y 26
#define NOW_ICON_X 52
#define NOW_ICON_Y 34
#define TITLE_TOP 72
#define TITLE_H 16
#define TITLE_BASE 85
#define ARTIST_TOP 96
#define ARTIST_H 16
#define ARTIST_BASE 108
#define ROW_TOP 32
#define ROW_H 24
#define ROWS_VISIBLE 4

static Screen sScreen = SCR_FOLDERS;
static uint16_t sCursor = 0;
static uint16_t sTopRow = 0;
static uint32_t sLastActivity = 0;
static bool sAsleep = false;
static bool sDirty = true;
static bool sDirtyTop = false;

static char sToast[24] = {0};
static uint32_t sToastUntil = 0;

// Marquee slots: 0 = title, 1 = artist, 2 = selected list row
struct MqState {
  int16_t off;
  uint16_t total;
};
static MqState sMq[3];
static uint32_t sLastStep = 0;
static bool sAnim = false;

static uint8_t sBandTy[3], sBandTh[3], sBandN = 0;
#define TILES_PER_AXIS (OLED_W / 8)

// Change detection so we only push pixels when something actually moved
static uint8_t sPrevPct = 255;
static bool sPrevFlash = false;
static char sPrevTitle[80] = {0};
static char sPrevArtist[80] = {0};
static bool sPrevPlaying = false;
static uint8_t sPrevVol = 255;

// -------------------------------------------------------------
// helpers
// -------------------------------------------------------------

static void registerBand(int top, int h) {
  if (sBandN >= 3) return;
  uint8_t ty = (uint8_t)(top / 8);
  uint8_t tyEnd = (uint8_t)((top + h + 7) / 8);
  sBandTy[sBandN] = ty;
  sBandTh[sBandN] = (uint8_t)(tyEnd - ty);
  sBandN++;
}

// With the panel upright a text line lies along the SH1107's pages, so a band
// is one transaction per tile row. Kept separate rather than merged: merging
// would only drag in the untouched rows between them.
static void flushBands() {
  for (uint8_t i = 0; i < sBandN; i++)
    u8g2.updateDisplayArea(0, sBandTy[i], TILES_PER_AXIS, sBandTh[i]);
}

static void drawCentered(const char* s, int baseline) {
  int w = u8g2.getUTF8Width(s);
  u8g2.drawUTF8((OLED_W - w) / 2, baseline, s);
}

// Draws `s` clipped to a band, scrolling it continuously when it overflows.
static void drawScroll(uint8_t slot, const char* s, int x, int w, int baseline, int top, int h) {
  int tw = u8g2.getUTF8Width(s);
  u8g2.setClipWindow(x, top, x + w, top + h);
  if (tw <= w) {
    sMq[slot].total = 0;
    sMq[slot].off = 0;
    u8g2.drawUTF8(x, baseline, s);
  } else {
    uint16_t total = tw + MARQUEE_GAP_PX;
    if (sMq[slot].total != total) {
      sMq[slot].total = total;
      sMq[slot].off = 0;
    }
    u8g2.drawUTF8(x - sMq[slot].off, baseline, s);
    u8g2.drawUTF8(x - sMq[slot].off + total, baseline, s);
    registerBand(top, h);
  }
  u8g2.setMaxClipWindow();
}

static const unsigned char* batteryIcon(int pct) {
  if (pct >= 81) return icon_bat_full;
  if (pct >= 31) return icon_bat_half;
  if (pct >= BAT_LOCKOUT_PCT) return icon_bat_low;
  return icon_bat_empty;
}

static const unsigned char* screenIcon(Screen s) {
  switch (s) {
    case SCR_NOW: return icon_music;
    case SCR_FOLDERS: return icon_folder;
    case SCR_SYNC: return icon_sync;
    default: return icon_volume;
  }
}

static void showToast(const char* msg) {
  strlcpy(sToast, msg, sizeof(sToast));
  sToastUntil = millis() + TOAST_MS;
  sDirty = true;
}

// -------------------------------------------------------------
// drawing
// -------------------------------------------------------------

static void drawTopBar(int pct, bool hideBattery) {
  u8g2.drawBitmap(2, 0, 3, 24, screenIcon(sScreen));
  if (!hideBattery) {
    u8g2.drawBitmap(102, 0, 3, 24, batteryIcon(pct));
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", pct);
    u8g2.setFont(FONT_BODY);
    u8g2.drawStr(99 - u8g2.getStrWidth(buf), 16, buf);
  }
  u8g2.drawHLine(0, DIVIDER_Y, OLED_W);
}

static void drawNowPlaying(const PlayerStatus& st) {
  if (!st.hasTrack) {
    u8g2.setFont(FONT_BODY);
    drawCentered("Nothing playing", 76);
    drawCentered("Pick a folder ->", 94);
    return;
  }
  u8g2.drawBitmap(NOW_ICON_X, NOW_ICON_Y, 3, 24, st.playing ? icon_play : icon_pause);
  u8g2.setFont(FONT_TITLE);
  drawScroll(0, st.title, 4, OLED_W - 8, TITLE_BASE, TITLE_TOP, TITLE_H);
  if (st.artist[0]) {
    u8g2.setFont(FONT_BODY);
    drawScroll(1, st.artist, 4, OLED_W - 8, ARTIST_BASE, ARTIST_TOP, ARTIST_H);
  }
}

static void drawFolders() {
  uint16_t n = libraryFolderCount();
  u8g2.setFont(FONT_BODY);
  if (n == 0) {
    drawCentered("No music found", 80);
    return;
  }
  for (uint8_t i = 0; i < ROWS_VISIBLE; i++) {
    uint16_t idx = sTopRow + i;
    if (idx >= n) break;
    int top = ROW_TOP + i * ROW_H;
    bool sel = (idx == sCursor);
    if (sel) {
      u8g2.drawBox(0, top, OLED_W, ROW_H - 1);
      u8g2.setDrawColor(0);
    }
    char buf[48];
    snprintf(buf, sizeof(buf), "%s (%u)", libraryFolderName(idx), libraryFolderSize(idx));
    if (sel) {
      drawScroll(2, buf, 4, OLED_W - 8, top + 17, top, ROW_H - 1);
      u8g2.setDrawColor(1);
    } else {
      u8g2.setClipWindow(4, top, OLED_W - 4, top + ROW_H - 1);
      u8g2.drawUTF8(4, top + 17, buf);
      u8g2.setMaxClipWindow();
    }
  }
}

static void drawVolume(const PlayerStatus& st) {
  u8g2.drawFrame(14, 60, 100, 24);
  int fill = (96 * st.volume) / VOL_MAX;
  if (fill > 0) u8g2.drawBox(16, 62, fill, 20);
  char buf[16];
  snprintf(buf, sizeof(buf), "%u / %u", st.volume, VOL_MAX);
  u8g2.setFont(FONT_BODY);
  drawCentered(buf, 104);
}

static void drawSyncScreen() {
  SyncStatus s;
  syncGetStatus(&s);
  u8g2.setFont(FONT_BODY);

  if (s.phase == SYNC_OFF) {
    u8g2.drawBitmap(NOW_ICON_X, 40, 3, 24, icon_sync);
    drawCentered("Sync Mode", 88);
    drawCentered("hold centre to start", 104);
    return;
  }

  if (s.phase == SYNC_REINDEX) {
    drawCentered("Re-indexing", 62);
    u8g2.drawFrame(14, 72, 100, 10);
    if (s.reindexPct) u8g2.drawBox(16, 74, (96 * s.reindexPct) / 100, 6);
    return;
  }

  u8g2.drawBitmap(NOW_ICON_X, 32, 3, 24, icon_sync);
  char buf[32];
  snprintf(buf, sizeof(buf), "%u ok  %u fail", s.okCount, s.failCount);
  drawCentered(buf, 122);

  if (s.phase == SYNC_BUSY && s.total) {
    drawScroll(0, s.file, 4, OLED_W - 8, TITLE_BASE, TITLE_TOP, TITLE_H);
    uint32_t pct = (uint32_t)(((uint64_t)s.done * 100) / s.total);
    u8g2.drawFrame(14, 96, 100, 10);
    if (pct) u8g2.drawBox(16, 98, (96 * pct) / 100, 6);
  } else {
    drawCentered("Waiting for PC", 84);
    drawCentered("hold centre to exit", 102);
  }
}

static void drawAll(uint32_t now, const PlayerStatus& st, int pct, bool hideBattery) {
  sBandN = 0;
  u8g2.clearBuffer();
  u8g2.setDrawColor(1);
  drawTopBar(pct, hideBattery);

  switch (sScreen) {
    case SCR_NOW: drawNowPlaying(st); break;
    case SCR_FOLDERS: drawFolders(); break;
    case SCR_SYNC: drawSyncScreen(); break;
    default: drawVolume(st); break;
  }

  if (now < sToastUntil) {
    u8g2.setDrawColor(1);
    u8g2.drawRBox(8, 52, OLED_W - 16, 26, 4);
    u8g2.setDrawColor(0);
    u8g2.setFont(FONT_BODY);
    drawCentered(sToast, 70);
    u8g2.setDrawColor(1);
    sBandN = 0;  // a toast always needs a full flush
  }
}

// -------------------------------------------------------------
// input
// -------------------------------------------------------------

static void goSleep() {
  u8g2.setPowerSave(1);
  sAsleep = true;
}

static void wake(uint32_t now, bool keepScreen) {
  sAsleep = false;
  if (!keepScreen) sScreen = SCR_NOW;
  sLastActivity = now;
  sDirty = true;
  u8g2.setPowerSave(0);
}

static void cycleScreen(int dir) {
  int s = (int)sScreen + dir;
  if (s < 0) s = SCR_COUNT - 1;
  if (s >= SCR_COUNT) s = 0;
  sScreen = (Screen)s;
  sDirty = true;
}

static void moveCursor(int dir) {
  uint16_t n = libraryFolderCount();
  if (n == 0) return;
  int c = (int)sCursor + dir;
  if (c < 0) c = 0;  // no wrap
  if (c >= (int)n) c = n - 1;
  sCursor = (uint16_t)c;
  if (sCursor < sTopRow) sTopRow = sCursor;
  if (sCursor >= sTopRow + ROWS_VISIBLE) sTopRow = sCursor - ROWS_VISIBLE + 1;
  sMq[2].total = 0;
  sMq[2].off = 0;
  sDirty = true;
}

static void handleInput(const BtnPress& p, uint32_t now, const PlayerStatus& st) {
  bool armed = syncArmed();

  if (sAsleep) {
    // Tethered to a PC in sync mode, so the pocket-protection rule is relaxed:
    // any press wakes, and returning here consumes it.
    if (armed || (p.btn == BTN_MID && p.ev == EV_LONG)) wake(now, armed);
    return;
  }

  sLastActivity = now;
  sDirty = true;

  if (armed) {
    if (p.btn == BTN_MID && p.ev == EV_LONG) syncRequestExit();
    return;  // navigation and playback are locked while armed
  }

  if (p.ev == EV_LONG) {
    if (p.btn == BTN_LEFT) {
      cycleScreen(-1);
    } else if (p.btn == BTN_RIGHT) {
      cycleScreen(+1);
    } else if (p.btn == BTN_MID) {
      if (sScreen == SCR_NOW) goSleep();
      else if (sScreen == SCR_SYNC) playerEnterSync();
    }
    return;
  }

  switch (sScreen) {
    case SCR_NOW:
      if (!st.hasTrack) break;
      if (p.btn == BTN_LEFT) playerPrev();
      else if (p.btn == BTN_RIGHT) playerNext();
      else if (p.btn == BTN_MID) {
        if (!st.playing && batteryCritical()) showToast("Battery too low");
        else playerToggle();
      }
      break;

    case SCR_FOLDERS:
      if (p.btn == BTN_LEFT) moveCursor(-1);
      else if (p.btn == BTN_RIGHT) moveCursor(+1);
      else if (p.btn == BTN_MID && libraryFolderCount()) {
        if (batteryCritical()) {
          showToast("Battery too low");
        } else {
          playerPlayFolder(sCursor);
          sScreen = SCR_NOW;
          sMq[0].total = 0;
          sMq[0].off = 0;
        }
      }
      break;

    case SCR_VOLUME:
      if (p.btn == BTN_LEFT && st.volume > 0) playerSetVolume(st.volume - 1);
      else if (p.btn == BTN_RIGHT && st.volume < VOL_MAX) playerSetVolume(st.volume + 1);
      break;

    default: break;  // SCR_SYNC: all short presses disabled
  }
}

// -------------------------------------------------------------
// public
// -------------------------------------------------------------

void uiBegin() {
  Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL);
  u8g2.setBusClock(I2C_HZ);  // begin() would overwrite a plain Wire.setClock()
  u8g2.begin();
  u8g2.setContrast(OLED_CONTRAST);
  u8g2.setFontMode(1);
  u8g2.clearBuffer();
  u8g2.sendBuffer();
  memset(sMq, 0, sizeof(sMq));
}

void uiSplash(uint8_t pct) {
  static int lastPct = -1;
  if (pct == lastPct) return;  // a full flush costs ~46ms, don't repeat it
  lastPct = pct;

  u8g2.clearBuffer();
  u8g2.setDrawColor(1);
  u8g2.drawBitmap(32, 28, 8, 48, chirp_logo_bmp);
  u8g2.drawFrame(14, 90, 100, 10);
  if (pct) u8g2.drawBox(16, 92, (96 * pct) / 100, 6);
  u8g2.setFont(FONT_BODY);
  drawCentered("Indexing...", 116);
  u8g2.sendBuffer();
}

void uiFatal(const char* msg) {
  u8g2.clearBuffer();
  u8g2.setDrawColor(1);
  u8g2.setFont(FONT_TITLE);
  drawCentered(msg, 64);
  u8g2.setFont(FONT_BODY);
  drawCentered("Power cycle to retry", 84);
  u8g2.sendBuffer();
  for (;;) delay(1000);
}

void uiReady() {
  sScreen = SCR_FOLDERS;
  sCursor = 0;
  sTopRow = 0;
  sLastActivity = millis();
  sDirty = true;
}

void uiTick() {
  uint32_t now = millis();
  batteryTick();

  PlayerStatus st;
  playerGetStatus(&st);

  // Land back on Now Playing once the post-sync re-index has finished.
  bool armed = syncArmed();
  static bool sWasArmed = false;
  if (sWasArmed && !armed) {
    sScreen = SCR_NOW;
    sDirty = true;
  }
  sWasArmed = armed;

  // Hard cutoff: below BAT_LOCKOUT_PCT nothing is allowed to play.
  static bool sLockoutFired = false;
  if (batteryCritical() && !armed) {
    if (st.playing && !sLockoutFired) {
      sLockoutFired = true;
      playerPause();
      showToast("Battery too low");
    }
  } else {
    sLockoutFired = false;
  }

  BtnPress p = buttonsPoll();
  if (p.ev != EV_NONE) handleInput(p, now, st);

  if (sAsleep) return;

  if (!sAsleep && (now - sLastActivity) >= SCREEN_SLEEP_MS) {
    goSleep();
    return;
  }

  // Sync progress changes on its own, so refresh it while the screen is on.
  static uint32_t sLastSyncDraw = 0;
  if (armed && sScreen == SCR_SYNC && (now - sLastSyncDraw) >= 250) {
    sLastSyncDraw = now;
    sDirty = true;
  }

  int pct = batteryPercent();
  bool critical = pct < BAT_LOCKOUT_PCT;
  bool flashOff = critical && ((now / BAT_FLASH_MS) & 1);

  if (pct != sPrevPct || flashOff != sPrevFlash) {
    sPrevPct = pct;
    sPrevFlash = flashOff;
    sDirtyTop = true;
  }
  if (st.playing != sPrevPlaying || st.volume != sPrevVol ||
      strcmp(st.title, sPrevTitle) != 0 || strcmp(st.artist, sPrevArtist) != 0) {
    sPrevPlaying = st.playing;
    sPrevVol = st.volume;
    strlcpy(sPrevTitle, st.title, sizeof(sPrevTitle));
    strlcpy(sPrevArtist, st.artist, sizeof(sPrevArtist));
    sDirty = true;
  }
  if (sToastUntil && now >= sToastUntil) {
    sToastUntil = 0;
    sDirty = true;
  }

  bool step = sAnim && (now - sLastStep) >= MARQUEE_STEP_MS;
  if (!sDirty && !sDirtyTop && !step) return;

  if (step) {
    sLastStep = now;
    for (int i = 0; i < 3; i++) {
      if (sMq[i].total) sMq[i].off = (sMq[i].off + MARQUEE_STEP_PX) % sMq[i].total;
    }
  }

  drawAll(now, st, pct, flashOff);
  sAnim = (sBandN > 0);

  if (sDirty) {
    u8g2.sendBuffer();
    sDirty = false;
    sDirtyTop = false;
  } else if (sDirtyTop) {
    u8g2.updateDisplayArea(0, 0, TILES_PER_AXIS, TOPBAR_TILES);
    sDirtyTop = false;
    flushBands();
  } else {
    // Marquee only: push the scrolling bands, not the whole 2KB buffer.
    flushBands();
  }
}

void uiCalibrationLoop() {
  u8g2.setFont(FONT_BODY);
  for (;;) {
    int raw = buttonsRaw();
    float v = batteryReadRawVolts();
    Serial.printf("BTN raw=%4d   BAT=%.3fV\n", raw, v);

    char l1[32], l2[32];
    snprintf(l1, sizeof(l1), "BTN %d", raw);
    snprintf(l2, sizeof(l2), "BAT %.3fV", v);
    u8g2.clearBuffer();
    drawCentered("CALIBRATION", 40);
    drawCentered(l1, 70);
    drawCentered(l2, 90);
    u8g2.sendBuffer();
    delay(250);
  }
}
