#include "wifi.h"

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_wifi.h>

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
bool g_paused    = false;
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
        Serial.println("[wifi] no credentials in NVS — radio up, scan-only");
    }

    // Pre-init the WiFi driver with a minimum buffer config. NimBLE has
    // already taken its share of the heap, and the Arduino default config
    // (10 static + 32 dynamic RX buffers, ~67 KB) won't fit in what's
    // left, so WiFi.mode(STA) below would otherwise fail with NO_MEM.
    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    wcfg.static_rx_buf_num  = 2;
    wcfg.dynamic_rx_buf_num = 0;
    wcfg.dynamic_tx_buf_num = 8;
    esp_err_t werr = esp_wifi_init(&wcfg);
    Serial.printf("[wifi] esp_wifi_init rc=%d (heap=%u)\n",
                  werr, (unsigned)ESP.getFreeHeap());

    g_enabled = true;
    g_ssid    = ssid.c_str();
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.onEvent(onWifiEvent);
    // Modem power save: significant idle-current reduction at the cost of
    // mild added latency on incoming packets. Both are acceptable for our
    // mostly-interactive workload.
    WiFi.setSleep(WIFI_PS_MIN_MODEM);
    if (!ssid.isEmpty()) {
        WiFi.begin(ssid.c_str(), pass.c_str());
        Serial.printf("[wifi] connecting to %s\n", ssid.c_str());
    }
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
}

void connectNow(const std::string& ssidNew, const std::string& passNew) {
    setCredentials(ssidNew, passNew);

    if (!g_enabled) {
        WiFi.mode(WIFI_STA);
        WiFi.setAutoReconnect(true);
        WiFi.onEvent(onWifiEvent);
        g_enabled = true;
    } else {
        WiFi.disconnect(false, true);
    }
    g_ssid = ssidNew;
    WiFi.begin(ssidNew.c_str(), passNew.c_str());
    Serial.printf("[wifi] connectNow to %s\n", ssidNew.c_str());
}

void startScan() {
    if (!g_enabled) {
        WiFi.mode(WIFI_STA);
        WiFi.setAutoReconnect(true);
        WiFi.onEvent(onWifiEvent);
        g_enabled = true;
    }
    WiFi.scanNetworks(true /*async*/, true /*show hidden*/);
}

int scanStatus() {
    int rc = WiFi.scanComplete();
    return rc;  // -1=running, -2=failed, >=0=count
}

ScanResult scanNetwork(int idx) {
    ScanResult r;
    r.ssid   = WiFi.SSID(idx).c_str();
    r.rssi   = WiFi.RSSI(idx);
    r.secure = WiFi.encryptionType(idx) != WIFI_AUTH_OPEN;
    return r;
}

void clearCredentials() {
    Preferences prefs;
    prefs.begin("wifi", false);
    prefs.clear();
    prefs.end();
    Serial.println("[wifi] credentials cleared");
}

void pause() {
    if (g_paused) return;
    Serial.println("[wifi] pause (radio off)");
    WiFi.disconnect(true, false);
    WiFi.mode(WIFI_OFF);
    g_paused    = true;
    g_connected = false;
}

void resume() {
    if (!g_paused && g_enabled) return;
    if (g_ssid.empty()) return;
    Preferences prefs;
    prefs.begin("wifi", true);
    String pass = prefs.getString("pass", "");
    prefs.end();
    Serial.printf("[wifi] resume (connecting %s)\n", g_ssid.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(WIFI_PS_MIN_MODEM);
    WiFi.onEvent(onWifiEvent);
    WiFi.begin(g_ssid.c_str(), pass.c_str());
    g_paused  = false;
    g_enabled = true;
}

bool isPaused() { return g_paused; }

}  // namespace wifi
