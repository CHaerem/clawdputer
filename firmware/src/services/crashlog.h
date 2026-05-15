#pragma once

#include <stdint.h>
#include <string>

// crashlog: post-mortem context for the GitHub-issue reporter app.
//
// On boot, reads esp_reset_reason() and (when an SD card is present)
// reads back the previous session's last-known active app + serial
// tail from /clawd/. During the session, mirrors writes to a 4 KB RAM
// ring buffer; if SD is mounted those writes are batched out to
// /clawd/serial.log so they survive the next reboot.
//
// The whole subsystem is a no-op when settings::reportEnabled() is
// false at boot.

namespace crashlog {

void begin();

// Periodic housekeeping — flush pending SD writes if the batch window
// has elapsed.
void tick();

// Called by the framework on every app switch. Persists the new active
// app id (NVS for survival across reset, SD for serial log header).
void noteAppEntered(const char* appId);

// Write a line into the ring buffer (and Serial). Use instead of
// Serial.printf when you want the output to survive into a crash
// report. Format is printf-style. A trailing newline is added.
void logf(const char* fmt, ...);

// Reset-reason classification.
bool        hasPriorCrash();
const char* priorReason();   // "panic", "int_wdt", "task_wdt", "brownout", or "" if none
std::string priorApp();      // app id active just before the prior crash, or ""

// Best-effort serial tail. If SD has the prior session's log, returns
// the last ~tailBytes of it. Otherwise returns the live RAM ring
// buffer contents (current session only).
std::string serialTail(size_t tailBytes = 4096);

// Clear the prior-crash flag so we don't re-prompt on the next boot.
void acknowledgePriorCrash();

}  // namespace crashlog
