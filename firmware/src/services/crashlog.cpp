#include "crashlog.h"

#include <Arduino.h>
#include <Preferences.h>
#include <SD.h>
#include <esp_system.h>
#include <stdarg.h>

#include "sd.h"
#include "settings.h"

namespace {

constexpr size_t       RING_BYTES         = 4096;
constexpr size_t       SD_FLUSH_BYTES     = 1024;
constexpr uint32_t     SD_FLUSH_INTERVAL  = 2000;
constexpr size_t       SD_LOG_MAX_BYTES   = 64 * 1024;
constexpr size_t       SD_LOG_KEEP_BYTES  = 32 * 1024;
constexpr const char*  DIR_PATH           = "/clawd";
constexpr const char*  CURRENT_LOG_PATH   = "/clawd/serial.log";
constexpr const char*  PREVIOUS_LOG_PATH  = "/clawd/serial.prev.log";

bool        g_enabled         = false;
char        g_ring[RING_BYTES];
size_t      g_ringHead        = 0;   // next write index
size_t      g_ringFilled      = 0;   // bytes valid in ring (≤ RING_BYTES)

// Pending bytes not yet flushed to SD. Stored as a separate growing
// buffer so a partial flush failure doesn't corrupt the ring.
std::string g_pending;
uint32_t    g_lastFlushMs     = 0;

bool        g_sdLogActive     = false;
size_t      g_sdLogSize       = 0;

esp_reset_reason_t g_priorReason = ESP_RST_UNKNOWN;
std::string        g_priorApp;
std::string        g_priorTail;
bool               g_priorCrashAcked = false;

const char* reasonString(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_PANIC:    return "panic";
        case ESP_RST_INT_WDT:  return "int_wdt";
        case ESP_RST_TASK_WDT: return "task_wdt";
        case ESP_RST_WDT:      return "wdt";
        case ESP_RST_BROWNOUT: return "brownout";
        default:               return "";
    }
}

bool isCrashReason(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_PANIC:
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT:
        case ESP_RST_BROWNOUT:
            return true;
        default:
            return false;
    }
}

void ringAppend(const char* data, size_t n) {
    for (size_t i = 0; i < n; i++) {
        g_ring[g_ringHead] = data[i];
        g_ringHead = (g_ringHead + 1) % RING_BYTES;
        if (g_ringFilled < RING_BYTES) g_ringFilled++;
    }
}

std::string ringSnapshot() {
    if (g_ringFilled == 0) return "";
    std::string out;
    out.reserve(g_ringFilled);
    if (g_ringFilled < RING_BYTES) {
        out.append(g_ring, g_ringFilled);
    } else {
        out.append(g_ring + g_ringHead, RING_BYTES - g_ringHead);
        out.append(g_ring, g_ringHead);
    }
    return out;
}

void rotateSdLogIfNeeded() {
    if (!sd::isAvailable()) return;
    if (g_sdLogSize < SD_LOG_MAX_BYTES) return;
    File f = SD.open(CURRENT_LOG_PATH, FILE_READ);
    if (!f) return;
    size_t skip = g_sdLogSize - SD_LOG_KEEP_BYTES;
    f.seek(skip);
    std::string tail;
    tail.reserve(SD_LOG_KEEP_BYTES);
    uint8_t buf[512];
    while (f.available()) {
        int got = f.read(buf, sizeof(buf));
        if (got <= 0) break;
        tail.append((const char*)buf, got);
    }
    f.close();
    File w = SD.open(CURRENT_LOG_PATH, FILE_WRITE);
    if (!w) return;
    w.write((const uint8_t*)tail.data(), tail.size());
    w.close();
    g_sdLogSize = tail.size();
}

