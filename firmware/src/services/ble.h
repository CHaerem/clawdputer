#pragma once

#include <string>

#include "core/event_bus.h"

namespace ble {

// Initialise NimBLE, register both the Nordic UART Service (buddy) and the
// clawd-bridge service, and start advertising. Connection events are
// published on the global event bus tagged with the source link.
void begin();

// Send one line on the given link (a trailing '\n' is appended). No-op if no
// peer is subscribed to that link's TX characteristic.
void sendLine(EventSource source, const std::string& line);

// True iff a peer has notifications enabled on the given link's TX.
bool isConnected(EventSource source);

std::string deviceName();
void        setDeviceName(const std::string& name);

// Erase stored bonds. The next pairing will display a fresh passkey.
void clearBonds();

// Power-management hooks. Framework calls pause() when the active app
// declares SVC_NONE; resume() when an app with SVC_BLE is entered.
// pause() stops advertising only — existing connections survive.
void pause();
void resume();
bool isPaused();

}  // namespace ble
