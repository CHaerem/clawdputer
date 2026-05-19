// GitOps self-update — robust pipeline that never gets stuck.
//
// Flow (each step bounded by a wall-clock timeout):
//   1. Preparing      — pause BLE + release canvas + pause SD to free
//                       contiguous heap for the TLS handshake.
//   2. WaitingForWifi — up to 10 s; fails fast if STA never associates.
//   3. FetchingManifest — pull manifest.json (single source of truth:
//                       sha / date / size / sha256 / notes / url) with
//                       up to 3 retries + exponential backoff. Falls
//                       back to legacy version.txt if the JSON file is
//                       missing (older releases pre-dating schema v1).
//   4. ComparingVersions — short-circuit if device sha matches manifest
//                       or device is newer.
//   5. Downloading    — stream firmware.bin straight into the inactive
//                       OTA partition while feeding every chunk through
//                       a mbedtls_sha256 context. Stall watchdog aborts
//                       if 20 s pass without a byte; 3 retries with
//                       backoff before declaring failure.
//   6. Verifying      — compare computed SHA-256 against manifest.sha256
//                       BEFORE Update.end(true) — partition boot pointer
//                       is not flipped on mismatch, so a corrupted
//                       download can never brick the device.
//   7. Rebooting      — Update.end(true) flipped the boot partition;
//                       NVS "pending" flag was written earlier; restart.
//
// TLS:  WiFiClientSecure attaches the IDF CA bundle
// (esp_crt_bundle_attach via setCACertBundle) so github.com and
// releases.githubusercontent.com are validated against ~30 bundled
// Mozilla NSS roots. setInsecure() is gone.
//
// Rollback: pre-flash we write {pending=true, boots=0, target=sha} to
// NVS. updater::begin() on the next boot increments boots; if >3 we
// switch boot partition back. tick() clears pending after 30 s of
// healthy main loop.

#include "updater.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <M5Cardputer.h>
#include <Preferences.h>
#include <Update.h>
#include <WiFiClientSecure.h>
#include <esp_ota_ops.h>
#include <mbedtls/sha256.h>
#include <time.h>

#include <algorithm>
#include <cstdio>

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

constexpr const char* MANIFEST_JSON_URL =
    "https://github.com/CHaerem/clawdputer/releases/latest/download/manifest.json";
constexpr const char* MANIFEST_TXT_URL =
    "https://github.com/CHaerem/clawdputer/releases/latest/download/version.txt";
constexpr const char* DEFAULT_FIRMWARE_URL =
    "https://github.com/CHaerem/clawdputer/releases/latest/download/firmware.bin";

constexpr uint32_t HEALTH_CONFIRM_MS    = 30UL * 1000UL;
constexpr int      MAX_BOOT_ATTEMPTS    = 3;
constexpr uint32_t BG_FIRST_CHECK_MS    = 60UL * 1000UL;
constexpr uint32_t BG_CHECK_INTERVAL_MS = 6UL * 60UL * 60UL * 1000UL;
constexpr size_t   BG_MIN_LARGEST_BLOCK = 28UL * 1024UL;

constexpr int      NET_RETRIES            = 3;
constexpr uint32_t NET_BACKOFF_BASE_MS    = 1000;
// HTTPClient::setTimeout takes uint16_t (per-read stall, milliseconds),
// so it has to fit in 65535. Total download wall-clock is enforced by
// DOWNLOAD_BUDGET_MS in the streaming loop below.
constexpr uint16_t MANIFEST_HTTP_TIMEOUT  = 15000;
constexpr uint16_t DOWNLOAD_HTTP_TIMEOUT  = 30000;
constexpr uint32_t DOWNLOAD_BUDGET_MS     = 120UL * 1000UL;
constexpr uint32_t DOWNLOAD_STALL_MS      = 20000;
constexpr uint32_t WIFI_WAIT_MS           = 10000;
constexpr uint32_t INSTALL_WALL_BUDGET_MS = 5UL * 60UL * 1000UL;

// IDF certificate bundle (compiled in by CONFIG_MBEDTLS_CERTIFICATE_BUNDLE).
// arduino-esp32's WiFiClientSecure::setCACertBundle attaches it to the
// mbedtls SSL context.
extern const uint8_t kRootCABundleStart[] asm("_binary_x509_crt_bundle_start");