void flushPendingToSd() {
    if (g_pending.empty()) return;
    if (!sd::isAvailable() || !g_sdLogActive) {
        g_pending.clear();
        return;
    }
    File f = SD.open(CURRENT_LOG_PATH, FILE_APPEND);
    if (!f) {
        g_sdLogActive = false;
        return;
    }
    size_t wrote = f.write((const uint8_t*)g_pending.data(), g_pending.size());
    f.close();
    if (wrote == g_pending.size()) {
        g_sdLogSize += wrote;
        g_pending.clear();
        rotateSdLogIfNeeded();
    } else {
        g_sdLogActive = false;
    }
    g_lastFlushMs = millis();
}

void rollPriorLog() {
    if (!sd::isAvailable()) return;
    if (!SD.exists(DIR_PATH)) SD.mkdir(DIR_PATH);
    if (SD.exists(CURRENT_LOG_PATH)) {
        if (SD.exists(PREVIOUS_LOG_PATH)) SD.remove(PREVIOUS_LOG_PATH);
        SD.rename(CURRENT_LOG_PATH, PREVIOUS_LOG_PATH);
    }
}

std::string readFile(const char* path, size_t maxBytes) {
    if (!sd::isAvailable()) return "";
    File f = SD.open(path, FILE_READ);
    if (!f) return "";
    size_t total = f.size();
    size_t skip  = total > maxBytes ? total - maxBytes : 0;
    if (skip) f.seek(skip);
    std::string out;
    out.reserve(total - skip);
    uint8_t buf[512];
    while (f.available()) {
        int got = f.read(buf, sizeof(buf));
        if (got <= 0) break;
        out.append((const char*)buf, got);
    }
    f.close();
    return out;
}

}  // namespace

namespace crashlog {

void begin() {
    if (!settings::reportEnabled()) {
        Serial.println("[crashlog] disabled (settings.reportEnabled=false)");
        return;
    }
    g_enabled = true;
    g_priorReason = esp_reset_reason();

    Preferences p;
    p.begin("crashlog", true);
    g_priorApp = p.getString("last_app", "").c_str();
    p.end();

    Serial.printf("[crashlog] reset_reason=%d (%s) prior_app=%s\n",
                  (int)g_priorReason, reasonString(g_priorReason),
                  g_priorApp.c_str());

    if (sd::isAvailable()) {
        if (!SD.exists(DIR_PATH)) SD.mkdir(DIR_PATH);
        if (isCrashReason(g_priorReason)) {
            g_priorTail = readFile(CURRENT_LOG_PATH, 8 * 1024);
        }
        rollPriorLog();
        g_sdLogActive = true;
        g_sdLogSize   = 0;
    }
}

void tick() {
    if (!g_enabled) return;
    if (g_pending.empty()) return;
    uint32_t now = millis();
    if (g_pending.size() >= SD_FLUSH_BYTES ||
        now - g_lastFlushMs >= SD_FLUSH_INTERVAL) {
        flushPendingToSd();
    }
}

void noteAppEntered(const char* appId) {
    if (!g_enabled || !appId) return;
    Preferences p;
    p.begin("crashlog", false);
    p.putString("last_app", appId);
    p.end();
}

void logf(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if ((size_t)n >= sizeof(buf)) n = sizeof(buf) - 1;
    Serial.write((const uint8_t*)buf, n);
    Serial.write('\n');
    if (!g_enabled) return;
    ringAppend(buf, n);
    ringAppend("\n", 1);
    if (g_sdLogActive) {
        g_pending.append(buf, n);
        g_pending.push_back('\n');
    }
}

bool hasPriorCrash() {
    return g_enabled && !g_priorCrashAcked && isCrashReason(g_priorReason);
}

const char* priorReason() {
    return reasonString(g_priorReason);
}

std::string priorApp() { return g_priorApp; }

std::string serialTail(size_t tailBytes) {
    if (!g_priorTail.empty()) {
        if (g_priorTail.size() > tailBytes)
            return g_priorTail.substr(g_priorTail.size() - tailBytes);
        return g_priorTail;
    }
    std::string ring = ringSnapshot();
    if (ring.size() > tailBytes)
        return ring.substr(ring.size() - tailBytes);
    return ring;
}

void acknowledgePriorCrash() {
    g_priorCrashAcked = true;
}

}  // namespace crashlog
