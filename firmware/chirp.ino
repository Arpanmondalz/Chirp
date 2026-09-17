#include <Arduino.h>
#include <SPI.h>
#include <SD.h>

#include "config.h"
#include "battery.h"
#include "buttons.h"
#include "library.h"
#include "player.h"
#include "sync.h"
#include "ui.h"

void setup() {
  Serial.begin(115200);

  batteryBegin();
  buttonsBegin();
  uiBegin();

#if CHIRP_CALIBRATE
  uiCalibrationLoop();
#endif

  uiSplash(0);

  SPI.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
  if (!SD.begin(PIN_SD_CS, SPI, SD_SPI_HZ)) uiFatal("SD Error");

  syncBegin();

  if (!libraryBegin()) uiFatal("PSRAM Error");
  libraryScan(uiSplash);

  randomSeed(esp_random());
  if (!playerBegin()) uiFatal("Audio Error");

  uiReady();
}

void loop() {
  uiTick();
  delay(2);
}
