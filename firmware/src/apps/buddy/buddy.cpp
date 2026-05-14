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

// Crab body colour per state — keeps the shape recognisable as a crab
// while the mood comes through colour + facial features.
struct CrabSkin {
    uint16_t body;   // shell colour
    uint16_t accent; // claw / leg accent
    uint16_t eye;    // pupil / blink colour
};

CrabSkin crabSkin(PetState s) {
    switch (s) {
        case PetState::Sleep:     return { 0x4A49, 0x52AA, 0x0000 };  // dim red
        case PetState::Idle:      return { 0xF800, 0xC800, 0x0000 };  // bright red
        case PetState::Busy:      return { 0xFD20, 0xC400, 0x0000 };  // orange
        case PetState::Attention: return { 0xF800, 0xFFE0, 0xFFFF };  // red, yellow accents
        case PetState::Celebrate: return { 0xFC1F, 0xFFE0, 0x0000 };  // pink + yellow
        case PetState::Dizzy:     return { 0xFD20, 0x8800, 0x0000 };
        case PetState::Heart:     return { 0xFC1F, 0xF81F, 0xF800 };  // pink with red heart pupils
    }
    return { 0xF800, 0xC800, 0x0000 };
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

// Draws a small crab (~56×44 px) anchored at (originX, originY top-left).
// Layout:
//                .--.        .--.
//               /    \      /    \         <- claws
//               \    /------\    /
//                '--'  body  '--'
//                     |o  o|                <- eyes on top of shell
//                     +----+
//                     | || |                <- under-belly
//                     ||  ||                <- legs
void drawCrab(int ox, int oy, PetState s, uint32_t now) {
    auto& d  = ui::display();
    auto sk  = crabSkin(s);
    bool blink = (now / 400) % 12 == 0;

    // Body (rounded shell).
    int bx = ox + 14, by = oy + 10, bw = 28, bh = 18;
    d.fillRoundRect(bx, by, bw, bh, 6, sk.body);
    // Shell highlight stripe
    d.drawFastHLine(bx + 4, by + 4, bw - 8, sk.accent);

    // Eye stalks
    d.drawFastVLine(bx + 8,  by - 3, 4, sk.accent);
    d.drawFastVLine(bx + 19, by - 3, 4, sk.accent);
    // Eyes
    if (s == PetState::Sleep) {
        d.drawFastHLine(bx + 7,  by - 4, 4, 0xFFFF);
        d.drawFastHLine(bx + 18, by - 4, 4, 0xFFFF);
    } else {
        d.fillCircle(bx + 9,  by - 4, 2, 0xFFFF);
        d.fillCircle(bx + 20, by - 4, 2, 0xFFFF);
        if (!blink || s == PetState::Attention) {
            d.fillCircle(bx + 9,  by - 4, 1, sk.eye);
            d.fillCircle(bx + 20, by - 4, 1, sk.eye);
        }
    }

    // Claws — two larger ovals, raised when celebrating
    int clawDrop = (s == PetState::Celebrate) ? -6 : 0;
    int clawL_x = ox + 2,  clawR_x = ox + bw + 14;
    int claw_y  = oy + 10 + clawDrop;
    d.fillRoundRect(clawL_x, claw_y, 12, 10, 4, sk.body);
    d.fillRoundRect(clawR_x, claw_y, 12, 10, 4, sk.body);
    // Pincer split (a small accent slit)
    d.drawFastHLine(clawL_x + 2, claw_y + 4, 4, sk.accent);
    d.drawFastHLine(clawR_x + 6, claw_y + 4, 4, sk.accent);
    // Connecting "arms" to body
    d.drawLine(clawL_x + 12, claw_y + 5, bx,      by + 6, sk.accent);
    d.drawLine(clawR_x,      claw_y + 5, bx + bw, by + 6, sk.accent);

    // Legs — three pairs sticking out and down
    for (int i = 0; i < 3; i++) {
        int lx = bx + 4 + i * 8;
        int ly = by + bh;
        d.drawLine(lx,     ly,     lx - 2, ly + 5, sk.accent);
        d.drawLine(lx + 18 - i * 8, ly,
                   lx + 20 - i * 8, ly + 5, sk.accent);
    }

    // Mouth / expression cue under the eyes
    int mx = bx + bw / 2 - 3, my = by + 6;
    switch (s) {
        case PetState::Sleep:
            d.drawFastHLine(mx, my, 6, sk.accent);
            // little zzz
            d.setTextSize(1);
            d.setTextColor(0xC618);
            d.setCursor(ox + bw + 24, oy + 4);
            d.print("z");
            d.setCursor(ox + bw + 28, oy);
            d.print("Z");
            break;
        case PetState::Busy:
            d.drawLine(mx, my + 1, mx + 2, my - 1, sk.accent);
            d.drawLine(mx + 4, my - 1, mx + 6, my + 1, sk.accent);
            break;
        case PetState::Attention:
            // open "!" mouth
            d.fillCircle(bx + bw / 2, my + 1, 2, 0x0000);
            // bang above head
            d.setTextSize(1);
            d.setTextColor(0xFFE0);
            d.setCursor(ox + bw / 2 + 10, oy);
            d.print("!");
            break;
        case PetState::Celebrate:
            // smiling arc
            d.drawPixel(mx,     my,     sk.accent);
            d.drawPixel(mx + 6, my,     sk.accent);
            d.drawFastHLine(mx + 1, my + 1, 5, sk.accent);
            // confetti dots
            d.fillCircle(ox + 4,  oy + 2, 1, 0xFFE0);
            d.fillCircle(ox + 50, oy + 5, 1, 0x07FF);
            d.fillCircle(ox + 30, oy,     1, 0xF81F);
            break;
        case PetState::Dizzy:
            // tongue out
            d.fillRect(mx + 2, my, 3, 3, 0xF800);
            // swirl above
            d.drawCircle(ox + 28, oy + 2, 3, 0x8C71);
            d.drawCircle(ox + 28, oy + 2, 5, 0x8C71);
            break;
        case PetState::Heart:
            // smile + floating heart
            d.drawFastHLine(mx + 1, my + 1, 4, sk.accent);
            d.fillCircle(ox + 28, oy + 2, 2, 0xF81F);
            d.fillCircle(ox + 31, oy + 2, 2, 0xF81F);
            d.fillTriangle(ox + 26, oy + 4, ox + 33, oy + 4, ox + 29, oy + 8, 0xF81F);
            break;
        case PetState::Idle:
        default:
            d.drawFastHLine(mx + 1, my + 1, 4, sk.accent);
            break;
    }
}

void drawPet() {
    if (!settings::petEnabled()) return;
    uint32_t now = millis();
    int ox = SCREEN_W - 70;
    int oy = ui::statusbar::HEIGHT + 10;
    // Idle "breath" — 1 px vertical wobble every half second
    if (g_pet == PetState::Idle && (now / 500) % 2 == 0) oy += 1;
    drawCrab(ox, oy, g_pet, now);
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
