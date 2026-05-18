// GitOps self-update: on user request, fetch the project's "latest" GitHub
// release manifest. If newer than the running build, stream firmware.bin
// into the inactive OTA partition via HTTPUpdate and reboot into it.
//
// As of Phase 4 this runs live from normal boot — the IDF-component build
// with mbedTLS DYNAMIC_BUFFER fits the github.com handshake in fragmented
// heap, so we no longer need the two-reboot recovery dance. installNow()
// pauses BLE + releases the canvas to free contiguous bytes for HTTPUpdate's
// internal buffers, then flashes in place.
//
// scheduleRecoveryUpdate() + runRecoveryImpl() are kept as a fallback for
// pathological cases (low heap, etc.) but no longer the primary path. They
// will be deleted in a follow-up once the live path proves stable.
//
// Rollback: before flashing we mark a "pending" flag in NVS. The new image
// must run stably for HEALTH_CONFIRM_MS to clear the flag. Three failed
// boot attempts trigger an automatic switch back to the previous partition.

#include "updater.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <M5Cardputer.h>
#include <Preferences.h>
#include <WiFiClientSecure.h>
#include <esp_ota_ops.h>
#include <time.h>

#include "ble.h"
#include "sd.h"
#include "telemetry.h"
#include "wifi.h"
#include "ui/canvas.h"

#ifndef CLAWD_BUILD_SHA
#define CLAWD_BUILD_SHA "unknown"
#endif
#ifndef CLAWD_BUILD_DATE
#define CLAWD_BUILD_DATE ""
#endif

