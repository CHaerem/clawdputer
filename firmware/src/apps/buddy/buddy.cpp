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

// "Clawd" — Anthropic's pixel-crab mascot. 11×8 sprite, Space-Invaders
// style, rendered as 4×4 blocks so the whole critter is 44×32 on screen.
// Each row is encoded as a bitmask (bit 10 = leftmost column).
constexpr uint16_t CRAB_SPRITE_BASE[8] = {
    0b00100000100,
    0b00010001000,
    0b00111111100,
    0b01101110110,
    0b11111111111,
    0b10111111101,
    0b10100000101,
    0b00011001100,
};
// Alternate frame: legs shifted out (for idle/celebrate animation).
constexpr uint16_t CRAB_SPRITE_ALT[8] = {
    0b00100000100,
    0b00010001000,
    0b00111111100,
    0b01101110110,
    0b11111111111,
    0b10111111101,
    0b10100000101,
    0b11000110011,
};

struct CrabSkin {
    uint16_t body;   // primary fill
    uint16_t accent; // halo / outline glow
};

CrabSkin crabSkin(PetState s) {
    switch (s) {
        case PetState::Sleep:     return { 0x6800, 0x2000 };  // dim crimson
        case PetState::Idle:      return { 0xF800, 0xC800 };  // bright red
        case PetState::Busy:      return { 0xFD20, 0xC400 };  // orange
        case PetState::Attention: return { 0xF800, 0xFFE0 };  // red + yellow
        case PetState::Celebrate: return { 0xFC1F, 0xFFE0 };  // pink + yellow
        case PetState::Dizzy:     return { 0xFD20, 0x8800 };
        case PetState::Heart:     return { 0xFC1F, 0xF81F };  // pink + magenta
    }
    return { 0xF800, 0xC800 };
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

// Render the Space-Invaders-style "Clawd" sprite at (ox, oy). Each
// sprite pixel is rendered as a 4×4 block on the panel; total footprint
// 44×32 pixels with state-specific colours and overlays.
void drawCrab(int ox, int oy, PetState s, uint32_t now) {
    auto& d = ui::display();
    auto sk = crabSkin(s);

    // Frame select: alternate every 350 ms so legs/claws appear to flex.
    bool altFrame = ((now / 350) & 1) != 0;
    const uint16_t* rows = altFrame ? CRAB_SPRITE_ALT : CRAB_SPRITE_BASE;

    // State-specific tweaks layered on top of the base sprite.
    bool blinkClosed = (s != PetState::Sleep) && ((now / 400) % 12 == 0);
    int  bob        = 0;
    if (s == PetState::Idle && (now / 600) % 2 == 0) bob = -1;
    if (s == PetState::Celebrate) bob = -((int)((now / 120) % 3));   // little hop
    bool flashBright = (s == PetState::Attention) && ((now / 250) & 1);

    uint16_t bodyColor = flashBright ? 0xFFE0 : sk.body;

    constexpr int PIX = 4;
    for (int row = 0; row < 8; row++) {
        uint16_t mask = rows[row];
        for (int col = 0; col < 11; col++) {
            if (!(mask & (1 << (10 - col)))) continue;

            // The "eye gap" in row 3 is the eyes — render them in white
            // (or close them while blinking / sleeping).
            bool isEyeRow = (row == 3);
            // Eyes are the unset bits within the body. To draw them we
            // overlay later — here just draw body pixels.
            int px = ox + col * PIX;
            int py = oy + row * PIX + bob;
            d.fillRect(px, py, PIX, PIX, bodyColor);
        }
    }

    // Eyes — two 4×4 white squares in the gap of row 3, become slits when
    // sleeping or blinking.
    int eyeY = oy + 3 * PIX + bob;
    int eyeLx = ox + 3 * PIX + 1;   // col 3, but shrunken to fit gap
    int eyeRx = ox + 7 * PIX + 1;   // col 7
    bool closed = blinkClosed || s == PetState::Sleep;
    uint16_t eyeFill = closed ? 0x0000 : 0xFFFF;
    if (closed) {
        d.fillRect(eyeLx, eyeY + 1, PIX - 2, 1, 0xC618);
        d.fillRect(eyeRx, eyeY + 1, PIX - 2, 1, 0xC618);
    } else {
        d.fillRect(eyeLx, eyeY, PIX - 2, PIX - 1, eyeFill);
        d.fillRect(eyeRx, eyeY, PIX - 2, PIX - 1, eyeFill);
        // Pupils — for heart we replace with magenta hearts.
        if (s == PetState::Heart) {
            d.fillRect(eyeLx, eyeY + 1, PIX - 2, PIX - 2, 0xF81F);
            d.fillRect(eyeRx, eyeY + 1, PIX - 2, PIX - 2, 0xF81F);
        } else {
            d.drawPixel(eyeLx,     eyeY + 1, 0x0000);
            d.drawPixel(eyeRx,     eyeY + 1, 0x0000);
        }
    }

    // Floating overlays per state — kept above the sprite.
    switch (s) {
        case PetState::Sleep:
            d.setTextSize(1); d.setTextColor(0xC618);
            d.setCursor(ox + 38, oy);      d.print("z");
            d.setCursor(ox + 42, oy - 4);  d.print("Z");
            break;
        case PetState::Attention:
            d.setTextSize(2); d.setTextColor(0xFFE0);
            d.setCursor(ox + 18, oy - 12); d.print("!");
            break;
        case PetState::Celebrate:
            // confetti
            d.fillRect(ox + 2,  oy - 3, 2, 2, 0xFFE0);
            d.fillRect(ox + 38, oy - 1, 2, 2, 0x07FF);
            d.fillRect(ox + 20, oy - 6, 2, 2, 0xF81F);
            break;
        case PetState::Dizzy:
            // swirl
            d.drawCircle(ox + 22, oy - 4, 3, 0x8C71);
            d.drawPixel(ox + 22,  oy - 4, 0xC618);
            break;
        case PetState::Heart:
            // floating heart
            d.fillRect(ox + 19, oy - 5, 2, 2, 0xF81F);
            d.fillRect(ox + 23, oy - 5, 2, 2, 0xF81F);
            d.fillRect(ox + 18, oy - 4, 8, 2, 0xF81F);
            d.fillRect(ox + 19, oy - 2, 6, 1, 0xF81F);
            d.fillRect(ox + 20, oy - 1, 4, 1, 0xF81F);
            d.fillRect(ox + 21, oy,     2, 1, 0xF81F);
            break;
        default: break;
    }
}

void drawPet() {
    if (!settings::petEnabled()) return;
    uint32_t now = millis();
    // Sprite is 44×32 px — anchor it so it sits in the top-right of the
    // buddy content area without overlapping the session-count text.
    int ox = SCREEN_W - 50;
    int oy = ui::statusbar::HEIGHT + 14;
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
