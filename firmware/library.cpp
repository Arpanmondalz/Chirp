#include "library.h"
#include "config.h"
#include <SD.h>
#include <strings.h>

struct TrackRec {
  uint32_t off;
  uint16_t folder;
};
struct FolderRec {
  uint32_t nameOff;
  uint16_t first;
  uint16_t count;
};

static char* sArena = nullptr;
static uint32_t sArenaUsed = 0;
static TrackRec* sTracks = nullptr;
static FolderRec* sFolders = nullptr;
static uint16_t sTrackN = 0;
static uint16_t sFolderN = 0;

static const uint32_t ARENA_BAD = 0xFFFFFFFFu;

static uint32_t arenaPut(const char* s) {
  size_t len = strlen(s) + 1;
  if (sArenaUsed + len > LIB_ARENA_BYTES) return ARENA_BAD;
  uint32_t off = sArenaUsed;
  memcpy(sArena + off, s, len);
  sArenaUsed += len;
  return off;
}

static const char* baseName(const char* path) {
  const char* slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

static bool isMp3(const char* name) {
  const char* dot = strrchr(name, '.');
  return dot && strcasecmp(dot, ".mp3") == 0;
}

// FAT volumes written by macOS/Windows are full of metadata we must not index.
static bool skipName(const char* name) {
  if (name[0] == '.') return true;
  if (strcasecmp(name, "System Volume Information") == 0) return true;
  return false;
}

bool libraryBegin() {
  sArena = (char*)ps_malloc(LIB_ARENA_BYTES);
  sTracks = (TrackRec*)ps_malloc(sizeof(TrackRec) * LIB_MAX_TRACKS);
  sFolders = (FolderRec*)ps_malloc(sizeof(FolderRec) * LIB_MAX_FOLDERS);
  return sArena && sTracks && sFolders;
}

// Adds every .mp3 directly inside `path` as one contiguous block, then
// registers it as a folder. Contiguity is what makes first/count work.
static void addFilesIn(const char* path, const char* displayName) {
  if (sFolderN >= LIB_MAX_FOLDERS) return;

  File dir = SD.open(path);
  if (!dir || !dir.isDirectory()) return;

  uint16_t start = sTrackN;
  for (;;) {
    File e = dir.openNextFile();
    if (!e) break;
    if (!e.isDirectory()) {
      const char* full = e.path();
      const char* base = baseName(full);
      if (!skipName(base) && isMp3(base) && sTrackN < LIB_MAX_TRACKS) {
        uint32_t off = arenaPut(full);
        if (off != ARENA_BAD) {
          sTracks[sTrackN].off = off;
          sTracks[sTrackN].folder = sFolderN;
          sTrackN++;
        }
      }
    }
    e.close();
  }
  dir.close();

  if (sTrackN > start) {
    uint32_t nameOff = arenaPut(displayName);
    if (nameOff == ARENA_BAD) {
      sTrackN = start;  // no room for the name, drop the block
      return;
    }
    sFolders[sFolderN].nameOff = nameOff;
    sFolders[sFolderN].first = start;
    sFolders[sFolderN].count = sTrackN - start;
    sFolderN++;
  }
}

static void scanDir(const char* path, uint8_t depth) {
  addFilesIn(path, baseName(path));
  if (depth >= LIB_MAX_DEPTH) return;

  File dir = SD.open(path);
  if (!dir || !dir.isDirectory()) return;
  for (;;) {
    File e = dir.openNextFile();
    if (!e) break;
    if (e.isDirectory()) {
      char child[256];
      strlcpy(child, e.path(), sizeof(child));
      e.close();
      if (!skipName(baseName(child))) scanDir(child, depth + 1);
      continue;
    }
    e.close();
  }
  dir.close();
}

void libraryScan(LibProgressCb progress) {
  sArenaUsed = 0;
  sTrackN = 0;
  sFolderN = 1;  // slot 0 reserved for "All Songs"

  uint32_t allOff = arenaPut("All Songs");
  sFolders[LIB_ALL_SONGS].nameOff = allOff;
  sFolders[LIB_ALL_SONGS].first = 0;
  sFolders[LIB_ALL_SONGS].count = 0;

  // The total track count is unknown up front, so progress is measured in
  // top-level entries completed. Good enough for an 8s splash.
  uint16_t total = 0;
  File root = SD.open("/");
  if (root && root.isDirectory()) {
    for (;;) {
      File e = root.openNextFile();
      if (!e) break;
      total++;
      e.close();
    }
  }
  root.close();
  if (total == 0) total = 1;
  if (progress) progress(0);

  addFilesIn("/", "Root");

  uint16_t done = 0;
  root = SD.open("/");
  if (root && root.isDirectory()) {
    for (;;) {
      File e = root.openNextFile();
      if (!e) break;
      if (e.isDirectory()) {
        char child[256];
        strlcpy(child, e.path(), sizeof(child));
        e.close();
        if (!skipName(baseName(child))) scanDir(child, 1);
      } else {
        e.close();
      }
      done++;
      if (progress) progress((uint8_t)((uint32_t)done * 100 / total));
    }
  }
  root.close();

  sFolders[LIB_ALL_SONGS].count = sTrackN;
  if (progress) progress(100);
}

uint16_t libraryTrackCount() { return sTrackN; }

uint16_t libraryFolderCount() {
  // Hide "All Songs" (and everything else) when the card has no music.
  return sTrackN ? sFolderN : 0;
}

const char* libraryTrackPath(uint16_t idx) {
  if (idx >= sTrackN) return "";
  return sArena + sTracks[idx].off;
}

void libraryTrackName(uint16_t idx, char* out, size_t n) {
  if (!n) return;
  out[0] = 0;
  if (idx >= sTrackN) return;
  strlcpy(out, baseName(sArena + sTracks[idx].off), n);
  char* dot = strrchr(out, '.');
  if (dot) *dot = 0;
}

const char* libraryFolderName(uint16_t folderId) {
  if (folderId >= sFolderN) return "";
  return sArena + sFolders[folderId].nameOff;
}

uint16_t libraryFolderFirst(uint16_t folderId) {
  return folderId < sFolderN ? sFolders[folderId].first : 0;
}

uint16_t libraryFolderSize(uint16_t folderId) {
  return folderId < sFolderN ? sFolders[folderId].count : 0;
}
