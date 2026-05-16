#include "health.h"

#include <Arduino.h>
#include <M5Cardputer.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <array>
#include <string>

#include "crashlog.h"
#include "github.h"
#include "settings.h"
#include "wifi.h"

#ifndef CLAWD_BUILD_SHA
#define CLAWD_BUILD_SHA "unknown"
#endif

namespace health {

namespace {

constexpr uint32_t SAMPLE_INTERVAL_MS = 30 * 1000;
constexpr size_t   RING_CAP           = 240;        // ~2h at 30s
constexpr size_t   ISSUE_TAIL         = 30;         // samples attached
constexpr uint32_t DEDUP_MIN_SECS     = 24 * 3600;  // one issue per kind per day

constexpr uint32_t TH_LARGEST_BLOCK   = 25 * 1024;  // < 25 KB = TLS won't fit
constexpr uint32_t TH_MIN_FREE_HEAP   = 30 * 1024;
constexpr uint32_t TH_STACK_HWM       = 1024;       // < 1 KB headroom
constexpr int      TH_FRAG_RUN        = 3;          // consecutive low-block samples

struct Sample {
    uint32_t tSec;           // seconds since boot
    uint32_t freeHeap;
    uint32_t minFreeHeap;
    uint32_t largestBlock;
    uint32_t loopStackHwm;
    uint16_t battMv;
    uint8_t  battPct;
    char     appId[16];
};

std::array<Sample, RING_CAP> g_ring;
size_t   g_count    = 0;
size_t   g_head     = 0;
uint32_t g_lastSample = 0;
char     g_currentApp[16] = "boot";

int g_fragRun = 0;

// Persisted dedup (epoch-style, seconds since boot of *some* prior session).
// We just store the last-fired tick so a single device only refiles after
// reboot or the dedup window elapses. Good enough for our volume.
Preferences g_prefs;

bool dedupAllow(const char* kind) {
    g_prefs.begin("health", false);
    String key = String("la_") + kind;
    uint32_t last = g_prefs.getUInt(key.c_str(), 0);
    uint32_t now  = (uint32_t)(millis() / 1000);
    // Use absolute uptime as a coarse cooldown; surviving reboots is fine
    // because each boot resets millis and we re-allow once per boot anyway.
    bool allow = (last == 0) || (now > last && now - last >= DEDUP_MIN_SECS);
    if (allow) g_prefs.putUInt(key.c_str(), now);
    g_prefs.end();
    return allow;
}

const char* battLabel(int pct) { return pct < 0 ? "n/a" : nullptr; }

void takeSample(Sample& s) {
    s.tSec         = millis() / 1000;
    s.freeHeap     = ESP.getFreeHeap();
    s.minFreeHeap  = ESP.getMinFreeHeap();
    s.largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    s.loopStackHwm = uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t);
    s.battMv       = (uint16_t)M5Cardputer.Power.getBatteryVoltage();
    int pct        = M5Cardputer.Power.getBatteryLevel();
    s.battPct      = (uint8_t)(pct < 0 ? 0 : pct > 100 ? 100 : pct);
    strncpy(s.appId, g_currentApp, sizeof(s.appId) - 1);
    s.appId[sizeof(s.appId) - 1] = '\0';
}

void pushSample(const Sample& s) {
    g_ring[g_head] = s;
    g_head = (g_head + 1) % RING_CAP;
    if (g_count < RING_CAP) g_count++;
}

std::string formatSamplesTable() {
    // Markdown table with the most recent ISSUE_TAIL samples (oldest first).
    std::string out = "| t (s) | app | freeHeap | minFree | largest | stackHwm | batt mV | batt %% |\n";
    out += "|---|---|---|---|---|---|---|---|\n";
    size_t n = g_count < ISSUE_TAIL ? g_count : ISSUE_TAIL;
    size_t start = (g_head + RING_CAP - n) % RING_CAP;
    char line[160];
    for (size_t i = 0; i < n; i++) {
        const Sample& s = g_ring[(start + i) % RING_CAP];
        snprintf(line, sizeof(line),
                 "| %u | %s | %u | %u | %u | %u | %u | %u |\n",
                 (unsigned)s.tSec, s.appId,
                 (unsigned)s.freeHeap, (unsigned)s.minFreeHeap,
                 (unsigned)s.largestBlock, (unsigned)s.loopStackHwm,
                 (unsigned)s.battMv, (unsigned)s.battPct);
        out += line;
    }
    return out;
}

std::string buildBody(const char* kind, const char* trigger, const Sample& latest) {
    std::string body;
    body.reserve(2048);
    char hdr[512];
    snprintf(hdr, sizeof(hdr),
             "**Auto-filed by on-device health monitor.**\n\n"
             "- **Kind:** `%s`\n"
             "- **Trigger:** %s\n"
             "- **Build:** `%s`\n"
             "- **Uptime:** %us\n"
             "- **Active app:** `%s`\n"
             "- **Free heap:** %u\n"
             "- **Min free heap:** %u\n"
             "- **Largest block:** %u\n"
             "- **Loop stack HWM:** %u\n"
             "- **Battery:** %u mV (%u%%)\n"
             "- **Prior reset:** %s\n\n",
             kind, trigger, CLAWD_BUILD_SHA,
             (unsigned)latest.tSec, latest.appId,
             (unsigned)latest.freeHeap, (unsigned)latest.minFreeHeap,
             (unsigned)latest.largestBlock, (unsigned)latest.loopStackHwm,
             (unsigned)latest.battMv, (unsigned)latest.battPct,
             crashlog::priorReason());
    body = hdr;
    body += "### Recent samples\n\n";
    body += formatSamplesTable();
    body += "\n_Filed automatically — see `firmware/src/services/health.cpp`._\n";
    return body;
}

void fileIssue(const char* kind, const char* trigger, const Sample& latest) {
    if (!settings::reportEnabled()) return;
    if (!wifi::isConnected())       return;
    if (!dedupAllow(kind))          return;

    std::string title = std::string("[health] ") + kind + ": " + trigger;
    std::string body  = buildBody(kind, trigger, latest);

    Serial.printf("[health] filing issue: %s\n", title.c_str());
    auto r = github::submitIssue(title, body, "auto-health");
    if (r.ok) Serial.printf("[health] issue #%d: %s\n",
                            r.issueNumber, r.issueUrl.c_str());
    else      Serial.printf("[health] submit failed: %s\n", r.error.c_str());
}

void evaluate(const Sample& s) {
    if (s.largestBlock < TH_LARGEST_BLOCK) g_fragRun++;
    else                                   g_fragRun = 0;

    if (g_fragRun >= TH_FRAG_RUN) {
        char trig[96];
        snprintf(trig, sizeof(trig),
                 "largestBlock=%u < %u for %d consecutive samples",
                 (unsigned)s.largestBlock, (unsigned)TH_LARGEST_BLOCK, g_fragRun);
        fileIssue("heap-fragmentation", trig, s);
        g_fragRun = 0;
    }

    if (s.minFreeHeap < TH_MIN_FREE_HEAP) {
        char trig[96];
        snprintf(trig, sizeof(trig),
                 "minFreeHeap=%u < %u", (unsigned)s.minFreeHeap,
                 (unsigned)TH_MIN_FREE_HEAP);
        fileIssue("heap-low", trig, s);
    }

    if (s.loopStackHwm < TH_STACK_HWM) {
        char trig[96];
        snprintf(trig, sizeof(trig),
                 "loopStackHwm=%u < %u", (unsigned)s.loopStackHwm,
                 (unsigned)TH_STACK_HWM);
        fileIssue("stack-near-limit", trig, s);
    }
}

}  // namespace

