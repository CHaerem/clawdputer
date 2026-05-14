// Chat app — sends typed prompts to the Mac-side clawd-bridge daemon over
// the BLE bridge link, and renders streamed responses from the host's
// `claude` CLI session.
//
// Wire protocol: see protocol/WIRE.md (chat.send / chat.chunk / chat.status
// / chat.end).

#include <Arduino.h>
#include <ArduinoJson.h>
#include <M5Cardputer.h>

#include <algorithm>
#include <deque>
#include <string>
#include <vector>

#include "core/app.h"
#include "core/event_bus.h"
#include "services/ble.h"
#include "services/bridge.h"
#include "ui/canvas.h"
#include "ui/statusbar.h"

namespace {

enum class LineKind : uint8_t { User, Assistant, Status, ErrorLine };

struct Line {
    std::string text;
    LineKind    kind;
};

std::deque<Line> g_lines;
std::string      g_input;
bool             g_busy  = false;
bool             g_dirty = true;
int              g_sub   = 0;

constexpr size_t MAX_LINES = 200;
constexpr int    LINE_H    = 10;
constexpr int    MAX_W     = 38;   // chars per line at text size 1 on 240px wide
constexpr int    TOP_Y     = ui::statusbar::HEIGHT + 4;
constexpr int    INPUT_H   = 14;

uint16_t colorFor(LineKind k) {
    switch (k) {
        case LineKind::User:      return 0x07E0;  // green
        case LineKind::Assistant: return 0xFFFF;  // white
        case LineKind::Status:    return 0x7BEF;  // dim grey
        case LineKind::ErrorLine: return 0xF800;  // red
    }
    return 0xFFFF;
}

void push(const std::string& s, LineKind kind) {
    g_lines.push_back({s, kind});
    while (g_lines.size() > MAX_LINES) g_lines.pop_front();
    g_dirty = true;
}

void appendAssistant(const std::string& chunk) {
    if (g_lines.empty() || g_lines.back().kind != LineKind::Assistant) {
        g_lines.push_back({chunk, LineKind::Assistant});
    } else {
        g_lines.back().text += chunk;
    }
    while (g_lines.size() > MAX_LINES) g_lines.pop_front();
    g_dirty = true;
}

// Word-aware wrap: break on the latest space ≤ maxW; honour explicit '\n'.
std::vector<std::string> wrap(const std::string& s, size_t maxW) {
    std::vector<std::string> out;
    auto flushSegment = [&](const std::string& seg) {
        if (seg.empty()) {
            out.push_back("");
            return;
        }
        size_t i = 0;
        while (i < seg.size()) {
            size_t end = std::min(i + maxW, seg.size());
            if (end < seg.size()) {
                size_t spc = seg.rfind(' ', end);
                if (spc != std::string::npos && spc > i) end = spc;
            }
            out.push_back(seg.substr(i, end - i));
            i = end;
            while (i < seg.size() && seg[i] == ' ') i++;
        }
    };
    std::string current;
    for (char c : s) {
        if (c == '\r') continue;
        if (c == '\n') {
            flushSegment(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    flushSegment(current);
    return out;
}

void render() {
    auto& d = ui::display();
    ui::beginFrame();
    ui::statusbar::draw();

    if (g_busy) {
        d.setTextSize(1);
        d.setTextColor(0xFFE0);
        d.setCursor(4, TOP_Y);
        d.print("streaming…");
    } else if (!ble::isConnected(EventSource::BridgeLink)) {
        d.setTextSize(1);
        d.setTextColor(0xF800);
        d.setCursor(4, TOP_Y);
        d.print("no bridge");
    }

    const int bottomY = SCREEN_H - INPUT_H;

    // Walk history newest-first, collect wrapped rows bottom-up until full.
    struct Row {
        std::string text;
        LineKind    kind;
    };
    std::vector<Row> rows;
    int              rowsLeft = (bottomY - TOP_Y) / LINE_H;
    for (auto it = g_lines.rbegin(); it != g_lines.rend() && rowsLeft > 0; ++it) {
        auto wrapped = wrap(it->text, MAX_W);
        for (auto rit = wrapped.rbegin(); rit != wrapped.rend() && rowsLeft > 0; ++rit) {
            rows.push_back({*rit, it->kind});
            rowsLeft--;
        }
    }

    int yLine = bottomY - LINE_H;
    for (auto& r : rows) {
        d.setCursor(2, yLine);
        d.setTextColor(colorFor(r.kind));
        d.print(r.text.c_str());
        yLine -= LINE_H;
    }

    d.fillRect(0, SCREEN_H - INPUT_H, SCREEN_W, INPUT_H, 0x2104);
    d.setTextColor(WHITE);
    d.setCursor(2, SCREEN_H - INPUT_H + 3);
    d.print("> ");
    std::string in = g_input;
    if (in.size() > 36) in = std::string("…") + in.substr(in.size() - 35);
    d.print(in.c_str());
    if ((millis() / 500) % 2 == 0) {
        d.print("_");
    }

    ui::flush();
}

void sendCurrent() {
    if (g_input.empty()) return;
    if (!bridge::isConnected()) {
        push("(no bridge connected)", LineKind::ErrorLine);
        return;
    }
    JsonDocument doc;
    doc["evt"]  = "chat.send";
    doc["text"] = g_input;
    std::string out;
    serializeJson(doc, out);
    bridge::sendLine(out);
    push(g_input, LineKind::User);
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
            } else if (strcmp(evt, "chat.status") == 0) {
                push(doc["text"] | "", LineKind::Status);
            } else if (strcmp(evt, "chat.end") == 0) {
                if (!doc["tokens"].isNull()) {
                    char buf[32];
                    snprintf(buf, sizeof(buf), "[%u tokens]",
                             (unsigned)(doc["tokens"] | 0));
                    push(buf, LineKind::Status);
                }
                g_busy  = false;
                g_dirty = true;
            } else if (strcmp(evt, "error") == 0) {
                push(std::string("[err] ") + (doc["msg"] | ""), LineKind::ErrorLine);
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

void onTick() {
    // Repaint twice a second so the cursor blinks.
    static uint32_t lastBlink = 0;
    uint32_t now = millis();
    if (now - lastBlink >= 500) {
        lastBlink = now;
        g_dirty   = true;
    }
}

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
    .id           = "chat",
    .name         = "Chat",
    .description  = "Claude CLI remote",
    .services     = SVC_BLE,
    .onEnter      = onEnter,
    .onExit       = onExit,
    .onTick       = onTick,
    .onKey        = onKey,
    .onDraw       = onDraw,
    .onEvent      = nullptr,
    .keysAsArrows = false,  // chat takes raw ;./,/ as text
};

}  // namespace

REGISTER_APP(chat_app);
