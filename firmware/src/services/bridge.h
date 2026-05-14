#pragma once

#include <string>

#include "core/event_bus.h"

// Transport-agnostic bridge link. Apps that used to call
// ble::sendLine(EventSource::BridgeLink, …) now call bridge::sendLine(…),
// and don't have to care whether the bridge daemon is reachable over BLE
// or over WiFi/TCP. Events still publish on EventSource::BridgeLink so
// app-side handlers keep working unchanged.

namespace bridge {

void begin();  // wires up internals; mDNS-driven TCP fallback starts when WiFi is up

bool isConnected();          // true if either transport has a peer
void sendLine(const std::string& line);

// Which transport is currently active — for UI hints only.
enum class Transport { None, Ble, Tcp };
Transport activeTransport();

// Apps that need a quiet moment (e.g. SSH doing a blocking handshake)
// call pause()/resume() so the periodic mDNS discovery and TCP pump
// don't fight for the mbedTLS / mDNS heap.
void pause();
void resume();

}  // namespace bridge
