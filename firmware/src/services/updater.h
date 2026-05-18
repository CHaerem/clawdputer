#pragma once

#include <stdint.h>

#include <string>

namespace updater {

enum class Status {
    Idle,             // no check has run this boot
    Checking,         // manifest fetch in progress
    UpToDate,         // running SHA matches the published latest
    UpdateAvailable,  // a newer SHA exists; awaiting installNow() (manual mode)
    Downloading,      // streaming firmware.bin into the OTA partition
    Failed,           // last check or flash failed; see lastError()
};

// Should be called from setup() once, before the main loop. Reads the
// pending-flash flag from NVS and either increments the boot-attempt
// counter (rolling back if we exceed the threshold) or does nothing.
// Also loads persisted status from previous boot so the Settings UI has
// something to show before the first network check completes.
void begin();

// Should be called from the main loop. Throttles network checks internally —
// cheap when nothing is due. No-op until WiFi is up. Also confirms a freshly
// flashed image as healthy once the device has been running stably for a
// while.
void tick();

// Run a live OTA from the current normal boot: pauses BLE + releases
// canvas + pauses SD, fetches the manifest, streams firmware.bin via
// HTTPUpdate, and reboots into the new image on success. Returns to the
// caller only on failure (UI is restored before returning).
void installNow();

Status      status();
const char* statusText();

const char* currentVersion();   // CLAWD_BUILD_SHA at compile time
const char* currentBuiltAt();   // CLAWD_BUILD_DATE (ISO 8601) at compile time

std::string latestVersion();    // last seen sha from manifest, "" if unchecked
std::string latestBuiltAt();    // last seen build date (ISO 8601), "" if absent

// Wall-clock epoch (seconds) of the last completed check. 0 if never checked
// or NTP wasn't synced when the check ran.
uint32_t    lastCheckEpoch();
// Monotonic millis() of the last completed check this boot. 0 if no check has
// finished since boot — useful when wall-clock is unknown.
uint32_t    lastCheckMs();

std::string lastError();        // "" if last check succeeded

}  // namespace updater
