// GitOps self-update: poll the project's "latest" GitHub release for a
// newer firmware build than the one currently running. When found, stream
// firmware.bin into the inactive OTA partition via HTTPUpdate and reboot.
//
// Rollback: before flashing we mark a "pending" flag in NVS. After reboot,
// the new image must call confirmHealthy() (we do that after the device
// has run 30s without crash). If it fails to confirm across three boot
// attempts, we manually point esp_ota at the previous partition and
// reset — restoring the last-known-good firmware.
//
// State persistence: the last check result (status / sha / error / epoch)
// is written to NVS at the end of every check, so the Settings UI can
// surface real status immediately after boot — before any new check runs
// — and can clearly distinguish "never checked" from "checked, all good".

#include "updater.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <M5Cardputer.h>
#include <Preferences.h>
#include <WiFiClientSecure.h>
#include <esp_ota_ops.h>
#include <time.h>

#include "settings.h"
#include "wifi.h"
#include "github.h"
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
constexpr uint32_t CHECK_INTERVAL_MS = 5UL * 60UL * 1000UL;
constexpr uint32_t HEALTH_CONFIRM_MS = 30UL * 1000UL;
constexpr int      MAX_BOOT_ATTEMPTS = 3;

updater::Status g_status      = updater::Status::Idle;
uint32_t        g_lastCheckMs = 0;
uint32_t        g_lastCheckEpoch = 0;
bool            g_forceCheck  = true;
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

constexpr uint32_t REPORT_DEDUP_SECS = 24 * 3600;

bool reportDedupAllow(const char* kind) {
    Preferences prefs;
    prefs.begin("updater", false);
    // NVS keys cap at 15 chars; "rep_" + kind must fit. Strip non-alnum
    // from kind (e.g. "check-failed" -> "checkfailed") and clamp.
    String k;
    for (const char* p = kind; *p && k.length() < 11; ++p) {
        if (isalnum((unsigned char)*p)) k += *p;
    }
    String key = String("rep_") + k;
    uint32_t last = prefs.getUInt(key.c_str(), 0);
    uint32_t now  = (uint32_t)(millis() / 1000);
    bool allow = (last == 0) || (now > last && now - last >= REPORT_DEDUP_SECS);
    if (allow) prefs.putUInt(key.c_str(), now);
    prefs.end();
    return allow;
}

// File a GitHub issue for an OTA failure. Caller must have already
// released the canvas (so TLS has the contiguous heap it needs) and is
// responsible for reacquiring afterward. No-op when reporting is off,
// no PAT is sealed in, WiFi is down, or this kind fired recently.
void reportOtaFailure(const char* kind, const std::string& detail) {
    if (!settings::reportEnabled())  return;
    if (!github::hasToken())         return;
    if (!wifi::isConnected())        return;
    if (!reportDedupAllow(kind))     return;

    std::string title = std::string("[ota] ") + kind + ": " + detail;
    if (title.size() > 120) title = title.substr(0, 119) + "…";

    char hdr[640];
    snprintf(hdr, sizeof(hdr),
             "Automated OTA failure report.\n\n"
             "- **Kind:** `%s`\n"
             "- **Detail:** `%s`\n"
             "- **Installed build:** `%s` (%s)\n"
             "- **Latest seen:** `%s` (%s)\n"
             "- **Manifest URL:** %s\n"
             "- **Firmware URL:** %s\n"
             "- **Uptime:** %us\n"
             "- **Free heap:** %u\n"
             "- **Largest block:** %u\n\n"
             "_Filed automatically — see `firmware/src/services/updater.cpp`._\n",
             kind, detail.c_str(),
             CLAWD_BUILD_SHA, CLAWD_BUILD_DATE,
             g_latest.empty() ? "—" : g_latest.c_str(),
             g_latestBuiltAt.empty() ? "—" : g_latestBuiltAt.c_str(),
             MANIFEST_URL, FIRMWARE_URL,
             (unsigned)(millis() / 1000),
             (unsigned)ESP.getFreeHeap(),
             (unsigned)ESP.getMaxAllocHeap());

    Serial.printf("[updater] filing issue: %s\n", title.c_str());
    auto r = github::submitIssue(title, hdr, "auto-ota");
    if (r.ok) Serial.printf("[updater] issue #%d: %s\n",
                            r.issueNumber, r.issueUrl.c_str());
    else      Serial.printf("[updater] submit failed: %s\n", r.error.c_str());
}

void drawUpdating(int pct) {
    auto& d = M5Cardputer.Display;
    d.fillScreen(BLACK);
    d.setTextSize(2);
    d.setTextColor(WHITE);
    d.setCursor(8, 8);
    d.print("UPDATING");
    d.setTextSize(1);
    d.setCursor(8, 36);
    d.printf("self-flash %d%%", pct);
    int barX = 8, barY = 56, barW = 224, barH = 10;
    d.drawRect(barX, barY, barW, barH, WHITE);
    int fill = (int)((barW - 2) * pct / 100);
    d.fillRect(barX + 1, barY + 1, fill, barH - 2, 0x07E0);
    d.setCursor(8, 88);
    d.setTextColor(0x7BEF);
    d.print("do not unplug");
}

