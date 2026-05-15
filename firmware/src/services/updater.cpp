// GitOps self-update: poll the project's "latest" GitHub release for a
// newer firmware build than the one currently running. When found, stream
// firmware.bin into the inactive OTA partition via HTTPUpdate and reboot.
//
// Rollback: before flashing we mark a "pending" flag in NVS. After reboot,
// the new image must call confirmHealthy() (we do that after the device
// has run 30s without crash). If it fails to confirm across three boot
// attempts, we manually point esp_ota at the previous partition and
// reset — restoring the last-known-good firmware.

#include "updater.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <M5Cardputer.h>
#include <Preferences.h>
#include <WiFiClientSecure.h>
#include <esp_ota_ops.h>

#include "settings.h"
#include "wifi.h"

#ifndef CLAWD_BUILD_SHA
#define CLAWD_BUILD_SHA "unknown"
#endif

namespace {

constexpr const char* MANIFEST_URL =
    "https://github.com/CHaerem/clawdputer/releases/latest/download/version.txt";
constexpr const char* FIRMWARE_URL =
    "https://github.com/CHaerem/clawdputer/releases/latest/download/firmware.bin";
constexpr uint32_t CHECK_INTERVAL_MS = 5UL * 60UL * 1000UL;
constexpr uint32_t HEALTH_CONFIRM_MS = 30UL * 1000UL;
constexpr int      MAX_BOOT_ATTEMPTS = 3;

updater::Status g_status      = updater::Status::Idle;
uint32_t        g_lastCheckMs = 0;
bool            g_forceCheck  = true;
bool            g_confirmed   = false;
std::string     g_latest;
std::string     g_lastError;

const char* statusTextFor(updater::Status s) {
    switch (s) {
        case updater::Status::Idle:        return "idle";
        case updater::Status::Checking:    return "checking…";
        case updater::Status::UpToDate:    return "up to date";
        case updater::Status::Downloading: return "downloading…";
        case updater::Status::Failed:      return "failed";
    }
    return "?";
}

void drawUpdating(int pct) {
    auto& d = M5Cardputer.Display;
    d.fillScreen(BLACK);
    d.setTextSize(2);
    d.setTextColor(WHITE);
    d.setCursor(8, 24);
    d.print("UPDATING");
    d.setTextSize(1);
    d.setCursor(8, 56);
    d.printf("self-flash %d%%", pct);
    int barX = 8, barY = 80, barW = 304, barH = 12;
    d.drawRect(barX, barY, barW, barH, WHITE);
    int fill = (int)((barW - 2) * pct / 100);
    d.fillRect(barX + 1, barY + 1, fill, barH - 2, 0x07E0);
    d.setCursor(8, 110);
    d.setTextColor(0x7BEF);
    d.print("do not unplug");
}

void onUpdateProgress(int cur, int total) {
    if (total <= 0) return;
    static int lastPct = -1;
    int pct = (int)((int64_t)cur * 100 / total);
    if (pct == lastPct) return;
    lastPct = pct;
    drawUpdating(pct);
}

void rollbackNow() {
    Serial.println("[updater] rolling back to previous partition");
    const esp_partition_t* other = esp_ota_get_next_update_partition(nullptr);
    if (other && esp_ota_set_boot_partition(other) == ESP_OK) {
        Preferences prefs;
        prefs.begin("updater", false);
        prefs.putBool("pending", false);
        prefs.putInt("boots", 0);
        prefs.putBool("rolled_back", true);
        prefs.end();
        delay(200);
        ESP.restart();
    } else {
        Serial.println("[updater] rollback FAILED — no valid partition to revert to");
    }
}

bool fetchManifest(WiFiClientSecure& client, std::string& latestOut) {
    HTTPClient http;
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    if (!http.begin(client, MANIFEST_URL)) {
        g_lastError = "manifest http begin failed";
        return false;
    }
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        char buf[64];
        snprintf(buf, sizeof(buf), "manifest http %d", code);
        g_lastError = buf;
        http.end();
        return false;
    }
    String body = http.getString();
    http.end();
    body.trim();
    int nl = body.indexOf('\n');
    if (nl >= 0) body = body.substring(0, nl);
    latestOut = body.c_str();
    return true;
}

