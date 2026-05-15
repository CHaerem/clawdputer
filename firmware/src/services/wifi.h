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

// Persist + connect immediately, without rebooting. Saves to NVS and starts
// a fresh WiFi.begin() against the new SSID/password. Status reflected via
// isConnected().
void connectNow(const std::string& ssid, const std::string& pass);

// Wipe stored credentials.
void clearCredentials();

// Power-management hooks. Apps that need WiFi (SSH, WiFi setup, Chat / Usage
// when the bridge is on TCP) call resume() in onEnter; power::tick() calls
// pause() once the device has been idle for a few minutes.
void pause();   // turns the radio off; safe no-op if already off
void resume();  // turns the radio on; reconnects with stored credentials
bool isPaused();

// Async WiFi scan. startScan() kicks off, scanStatus() polls. Returns count
// of networks when finished (>=0), -1 while running, -2 if failed.
void startScan();
int  scanStatus();  // -1 running, -2 failed, >=0 finished with N nets

struct ScanResult {
    std::string ssid;
    int         rssi;
    bool        secure;
};
ScanResult scanNetwork(int idx);

}  // namespace wifi