// Recovery-mode painter — header is "OTA RECOVERY" and the body shows
// the current phase plus a progress bar. Draws directly to the LCD; never
// touches the canvas sprite (which is what we skipped allocating to keep
// the heap unfragmented for the TLS handshake).
void drawRecovery(const char* phase, int pct) {
    auto& d = M5Cardputer.Display;
    d.fillScreen(BLACK);
    d.setTextSize(2);
    d.setTextColor(WHITE);
    d.setCursor(8, 8);
    d.print("OTA RECOVERY");
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

void runFlash(WiFiClientSecure& client, const std::string& target) {
    Serial.printf("[updater] flashing %s (have %s)\n",
                  target.c_str(), CLAWD_BUILD_SHA);
    g_status = updater::Status::Downloading;
    drawUpdating(0);

    // Mark the upcoming partition as pending verification BEFORE the flash,
    // so a crash mid-flash or a broken new image is recoverable.
    {
        Preferences prefs;
        prefs.begin("updater", false);
        prefs.putBool("pending", true);
        prefs.putInt("boots", 0);
        prefs.putString("target", target.c_str());
        prefs.end();
    }

    httpUpdate.rebootOnUpdate(true);
    httpUpdate.onProgress(onUpdateProgress);
    // GitHub serves the release artifact via a 302 to objects.githubusercontent.com.
    httpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
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
        persistResult();
        reportOtaFailure("flash-failed", g_lastError);
    }
}

void runCheck(bool installIfNewer) {
    if (!wifi::isConnected()) return;

    g_status    = updater::Status::Checking;
    g_lastError = "";

    // mbedTLS allocates from PSRAM (see main.cpp), so the handshake no
    // longer competes for the ~31 KB largest free internal block. No
    // canvas-release or bridge-pause dance needed.
    WiFiClientSecure client;
    client.setInsecure();

    std::string latest, builtAt;
    bool ok = fetchManifest(client, latest, builtAt);

    g_lastCheckMs = millis();
    if (wifi::timeSynced()) g_lastCheckEpoch = (uint32_t)time(nullptr);

    if (!ok) {
        g_status = updater::Status::Failed;
        Serial.printf("[updater] check failed: %s\n", g_lastError.c_str());
        persistResult();
        reportOtaFailure("check-failed", g_lastError);
        return;
    }

    g_latest        = latest;
    g_latestBuiltAt = builtAt;

    if (latest == CLAWD_BUILD_SHA) {
        g_status = updater::Status::UpToDate;
        Serial.printf("[updater] up to date (%s)\n", latest.c_str());
        persistResult();
        return;
    }

    if (!installIfNewer) {
        g_status = updater::Status::UpdateAvailable;
        Serial.printf("[updater] update available: %s (have %s) — waiting for user\n",
                      latest.c_str(), CLAWD_BUILD_SHA);
        persistResult();
        return;
    }

    if (ui::canvasActive()) ui::releaseCanvas();
    runFlash(client, latest);
}

// Recovery-mode globals — used by onRecoveryProgress so the painter knows
// which phase string to keep above the bar during the flash.
const char* g_recoveryPhase = "downloading…";

void onRecoveryProgress(int cur, int total) {
    if (total <= 0) return;
    static int lastPct = -1;
    int pct = (int)((int64_t)cur * 100 / total);
    if (pct == lastPct) return;
    lastPct = pct;
    drawRecovery(g_recoveryPhase, pct);
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
    g_recoveryPhase = "downloading…";
    drawRecovery(g_recoveryPhase, 0);

    // Same NVS bookkeeping as the normal-mode flash path so rollback works.
    {
        Preferences prefs;
        prefs.begin("updater", false);
        prefs.putBool("pending", true);
        prefs.putInt("boots", 0);
        prefs.putString("target", latest.c_str());
        prefs.end();
    }

    httpUpdate.rebootOnUpdate(true);
    httpUpdate.onProgress(onRecoveryProgress);
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

    if (!wifi::isConnected()) return;
    uint32_t now = millis();

    // Small defer after boot — gives WiFi connect scratch buffers time to
    // free before we open a TLS socket.
    if (now < 5000) return;
    bool autoUpdate = settings::autoUpdateEnabled();
    // Forced checks always run; periodic checks only when auto-update is
    // enabled in Settings.
    bool periodic = autoUpdate &&
                    (g_lastCheckMs == 0 ||
                     now - g_lastCheckMs >= CHECK_INTERVAL_MS);
    if (!g_forceCheck && !periodic) return;
    bool forced = g_forceCheck;
    g_forceCheck = false;
    // Auto-update on: flash immediately on a newer SHA (both periodic and
    // forced checks). Auto-update off: only report; user installs via
    // Settings → install update.
    runCheck(autoUpdate);
    // A user-triggered manual check with auto-update on is the same as a
    // periodic check — both auto-flash. (forced var kept for clarity.)
    (void)forced;
}

void checkNow()   { g_forceCheck = true; }
void installNow() { scheduleRecoveryUpdate(); }   // never returns

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
