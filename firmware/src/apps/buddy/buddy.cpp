// Buddy app — Cardputer port of anthropics/claude-desktop-buddy.
//
// Subscribes to the BLE service, displays session status, and lets the user
// approve or deny pending permission prompts from the keyboard.
//
// Wire protocol: https://github.com/anthropics/claude-desktop-buddy/blob/main/REFERENCE.md

#include <Arduino.h>
#include <ArduinoJson.h>
#include <M5Cardputer.h>

#include "core/app.h"
#include "core/event_bus.h"
#include "services/audio.h"
#include "services/ble.h"
#include "services/imu.h"
#include "services/settings.h"
#include "ui/canvas.h"
#include "ui/statusbar.h"

namespace {

String g_owner;
String g_msg;
int    g_total = 0, g_running = 0, g_waiting = 0;
uint32_t g_tokensToday     = 0;
uint32_t g_lastHeartbeatMs = 0;

String   g_promptId, g_promptTool, g_promptHint;
uint32_t g_bootMs    = 0;
uint32_t g_approvals = 0, g_denials = 0;
bool     g_screenDirty = true;
int      g_sub         = 0;

enum class PetState : uint8_t {
    Sleep, Idle, Busy, Attention, Celebrate, Dizzy, Heart
};
PetState g_pet            = PetState::Sleep;
PetState g_petLast        = PetState::Sleep;
uint32_t g_petStateUntil  = 0;
uint32_t g_lastTokensSeen = 0;
uint32_t g_levelTokens    = 0;   // next celebration milestone
bool     g_promptShown    = false;
uint32_t g_promptAt       = 0;
bool     g_attentionPinged = false;

constexpr uint32_t CELEBRATE_STEP_TOKENS = 50000;

const char* petFace(PetState s) {
    switch (s) {
        case PetState::Sleep:     return "(-.-) zzz";
        case PetState::Idle:      return "(o.o)";
        case PetState::Busy:      return "(>_<)";
        case PetState::Attention: return "(O_O)!";
        case PetState::Celebrate: return "\\(^o^)/";
        case PetState::Dizzy:     return "(@_@)";
        case PetState::Heart:     return "(<3_<3)";
    }
    return "(o.o)";
}

uint16_t petColor(PetState s) {
    switch (s) {
        case PetState::Sleep:     return 0x7BEF;
        case PetState::Idle:      return 0xFFFF;
        case PetState::Busy:      return 0xFFE0;
        case PetState::Attention: return 0xF800;
        case PetState::Celebrate: return 0x07E0;
        case PetState::Dizzy:     return 0xFD20;
        case PetState::Heart:     return 0xF81F;
    }
    return 0xFFFF;
}

void drawPet() {
    if (!settings::petEnabled()) return;
    auto& d = ui::display();
    d.setTextSize(2);
    d.setTextColor(petColor(g_pet));
    int x = SCREEN_W - 88;
    int y = ui::statusbar::HEIGHT + 4;
    // Tiny "breath" wobble: jiggle x by 1 px on alternating half-seconds
    // when idle, so it doesn't feel frozen.
    if (g_pet == PetState::Idle && ((millis() / 500) % 2 == 0)) x += 1;
    d.setCursor(x, y);
    d.print(petFace(g_pet));
}

void render() {
    auto& d = ui::display();
    ui::beginFrame();
    ui::statusbar::draw();
    drawPet();

    int y = ui::statusbar::HEIGHT + 4;

    d.setTextSize(1);
    if (!ble::isConnected(EventSource::NusLink)) {
        d.setTextColor(0x8C71);
        d.setCursor(6, y);
        d.print("advertising as");
        y += 11;
        d.setTextColor(0xFFFF);
        d.setCursor(6, y);
        d.print(ble::deviceName().c_str());
        y += 16;
        d.setTextColor(0x8C71);
        d.setCursor(6, y);
        d.print("Claude > Developer >");
        y += 10;
        d.setCursor(6, y);
        d.print("Open Hardware Buddy");
        ui::flush();
        return;
    }

    d.setTextColor(0x07E0);
    d.setCursor(6, y);
    d.print("connected");
    if (g_owner.length()) {
        d.setTextColor(0xFFFF);
        d.print(" ");
        d.print(g_owner);
    }
    y += 12;

    if (g_promptId.length()) {
        d.fillRoundRect(4, y, SCREEN_W - 8, 80, 4, 0x4800);
        d.setTextColor(WHITE);
        d.setTextSize(2);
        d.setCursor(10, y + 4);
        d.print("APPROVE?");
        d.setTextSize(1);
        d.setCursor(10, y + 26);
        d.setTextColor(0xFFE0);
        d.print(g_promptTool);
        d.setCursor(10, y + 38);
        d.setTextColor(WHITE);
        String h = g_promptHint;
        if (h.length() > 36) h = h.substring(0, 33) + "...";
        d.print(h);
        d.setCursor(10, y + 58);
        d.setTextColor(0x07E0);
        d.print("[Y] approve");
        d.setCursor(110, y + 58);
        d.setTextColor(0xF800);
        d.print("[N] deny");
        ui::flush();
        return;
    }

    d.setTextColor(0x8C71);
    d.setCursor(6, y);
    d.print("sessions");
    d.setCursor(64, y);
    d.setTextColor(0xFFFF);
    d.printf("%d (r%d w%d)", g_total, g_running, g_waiting);
    y += 11;

    d.setTextColor(0x8C71);
    d.setCursor(6, y);
    d.print("tokens");
    d.setCursor(64, y);
    d.setTextColor(0xFFFF);
    d.printf("%u today", (unsigned)g_tokensToday);
    y += 11;

    if (g_msg.length()) {
        d.setCursor(6, y);
        d.setTextColor(0xFFE0);
        String m = g_msg;
        if (m.length() > 38) m = m.substring(0, 35) + "...";
        d.print(m);
    }

    d.fillRect(0, 124, SCREEN_W, 11, 0x1082);
    d.drawFastHLine(0, 124, SCREEN_W, 0x2945);
    d.setTextColor(0x8C71);
    d.setCursor(4, 127);
    d.printf("a:%u d:%u  tab: home", (unsigned)g_approvals, (unsigned)g_denials);

    ui::flush();
}

void sendPermission(const String& id, const char* decision) {
    JsonDocument doc;
    doc["cmd"]      = "permission";
    doc["id"]       = id;
    doc["decision"] = decision;
    std::string out;
    serializeJson(doc, out);
    ble::sendLine(EventSource::NusLink, out);
}

void handleSnapshot(JsonDocument& doc) {
    g_total       = doc["total"] | 0;
    g_running     = doc["running"] | 0;
    g_waiting     = doc["waiting"] | 0;
    g_tokensToday = doc["tokens_today"] | 0;
    if (doc["msg"].is<const char*>()) g_msg = (const char*)doc["msg"];

    if (doc["prompt"].is<JsonObject>()) {
        JsonObject p = doc["prompt"];
        g_promptId   = (const char*)(p["id"] | "");
        g_promptTool = (const char*)(p["tool"] | "");
        g_promptHint = (const char*)(p["hint"] | "");
    } else {
        g_promptId = "";
        g_promptTool = "";
        g_promptHint = "";
    }
    g_lastHeartbeatMs = millis();
    g_screenDirty     = true;
}

void handleCommand(JsonDocument& doc) {
    String c = (const char*)(doc["cmd"] | "");
    if (c.length() == 0) return;

    if (c == "owner") {
        if (doc["name"].is<const char*>()) g_owner = (const char*)doc["name"];
        ble::sendLine(EventSource::NusLink, "{\"ack\":\"owner\",\"ok\":true,\"n\":0}");
        g_screenDirty = true;
    } else if (c == "name") {
        if (doc["name"].is<const char*>()) ble::setDeviceName((const char*)doc["name"]);
        ble::sendLine(EventSource::NusLink, "{\"ack\":\"name\",\"ok\":true,\"n\":0}");
    } else if (c == "status") {
        JsonDocument resp;
        resp["ack"] = "status";
        resp["ok"]  = true;
        resp["n"]   = 0;
        auto data   = resp["data"].to<JsonObject>();
        data["name"] = ble::deviceName();
        data["sec"]  = true;
        auto sys     = data["sys"].to<JsonObject>();
        sys["up"]    = (millis() - g_bootMs) / 1000;
        sys["heap"]  = (uint32_t)ESP.getFreeHeap();
        auto stats   = data["stats"].to<JsonObject>();
        stats["appr"] = g_approvals;
        stats["deny"] = g_denials;
        std::string out;
        serializeJson(resp, out);
        ble::sendLine(EventSource::NusLink, out);
    } else if (c == "unpair") {
        ble::sendLine(EventSource::NusLink, "{\"ack\":\"unpair\",\"ok\":true,\"n\":0}");
        ble::clearBonds();
    }
    // Other commands (char_begin/file/chunk/...): intentionally no ack.
    // Per protocol, declining a folder push is signaled by NOT acking.
}

void handleLine(const std::string& line) {
    if (line.empty()) return;
    JsonDocument doc;
    if (deserializeJson(doc, line)) return;

    if (doc["cmd"].is<const char*>()) {
        handleCommand(doc);
        return;
    }
    if (!doc["total"].isNull() || doc["msg"].is<const char*>() ||
        doc["prompt"].is<JsonObject>()) {
        handleSnapshot(doc);
    }
}

void onEvent(const Event& e) {
    if (e.source != EventSource::NusLink) return;
    switch (e.type) {
        case EventType::LinkConnected:
            g_screenDirty = true;
            break;
        case EventType::LinkDisconnected:
            g_msg             = "";
            g_promptId        = "";
            g_promptTool      = "";
            g_promptHint      = "";
            g_lastHeartbeatMs = 0;
            g_screenDirty     = true;
            break;
        case EventType::LinkLine:
            handleLine(e.data);
            break;
    }
}

void onEnter() {
    g_bootMs = millis();
    g_sub    = events::subscribe(onEvent);
    g_screenDirty = true;
}

void onExit() {
    events::unsubscribe(g_sub);
    g_sub = 0;
}

void updatePetState() {
    uint32_t now = millis();

    // Transient states (celebrate / dizzy / heart) keep priority until they
    // expire.
    if (g_petStateUntil && now < g_petStateUntil) {
        if (g_pet != g_petLast) {
            g_petLast = g_pet;
            g_screenDirty = true;
        }
        return;
    }
    g_petStateUntil = 0;

    if (settings::shakeEnabled() && imu::consumeShake()) {
        g_pet           = PetState::Dizzy;
        g_petStateUntil = now + 2000;
        g_petLast       = g_pet;
        g_screenDirty   = true;
        return;
    }

    PetState next;
    bool connected = ble::isConnected(EventSource::NusLink);
    if (!connected)                    next = PetState::Sleep;
    else if (g_promptId.length() > 0)  next = PetState::Attention;
    else if (g_running > 0)            next = PetState::Busy;
    else                               next = PetState::Idle;

    if (next != g_pet) {
        g_pet         = next;
        g_petLast     = next;
        g_screenDirty = true;
    }
}

void onTick() {
    if (ble::isConnected(EventSource::NusLink) && g_lastHeartbeatMs &&
        (millis() - g_lastHeartbeatMs > 30000)) {
        g_msg             = "(no heartbeat)";
        g_screenDirty     = true;
        g_lastHeartbeatMs = 0;
    }

    // Token-celebration: every 50K of cumulative tokens_today, fire celebrate.
    if (g_tokensToday >= g_levelTokens + CELEBRATE_STEP_TOKENS) {
        g_levelTokens   = (g_tokensToday / CELEBRATE_STEP_TOKENS) * CELEBRATE_STEP_TOKENS;
        g_pet           = PetState::Celebrate;
        g_petStateUntil = millis() + 3000;
        audio::play(audio::Cue::Celebrate);
        g_screenDirty   = true;
    }

    // Attention chime: ping once when a new prompt arrives.
    if (g_promptId.length() > 0 && !g_attentionPinged) {
        audio::play(audio::Cue::Attention);
        g_attentionPinged = true;
        g_promptAt        = millis();
    }
    if (g_promptId.length() == 0) g_attentionPinged = false;

    updatePetState();
}

void onKey(char ch) {
    if (g_promptId.length() == 0) return;
    bool fast = (millis() - g_promptAt) < 5000;
    if (ch == '\n' || ch == 'y' || ch == 'Y') {
        sendPermission(g_promptId, "once");
        g_approvals++;
        g_promptId = "";
        g_promptTool = "";
        g_promptHint = "";
        audio::play(audio::Cue::Approve);
        if (fast) {
            g_pet           = PetState::Heart;
            g_petStateUntil = millis() + 2500;
        }
        g_screenDirty = true;
    } else if (ch == 0x1B /*ESC*/ || ch == 'n' || ch == 'N' || ch == '\b') {
        sendPermission(g_promptId, "deny");
        g_denials++;
        g_promptId = "";
        g_promptTool = "";
        g_promptHint = "";
        audio::play(audio::Cue::Deny);
        g_screenDirty = true;
    }
}

void onDraw() {
    if (!g_screenDirty) return;
    render();
    g_screenDirty = false;
}

App buddy_app = {
    .id          = "buddy",
    .name        = "Buddy",
    .description = "Claude Desktop companion",  // wraps to 2 lines in tile
    .services    = SVC_BLE,
    .onEnter     = onEnter,
    .onExit      = onExit,
    .onTick      = onTick,
    .onKey       = onKey,
    .onDraw      = onDraw,
    .onEvent     = nullptr,  // app routes BLE events through events::subscribe in onEnter
};

}  // namespace

REGISTER_APP(buddy_app);