void runCheck() {
    if (!wifi::isConnected()) return;

    g_status    = updater::Status::Checking;
    g_lastError = "";

    WiFiClientSecure client;
    client.setInsecure();

    std::string latest;
    if (!fetchManifest(client, latest)) {
        g_status      = updater::Status::Failed;
        g_lastCheckMs = millis();
        Serial.printf("[updater] check failed: %s\n", g_lastError.c_str());
        return;
    }

    g_latest      = latest;
    g_lastCheckMs = millis();

    if (latest == CLAWD_BUILD_SHA) {
        g_status = updater::Status::UpToDate;
        Serial.printf("[updater] up to date (%s)\n", latest.c_str());
        return;
    }

    Serial.printf("[updater] new version %s (have %s) — flashing\n",
                  latest.c_str(), CLAWD_BUILD_SHA);
    g_status = updater::Status::Downloading;
    drawUpdating(0);

    // Mark the upcoming partition as pending verification BEFORE the flash,
    // so a crash mid-flash or a broken new image is recoverable.
    {
        Preferences prefs;
        prefs.begin("updater", false);
        prefs.putBool("pending", true);
        prefs.putInt("boots", 0);
        prefs.putString("target", latest.c_str());
        prefs.end();
    }

    httpUpdate.rebootOnUpdate(true);
    httpUpdate.onProgress(onUpdateProgress);
    auto ret = httpUpdate.update(client, FIRMWARE_URL);
    if (ret == HTTP_UPDATE_FAILED) {
        g_status    = updater::Status::Failed;
        g_lastError = httpUpdate.getLastErrorString().c_str();
        Serial.printf("[updater] failed: %s\n", g_lastError.c_str());
        // The download didn't complete, so the inactive partition is junk —
        // clear the pending flag to avoid a useless rollback on next boot.
        Preferences prefs;
        prefs.begin("updater", false);
        prefs.putBool("pending", false);
        prefs.putInt("boots", 0);
        prefs.end();
    }
}

}  // namespace

namespace updater {

void begin() {
    Preferences prefs;
    prefs.begin("updater", false);
    bool pending      = prefs.getBool("pending", false);
    int  bootAttempts = prefs.getInt("boots", 0);
    bool rolledBack   = prefs.getBool("rolled_back", false);

    if (rolledBack) {
        Serial.println("[updater] booted after a rollback — previous flash was unhealthy");
        prefs.putBool("rolled_back", false);
    }

    if (pending) {
        bootAttempts++;
        prefs.putInt("boots", bootAttempts);
        Serial.printf("[updater] pending flash, boot attempt %d/%d\n",
                      bootAttempts, MAX_BOOT_ATTEMPTS);
        prefs.end();

        if (bootAttempts > MAX_BOOT_ATTEMPTS) {
            rollbackNow();   // does not return
        }
        return;
    }
    prefs.end();
}

void tick() {
    // Confirm boot health after the device has been running stably.
    if (!g_confirmed && millis() >= HEALTH_CONFIRM_MS) {
        Preferences prefs;
        prefs.begin("updater", false);
        if (prefs.getBool("pending", false)) {
            prefs.putBool("pending", false);
            prefs.putInt("boots", 0);
            Serial.printf("[updater] boot confirmed healthy (sha=%s)\n", CLAWD_BUILD_SHA);
            // Also tell the ESP-IDF rollback system, in case the framework
            // is built with CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE.
            const esp_partition_t* running = esp_ota_get_running_partition();
            esp_ota_img_states_t state;
            if (running && esp_ota_get_state_partition(running, &state) == ESP_OK
                && state == ESP_OTA_IMG_PENDING_VERIFY) {
                esp_ota_mark_app_valid_cancel_rollback();
            }
        }
        prefs.end();
        g_confirmed = true;
    }

    if (!wifi::isConnected()) return;
    uint32_t now = millis();
    // Forced checks always run; periodic checks only when auto-update is
    // enabled in Settings. The Settings → "check for updates" action sets
    // g_forceCheck so it works regardless of the toggle.
    bool periodic = settings::autoUpdateEnabled() &&
                    (g_lastCheckMs == 0 ||
                     now - g_lastCheckMs >= CHECK_INTERVAL_MS);
    if (!g_forceCheck && !periodic) return;
    g_forceCheck = false;
    runCheck();
}

void checkNow() { g_forceCheck = true; }

Status      status()         { return g_status; }
const char* statusText()     { return statusTextFor(g_status); }
const char* currentVersion() { return CLAWD_BUILD_SHA; }
std::string latestVersion()  { return g_latest; }
uint32_t    lastCheckMs()    { return g_lastCheckMs; }
std::string lastError()      { return g_lastError; }

}  // namespace updater