struct Manifest {
    std::string sha;
    std::string date;
    uint32_t    size = 0;
    std::string sha256;
    std::string notes;
    std::string url;
};

bool isRealUpgrade(const std::string& latestSha, const std::string& latestDate) {
    if (latestSha == CLAWD_BUILD_SHA) return false;
    std::string deviceDate = CLAWD_BUILD_DATE;
    if (!deviceDate.empty() && !latestDate.empty()) {
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
const char*     g_progressPhase = "downloading…";

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

// Pre-flash NVS bookkeeping so the boot-attempt rollback can fire if
// the new image doesn't boot cleanly.
void markFlashPending(const std::string& targetSha) {
    Preferences prefs;
    prefs.begin("updater", false);
    prefs.putBool("pending", true);
    prefs.putInt("boots", 0);
    prefs.putString("target", targetSha.c_str());
    prefs.end();
}

void clearFlashPending() {
    Preferences prefs;
    prefs.begin("updater", false);
    prefs.putBool("pending", false);
    prefs.putInt("boots", 0);
    prefs.end();
}

// Apply the standard hardening to a WiFiClientSecure: CA bundle, bounded
// handshake/socket waits. All HTTPS in this file goes through one of
// these.
void configureClient(WiFiClientSecure& client) {
    client.setCACertBundle(kRootCABundleStart);
    client.setHandshakeTimeout(15);   // seconds
    client.setTimeout(15000);          // ms
}

// Drain an HTTPClient body into a String, then close. Used for small
// responses (manifest.json, version.txt) — not for firmware.bin.
bool httpGetString(WiFiClientSecure& client, const char* url, String& out,
                   int& httpCode, uint16_t timeoutMs) {
    HTTPClient http;
    http.setConnectTimeout(timeoutMs);
    http.setTimeout(timeoutMs);
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    if (!http.begin(client, url)) {
        httpCode = -1;
        return false;
    }
    httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        http.end();
        return false;
    }
    out = http.getString();
    http.end();
    return true;
}

bool fetchManifestJson(WiFiClientSecure& client, Manifest& out) {
    String body;
    int code = 0;
    if (!httpGetString(client, MANIFEST_JSON_URL, body, code, MANIFEST_HTTP_TIMEOUT)) {
        char buf[64];
        snprintf(buf, sizeof(buf), "manifest.json http %d", code);
        g_lastError = buf;
        return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err) {
        char buf[80];
        snprintf(buf, sizeof(buf), "manifest parse: %s", err.c_str());
        g_lastError = buf;
        return false;
    }
    out.sha    = doc["sha"]    | "";
    out.date   = doc["date"]   | "";
    out.size   = doc["size"]   | 0u;
    out.sha256 = doc["sha256"] | "";
    out.notes  = doc["notes"]  | "";
    out.url    = doc["url"]    | "";
    if (out.url.empty()) out.url = DEFAULT_FIRMWARE_URL;
    if (out.sha.empty()) {
        g_lastError = "manifest: missing sha";
        return false;
    }
    if (out.sha256.empty() || out.size == 0) {
        g_lastError = "manifest: missing sha256/size";
        return false;
    }
    return true;
}

// version.txt fallback. Two lines: sha + date. No sha256/size — the
// download path skips verification (degraded mode for releases predating
// schema v1). Used only if manifest.json 404s.
bool fetchManifestLegacy(WiFiClientSecure& client, Manifest& out) {
    String body;
    int code = 0;
    if (!httpGetString(client, MANIFEST_TXT_URL, body, code, MANIFEST_HTTP_TIMEOUT)) {
        char buf[64];
        snprintf(buf, sizeof(buf), "version.txt http %d", code);
        g_lastError = buf;
        return false;
    }
    body.trim();
    int nl = body.indexOf('\n');
    if (nl < 0) {
        out.sha = body.c_str();
    } else {
        String first  = body.substring(0, nl);
        String second = body.substring(nl + 1);
        first.trim(); second.trim();
        int nl2 = second.indexOf('\n');
        if (nl2 >= 0) second = second.substring(0, nl2);
        out.sha  = first.c_str();
        out.date = second.c_str();
    }
    out.url = DEFAULT_FIRMWARE_URL;
    if (out.sha.empty()) {
        g_lastError = "version.txt: empty sha";
        return false;
    }
    Serial.println("[updater] using legacy version.txt manifest (no sha256 verify)");
    return true;
}

bool fetchManifestWithRetries(WiFiClientSecure& client, Manifest& out) {
    for (int i = 0; i < NET_RETRIES; i++) {
        g_lastError.clear();
        if (fetchManifestJson(client, out)) return true;
        // Only fall through to version.txt if json was unreachable (404
        // or network), not if it parsed but was malformed.
        if (g_lastError.find("http 404") != std::string::npos ||
            g_lastError.find("http -") != std::string::npos) {
            if (fetchManifestLegacy(client, out)) return true;
        }
        if (i + 1 < NET_RETRIES) {
            uint32_t backoff = NET_BACKOFF_BASE_MS << i;
            Serial.printf("[updater] manifest retry %d/%d in %ums (last: %s)\n",
                          i + 1, NET_RETRIES, (unsigned)backoff,
                          g_lastError.c_str());
            delay(backoff);
        }
    }
    return false;
}

// Stream firmware into the inactive OTA partition while computing
// SHA-256 inline. On mismatch we call Update.abort() so the boot
// partition pointer is NOT flipped — a corrupted download cannot brick
// the device.
//
// Returns true iff Update.end(true) has committed the new boot partition
// and the caller can reboot.
bool downloadAndVerifyOnce(WiFiClientSecure& client, const Manifest& m) {
    HTTPClient http;
    http.setConnectTimeout(DOWNLOAD_HTTP_TIMEOUT);
    http.setTimeout(DOWNLOAD_HTTP_TIMEOUT);
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    if (!http.begin(client, m.url.c_str())) {
        g_lastError = "download begin failed";
        return false;
    }
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        char buf[64];
        snprintf(buf, sizeof(buf), "download http %d", code);
        g_lastError = buf;
        http.end();
        return false;
    }
    int contentLength = http.getSize();
    if (contentLength <= 0) contentLength = (int)m.size;
    if (contentLength <= 0) {
        g_lastError = "download: unknown size";
        http.end();
        return false;
    }
    if (m.size && (uint32_t)contentLength != m.size) {
        char buf[80];
        snprintf(buf, sizeof(buf), "size mismatch %d != %u",
                 contentLength, (unsigned)m.size);
        g_lastError = buf;
        http.end();
        return false;
    }

    if (!Update.begin((size_t)contentLength, U_FLASH)) {
        char buf[96];
        snprintf(buf, sizeof(buf), "Update.begin: %s", Update.errorString());
        g_lastError = buf;
        http.end();
        return false;
    }

    mbedtls_sha256_context shaCtx;
    mbedtls_sha256_init(&shaCtx);
    mbedtls_sha256_starts(&shaCtx, 0);  // 0 = SHA-256 (not SHA-224)

    WiFiClient*  stream         = http.getStreamPtr();
    uint8_t      buf[2048];
    size_t       written        = 0;
    uint32_t     lastProgressMs = 0;
    uint32_t     lastDataMs     = millis();
    uint32_t     startMs        = millis();
    int          lastPct        = -1;

    auto teardown = [&](const char* err) {
        Update.abort();
        mbedtls_sha256_free(&shaCtx);
        http.end();
        if (err) g_lastError = err;
    };

    while (written < (size_t)contentLength) {
        if (!http.connected() && !stream->available()) {
            teardown("connection dropped");
            return false;
        }
        size_t avail = stream->available();
        if (avail > 0) {
            int n = stream->readBytes(buf, std::min(avail, sizeof(buf)));
            if (n > 0) {
                if (Update.write(buf, (size_t)n) != (size_t)n) {
                    char errbuf[96];
                    snprintf(errbuf, sizeof(errbuf), "flash write: %s",
                             Update.errorString());
                    teardown(errbuf);
                    return false;
                }
                mbedtls_sha256_update(&shaCtx, buf, (size_t)n);
                written += (size_t)n;
                lastDataMs = millis();
                if (millis() - lastProgressMs > 200) {
                    int pct = (int)((int64_t)written * 100 / contentLength);
                    if (pct != lastPct) {
                        drawInstall(g_progressPhase, pct);
                        lastPct = pct;
                    }
                    lastProgressMs = millis();
                }
            }
        } else {
            if (millis() - lastDataMs > DOWNLOAD_STALL_MS) {
                teardown("download stalled");
                return false;
            }
            delay(1);
        }
        if (millis() - startMs > DOWNLOAD_BUDGET_MS) {
            teardown("download timeout");
            return false;
        }
    }

    http.end();

    if (written != (size_t)contentLength) {
        Update.abort();
        mbedtls_sha256_free(&shaCtx);
        char errbuf[80];
        snprintf(errbuf, sizeof(errbuf), "truncated %u/%d",
                 (unsigned)written, contentLength);
        g_lastError = errbuf;
        return false;
    }

    uint8_t hash[32];
    mbedtls_sha256_finish(&shaCtx, hash);
    mbedtls_sha256_free(&shaCtx);

    char hashHex[65];
    for (int i = 0; i < 32; i++) {
        snprintf(hashHex + i * 2, 3, "%02x", hash[i]);
    }

    if (!m.sha256.empty() && m.sha256 != hashHex) {
        Update.abort();
        char errbuf[120];
        snprintf(errbuf, sizeof(errbuf),
                 "sha256 mismatch (got %.8s…, want %.8s…)",
                 hashHex, m.sha256.c_str());
        g_lastError = errbuf;
        Serial.printf("[updater] %s\n", errbuf);
        return false;
    }

    g_progressPhase = "committing…";
    drawInstall(g_progressPhase, 100);

    if (!Update.end(true)) {
        char errbuf[96];
        snprintf(errbuf, sizeof(errbuf), "Update.end: %s", Update.errorString());
        g_lastError = errbuf;
        return false;
    }

    Serial.printf("[updater] flash committed: %u bytes, sha256=%.16s…\n",
                  (unsigned)written, hashHex);
    return true;
}

