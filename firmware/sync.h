#pragma once
#include <Arduino.h>

enum SyncPhase : uint8_t {
  SYNC_OFF = 0,   // not armed, normal player behaviour
  SYNC_WAIT,      // armed, listening, no transfer in flight
  SYNC_BUSY,      // receiving or deleting
  SYNC_REINDEX,   // exiting: rescanning the card
};

struct SyncStatus {
  SyncPhase phase;
  char file[64];      // current remote filename, basename only
  uint32_t done;
  uint32_t total;
  uint16_t okCount;
  uint16_t failCount;
  uint8_t reindexPct;
};

// All of these run on the audio task, which owns SPI/SD.
void syncEnter();
void syncService();

// Called from the UI task.
void syncRequestExit();
bool syncArmed();
void syncGetStatus(SyncStatus* out);

// Called once from setup(): creates the status mutex and removes any temp
// file left behind by an interrupted transfer.
void syncBegin();
