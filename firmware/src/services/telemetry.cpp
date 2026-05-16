#include "telemetry.h"

#include <Arduino.h>
#include <Preferences.h>

#include "github.h"

namespace {

constexpr const char* NS = "telemetry";

// NVS String values cap at 4000 bytes. Issue bodies can run long; trim
// aggressively so a single enqueue never blows the slot.
constexpr size_t MAX_BODY = 3500;

}  // namespace

namespace telemetry {

void enqueue(const std::string& title, const std::string& body) {
    Preferences p;
    if (!p.begin(NS, false)) return;
    p.putString("title", title.c_str());
    std::string b = body;
    if (b.size() > MAX_BODY) {
        b.resize(MAX_BODY);
        b += "\n\n_[truncated]_\n";
    }
    p.putString("body", b.c_str());
    p.end();
    Serial.printf("[telemetry] queued: %s\n", title.c_str());
}

bool pending() {
    Preferences p;
    if (!p.begin(NS, true)) return false;
    bool has = p.isKey("title") && p.getString("title", "").length() > 0;
    p.end();
    return has;
}

void drain() {
    if (!github::hasToken()) return;

    std::string title, body;
    {
        Preferences p;
        if (!p.begin(NS, true)) return;
        title = p.getString("title", "").c_str();
        body  = p.getString("body",  "").c_str();
        p.end();
    }
    if (title.empty()) return;

    Serial.printf("[telemetry] draining: %s\n", title.c_str());
    auto r = github::submitIssue(title, body, "auto-telemetry");
    if (!r.ok) {
        Serial.printf("[telemetry] drain failed: %s (keeping queued)\n",
                      r.error.c_str());
        return;
    }
    Serial.printf("[telemetry] filed issue #%d: %s\n",
                  r.issueNumber, r.issueUrl.c_str());
    Preferences p;
    if (p.begin(NS, false)) {
        p.remove("title");
        p.remove("body");
        p.end();
    }
}

}  // namespace telemetry