void begin() {
    g_count = g_head = 0;
    g_lastSample = 0;
    g_fragRun = 0;
    Serial.printf("[health] enabled=%d report=%d\n",
                  (int)true, (int)settings::reportEnabled());

    if (crashlog::hasPriorCrash() && settings::reportEnabled()) {
        Sample s; takeSample(s);
        char trig[96];
        snprintf(trig, sizeof(trig), "prior reset reason=%s in app=%s",
                 crashlog::priorReason(),
                 crashlog::priorApp().empty() ? "?" : crashlog::priorApp().c_str());
        fileIssue("prior-crash", trig, s);
    }
}

void tick() {
    uint32_t now = millis();
    if (g_lastSample != 0 && now - g_lastSample < SAMPLE_INTERVAL_MS) return;
    g_lastSample = now;

    Sample s; takeSample(s);
    pushSample(s);
    Serial.printf("[health] t=%us heap=%u minFree=%u largest=%u stackHwm=%u batt=%umV app=%s\n",
                  (unsigned)s.tSec, (unsigned)s.freeHeap, (unsigned)s.minFreeHeap,
                  (unsigned)s.largestBlock, (unsigned)s.loopStackHwm,
                  (unsigned)s.battMv, s.appId);
    evaluate(s);
}

void noteAppEntered(const char* appId) {
    if (!appId) return;
    strncpy(g_currentApp, appId, sizeof(g_currentApp) - 1);
    g_currentApp[sizeof(g_currentApp) - 1] = '\0';
}

}  // namespace health
