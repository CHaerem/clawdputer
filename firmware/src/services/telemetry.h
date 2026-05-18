#pragma once

#include <string>

// Last-wins NVS slot for an issue that couldn't be filed at the time it
// happened — typically a transient WiFi outage or rare heap pressure
// when the report app tries to submit live. `updater::tick()` drains the
// slot opportunistically (every ~30 s when WiFi is up).

namespace telemetry {

// Overwrites any previously-queued entry. Cheap (one NVS write); call
// from any context. `label` is forwarded to GitHub when drained.
void enqueue(const std::string& title,
             const std::string& body,
             const std::string& label = "auto-telemetry");

// True iff a queued entry is waiting.
bool pending();

// Submit the queued entry to GitHub Issues. No-op if nothing pending,
// no PAT compiled in, or WiFi is down. Clears the slot on a successful
// submit so we never duplicate. Logs result to Serial.
void drain();

}  // namespace telemetry
