#pragma once

#include <stdint.h>

// Simple power-management knobs. The CPU runs at full speed (240 MHz) for
// a short window after every keypress; outside that window it drops to
// 80 MHz so static menus and idle screens cost less.
//
// Display dims after a longer idle interval and turns off after that.
// Any keypress wakes both clock and backlight.

namespace power {

void begin();
void tick();
void noteActivity();   // call from key dispatch on real input

uint8_t backlightLevel();    // 0..255
}  // namespace power
