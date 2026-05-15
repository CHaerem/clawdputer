#pragma once

#include <stdint.h>

// Simple power-management knobs. The CPU runs at full speed (240 MHz) for
// a short window after every keypress; outside that window it drops to
// 80 MHz so static menus and idle screens cost less.
//
// Display dims after a longer idle interval and turns off after that.
// Any keypress wakes both clock and backlight.

#include <stdint.h>

namespace power {

void begin();
void tick();
void noteActivity();   // call from key dispatch on real input

uint8_t  backlightLevel();    // 0..255
uint32_t idleMs();             // millis since last keypress / button
uint16_t loopDelayMs();        // suggested delay for next loop iteration

// Reads on the most recent boot — useful for "we just woke from deep
// sleep" UX. Set in begin() based on esp_sleep_get_wakeup_cause().
bool wokeFromDeepSleep();

// Tear everything down and call esp_deep_sleep_start(). Wakes on a G0
// (side button) press. Never returns; the device resumes via a fresh
// boot. Safe to call from any task.
[[noreturn]] void deepSleep();

// Returns true if the device is currently displaying the "low battery,
// sleeping in Ns" prompt. Apps may render around it but onKey is
// hijacked by the warning (any key cancels).
bool lowBatteryWarning();
int  lowBatterySecondsLeft();  // -1 when no warning is active
void cancelLowBatteryWarning();

}  // namespace power
