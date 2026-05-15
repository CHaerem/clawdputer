// Report app — file a bug report or feature request directly to GitHub
// from the device. Reachable from Settings, and auto-prompted on boot
// when crashlog detected the previous session crashed.

#include <Arduino.h>
#include <M5Cardputer.h>

#include <string>

#include "core/app.h"
#include "core/key.h"
#include "core/registry.h"
#include "services/ble.h"
#include "services/crashlog.h"
#include "services/github.h"
#include "services/sd.h"
#include "services/updater.h"
#include "services/wifi.h"
#include "ui/canvas.h"
#include "ui/statusbar.h"

extern void clawd_request_app(const App* app);

namespace {

enum class Stage : uint8_t {
    TypeSelect,
    TitleEntry,
    Confirm,
    Submitting,
    Done,
    Failed,
};

enum class Kind : uint8_t { Bug, Feature };

Stage       g_stage    = Stage::TypeSelect;
Kind        g_kind     = Kind::Bug;
int         g_typeSel  = 0;        // 0=bug, 1=feature
std::string g_title;
bool        g_dirty    = true;
bool        g_prefilled = false;
github::SubmitResult g_result;

bool g_submitPending = false;

const char* kindLabel() {
    return g_kind == Kind::Bug ? "bug" : "enhancement";
}

std::string buildBody() {
    std::string sd_state = sd::isAvailable() ? "mounted" : "absent";
    std::string priorReason = crashlog::priorReason();
    if (priorReason.empty()) priorReason = "manual";
    std::string priorApp = crashlog::priorApp();
    if (priorApp.empty()) priorApp = "—";

    char header[512];
    snprintf(header, sizeof(header),
             "**Build:** %s\n"
             "**Device:** %s\n"
             "**Reset reason:** %s\n"
             "**Last active app:** %s\n"
             "**Uptime at report:** %lus\n"
             "**Free heap:** %u bytes\n"
             "**WiFi:** %s @ %s\n"
             "**SD:** %s\n\n---\n%s\n\n",
             updater::currentVersion(),
             ble::deviceName().c_str(),
             priorReason.c_str(),
             priorApp.c_str(),
             (unsigned long)(millis() / 1000),
             (unsigned)ESP.getFreeHeap(),
             wifi::ssid().c_str(),
             wifi::ip().c_str(),
             sd_state.c_str(),
             g_title.c_str());

    std::string body = header;
    std::string tail = crashlog::serialTail(4096);
    if (!tail.empty()) {
        body += "```\n";
        body += tail;
        if (body.back() != '\n') body += "\n";
        body += "```\n";
    } else if (g_prefilled && !sd::isAvailable()) {
        body += "_(no SD — pre-crash logs unavailable)_\n";
    }
    return body;
}

void renderHeader(const char* title) {
    auto& d = ui::display();
    d.setTextSize(1);
    d.setTextColor(0xFFFF);
    d.setCursor(6, ui::statusbar::HEIGHT + 3);
    d.print(title);
}

void drawFooter(const char* hint) {
    auto& d = ui::display();
    d.fillRect(0, 124, SCREEN_W, 11, 0x1082);
    d.drawFastHLine(0, 124, SCREEN_W, 0x2945);
    d.setTextColor(0x8C71);
    d.setCursor(4, 127);
    d.print(hint);
}

void renderTypeSelect() {
    auto& d = ui::display();
    ui::beginFrame();
    ui::statusbar::draw();
    renderHeader("file an issue");

    const char* opts[2] = { "bug report", "feature request" };
    int y = ui::statusbar::HEIGHT + 24;
    for (int i = 0; i < 2; i++) {
        bool sel = (i == g_typeSel);
        if (sel) d.fillRoundRect(20, y - 2, SCREEN_W - 40, 14, 3, 0x18E3);
        d.setCursor(28, y + 1);
        d.setTextColor(sel ? 0xFFE0 : 0xC618);
        d.print(opts[i]);
        y += 18;
    }

    drawFooter(";/  move   enter pick   tab home");
    ui::flush();
}

void renderTitleEntry() {
    auto& d = ui::display();
    ui::beginFrame();
    ui::statusbar::draw();
    renderHeader(g_kind == Kind::Bug ? "bug — title" : "feature — title");

    int boxY = ui::statusbar::HEIGHT + 24;
    d.fillRoundRect(4, boxY, SCREEN_W - 8, 22, 3, 0x2104);
    d.setCursor(8, boxY + 7);
    d.setTextColor(0xFFFF);
    std::string visible = g_title;
    if (visible.size() > 36) visible = visible.substr(visible.size() - 36);
    d.print(visible.c_str());
    if ((millis() / 500) % 2 == 0) d.print("_");

    d.setCursor(6, boxY + 32);
    d.setTextColor(0x8C71);
    d.print("device info auto-attached");

    drawFooter("enter next   bksp delete   tab home");
    ui::flush();
}

void renderConfirm() {
    auto& d = ui::display();
    ui::beginFrame();
    ui::statusbar::draw();
    renderHeader("confirm");

    int y = ui::statusbar::HEIGHT + 18;
    d.setCursor(6, y);
    d.setTextColor(0x8C71);
    d.print("label:");
    d.setCursor(50, y);
    d.setTextColor(0xFFE0);
    d.print(kindLabel());

    y += 12;
    d.setCursor(6, y);
    d.setTextColor(0x8C71);
    d.print("title:");
    y += 10;
    d.setCursor(6, y);
    d.setTextColor(0xFFFF);
    std::string t = g_title;
    if (t.size() > 36) t = t.substr(0, 35) + "…";
    d.print(t.c_str());

    y += 14;
    d.setCursor(6, y);
    d.setTextColor(0x07E0);
    d.print("enter to submit");
    y += 10;
    d.setCursor(6, y);
    d.setTextColor(0x8C71);
    d.print("bksp to edit title");

    drawFooter("enter submit   bksp back   tab home");
    ui::flush();
}

void renderSubmitting() {
    auto& d = ui::display();
    ui::beginFrame();
    ui::statusbar::draw();
    renderHeader("submitting…");
    d.setCursor(6, 60);
    d.setTextColor(0xFFE0);
    d.print("posting to GitHub");
    d.setCursor(6, 72);
    d.setTextColor(0x8C71);
    d.print("a few seconds…");
    drawFooter("");
    ui::flush();
}

void renderDone() {
    auto& d = ui::display();
    ui::beginFrame();
    ui::statusbar::draw();
    renderHeader("submitted");

    int y = ui::statusbar::HEIGHT + 22;
    d.setCursor(6, y);
    d.setTextColor(0x07E0);
    char buf[32];
    snprintf(buf, sizeof(buf), "issue #%d", g_result.issueNumber);
    d.print(buf);
    y += 14;
    d.setCursor(6, y);
    d.setTextColor(0xFFFF);
    std::string url = g_result.issueUrl;
    if (url.size() > 38) url = url.substr(0, 37) + "…";
    d.print(url.c_str());
    drawFooter("tab home");
    ui::flush();
}

void renderFailed() {
    auto& d = ui::display();
    ui::beginFrame();
    ui::statusbar::draw();
    renderHeader("submit failed");

    int y = ui::statusbar::HEIGHT + 22;
    d.setCursor(6, y);
    d.setTextColor(0xF800);
    std::string e = g_result.error;
    if (e.size() > 38) e = e.substr(0, 37) + "…";
    d.print(e.c_str());

    drawFooter("enter retry   bksp edit   tab home");
    ui::flush();
}

void render() {
    switch (g_stage) {
        case Stage::TypeSelect: renderTypeSelect(); break;
        case Stage::TitleEntry: renderTitleEntry(); break;
        case Stage::Confirm:    renderConfirm();    break;
        case Stage::Submitting: renderSubmitting(); break;
        case Stage::Done:       renderDone();       break;
        case Stage::Failed:     renderFailed();     break;
    }
}

void doSubmit() {
    g_stage = Stage::Submitting;
    g_dirty = true;
    g_submitPending = true;
}

void onEnter() {
    if (crashlog::hasPriorCrash()) {
        g_kind     = Kind::Bug;
        g_prefilled = true;
        char buf[80];
        const char* app = crashlog::priorApp().empty()
                            ? "?" : crashlog::priorApp().c_str();
        snprintf(buf, sizeof(buf), "Crash on boot: %s in %s",
                 crashlog::priorReason(), app);
        g_title = buf;
        g_stage = Stage::TitleEntry;
        crashlog::acknowledgePriorCrash();
    } else {
        g_kind      = Kind::Bug;
        g_typeSel   = 0;
        g_title.clear();
        g_prefilled = false;
        g_stage     = Stage::TypeSelect;
    }
    g_dirty = true;
}

void onExit() {}

void onTick() {
    static uint32_t lastBlink = 0;
    uint32_t now = millis();
    if (now - lastBlink >= 500) {
        lastBlink = now;
        if (g_stage == Stage::TitleEntry) g_dirty = true;
    }

    if (g_submitPending) {
        g_submitPending = false;
        std::string body = buildBody();
        g_result = github::submitIssue(g_title, body, kindLabel());
        g_stage  = g_result.ok ? Stage::Done : Stage::Failed;
        g_dirty  = true;
    }
}

void onKey(char ch) {
    switch (g_stage) {
        case Stage::TypeSelect:
            if (ch == key::Up || ch == key::Down) {
                g_typeSel = 1 - g_typeSel;
                g_dirty   = true;
            } else if (ch == '\n') {
                g_kind  = g_typeSel == 0 ? Kind::Bug : Kind::Feature;
                g_stage = Stage::TitleEntry;
                g_dirty = true;
            }
            break;

        case Stage::TitleEntry:
            if (ch == '\n') {
                if (!g_title.empty()) {
                    g_stage = Stage::Confirm;
                    g_dirty = true;
                }
            } else if (ch == '\b') {
                if (!g_title.empty()) {
                    g_title.pop_back();
                    g_dirty = true;
                }
            } else if (ch >= 0x20 && ch <= 0x7E) {
                if (g_title.size() < 120) {
                    g_title.push_back(ch);
                    g_dirty = true;
                }
            }
            break;

        case Stage::Confirm:
            if (ch == '\n')      doSubmit();
            else if (ch == '\b') { g_stage = Stage::TitleEntry; g_dirty = true; }
            break;

        case Stage::Failed:
            if (ch == '\n')      doSubmit();
            else if (ch == '\b') { g_stage = Stage::TitleEntry; g_dirty = true; }
            break;

        case Stage::Done:
        case Stage::Submitting:
        default:
            break;
    }
}

void onDraw() {
    if (!g_dirty) return;
    render();
    g_dirty = false;
}

App report_app = {
    .id           = "report",
    .name         = "Report",
    .description  = "File an issue",
    .services     = SVC_WIFI | SVC_SD | SVC_CANVAS,
    .onEnter      = onEnter,
    .onExit       = onExit,
    .onTick       = onTick,
    .onKey        = onKey,
    .onDraw       = onDraw,
    .onEvent      = nullptr,
    .keysAsArrows = false,  // title field accepts ;./,/ as text
    .hidden       = true,   // launched from Settings or auto-prompt
};

}  // namespace

REGISTER_APP(report_app);
