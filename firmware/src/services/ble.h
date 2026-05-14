#pragma once

#include <string>

namespace ble {

// Initialise NimBLE, register the Nordic UART Service, and start advertising
// under a name derived from the BT MAC. Publishes events on the global event
// bus: BleConnected/BleDisconnected, and BleLine for each newline-terminated
// payload received on the RX characteristic.
void begin();

// Send one line to the connected peer (a trailing '\n' is appended). No-op if
// the link isn't up.
void sendLine(const std::string& line);

bool        isConnected();
std::string deviceName();
void        setDeviceName(const std::string& name);

// Erase stored bonds. The next pairing will display a fresh passkey.
void clearBonds();

}  // namespace ble
