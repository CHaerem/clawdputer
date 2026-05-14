#pragma once

#include <stdint.h>

#include <string>

namespace updater {

enum class Status {
    Idle,
    Checking,
    UpToDate,
    Downloading,
    Failed,
};

// Should be called from setup() once, before the main loop. Reads the
// pending-flash flag from NVS and either increments the boot-attempt
// counter (rolling back if we exceed the threshold) or does nothing.
void begin();

// Should be called from the main loop. Throttles network checks internally —
// cheap when nothing is due. No-op until WiFi is up. Also confirms a freshly
// flashed image as healthy once the device has been running stably for a
// while.
void tick();

// Force a check on the next tick (used by Settings → Check for updates).
void checkNow();

Status      status();
const char* statusText();
const char* currentVersion();   // CLAWD_BUILD_SHA at compile time
std::string latestVersion();    // last seen from manifest, "" if unchecked
uint32_t    lastCheckMs();      // millis() when last check completed; 0 if none
std::string lastError();

}  // namespace updater
