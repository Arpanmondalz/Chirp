#pragma once
#include <Arduino.h>

void batteryBegin();
void batteryTick();  // cheap; reads the ADC at most every BAT_PERIOD_MS

int batteryPercent();
float batteryVolts();
bool batteryCritical();  // below BAT_LOCKOUT_PCT

float batteryReadRawVolts();  // uncalibrated-by-gain helper for calibration mode
