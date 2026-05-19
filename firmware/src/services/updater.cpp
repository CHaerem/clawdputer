// GitOps self-update: fetch the project's "latest" GitHub release manifest.
// If newer than the running build, stream firmware.bin into the inactive
// OTA partition via HTTPUpdate and reboot into it.
//
// Runs live from normal boot — the IDF-component build with mbedTLS
// DYNAMIC_BUFFER fits the github.com handshake in fragmented heap.
// installNow() pauses BLE + releases the canvas + pauses SD to free
// contiguous bytes for HTTPUpdate's internal buffers, then flashes in place.
//
// A background FreeRTOS task (updater_bg) polls the manifest periodically so
// the statusbar can show "update available" without the user blindly tapping
// "check & install →".
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
constexpr uint32_t HEALTH_CONFIRM_MS    = 30UL * 1000UL;
constexpr int      MAX_BOOT_ATTEMPTS    = 3;
constexpr uint32_t BG_FIRST_CHECK_MS    = 60UL * 1000UL;            // 60s after boot
constexpr uint32_t BG_CHECK_INTERVAL_MS = 6UL * 60UL * 60UL * 1000UL; // 6h thereafter
// Even with mbedTLS DYNAMIC_BUFFER the github.com handshake needs a
// contiguous working block. Empirically ~12 KB largest free isn't enough
// and crashes the wifi driver. Skip the check below this floor and try
// again next interval — the manual "check & install →" path pauses BLE
// + canvas which clears the headroom regardless.
constexpr size_t   BG_MIN_LARGEST_BLOCK = 28UL * 1024UL;

