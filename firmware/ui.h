#pragma once
#include <Arduino.h>

enum Screen : uint8_t { SCR_NOW = 0, SCR_FOLDERS, SCR_SYNC, SCR_VOLUME, SCR_COUNT };

void uiBegin();
void uiSplash(uint8_t pct);   // matches LibProgressCb
void uiFatal(const char* msg);  // never returns
void uiReady();
void uiTick();
void uiCalibrationLoop();  // never returns
