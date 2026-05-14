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
#include "services/ble.h"

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

void render() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(BLACK);

    d.setTextSize(2);
    d.setTextColor(WHITE);
    d.setCursor(6, 4);
    d.print("clawdputer");

    d.setTextSize(1);
    d.setCursor(6, 28);
    if (!ble::isConnected(EventSource::NusLink)) {
        d.setTextColor(0x7BEF);
        d.print("BLE advertising as");
        d.setCursor(6, 40);
        d.setTextColor(WHITE);
        d.print(ble::deviceName().c_str());
        d.setCursor(6, 60);
        d.setTextColor(0x7BEF);
        d.print("Claude > Developer >");
        d.setCursor(6, 72);
        d.print("Open Hardware Buddy");
        return;
    }

    d.setTextColor(0x07E0);
    d.print("connected");
    if (g_owner.length()) {
        d.setTextColor(WHITE);
        d.print("  ");
        d.print(g_owner);
    }

    if (g_promptId.length()) {
        d.fillRect(0, 44, 240, 90, 0x4800);
        d.setTextColor(WHITE);
        d.setTextSize(2);
        d.setCursor(6, 48);
        d.print("APPROVE?");
        d.setTextSize(1);
        d.setCursor(6, 70);
        d.setTextColor(0xFFE0);
        d.print(g_promptTool);
        d.setCursor(6, 82);
        d.setTextColor(WHITE);
        String h = g_promptHint;
        if (h.length() > 38) h = h.substring(0, 35) + "...";
        d.print(h);
        d.setCursor(6, 102);
        d.setTextColor(0x07E0);
        d.print("[Y] approve   ");
        d.setTextColor(0xF800);
        d.print("[N] deny");
        return;
    }

    d.setTextColor(WHITE);
    d.setCursor(6, 44);
    d.printf("sessions:%d  run:%d  wait:%d", g_total, g_running, g_waiting);
    d.setCursor(6, 56);
    d.printf("tokens today: %u", (unsigned)g_tokensToday);

    if (g_msg.length()) {
        d.setCursor(6, 76);
        d.setTextColor(0xFFE0);
        String m = g_msg;
        if (m.length() > 40) m = m.substring(0, 37) + "...";
        d.print(m);
    }

    d.setCursor(6, 122);
    d.setTextColor(0x7BEF);
    d.printf("appr:%u  deny:%u", (unsigned)g_approvals, (unsigned)g_denials);
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

void onTick() {
    if (ble::isConnected(EventSource::NusLink) && g_lastHeartbeatMs &&
        (millis() - g_lastHeartbeatMs > 30000)) {
        g_msg             = "(no heartbeat)";
        g_screenDirty     = true;
        g_lastHeartbeatMs = 0;
    }
}

void onKey(char ch) {
    if (g_promptId.length() == 0) return;
    if (ch == 'y' || ch == 'Y') {
        sendPermission(g_promptId, "once");
        g_approvals++;
        g_promptId = "";
        g_promptTool = "";
        g_promptHint = "";
        g_screenDirty = true;
    } else if (ch == 'n' || ch == 'N') {
        sendPermission(g_promptId, "deny");
        g_denials++;
        g_promptId = "";
        g_promptTool = "";
        g_promptHint = "";
        g_screenDirty = true;
    }
}

void onDraw() {
    if (!g_screenDirty) return;
    render();
    g_screenDirty = false;
}

App buddy_app = {
    .id       = "buddy",
    .name     = "Buddy",
    .services = SVC_BLE,
    .onEnter  = onEnter,
    .onExit   = onExit,
    .onTick   = onTick,
    .onKey    = onKey,
    .onDraw   = onDraw,
    .onEvent  = nullptr,  // app routes BLE events through events::subscribe in onEnter
};

}  // namespace

REGISTER_APP(buddy_app);
