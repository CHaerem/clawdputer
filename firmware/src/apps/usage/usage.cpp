// Usage — pulls a Claude usage snapshot from the Mac bridge and renders
// daily / weekly / monthly activity plus current cost and tokens.

#include <Arduino.h>
#include <ArduinoJson.h>
#include <M5Cardputer.h>

#include <string>

#include "core/app.h"
#include "core/event_bus.h"
#include "services/ble.h"
#include "services/bridge.h"
#include "services/wifi.h"
#include "ui/canvas.h"
#include "ui/statusbar.h"

namespace {

enum class State : uint8_t {
    Idle,
    Waiting,
    Loaded,
    Error,
};

State        g_state = State::Idle;
bool         g_dirty = true;
int          g_sub   = 0;
uint32_t     g_requestAt = 0;

int g_todayMsg = 0, g_todaySess = 0, g_todayTools = 0;
int g_weekMsg  = 0, g_weekSess  = 0, g_weekTools  = 0;
int g_monthMsg = 0, g_monthSess = 0, g_monthTools = 0;
double       g_costUsd = 0;
std::string  g_tier;
int          g_tIn = 0, g_tOut = 0, g_tCacheR = 0, g_tCacheC = 0;
std::string  g_asOf;
std::string  g_status;

constexpr uint32_t REQUEST_TIMEOUT_MS = 30000;

void requestUsage() {
    if (!bridge::isConnected()) {
        g_status = "no bridge — start clawd-bridge on Mac";
        g_state  = State::Error;
        g_dirty  = true;
        return;
    }
    bridge::sendLine("{\"evt\":\"usage.request\"}");
    g_state     = State::Waiting;
    g_status    = "loading…";
    g_requestAt = millis();
    g_dirty     = true;
}

void onEvent(const Event& e) {
    if (e.source != EventSource::BridgeLink) return;
    if (e.type != EventType::LinkLine) return;

    JsonDocument doc;
    if (deserializeJson(doc, e.data)) return;
    const char* evt = doc["evt"] | "";
    if (strcmp(evt, "usage.response") != 0 && strcmp(evt, "usage.update") != 0) return;

    auto today = doc["today"];
    auto week  = doc["week"];
    auto month = doc["month"];
    g_todayMsg   = today["messages"] | 0;
    g_todaySess  = today["sessions"] | 0;
    g_todayTools = today["tools"]    | 0;
    g_weekMsg    = week["messages"]  | 0;
    g_weekSess   = week["sessions"]  | 0;
    g_weekTools  = week["tools"]     | 0;
    g_monthMsg   = month["messages"] | 0;
    g_monthSess  = month["sessions"] | 0;
    g_monthTools = month["tools"]    | 0;

    g_costUsd = doc["cost"]["usd"]  | 0.0;
    g_tier    = (const char*)(doc["cost"]["tier"] | "");

    g_tIn      = doc["tokens"]["input"]       | 0;
    g_tOut     = doc["tokens"]["output"]      | 0;
    g_tCacheR  = doc["tokens"]["cacheRead"]   | 0;
    g_tCacheC  = doc["tokens"]["cacheCreate"] | 0;

    g_asOf = (const char*)(doc["asOf"] | "");

    g_state = State::Loaded;
    g_dirty = true;
}

void drawFooter(const char* hint) {
    auto& d = ui::display();
    d.fillRect(0, 124, SCREEN_W, 11, 0x1082);
    d.drawFastHLine(0, 124, SCREEN_W, 0x2945);
    d.setTextColor(0x8C71);
    d.setCursor(4, 127);
    d.print(hint);
}

void drawRow(int& y, const char* label, const char* value, uint16_t color = 0xFFFF) {
    auto& d = ui::display();
    d.setCursor(6, y);
    d.setTextColor(0x8C71);
    d.print(label);
    d.setCursor(56, y);
    d.setTextColor(color);
    d.print(value);
    y += 10;
}

void renderIdle() {
    auto& d = ui::display();
    ui::beginFrame();
    ui::statusbar::draw();
    d.setTextSize(1);
    d.setTextColor(0xFFFF);
    d.setCursor(6, ui::statusbar::HEIGHT + 4);
    d.print("Claude Usage");
    d.setTextColor(0x8C71);
    d.setCursor(6, ui::statusbar::HEIGHT + 24);
    d.print("press enter to load");
    if (!g_status.empty()) {
        d.setTextColor(0xF800);
        d.setCursor(6, ui::statusbar::HEIGHT + 44);
        d.print(g_status.c_str());
    }
    drawFooter("enter load   tab home");
    ui::flush();
}

void renderWaiting() {
    auto& d = ui::display();
    ui::beginFrame();
    ui::statusbar::draw();
    d.setTextSize(1);
    d.setTextColor(0xFFE0);
    d.setCursor(6, 50);
    d.print("loading…");
    drawFooter("");
    ui::flush();
}

void renderLoaded() {
    auto& d = ui::display();
    ui::beginFrame();
    ui::statusbar::draw();

    d.setTextSize(1);
    d.setTextColor(0xFFFF);
    d.setCursor(6, ui::statusbar::HEIGHT + 2);
    d.print("Claude Usage");
    d.setTextColor(0x8C71);
    d.setCursor(110, ui::statusbar::HEIGHT + 2);
    d.print(g_asOf.c_str());

    int y = ui::statusbar::HEIGHT + 16;
    char buf[40];

    snprintf(buf, sizeof(buf), "%d msg / %d sess", g_todayMsg, g_todaySess);
    drawRow(y, "today", buf, 0xFFE0);

    snprintf(buf, sizeof(buf), "%d msg / %d sess", g_weekMsg, g_weekSess);
    drawRow(y, "7d",    buf);

    snprintf(buf, sizeof(buf), "%d msg / %d sess", g_monthMsg, g_monthSess);
    drawRow(y, "30d",   buf);

    if (g_tier == "subscription") {
        drawRow(y, "tier", "subscription", 0x07E0);
    } else if (!g_tier.empty()) {
        snprintf(buf, sizeof(buf), "$%.2f (%s)", g_costUsd, g_tier.c_str());
        drawRow(y, "cost", buf, 0x07E0);
    }

    snprintf(buf, sizeof(buf), "in:%d out:%d", g_tIn, g_tOut);
    drawRow(y, "tokens", buf);
    if (g_tCacheR || g_tCacheC) {
        snprintf(buf, sizeof(buf), "rd:%d wr:%d", g_tCacheR, g_tCacheC);
        drawRow(y, "cache", buf);
    }

    drawFooter("enter refresh   tab home");
    ui::flush();
}

void renderError() {
    auto& d = ui::display();
    ui::beginFrame();
    ui::statusbar::draw();
    d.setTextSize(1);
    d.setTextColor(0xF800);
    d.setCursor(6, ui::statusbar::HEIGHT + 4);
    d.print("Usage error");
    d.setTextColor(0xFFFF);
    d.setCursor(6, ui::statusbar::HEIGHT + 22);
    d.print(g_status.c_str());
    drawFooter("enter retry   tab home");
    ui::flush();
}

void render() {
    switch (g_state) {
        case State::Idle:    renderIdle();    break;
        case State::Waiting: renderWaiting(); break;
        case State::Loaded:  renderLoaded();  break;
        case State::Error:   renderError();   break;
    }
}

void onEnter() {
    g_sub = events::subscribe(onEvent);
    if (g_state == State::Idle) g_status.clear();
    if (bridge::isConnected()) {
        requestUsage();
        bridge::sendLine("{\"cmd\":\"subscribe\",\"channel\":\"usage\"}");
    }
    g_dirty = true;
}

void onExit() {
    if (bridge::isConnected()) {
        bridge::sendLine("{\"cmd\":\"unsubscribe\",\"channel\":\"usage\"}");
    }
    events::unsubscribe(g_sub);
    g_sub = 0;
}

void onTick() {
    if (g_state == State::Waiting) {
        if (millis() - g_requestAt > REQUEST_TIMEOUT_MS) {
            g_status = "timed out";
            g_state  = State::Error;
            g_dirty  = true;
        }
    }
}

void onKey(char ch) {
    if (ch == '\n') {
        if (g_state == State::Loaded || g_state == State::Idle || g_state == State::Error) {
            requestUsage();
        }
    }
}

void onDraw() { if (g_dirty) { render(); g_dirty = false; } }

App usage_app = {
    .id           = "usage",
    .name         = "Usage",
    .description  = "Claude activity",
    .services     = SVC_WIFI | SVC_CANVAS,
    .onEnter      = onEnter,
    .onExit       = onExit,
    .onTick       = onTick,
    .onKey        = onKey,
    .onDraw       = onDraw,
    .onEvent      = nullptr,
    .hidden       = true,
};

}  // namespace

REGISTER_APP(usage_app);
