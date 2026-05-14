// Chat app — sends typed prompts to the Mac-side clawd-bridge daemon over
// the BLE bridge link, and renders streamed responses from the host's
// `claude` CLI session.
//
// Wire protocol: see protocol/WIRE.md (chat.send / chat.chunk / chat.end).

#include <Arduino.h>
#include <ArduinoJson.h>
#include <M5Cardputer.h>

#include <deque>
#include <string>
#include <vector>

#include "core/app.h"
#include "core/event_bus.h"
#include "services/ble.h"

namespace {

struct Line {
    std::string text;
    bool        fromUser;
};

std::deque<Line> g_lines;
std::string      g_input;
bool             g_busy  = false;
bool             g_dirty = true;
int              g_sub   = 0;

constexpr size_t MAX_LINES = 200;
constexpr int    LINE_H    = 10;
constexpr int    MAX_W     = 52;   // chars per line at text size 1 on 320px
constexpr int    TOP_Y     = 16;
constexpr int    INPUT_H   = 16;

void push(const std::string& s, bool fromUser) {
    g_lines.push_back({s, fromUser});
    while (g_lines.size() > MAX_LINES) g_lines.pop_front();
    g_dirty = true;
}

void appendAssistant(const std::string& chunk) {
    if (g_lines.empty() || g_lines.back().fromUser) {
        g_lines.push_back({chunk, false});
    } else {
        g_lines.back().text += chunk;
    }
    while (g_lines.size() > MAX_LINES) g_lines.pop_front();
    g_dirty = true;
}

void render() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(BLACK);

    d.setTextSize(1);
    d.setTextColor(0x7BEF);
    d.setCursor(2, 2);
    d.printf("chat — %s",
             ble::isConnected(EventSource::BridgeLink) ? "bridge" : "(no bridge)");
    if (g_busy) {
        d.setTextColor(0xFFE0);
        d.print("  streaming…");
    }
    d.setTextColor(0x7BEF);
    d.setCursor(240, 2);
    d.print("[TAB] app");

    const int bottomY = 240 - INPUT_H;

    // Walk history newest-first, collect wrapped rows bottom-up.
    struct Row {
        std::string text;
        bool        fromUser;
    };
    std::vector<Row> rows;
    int              rowsLeft = (bottomY - TOP_Y) / LINE_H;
    for (auto it = g_lines.rbegin(); it != g_lines.rend() && rowsLeft > 0; ++it) {
        const std::string& s = it->text;
        std::vector<std::string> parts;
        if (s.empty()) {
            parts.push_back("");
        } else {
            for (size_t i = 0; i < s.size(); i += MAX_W) {
                parts.push_back(s.substr(i, MAX_W));
            }
        }
        for (auto rit = parts.rbegin(); rit != parts.rend() && rowsLeft > 0; ++rit) {
            rows.push_back({*rit, it->fromUser});
            rowsLeft--;
        }
    }

    int yLine = bottomY - LINE_H;
    for (auto& r : rows) {
        d.setCursor(2, yLine);
        d.setTextColor(r.fromUser ? 0x07E0 : WHITE);
        d.print(r.text.c_str());
        yLine -= LINE_H;
    }

    d.fillRect(0, 240 - INPUT_H, 320, INPUT_H, 0x2104);
    d.setTextColor(WHITE);
    d.setCursor(2, 240 - INPUT_H + 4);
    d.print("> ");
    std::string in = g_input;
    if (in.size() > 50) in = std::string("…") + in.substr(in.size() - 49);
    d.print(in.c_str());
}

void sendCurrent() {
    if (g_input.empty()) return;
    if (!ble::isConnected(EventSource::BridgeLink)) {
        push("(no bridge connected)", false);
        return;
    }
    JsonDocument doc;
    doc["evt"]  = "chat.send";
    doc["text"] = g_input;
    std::string out;
    serializeJson(doc, out);
    ble::sendLine(EventSource::BridgeLink, out);
    push(g_input, true);
    g_input.clear();
    g_busy  = true;
    g_dirty = true;
}

void onEvent(const Event& e) {
    if (e.source != EventSource::BridgeLink) return;
    switch (e.type) {
        case EventType::LinkConnected:
        case EventType::LinkDisconnected:
            g_dirty = true;
            break;
        case EventType::LinkLine: {
            JsonDocument doc;
            if (deserializeJson(doc, e.data)) return;
            const char* evt = doc["evt"] | "";
            if (strcmp(evt, "chat.chunk") == 0) {
                appendAssistant(doc["text"] | "");
            } else if (strcmp(evt, "chat.end") == 0) {
                g_busy  = false;
                g_dirty = true;
            } else if (strcmp(evt, "error") == 0) {
                push(std::string("[err] ") + (doc["msg"] | ""), false);
            }
            break;
        }
    }
}

void onEnter() {
    g_sub   = events::subscribe(onEvent);
    g_dirty = true;
}

void onExit() {
    events::unsubscribe(g_sub);
    g_sub = 0;
}

void onTick() {}

void onKey(char ch) {
    if (ch == '\n') {
        sendCurrent();
    } else if (ch == '\b') {
        if (!g_input.empty()) {
            g_input.pop_back();
            g_dirty = true;
        }
    } else if (ch >= 0x20 && ch <= 0x7E) {
        g_input.push_back(ch);
        g_dirty = true;
    }
}

void onDraw() {
    if (!g_dirty) return;
    render();
    g_dirty = false;
}

App chat_app = {
    .id       = "chat",
    .name     = "Chat",
    .services = SVC_BLE,
    .onEnter  = onEnter,
    .onExit   = onExit,
    .onTick   = onTick,
    .onKey    = onKey,
    .onDraw   = onDraw,
    .onEvent  = nullptr,
};

}  // namespace

REGISTER_APP(chat_app);
