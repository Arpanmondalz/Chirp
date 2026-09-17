#include "player.h"
#include "config.h"
#include "library.h"
#include "sync.h"
#include <SD.h>
#include "Audio.h"
#include "esp_system.h"

// The audio task owns `audio`, the SPI bus and the SD card. Nothing else may
// touch them. The UI talks to it only through sQueue and reads state through
// playerGetStatus(), which copies under sMutex.

enum CmdType : uint8_t { CMD_PLAY_FOLDER, CMD_TOGGLE, CMD_PAUSE, CMD_NEXT, CMD_PREV, CMD_VOLUME, CMD_SYNC };

struct Cmd {
  CmdType type;
  uint16_t arg;
};

static Audio audio;
static QueueHandle_t sQueue = nullptr;
static SemaphoreHandle_t sMutex = nullptr;
static PlayerStatus sStatus;

static uint16_t* sShuffle = nullptr;  // indices into the library
static uint16_t sShuffleN = 0;
static uint16_t sShufflePos = 0;
static uint16_t sFolder = 0;
static volatile bool sEof = false;

static void statusLock() { xSemaphoreTake(sMutex, portMAX_DELAY); }
static void statusUnlock() { xSemaphoreGive(sMutex); }


static uint32_t randomBounded(uint32_t bound) {
  if (bound <= 1) return 0;

  // Rejection sampling avoids modulo bias.
  const uint32_t limit = UINT32_MAX - (UINT32_MAX % bound);

  uint32_t r;
  do {
    r = esp_random();
  } while (r >= limit);

  return r % bound;
}

static void buildShuffle(uint16_t folderId) {
  uint16_t first = libraryFolderFirst(folderId);
  uint16_t count = libraryFolderSize(folderId);

  sShuffleN = count;

  // Start with the tracks in their library order.
  for (uint16_t i = 0; i < count; i++) {
    sShuffle[i] = first + i;
  }

  // Fisher-Yates shuffle using fresh ESP32 hardware randomness.
  for (int i = (int)count - 1; i > 0; i--) {
    uint32_t j = randomBounded((uint32_t)i + 1);

    uint16_t t = sShuffle[i];
    sShuffle[i] = sShuffle[j];
    sShuffle[j] = t;
  }

  sShufflePos = 0;
}

static void startCurrent() {
  if (sShuffleN == 0) return;
  uint16_t idx = sShuffle[sShufflePos];
  const char* path = libraryTrackPath(idx);

  char name[80];
  libraryTrackName(idx, name, sizeof(name));

  statusLock();
  sStatus.hasTrack = true;
  sStatus.playing = true;
  sStatus.paused = false;
  sStatus.folderId = sFolder;
  strlcpy(sStatus.title, name, sizeof(sStatus.title));
  sStatus.artist[0] = 0;  // filled in later if the file carries an ID3 tag
  statusUnlock();

  sEof = false;
  audio.stopSong();
  audio.connecttoFS(SD, path);
}

static void advance(int delta) {
  if (sShuffleN == 0) return;
  if (delta > 0) {
    sShufflePos++;
    if (sShufflePos >= sShuffleN) {
      buildShuffle(sFolder);  // queue exhausted: reshuffle and keep going
    }
  } else {
    if (sShufflePos == 0) {
      sShufflePos = 0;
    } else {
      sShufflePos--;
    }
  }
  startCurrent();
}

