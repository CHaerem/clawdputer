#pragma once

#include <string>

#include "core/event_bus.h"

namespace wifi {

// Begin connecting using credentials from wifi_secrets.h. Non-blocking — the
// status is reflected in isConnected() and via WifiConnected/WifiDisconnected
// events on the bus. If no credentials are configured the service stays off.
void begin();

bool isConnected();

// Local IP as a dotted string, or empty when disconnected.
std::string ip();

}  // namespace wifi
