#include "wifi.h"

#include <Arduino.h>
#include <Preferences.h>
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
std::string g_ssid;

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
    Preferences prefs;
    prefs.begin("wifi", false);
    String ssid = prefs.getString("ssid", "");
    String pass = prefs.getString("pass", "");

    // First-boot migration: copy compile-time secrets (if present) into NVS so
    // subsequent CI-built firmware can OTA without losing credentials.
    if (ssid.isEmpty() && strlen(CLAWD_WIFI_SSID) > 0) {
        ssid = CLAWD_WIFI_SSID;
        pass = CLAWD_WIFI_PASS;
        prefs.putString("ssid", ssid);
        prefs.putString("pass", pass);
        Serial.println("[wifi] migrated compile-time credentials → NVS");
    }
    prefs.end();

    if (ssid.isEmpty()) {
        Serial.println("[wifi] no credentials in NVS — skipping");
        return;
    }

    g_enabled = true;
    g_ssid    = ssid.c_str();
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.onEvent(onWifiEvent);
    WiFi.begin(ssid.c_str(), pass.c_str());
    Serial.printf("[wifi] connecting to %s\n", ssid.c_str());
}

bool isConnected() { return g_enabled && g_connected; }

std::string ip() {
    if (!g_connected) return "";
    return WiFi.localIP().toString().c_str();
}

std::string ssid() { return g_ssid; }

void setCredentials(const std::string& ssidNew, const std::string& passNew) {
    Preferences prefs;
    prefs.begin("wifi", false);
    prefs.putString("ssid", ssidNew.c_str());
    prefs.putString("pass", passNew.c_str());
    prefs.end();
    Serial.printf("[wifi] credentials saved (ssid=%s)\n", ssidNew.c_str());
    Serial.println("[wifi] reboot to apply");
}

void clearCredentials() {
    Preferences prefs;
    prefs.begin("wifi", false);
    prefs.clear();
    prefs.end();
    Serial.println("[wifi] credentials cleared");
}

}  // namespace wifi
