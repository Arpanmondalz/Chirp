#pragma once
#include <Arduino.h>

enum Btn : uint8_t { BTN_NONE = 0, BTN_LEFT, BTN_MID, BTN_RIGHT };
enum BtnEvent : uint8_t { EV_NONE = 0, EV_SHORT, EV_LONG };

struct BtnPress {
  Btn btn;
  BtnEvent ev;
};

void buttonsBegin();

// Non-blocking. Returns at most one event per call; poll it often.
// A long press fires as soon as LONG_PRESS_MS elapses and suppresses
// the short press that would otherwise fire on release.
BtnPress buttonsPoll();

int buttonsRaw();
