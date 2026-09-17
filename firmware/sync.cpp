#include "sync.h"
#include "config.h"
#include "library.h"
#include <SD.h>
#include <strings.h>

// Line-based JSON commands from the host, with raw bytes following a "put".
// Every upload lands on SYNC_TMP_PATH and is only renamed into place after the
// length and CRC32 match, so an interrupted transfer can never leave a
// playable-looking partial file on the card.

static SemaphoreHandle_t sMtx = nullptr;
static SyncStatus sStatus;
static volatile bool sExitReq = false;
// Polled by the audio task every iteration, so it must not take the mutex.
static volatile bool sArmed = false;

static char sLine[SYNC_LINE_MAX];
static uint16_t sLineLen = 0;
static uint8_t sBuf[SYNC_CHUNK];

// ---- CRC32 (reflected, poly 0xEDB88320) - matches Python zlib.crc32 ----
static const uint32_t kCrcTab[16] = {
    0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4,
    0x4DB26158, 0x5005713C, 0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C,
    0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C};

static uint32_t crc32Update(uint32_t crc, const uint8_t* d, size_t n) {
  while (n--) {
    crc ^= *d++;
    crc = kCrcTab[crc & 0x0f] ^ (crc >> 4);
    crc = kCrcTab[crc & 0x0f] ^ (crc >> 4);
  }
  return crc;
}

// ---- status ----
static void lock() { if (sMtx) xSemaphoreTake(sMtx, portMAX_DELAY); }
static void unlock() { if (sMtx) xSemaphoreGive(sMtx); }

static void setPhase(SyncPhase p) {
  lock();
  sStatus.phase = p;
  unlock();
}

static const char* baseName(const char* path) {
  const char* s = strrchr(path, '/');
  return s ? s + 1 : path;
}

// ---- minimal JSON reads (host is local and trusted, but still bounded) ----
static const char* jsonFind(const char* s, const char* key) {
  char pat[32];
  snprintf(pat, sizeof(pat), "\"%s\"", key);
  const char* p = strstr(s, pat);
  if (!p) return nullptr;
  p += strlen(pat);
  while (*p == ' ') p++;
  if (*p != ':') return nullptr;
  p++;
  while (*p == ' ') p++;
  return p;
}

static bool jsonStr(const char* s, const char* key, char* out, size_t n) {
  const char* p = jsonFind(s, key);
  if (!p || *p != '"') return false;
  p++;
  size_t i = 0;
  while (*p && *p != '"' && i + 1 < n) {
    if (*p == '\\' && p[1]) p++;
    out[i++] = *p++;
  }
  out[i] = 0;
  return true;
}

static bool jsonU32(const char* s, const char* key, uint32_t* out) {
  const char* p = jsonFind(s, key);
  if (!p) return false;
  char* end = nullptr;
  unsigned long v = strtoul(p, &end, 10);
  if (end == p) return false;
  *out = (uint32_t)v;
  return true;
}

static void printEscaped(const char* s) {
  for (; *s; s++) {
    unsigned char c = (unsigned char)*s;
    if (c == '"' || c == '\\') {
      Serial.write('\\');
      Serial.write(c);
    } else if (c < 0x20) {
      Serial.printf("\\u%04x", c);
    } else {
      Serial.write(c);
    }
  }
}

static void respErr(const char* e) {
  Serial.print("{\"ok\":false,\"err\":\"");
  printEscaped(e);
  Serial.println("\"}");
}

static bool pathOk(const char* p) {
  size_t n = strlen(p);
  if (n < 2 || n >= SYNC_PATH_MAX || p[0] != '/') return false;
  if (strstr(p, "..")) return false;
  for (size_t i = 0; i < n; i++)
    if ((unsigned char)p[i] < 0x20) return false;
  return true;
}

static bool skipName(const char* name) {
  if (name[0] == '.') return true;
  return strcasecmp(name, "System Volume Information") == 0;
}

// ---- commands ----
static void cmdHello() {
  Serial.printf("{\"ok\":true,\"fw\":\"chirp\",\"proto\":1,\"total\":%llu,\"used\":%llu}\n",
                SD.totalBytes(), SD.usedBytes());
}

static void emitDirs(const char* path, uint8_t depth) {
  File dir = SD.open(path);
  if (!dir || !dir.isDirectory()) return;
  for (;;) {
    File e = dir.openNextFile();
    if (!e) break;
    if (!e.isDirectory()) {
      e.close();
      continue;
    }
    char child[SYNC_PATH_MAX];
    strlcpy(child, e.path(), sizeof(child));
    e.close();
    if (skipName(baseName(child))) continue;
    Serial.print(",\"");
    printEscaped(child);
    Serial.print('"');
    if (depth + 1 < LIB_MAX_DEPTH) emitDirs(child, depth + 1);
  }
  dir.close();
}

static void cmdDirs() {
  // A raw directory walk, not the playlist index: the index only registers
  // folders that already hold an mp3, which would hide empty upload targets.
  Serial.print("{\"ok\":true,\"dirs\":[\"/\"");
  emitDirs("/", 0);
  Serial.println("]}");
}

static void cmdList(const char* line) {
  char path[SYNC_PATH_MAX];
  if (!jsonStr(line, "dir", path, sizeof(path))) {
    respErr("badargs");
    return;
  }
  if (strcmp(path, "/") != 0 && !pathOk(path)) {
    respErr("badpath");
    return;
  }
  File dir = SD.open(path);
  if (!dir || !dir.isDirectory()) {
    respErr("notfound");
    return;
  }
  Serial.print("{\"ok\":true,\"files\":[");
  bool first = true;
  for (;;) {
    File e = dir.openNextFile();
    if (!e) break;
    if (!e.isDirectory()) {
      const char* base = baseName(e.path());
      if (!skipName(base)) {
        if (!first) Serial.print(',');
        Serial.print("{\"n\":\"");
        printEscaped(base);
        Serial.printf("\",\"s\":%u}", (unsigned)e.size());
        first = false;
      }
    }
    e.close();
  }
  dir.close();
  Serial.println("]}");
}

