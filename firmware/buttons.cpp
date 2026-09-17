#include "buttons.h"
#include "config.h"

static Btn sStable = BTN_NONE;     // debounced state
static Btn sCandidate = BTN_NONE;  // state awaiting confirmation
static uint8_t sStableCount = 0;
static uint32_t sLastSample = 0;
static uint32_t sPressStart = 0;
static bool sConsumed = false;

static Btn classify(int raw) {
  if (raw < BTN_LEFT_MAX) return BTN_LEFT;
  if (raw < BTN_MID_MAX) return BTN_MID;
  if (raw < BTN_RIGHT_MAX) return BTN_RIGHT;
  return BTN_NONE;
}

void buttonsBegin() {
  // No INPUT_PULLUP: a physical 10k pull-up is already fitted, and the
  // internal one would sit in parallel and shift every ladder level.
  pinMode(PIN_BTN_ADC, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_BTN_ADC, ADC_11db);
}

int buttonsRaw() { return analogRead(PIN_BTN_ADC); }

BtnPress buttonsPoll() {
  BtnPress out = {BTN_NONE, EV_NONE};
  uint32_t now = millis();

  if (now - sLastSample >= BTN_SAMPLE_MS) {
    sLastSample = now;
    Btn c = classify(analogRead(PIN_BTN_ADC));

    if (c == sCandidate) {
      if (sStableCount < BTN_STABLE_N) sStableCount++;
    } else {
      sCandidate = c;
      sStableCount = 1;
    }

    // The ladder sweeps through neighbouring levels while a button travels,
    // so a state only counts once it has held for BTN_STABLE_N samples.
    if (sStableCount >= BTN_STABLE_N && sCandidate != sStable) {
      if (sStable == BTN_NONE) {
        sPressStart = now;
        sConsumed = false;
      } else if (sCandidate == BTN_NONE && !sConsumed) {
        out.btn = sStable;
        out.ev = EV_SHORT;
      }
      sStable = sCandidate;
    }
  }

  if (out.ev == EV_NONE && sStable != BTN_NONE && !sConsumed &&
      (now - sPressStart) >= LONG_PRESS_MS) {
    sConsumed = true;
    out.btn = sStable;
    out.ev = EV_LONG;
  }

  return out;
}