// Decides whether a published release should be flagged as an update worth
// installing. SHA mismatch alone isn't enough — if the device's build date
// is on or after the published one, this is a downgrade (e.g. a local USB
// flash that ran ahead of CI). Falls back to plain SHA comparison when
// either date is missing.
bool isRealUpgrade(const std::string& latestSha, const std::string& latestDate) {
    if (latestSha == CLAWD_BUILD_SHA) return false;
    std::string deviceDate = CLAWD_BUILD_DATE;
    if (!deviceDate.empty() && !latestDate.empty()) {
        // ISO 8601 YYYY-MM-DD lex-compares correctly.
        return latestDate > deviceDate;
    }
    return true;
}

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
// released it to free contiguous heap for HTTPUpdate's buffers).
void drawInstall(const char* phase, int pct) {
    auto& d = M5Cardputer.Display;
    d.fillScreen(BLACK);
    d.setTextSize(2);
    d.setTextColor(WHITE);
    d.setCursor(8, 8);
    d.print("OTA UPDATE");
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

// HTTPUpdate's onProgress callback has no userdata pointer, so the painter
// reads the phase string from this global. Set by installNow() before the
// flash starts.
const char* g_progressPhase = "downloading…";

void onFlashProgress(int cur, int total) {
    static int lastPct = -1;
    if (total <= 0) {
        // Content-Length missing (chunked transfer through the GitHub
        // redirect). Show an indeterminate bar with the byte count so
        // the user knows we haven't hung.
        char buf[40];
        snprintf(buf, sizeof(buf), "%s %dKB", g_progressPhase, cur / 1024);
        drawInstall(buf, -1);
        return;
    }
    int pct = (int)((int64_t)cur * 100 / total);
    if (pct == lastPct) return;
    lastPct = pct;
    drawInstall(g_progressPhase, pct);
}

// Pre-flash NVS bookkeeping so the boot-attempt rollback can fire if the
// new image doesn't boot cleanly.
void markFlashPending(const std::string& targetSha) {
    Preferences prefs;
    prefs.begin("updater", false);
    prefs.putBool("pending", true);
    prefs.putInt("boots", 0);
    prefs.putString("target", targetSha.c_str());
    prefs.end();
}

// Run the manifest check (no flash) on the main thread, pausing BLE and
// the canvas if needed to free contiguous heap for the TLS handshake.
// Returns true if the fetch completed (regardless of whether a new version
// was found); false if we couldn't even attempt it.
bool runBackgroundCheck() {
    if (!wifi::isConnected()) return false;

    bool hadCanvas    = ui::canvasActive();
    bool wasBlePaused = ble::isPaused();

    // Only disturb BLE if no peer is actually using it. The buddy protocol
    // and clawd-bridge link each show up as NusLink / BridgeLink.
    bool blePeerActive = ble::isConnected(EventSource::NusLink) ||
                         ble::isConnected(EventSource::BridgeLink);

    size_t largest = ESP.getMaxAllocHeap();
    if (largest < BG_MIN_LARGEST_BLOCK) {
        if (blePeerActive) {
            Serial.printf("[updater] bg skip: largest=%u, BLE peer active\n",
                          (unsigned)largest);
            return false;
        }
        // Free contiguous space.
        if (hadCanvas)     ui::releaseCanvas();
        if (!wasBlePaused) ble::pause();
        delay(50);
        largest = ESP.getMaxAllocHeap();
    }

    auto restore = [&]() {
        if (!wasBlePaused) ble::resume();
        if (hadCanvas)     ui::tryAcquireCanvas();
    };

    if (largest < BG_MIN_LARGEST_BLOCK) {
        Serial.printf("[updater] bg skip: even after pause largest=%u\n",
                      (unsigned)largest);
        restore();
        return false;
    }

    Serial.printf("[updater] bg check: free=%u largest=%u\n",
                  (unsigned)ESP.getFreeHeap(), (unsigned)largest);
    WiFiClientSecure client;
    client.setInsecure();
    client.setHandshakeTimeout(15);
    client.setTimeout(15000);
    std::string latest, builtAt;
    bool ok = fetchManifest(client, latest, builtAt);
    if (ok) {
        g_latest         = latest;
        g_latestBuiltAt  = builtAt;
        g_lastError      = "";
        g_lastCheckMs    = millis();
        g_lastCheckEpoch = (uint32_t)time(nullptr);
        if (isRealUpgrade(latest, builtAt)) {
            g_status = updater::Status::UpdateAvailable;
            Serial.printf("[updater] bg: update available %s -> %s\n",
                          CLAWD_BUILD_SHA, latest.c_str());
        } else {
            g_status = updater::Status::UpToDate;
            if (latest != CLAWD_BUILD_SHA) {
                Serial.printf("[updater] bg: device sha=%s newer than published %s (date %s vs %s)\n",
                              CLAWD_BUILD_SHA, latest.c_str(),
                              CLAWD_BUILD_DATE, builtAt.c_str());
            }
        }
        persistResult();
    } else {
        Serial.printf("[updater] bg check failed: %s\n",
                      g_lastError.c_str());
        // Don't persist failure — likely transient (wifi flake). Last good
        // status stays in NVS so the UI doesn't flap.
    }
    restore();
    return true;
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

    if (pending) {
        bootAttempts++;
        prefs.putInt("boots", bootAttempts);
        Serial.printf("[updater] pending flash, boot attempt %d/%d\n",
                      bootAttempts, MAX_BOOT_ATTEMPTS);
        prefs.end();

        if (bootAttempts > MAX_BOOT_ATTEMPTS) {
            rollbackNow();   // does not return
        }
    } else {
        prefs.end();
    }

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

    // Periodic update check: BG_FIRST_CHECK_MS after boot, then every
    // BG_CHECK_INTERVAL_MS. Runs on the main thread so it can safely pause
    // BLE + canvas (the only way to free enough contiguous heap for the
    // mbedTLS handshake on this hardware). UI freezes for ~2-3 s during
    // the fetch; acceptable at this cadence.
    static uint32_t lastCheckAttemptMs = 0;
    static bool     didFirstCheck      = false;
    uint32_t        now                = millis();
    uint32_t        dueAt              = didFirstCheck
                                       ? lastCheckAttemptMs + BG_CHECK_INTERVAL_MS
                                       : BG_FIRST_CHECK_MS;
    if (now >= dueAt && wifi::isConnected()) {
        lastCheckAttemptMs = now;
        didFirstCheck      = true;
        runBackgroundCheck();
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
    // Bound the TLS handshake + socket waits so a stalled connection
    // surfaces as an error instead of hanging the UI forever (the
    // "download never finishes" symptom — github.com → releases.github
    // usercontent.com is a fresh handshake on a different host, and
    // without these the second handshake can sit waiting for bytes
    // that never arrive).
    client.setHandshakeTimeout(15);   // seconds
    client.setTimeout(15000);          // ms — socket read/connect

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

    if (!isRealUpgrade(latest, builtAt)) {
        g_status         = Status::UpToDate;
        g_latest         = latest;
        g_latestBuiltAt  = builtAt;
        g_lastError      = "";
        g_lastCheckEpoch = (uint32_t)time(nullptr);
        persistResult();
        const char* msg = (latest == CLAWD_BUILD_SHA)
                            ? "already up to date"
                            : "device is newer";
        drawInstall(msg, 100);
        delay(1500);
        restoreUi();
        return;
    }

    Serial.printf("[updater] live: flashing %s (have %s)\n",
                  latest.c_str(), CLAWD_BUILD_SHA);
    g_status        = Status::Downloading;
    g_latest        = latest;
    g_latestBuiltAt = builtAt;
    g_progressPhase = "downloading…";
    drawInstall(g_progressPhase, 0);

    markFlashPending(latest);

    // Fresh client for the firmware stream. The manifest fetch above
    // already consumed `client` for a github.com handshake; reusing it
    // for the redirect-followed releases.githubusercontent.com stream
    // left residual TLS state that could deadlock the second handshake
    // on this hardware (~31 KB largest free block, fragmented). A fresh
    // WiFiClientSecure starts clean and inherits the same timeouts.
    WiFiClientSecure dlClient;
    dlClient.setInsecure();
    dlClient.setHandshakeTimeout(15);
    dlClient.setTimeout(15000);

    httpUpdate.rebootOnUpdate(true);
    httpUpdate.onProgress(onFlashProgress);
    httpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    auto ret = httpUpdate.update(dlClient, FIRMWARE_URL);
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

}  // namespace updater
