#pragma once

#include <string>

// Last-wins NVS slot for an issue that couldn't be filed at the time it
// happened — typically because the github.com TLS handshake doesn't fit
// in the fragmented heap of a fully-booted firmware (see CLAUDE.md
// "Recovery-boot OTA"). The recovery boot drains the slot after its
// manifest fetch succeeds, since the heap there is unfragmented enough
// for the POST to go through.

namespace telemetry {

// Overwrites any previously-queued entry. Cheap (one NVS write); call
// from any context.
void enqueue(const std::string& title, const std::string& body);

// True iff a queued entry is waiting.
bool pending();

// Submit the queued entry to GitHub Issues. No-op if nothing pending,
// no PAT compiled in, or WiFi is down. Clears the slot on a successful
// submit so we never duplicate. Logs result to Serial.
void drain();

}  // namespace telemetry