namespace {

constexpr const char* MANIFEST_URL =
    "https://github.com/CHaerem/clawdputer/releases/latest/download/version.txt";
constexpr const char* FIRMWARE_URL =
    "https://github.com/CHaerem/clawdputer/releases/latest/download/firmware.bin";
constexpr uint32_t HEALTH_CONFIRM_MS = 30UL * 1000UL;
constexpr int      MAX_BOOT_ATTEMPTS = 3;

updater::Status g_status      = updater::Status::Idle;
uint32_t        g_lastCheckMs = 0;
uint32_t        g_lastCheckEpoch = 0;
bool            g_confirmed   = false;
std::string     g_latest;
std::string     g_latestBuiltAt;
std::string     g_lastError;

const char* statusTextFor(updater::Status s) {
    switch (s) {
        case updater::Status::Idle:            return "never checked";
        case updater::Status::Checking:        return "checking…";
        case updater::Status::UpToDate:        return "up to date";
        case updater::Status::UpdateAvailable: return "update available";
        case updater::Status::Downloading:     return "downloading…";
        case updater::Status::Failed:          return "check failed";
    }
    return "?";
}

uint8_t encodeStatus(updater::Status s) {
    // Only persist terminal states. Transient ones (Checking, Downloading)
    // are replaced by what they collapse to, so the UI never shows
    // "checking…" frozen after a reboot.
    switch (s) {
        case updater::Status::UpToDate:        return 1;
        case updater::Status::UpdateAvailable: return 2;
        case updater::Status::Failed:          return 3;
        default:                               return 0;
    }
}

updater::Status decodeStatus(uint8_t v) {
    switch (v) {
        case 1: return updater::Status::UpToDate;
        case 2: return updater::Status::UpdateAvailable;
        case 3: return updater::Status::Failed;
        default: return updater::Status::Idle;
    }
}

void persistResult() {
    Preferences prefs;
    prefs.begin("updater", false);
    prefs.putUChar("result", encodeStatus(g_status));
    prefs.putString("latest", g_latest.c_str());
    prefs.putString("builtAt", g_latestBuiltAt.c_str());
    prefs.putString("err", g_lastError.c_str());
    prefs.putUInt("epoch", g_lastCheckEpoch);
    prefs.end();
}

// Direct-to-LCD progress painter (no canvas — caller is expected to have
// released it to free contiguous heap for the TLS handshake + HTTPUpdate).
void drawProgress(const char* header, const char* phase, int pct) {
    auto& d = M5Cardputer.Display;
    d.fillScreen(BLACK);
    d.setTextSize(2);
    d.setTextColor(WHITE);
    d.setCursor(8, 8);
    d.print(header);
    d.setTextSize(1);
    d.setCursor(8, 36);
    d.print(phase);
    int barX = 8, barY = 56, barW = 224, barH = 10;
    d.drawRect(barX, barY, barW, barH, WHITE);
    if (pct >= 0) {
        int fill = (int)((barW - 2) * pct / 100);
        d.fillRect(barX + 1, barY + 1, fill, barH - 2, 0x07E0);
    }
    d.setCursor(8, 88);
    d.setTextColor(0x7BEF);
    d.print("do not unplug");
}

void drawRecovery(const char* phase, int pct) {
    drawProgress("OTA RECOVERY", phase, pct);
}

void drawInstall(const char* phase, int pct) {
    drawProgress("OTA UPDATE", phase, pct);
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

bool fetchManifest(WiFiClientSecure& client,
                   std::string& shaOut, std::string& builtAtOut) {
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
    // Line 1: short SHA. Line 2 (optional): ISO 8601 build date.
    int nl = body.indexOf('\n');
    if (nl < 0) {
        shaOut     = body.c_str();
        builtAtOut = "";
    } else {
        String first  = body.substring(0, nl);
        String second = body.substring(nl + 1);
        first.trim();
        second.trim();
        int nl2 = second.indexOf('\n');
        if (nl2 >= 0) second = second.substring(0, nl2);
        shaOut     = first.c_str();
        builtAtOut = second.c_str();
    }
    return true;
}

// Shared progress state for both the live and recovery flash paths —
// HTTPUpdate's onProgress callback has no userdata pointer, so the painter
// reads these globals.
const char* g_progressHeader = "OTA UPDATE";
const char* g_progressPhase  = "downloading…";

void onFlashProgress(int cur, int total) {
    if (total <= 0) return;
    static int lastPct = -1;
    int pct = (int)((int64_t)cur * 100 / total);
    if (pct == lastPct) return;
    lastPct = pct;
    drawProgress(g_progressHeader, g_progressPhase, pct);
}

// Pre-flash NVS bookkeeping. Used by both the live and recovery paths so
// the boot-attempt rollback works regardless of how we flashed.
void markFlashPending(const std::string& targetSha) {
    Preferences prefs;
    prefs.begin("updater", false);
    prefs.putBool("pending", true);
    prefs.putInt("boots", 0);
    prefs.putString("target", targetSha.c_str());
    prefs.end();
}

void persistRecoveryFailure(const char* reason) {
    Preferences prefs;
    prefs.begin("updater", false);
    prefs.putString("rec_fail", reason);
    // Clear any flash-pending bookkeeping — recovery failed before flashing.
    prefs.putBool("pending", false);
    prefs.putInt("boots", 0);
    prefs.end();
    Serial.printf("[updater] recovery failed: %s\n", reason);
}

[[noreturn]] void runRecoveryImpl() {
    // Consume the recovery flag immediately so a crash from here on falls
    // through to a normal boot rather than looping back into recovery.
    {
        Preferences prefs;
        prefs.begin("updater", false);
        prefs.putBool("recovery", false);
        prefs.putString("rec_fail", "");
        prefs.end();
    }

    drawRecovery("connecting to wifi…", -1);
    wifi::begin();

    uint32_t deadline = millis() + 20000;
    while (!wifi::isConnected() && millis() < deadline) {
        delay(100);
    }
    if (!wifi::isConnected()) {
        persistRecoveryFailure("wifi timeout");
        drawRecovery("wifi timeout — rebooting", -1);
        delay(2000);
        ESP.restart();
    }

    Serial.printf("[updater] recovery pre-TLS: free=%u largest=%u\n",
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());

    drawRecovery("checking manifest…", -1);
    WiFiClientSecure client;
    client.setInsecure();

    std::string latest, builtAt;
    if (!fetchManifest(client, latest, builtAt)) {
        persistRecoveryFailure(g_lastError.empty() ? "manifest fetch failed"
                                                   : g_lastError.c_str());
        drawRecovery("manifest failed — rebooting", -1);
        delay(2000);
        ESP.restart();
    }

    // Recovery boot is now OTA-only. Live HTTPS works from normal boot via
    // mbedTLS DYNAMIC_BUFFER, so health.cpp/report.cpp submit directly and
    // never need to piggyback on this codepath.

    if (latest == CLAWD_BUILD_SHA) {
        Serial.println("[updater] recovery: already up to date");
        // Not a failure — just nothing to do. Persist the result so the UI
        // reflects the successful check.
        g_latest         = latest;
        g_latestBuiltAt  = builtAt;
        g_status         = updater::Status::UpToDate;
        g_lastError      = "";
        persistResult();
        drawRecovery("already up to date", 100);
        delay(1500);
        ESP.restart();
    }

    Serial.printf("[updater] recovery: flashing %s (have %s)\n",
                  latest.c_str(), CLAWD_BUILD_SHA);
    g_progressHeader = "OTA RECOVERY";
    g_progressPhase  = "downloading…";
    drawRecovery(g_progressPhase, 0);

    markFlashPending(latest);

    httpUpdate.rebootOnUpdate(true);
    httpUpdate.onProgress(onFlashProgress);
    httpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    auto ret = httpUpdate.update(client, FIRMWARE_URL);
    // Success reboots inside httpUpdate.update(). Anything below is failure.
    (void)ret;

    std::string err = httpUpdate.getLastErrorString().c_str();
    if (err.empty()) err = "flash failed";
    persistRecoveryFailure(err.c_str());
    drawRecovery("flash failed — rebooting", -1);
    delay(2000);
    ESP.restart();
}

}  // namespace

namespace updater {

void begin() {
    Preferences prefs;
    prefs.begin("updater", false);
    bool pending      = prefs.getBool("pending", false);
    int  bootAttempts = prefs.getInt("boots", 0);
    bool rolledBack   = prefs.getBool("rolled_back", false);

    // Restore last-known status so the UI isn't blank on a fresh boot.
    g_status         = decodeStatus(prefs.getUChar("result", 0));
    g_latest         = prefs.getString("latest", "").c_str();
    g_latestBuiltAt  = prefs.getString("builtAt", "").c_str();
    g_lastError      = prefs.getString("err", "").c_str();
    g_lastCheckEpoch = prefs.getUInt("epoch", 0);

    if (rolledBack) {
        Serial.println("[updater] booted after a rollback — previous flash was unhealthy");
        prefs.putBool("rolled_back", false);
    }

    // If the previous boot was a failed recovery attempt, surface the reason
    // through the normal Status::Failed / lastError() channel so the Settings
    // UI shows it. Also file an issue once WiFi is up (handled in tick()).
    std::string recFail = prefs.getString("rec_fail", "").c_str();
    if (!recFail.empty()) {
        g_status    = Status::Failed;
        g_lastError = std::string("recovery: ") + recFail;
        prefs.putString("rec_fail", "");
        Serial.printf("[updater] previous recovery failed: %s\n", recFail.c_str());
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

    // Opportunistic telemetry flush — a queued report from a failed live
    // submit will get filed the next time wifi is available and we have a
    // free heartbeat. Cheap: drain() is a no-op when the queue is empty.
    static uint32_t lastDrainMs = 0;
    if (millis() - lastDrainMs > 30000 && wifi::isConnected()
        && telemetry::pending()) {
        lastDrainMs = millis();
        telemetry::drain();
    }
}

void installNow() {
    // Live OTA from normal boot. With mbedTLS DYNAMIC_BUFFER the github.com
    // handshake fits in fragmented heap, so we pause BLE + release canvas
    // to free contiguous bytes for HTTPUpdate's internal buffers, then
    // flash in place. Single reboot via HTTPUpdate.rebootOnUpdate(true) on
    // success. On failure we restore the UI and return to the caller.

    bool hadCanvas  = ui::canvasActive();
    bool wasBlePaused = ble::isPaused();
    bool wasSdPaused  = sd::isPaused();
    if (hadCanvas)     ui::releaseCanvas();
    if (!wasBlePaused) ble::pause();
    if (!wasSdPaused)  sd::pause();
    delay(100);

    Serial.printf("[updater] live install: free=%u largest=%u\n",
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)ESP.getMaxAllocHeap());

    auto restoreUi = [&]() {
        if (!wasSdPaused)  sd::resume();
        if (!wasBlePaused) ble::resume();
        if (hadCanvas)     ui::tryAcquireCanvas();
    };

    if (!wifi::isConnected()) {
        drawInstall("waiting for wifi…", -1);
        uint32_t deadline = millis() + 10000;
        while (!wifi::isConnected() && millis() < deadline) delay(100);
    }
    if (!wifi::isConnected()) {
        g_status    = Status::Failed;
        g_lastError = "wifi not connected";
        persistResult();
        drawInstall("wifi unavailable", -1);
        delay(1800);
        restoreUi();
        return;
    }

    drawInstall("checking manifest…", -1);
    WiFiClientSecure client;
    client.setInsecure();

    std::string latest, builtAt;
    if (!fetchManifest(client, latest, builtAt)) {
        g_status    = Status::Failed;
        // g_lastError set by fetchManifest
        persistResult();
        drawInstall(g_lastError.c_str(), -1);
        delay(2200);
        restoreUi();
        return;
    }

    if (latest == CLAWD_BUILD_SHA) {
        g_status         = Status::UpToDate;
        g_latest         = latest;
        g_latestBuiltAt  = builtAt;
        g_lastError      = "";
        g_lastCheckEpoch = (uint32_t)time(nullptr);
        persistResult();
        drawInstall("already up to date", 100);
        delay(1500);
        restoreUi();
        return;
    }

    Serial.printf("[updater] live: flashing %s (have %s)\n",
                  latest.c_str(), CLAWD_BUILD_SHA);
    g_status        = Status::Downloading;
    g_latest        = latest;
    g_latestBuiltAt = builtAt;
    g_progressHeader = "OTA UPDATE";
    g_progressPhase  = "downloading…";
    drawInstall(g_progressPhase, 0);

    markFlashPending(latest);

    httpUpdate.rebootOnUpdate(true);
    httpUpdate.onProgress(onFlashProgress);
    httpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    auto ret = httpUpdate.update(client, FIRMWARE_URL);
    // Success reboots inside httpUpdate.update(). Anything below is failure.
    (void)ret;

    std::string err = httpUpdate.getLastErrorString().c_str();
    if (err.empty()) err = "flash failed";
    g_status    = Status::Failed;
    g_lastError = err;
    persistResult();

    // Clear pending — flash never completed, so we shouldn't roll back on
    // the next boot.
    {
        Preferences prefs;
        prefs.begin("updater", false);
        prefs.putBool("pending", false);
        prefs.putInt("boots", 0);
        prefs.end();
    }

    drawInstall(err.c_str(), -1);
    delay(2500);
    restoreUi();
}

Status      status()           { return g_status; }
const char* statusText()       { return statusTextFor(g_status); }
const char* currentVersion()   { return CLAWD_BUILD_SHA; }
const char* currentBuiltAt()   { return CLAWD_BUILD_DATE; }
std::string latestVersion()    { return g_latest; }
std::string latestBuiltAt()    { return g_latestBuiltAt; }
uint32_t    lastCheckMs()      { return g_lastCheckMs; }
uint32_t    lastCheckEpoch()   { return g_lastCheckEpoch; }
std::string lastError()        { return g_lastError; }

void scheduleRecoveryUpdate() {
    Preferences prefs;
    prefs.begin("updater", false);
    prefs.putBool("recovery", true);
    prefs.putString("rec_fail", "");
    prefs.end();
    Serial.println("[updater] scheduled recovery-boot update");
    delay(100);
    ESP.restart();
}

bool isRecoveryBoot() {
    Preferences prefs;
    prefs.begin("updater", true);   // read-only
    bool v = prefs.getBool("recovery", false);
    prefs.end();
    return v;
}

[[noreturn]] void runRecovery() { runRecoveryImpl(); }

}  // namespace updater