static void handleCmd(const Cmd& c) {
  switch (c.type) {
    case CMD_PLAY_FOLDER:
      if (libraryFolderSize(c.arg) == 0) break;
      sFolder = c.arg;
      buildShuffle(sFolder);
      startCurrent();
      break;

    case CMD_TOGGLE:
      if (!sStatus.hasTrack) break;
      audio.pauseResume();
      statusLock();
      sStatus.paused = !sStatus.paused;
      sStatus.playing = !sStatus.paused;
      statusUnlock();
      break;

    case CMD_PAUSE:
      if (!sStatus.hasTrack || sStatus.paused) break;
      audio.pauseResume();
      statusLock();
      sStatus.paused = true;
      sStatus.playing = false;
      statusUnlock();
      break;

    case CMD_NEXT: advance(+1); break;
    case CMD_PREV: advance(-1); break;

    case CMD_VOLUME: {
      uint8_t v = c.arg > VOL_MAX ? VOL_MAX : (uint8_t)c.arg;
      audio.setVolume(v);
      statusLock();
      sStatus.volume = v;
      statusUnlock();
      break;
    }

    case CMD_SYNC:
      audio.stopSong();
      statusLock();
      sStatus.hasTrack = false;
      sStatus.playing = false;
      sStatus.paused = false;
      sStatus.title[0] = 0;
      sStatus.artist[0] = 0;
      statusUnlock();
      sShuffleN = 0;
      syncEnter();
      break;
  }
}

static void audioTask(void*) {
  for (;;) {
    Cmd c;
    while (xQueueReceive(sQueue, &c, 0) == pdTRUE) handleCmd(c);

    if (syncArmed()) {
      syncService();
    } else {
      audio.loop();
      if (sEof) {
        sEof = false;
        advance(+1);
      }
    }
    vTaskDelay(1);
  }
}

bool playerBegin() {
  sShuffle = (uint16_t*)ps_malloc(sizeof(uint16_t) * LIB_MAX_TRACKS);
  if (!sShuffle) return false;

  memset(&sStatus, 0, sizeof(sStatus));
  sStatus.volume = VOL_DEFAULT;

  sMutex = xSemaphoreCreateMutex();
  sQueue = xQueueCreate(8, sizeof(Cmd));
  if (!sMutex || !sQueue) return false;

  audio.setPinout(PIN_I2S_BCLK, PIN_I2S_LRC, PIN_I2S_DOUT);
  audio.setVolume(VOL_DEFAULT);

  // Core 0 is otherwise idle (no WiFi/BT), so decoding never competes with
  // the I2C display writes that run on core 1.
  BaseType_t ok = xTaskCreatePinnedToCore(audioTask, "audio", 8192, nullptr, 3, nullptr, 0);
  return ok == pdPASS;
}

static void send(CmdType t, uint16_t arg = 0) {
  if (!sQueue) return;
  Cmd c = {t, arg};
  xQueueSend(sQueue, &c, 0);
}

void playerPlayFolder(uint16_t folderId) { send(CMD_PLAY_FOLDER, folderId); }
void playerToggle() { send(CMD_TOGGLE); }
void playerPause() { send(CMD_PAUSE); }
void playerNext() { send(CMD_NEXT); }
void playerPrev() { send(CMD_PREV); }
void playerSetVolume(uint8_t v) { send(CMD_VOLUME, v); }
void playerEnterSync() { send(CMD_SYNC); }

void playerGetStatus(PlayerStatus* out) {
  if (!sMutex) {
    memset(out, 0, sizeof(*out));
    return;
  }
  statusLock();
  *out = sStatus;
  statusUnlock();
}

// ---- ESP32-audioI2S callbacks (invoked from audio.loop(), i.e. this task) ----

void audio_eof_mp3(const char* info) {
  (void)info;
  sEof = true;
}

void audio_id3data(const char* info) {
  if (!info) return;
  if (strncmp(info, "Title: ", 7) == 0 && info[7]) {
    statusLock();
    strlcpy(sStatus.title, info + 7, sizeof(sStatus.title));
    statusUnlock();
  } else if (strncmp(info, "Artist: ", 8) == 0 && info[8]) {
    statusLock();
    strlcpy(sStatus.artist, info + 8, sizeof(sStatus.artist));
    statusUnlock();
  }
  // Every other ID3 frame is deliberately ignored.
}

void audio_info(const char* info) { (void)info; }
