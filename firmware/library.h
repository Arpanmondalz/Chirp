#pragma once
#include <Arduino.h>

// Folder 0 is the virtual "All Songs" entry spanning every indexed track.
#define LIB_ALL_SONGS 0

typedef void (*LibProgressCb)(uint8_t pct);

bool libraryBegin();  // allocates the PSRAM arena
void libraryScan(LibProgressCb progress);

uint16_t libraryTrackCount();
uint16_t libraryFolderCount();

const char* libraryTrackPath(uint16_t idx);
void libraryTrackName(uint16_t idx, char* out, size_t n);  // basename, no extension

const char* libraryFolderName(uint16_t folderId);
uint16_t libraryFolderFirst(uint16_t folderId);
uint16_t libraryFolderSize(uint16_t folderId);
