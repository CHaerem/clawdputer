#pragma once

#include <string>

#include "core/event_bus.h"

namespace wifi {

// Begin connecting using credentials stored in NVS. On first boot, if NVS is
// empty and wifi_secrets.h provides compile-time defaults, those are migrated
// in. Non-blocking — status reflected via isConnected() and events on the bus.
void begin();

bool isConnected();

// Local IP as a dotted string, or empty when disconnected.
std::string ip();

// Currently configured SSID (empty if none).
std::string ssid();

// Persist a new SSID/password pair to NVS. Takes effect on next reboot.
void setCredentials(const std::string& ssid, const std::string& pass);

// Wipe stored credentials.
void clearCredentials();

}  // namespace wifi
