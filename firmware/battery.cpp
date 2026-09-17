#include "battery.h"
#include "config.h"

// Open-circuit-voltage curve for a single Li-ion cell. A linear 3.2-4.2V map
// reads ~70% for most of the real runtime and then collapses, so interpolate
// this table instead. Entries must be ordered high -> low.
static const struct {
  float v;
  uint8_t p;
} kOcv[] = {
    {4.15f, 100}, {4.08f, 95}, {4.02f, 90}, {3.95f, 80}, {3.89f, 70},
    {3.83f, 60},  {3.78f, 50}, {3.74f, 40}, {3.70f, 30}, {3.66f, 20},
    {3.60f, 15},  {3.50f, 10}, {3.40f, 5},  {3.20f, 0},
};
static const int kOcvN = sizeof(kOcv) / sizeof(kOcv[0]);

static float sEmaV = 0.0f;
static bool sSeeded = false;
static uint32_t sLastRead = 0;

static float readVolts() {
  uint32_t acc = 0;
  for (int i = 0; i < BAT_SAMPLES; i++) acc += analogReadMilliVolts(PIN_BAT_ADC);
  float pinMv = (float)acc / BAT_SAMPLES;
  return (pinMv * BAT_DIVIDER * BAT_CAL_GAIN) / 1000.0f;
}

static int voltsToPercent(float v) {
  if (v >= kOcv[0].v) return 100;
  if (v <= kOcv[kOcvN - 1].v) return 0;
  for (int i = 1; i < kOcvN; i++) {
    if (v >= kOcv[i].v) {
      float span = kOcv[i - 1].v - kOcv[i].v;
      float frac = (v - kOcv[i].v) / span;
      return (int)(kOcv[i].p + frac * (kOcv[i - 1].p - kOcv[i].p) + 0.5f);
    }
  }
  return 0;
}

void batteryBegin() {
  pinMode(PIN_BAT_ADC, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_BAT_ADC, ADC_11db);
  sEmaV = readVolts();
  sSeeded = true;
  sLastRead = millis();
}

void batteryTick() {
  uint32_t now = millis();
  if (now - sLastRead < BAT_PERIOD_MS) return;
  sLastRead = now;
  float v = readVolts();
  if (!sSeeded) {
    sEmaV = v;
    sSeeded = true;
  } else {
    sEmaV += BAT_EMA_ALPHA * (v - sEmaV);
  }
}

float batteryVolts() { return sEmaV; }
int batteryPercent() { return voltsToPercent(sEmaV); }
bool batteryCritical() { return batteryPercent() < BAT_LOCKOUT_PCT; }
float batteryReadRawVolts() { return readVolts(); }