static void cmdPut(const char* line) {
  char path[SYNC_PATH_MAX];
  uint32_t size = 0, crc = 0;
  if (!jsonStr(line, "path", path, sizeof(path)) || !jsonU32(line, "size", &size) ||
      !jsonU32(line, "crc", &crc)) {
    respErr("badargs");
    return;
  }
  if (!pathOk(path)) {
    respErr("badpath");
    return;
  }
  if (SD.exists(path)) {
    respErr("exists");
    return;
  }
  SD.remove(SYNC_TMP_PATH);
  File f = SD.open(SYNC_TMP_PATH, FILE_WRITE);
  if (!f) {
    respErr("openfail");
    return;
  }

  lock();
  sStatus.phase = SYNC_BUSY;
  strlcpy(sStatus.file, baseName(path), sizeof(sStatus.file));
  sStatus.done = 0;
  sStatus.total = size;
  unlock();

  Serial.println("{\"ok\":true,\"ready\":true}");

  uint32_t got = 0, run = 0xFFFFFFFFu, lastRx = millis();
  const char* fail = nullptr;

  while (got < size) {
    if (sExitReq) {
      fail = "aborted";
      break;
    }
    size_t want = size - got;
    if (want > SYNC_CHUNK) want = SYNC_CHUNK;
    size_t n = Serial.readBytes(sBuf, want);
    if (n == 0) {
      if (millis() - lastRx > SYNC_RX_TIMEOUT_MS) {
        fail = "timeout";
        break;
      }
      continue;
    }
    lastRx = millis();
    if (f.write(sBuf, n) != n) {
      fail = "writefail";
      break;
    }
    run = crc32Update(run, sBuf, n);
    got += n;
    lock();
    sStatus.done = got;
    unlock();
  }
  f.close();
  run ^= 0xFFFFFFFFu;

  if (!fail && run != crc) fail = "crc";
  if (!fail && !SD.rename(SYNC_TMP_PATH, path)) fail = "renamefail";

  lock();
  sStatus.phase = SYNC_WAIT;
  if (fail) sStatus.failCount++;
  else sStatus.okCount++;
  unlock();

  if (fail) {
    SD.remove(SYNC_TMP_PATH);
    respErr(fail);
  } else {
    Serial.printf("{\"ok\":true,\"written\":%u}\n", (unsigned)got);
  }
}

static void cmdDel(const char* line) {
  char path[SYNC_PATH_MAX];
  if (!jsonStr(line, "path", path, sizeof(path)) || !pathOk(path)) {
    respErr("badpath");
    return;
  }
  if (!SD.exists(path)) {
    respErr("notfound");
    return;
  }
  if (!SD.remove(path)) {
    respErr("delfail");
    return;
  }
  Serial.println("{\"ok\":true}");
}

static void handleLine(const char* line) {
  char cmd[16];
  if (!jsonStr(line, "cmd", cmd, sizeof(cmd))) return;  // not ours, stay silent

  if (!strcmp(cmd, "hello")) cmdHello();
  else if (!strcmp(cmd, "dirs")) cmdDirs();
  else if (!strcmp(cmd, "list")) cmdList(line);
  else if (!strcmp(cmd, "put")) cmdPut(line);
  else if (!strcmp(cmd, "del")) cmdDel(line);
  else if (!strcmp(cmd, "bye")) Serial.println("{\"ok\":true}");
  else respErr("unknown");
}

// ---- lifecycle ----
static void reindexProgress(uint8_t pct) {
  lock();
  sStatus.reindexPct = pct;
  unlock();
}

static void doExit() {
  sExitReq = false;
  SD.remove(SYNC_TMP_PATH);
  lock();
  sStatus.phase = SYNC_REINDEX;
  sStatus.reindexPct = 0;
  unlock();

  libraryScan(reindexProgress);

  Serial.setTimeout(1000);
  sArmed = false;
  setPhase(SYNC_OFF);
}

void syncBegin() {
  if (!sMtx) sMtx = xSemaphoreCreateMutex();
  memset(&sStatus, 0, sizeof(sStatus));
  sArmed = false;
  SD.remove(SYNC_TMP_PATH);
}

void syncEnter() {
  sExitReq = false;
  lock();
  memset(&sStatus, 0, sizeof(sStatus));
  sStatus.phase = SYNC_WAIT;
  unlock();

  sLineLen = 0;
  SD.remove(SYNC_TMP_PATH);
  Serial.setTimeout(50);
  while (Serial.available()) Serial.read();
  sArmed = true;
}

void syncService() {
  if (sExitReq) {
    doExit();
    return;
  }
  while (Serial.available()) {
    int c = Serial.read();
    if (c < 0) break;
    if (c == '\r') continue;
    if (c == '\n') {
      sLine[sLineLen] = 0;
      if (sLineLen) handleLine(sLine);
      sLineLen = 0;
      return;  // one command per service call
    }
    if (sLineLen + 1 < SYNC_LINE_MAX) sLine[sLineLen++] = (char)c;
    else sLineLen = 0;
  }
}

void syncRequestExit() { sExitReq = true; }

bool syncArmed() { return sArmed; }

void syncGetStatus(SyncStatus* out) {
  lock();
  *out = sStatus;
  unlock();
}