bool downloadAndVerifyWithRetries(WiFiClientSecure& client, const Manifest& m) {
    for (int i = 0; i < NET_RETRIES; i++) {
        g_lastError.clear();
        if (downloadAndVerifyOnce(client, m)) return true;
        if (i + 1 < NET_RETRIES) {
            uint32_t backoff = NET_BACKOFF_BASE_MS << i;
            Serial.printf("[updater] download retry %d/%d in %ums (last: %s)\n",
                          i + 1, NET_RETRIES, (unsigned)backoff,
                          g_lastError.c_str());
            delay(backoff);
        }
    }
    return false;
}

// Run the manifest check (no flash) on the main thread, pausing BLE and
// the canvas if needed to free contiguous heap for the TLS handshake.
// Returns true if the fetch completed (regardless of whether a new
// version was found); false if we couldn't even attempt it.
bool runBackgroundCheck() {
    if (!wifi::isConnected()) return false;

    bool hadCanvas    = ui::canvasActive();
    bool wasBlePaused = ble::isPaused();
    bool blePeerActive = ble::isConnected(EventSource::NusLink) ||
                         ble::isConnected(EventSource::BridgeLink);

    size_t largest = ESP.getMaxAllocHeap();
    if (largest < BG_MIN_LARGEST_BLOCK) {
        if (blePeerActive) {
            Serial.printf("[updater] bg skip: largest=%u, BLE peer active\n",
                          (unsigned)largest);
            return false;
        }
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
    configureClient(client);
    Manifest m;
    bool ok = fetchManifestWithRetries(client, m);
    if (ok) {
        g_latest         = m.sha;
        g_latestBuiltAt  = m.date;
        g_lastError      = "";
        g_lastCheckMs    = millis();
        g_lastCheckEpoch = (uint32_t)time(nullptr);
        if (isRealUpgrade(m.sha, m.date)) {
            g_status = updater::Status::UpdateAvailable;
            Serial.printf("[updater] bg: update available %s -> %s\n",
                          CLAWD_BUILD_SHA, m.sha.c_str());
        } else {
            g_status = updater::Status::UpToDate;
            if (m.sha != CLAWD_BUILD_SHA) {
                Serial.printf("[updater] bg: device sha=%s newer than published %s\n",
                              CLAWD_BUILD_SHA, m.sha.c_str());
            }
        }
        persistResult();
    } else {
        Serial.printf("[updater] bg check failed: %s\n", g_lastError.c_str());
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
    if (!g_confirmed && millis() >= HEALTH_CONFIRM_MS) {
        Preferences prefs;
        prefs.begin("updater", false);
        if (prefs.getBool("pending", false)) {
            prefs.putBool("pending", false);
            prefs.putInt("boots", 0);
            Serial.printf("[updater] boot confirmed healthy (sha=%s)\n", CLAWD_BUILD_SHA);
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

    static uint32_t lastDrainMs = 0;
    if (millis() - lastDrainMs > 30000 && wifi::isConnected()
        && telemetry::pending()) {
        lastDrainMs = millis();
        telemetry::drain();
    }

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
    const uint32_t wallStart = millis();

    bool hadCanvas    = ui::canvasActive();
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

    auto fail = [&](const char* msg) {
        g_status = Status::Failed;
        if (g_lastError.empty()) g_lastError = msg;
        persistResult();
        clearFlashPending();
        drawInstall(g_lastError.c_str(), -1);
        delay(2500);
        restoreUi();
    };

    auto budgetBlown = [&]() {
        return millis() - wallStart > INSTALL_WALL_BUDGET_MS;
    };

    if (!wifi::isConnected()) {
        drawInstall("waiting for wifi…", -1);
        uint32_t deadline = millis() + WIFI_WAIT_MS;
        while (!wifi::isConnected() && millis() < deadline) delay(100);
    }
    if (!wifi::isConnected()) { fail("wifi unavailable"); return; }
    if (budgetBlown())        { fail("install budget exceeded"); return; }

    drawInstall("checking manifest…", -1);
    WiFiClientSecure client;
    configureClient(client);

    Manifest m;
    if (!fetchManifestWithRetries(client, m)) {
        fail(g_lastError.c_str());
        return;
    }
    Serial.printf("[updater] manifest: sha=%s date=%s size=%u sha256=%.8s%s\n",
                  m.sha.c_str(), m.date.c_str(),
                  (unsigned)m.size,
                  m.sha256.empty() ? "(absent)" : m.sha256.c_str(),
                  m.sha256.empty() ? "" : "…");

    if (!isRealUpgrade(m.sha, m.date)) {
        g_status         = Status::UpToDate;
        g_latest         = m.sha;
        g_latestBuiltAt  = m.date;
        g_lastError      = "";
        g_lastCheckEpoch = (uint32_t)time(nullptr);
        persistResult();
        const char* msg = (m.sha == CLAWD_BUILD_SHA)
                            ? "already up to date"
                            : "device is newer";
        drawInstall(msg, 100);
        delay(1500);
        restoreUi();
        return;
    }
    if (budgetBlown()) { fail("install budget exceeded"); return; }

    Serial.printf("[updater] live: flashing %s (have %s)\n",
                  m.sha.c_str(), CLAWD_BUILD_SHA);
    g_status        = Status::Downloading;
    g_latest        = m.sha;
    g_latestBuiltAt = m.date;
    g_progressPhase = m.sha256.empty() ? "downloading…" : "downloading (sha256)";
    drawInstall(g_progressPhase, 0);

    markFlashPending(m.sha);

    // Fresh client for the download — the manifest fetch above consumed
    // `client` for a github.com handshake; reusing it for the redirect
    // target (releases.githubusercontent.com) left residual TLS state
    // that could deadlock the second handshake on this hardware.
    WiFiClientSecure dlClient;
    configureClient(dlClient);
    if (!downloadAndVerifyWithRetries(dlClient, m)) {
        fail(g_lastError.c_str());
        return;
    }

    Serial.println("[updater] reboot into new image");
    drawInstall("rebooting…", 100);
    delay(500);
    ESP.restart();
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
