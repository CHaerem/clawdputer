#include "wifi.h"

#include <Arduino.h>
#include <WiFi.h>

#include "core/event_bus.h"

#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#endif

#ifndef CLAWD_WIFI_SSID
#define CLAWD_WIFI_SSID ""
#endif
#ifndef CLAWD_WIFI_PASS
#define CLAWD_WIFI_PASS ""
#endif

namespace {

bool g_enabled   = false;
bool g_connected = false;

void onWifiEvent(WiFiEvent_t event) {
    bool nowConnected = (WiFi.status() == WL_CONNECTED);
    if (nowConnected == g_connected) return;
    g_connected = nowConnected;
    if (g_connected) {
        Serial.printf("[wifi] connected, ip=%s\n", WiFi.localIP().toString().c_str());
        events::publish({EventType::LinkConnected, EventSource::WifiLink, {}});
    } else {
        Serial.println("[wifi] disconnected");
        events::publish({EventType::LinkDisconnected, EventSource::WifiLink, {}});
    }
}

}  // namespace

namespace wifi {

void begin() {
    if (strlen(CLAWD_WIFI_SSID) == 0) {
        Serial.println("[wifi] no credentials configured — skipping");
        return;
    }
    g_enabled = true;
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.onEvent(onWifiEvent);
    WiFi.begin(CLAWD_WIFI_SSID, CLAWD_WIFI_PASS);
    Serial.printf("[wifi] connecting to %s\n", CLAWD_WIFI_SSID);
}

bool isConnected() { return g_enabled && g_connected; }

std::string ip() {
    if (!g_connected) return "";
    return WiFi.localIP().toString().c_str();
}

}  // namespace wifi
