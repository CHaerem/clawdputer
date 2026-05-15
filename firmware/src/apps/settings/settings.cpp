// Settings app — organised into sections. Up/Down to move, Enter to invoke.

#include <Arduino.h>
#include <M5Cardputer.h>

#include <string>
#include <vector>

#include "core/app.h"
#include "core/key.h"
#include "core/registry.h"
#include "services/ble.h"
#include "services/identity.h"
#include "services/power.h"
#include "services/settings.h"
#include "services/updater.h"
#include "services/wifi.h"
#include "ui/canvas.h"
#include "ui/statusbar.h"

extern void clawd_request_app(const App* app);

#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#endif

namespace {

struct Item {
    std::string label;
    std::string value;
    void (*action)() = nullptr;
    bool isHeader    = false;
};

std::vector<Item> g_items;
int  g_selected = 0;
bool g_dirty    = true;
std::string g_toast;
uint32_t    g_toastUntil = 0;
updater::Status g_lastStatus = updater::Status::Idle;

// Update monitor mode
bool g_monitoringUpdate = false;
std::string g_prevLatest;

void toast(const std::string& msg) {
    g_toast      = msg;
    g_toastUntil = millis() + 2500;
    g_dirty      = true;
}

// ── actions ─────────────────────────────────────────────────────────────────

void actGoSysinfo() {
    const App* a = registry::find("sysinfo");
    if (a) clawd_request_app(a);
    else toast("sysinfo not found");
}

void actGoBattery() {
    const App* a = registry::find("battery");
    if (a) clawd_request_app(a);
    else toast("battery not found");
}

void actClearBonds() {
    ble::clearBonds();
    toast("BLE bonds cleared");
}

void actReboot() {
    toast("rebooting…");
    delay(400);
    ESP.restart();
}

void actSleepNow() {
    toast("deep-sleep — press G0 to wake");
    delay(800);
    power::deepSleep();
}

void actCheckUpdate() {
    updater::checkNow();
    g_monitoringUpdate = true;
    g_prevLatest = updater::latestVersion();
    g_dirty = true;
}

void actClearWifi() {
    wifi::clearCredentials();
    toast("WiFi cleared (reboot to apply)");
}

void actConfigureWifi() {
    const App* w = registry::find("wifi");
    if (w) clawd_request_app(w);
    else toast("wifi app not registered");
}

void actToggleAudio() {
    bool v = !settings::audioEnabled();
    settings::setAudioEnabled(v);
    toast(v ? "audio: on" : "audio: off");
}

void actTogglePet() {
    bool v = !settings::petEnabled();
    settings::setPetEnabled(v);
    toast(v ? "pet: on" : "pet: off");
}

void actToggleShake() {
    bool v = !settings::shakeEnabled();
    settings::setShakeEnabled(v);
    toast(v ? "shake: on" : "shake: off");
}

void actToggleAutoUpdate() {
    bool v = !settings::autoUpdateEnabled();
    settings::setAutoUpdateEnabled(v);
    toast(v ? "auto-update: on (5 min)" : "auto-update: off (manual only)");
}

bool g_showPubkey  = false;
bool g_showSealKey = false;
bool g_showInstall = false;
int  g_pubkeyScroll = 0;

void actShowPubkey() {
    g_showPubkey   = true;
    g_showSealKey  = false;
    g_showInstall  = false;
    g_pubkeyScroll = 0;
    g_dirty        = true;
}

void actShowSealKey() {
    g_showSealKey  = true;
    g_showPubkey   = false;
    g_showInstall  = false;
    g_pubkeyScroll = 0;
    g_dirty        = true;
}

void actShowInstall() {
    g_showInstall  = true;
    g_showPubkey   = false;
    g_showSealKey  = false;
    g_pubkeyScroll = 0;
    g_dirty        = true;
}

// ── item list ────────────────────────────────────────────────────────────────

void rebuild() {
    g_items.clear();

    auto hdr = [](const char* label) {
        Item it;
        it.label    = label;
        it.isHeader = true;
        return it;
    };
    auto info = [](const char* label, const std::string& value) {
        Item it;
        it.label  = label;
        it.value  = value;
        return it;
    };
    auto act = [](const char* label, void (*fn)()) {
        Item it;
        it.label  = label;
        it.action = fn;
        return it;
    };
    auto tog = [](const char* label, const std::string& value, void (*fn)()) {
        Item it;
        it.label  = label;
        it.value  = value;
        it.action = fn;
        return it;
    };

    // ── DEVICE ──
    g_items.push_back(hdr("DEVICE"));
    g_items.push_back(act("Device info →", actGoSysinfo));
    g_items.push_back(act("Battery →",     actGoBattery));

    // ── IDENTITY ──
    g_items.push_back(hdr("IDENTITY"));
    g_items.push_back(info("device",       ble::deviceName()));
    g_items.push_back(info("SSH pubkey fp",
        identity::fingerprint().empty() ? std::string("—") : identity::fingerprint()));
    g_items.push_back(act("show SSH pubkey",   actShowPubkey));
    g_items.push_back(act("show seal key",     actShowSealKey));
    g_items.push_back(act("bridge install cmd",actShowInstall));

    // ── NETWORK ──
    g_items.push_back(hdr("NETWORK"));
    g_items.push_back(act("configure WiFi",    actConfigureWifi));
    g_items.push_back(act("clear WiFi creds",  actClearWifi));
    g_items.push_back(act("clear BLE bonds",   actClearBonds));

    // ── PREFERENCES ──
    g_items.push_back(hdr("PREFERENCES"));
    g_items.push_back(tog("audio",
        settings::audioEnabled() ? std::string("on") : std::string("off"),
        actToggleAudio));
    g_items.push_back(tog("pet face",
        settings::petEnabled()   ? std::string("on") : std::string("off"),
        actTogglePet));
    g_items.push_back(tog("shake → dizzy",
        settings::shakeEnabled() ? std::string("on") : std::string("off"),
        actToggleShake));
    g_items.push_back(tog("auto-update",
        settings::autoUpdateEnabled() ? std::string("every 5 min") : std::string("manual only"),
        actToggleAutoUpdate));

    // ── UPDATES ──
    g_items.push_back(hdr("UPDATES"));
    g_items.push_back(info("build", updater::currentVersion()));
    g_items.push_back(act("check for updates", actCheckUpdate));

    // ── SYSTEM ──
    g_items.push_back(hdr("SYSTEM"));
    g_items.push_back(act("reboot device",    actReboot));
    g_items.push_back(act("sleep (G0 wakes)", actSleepNow));
}

void clampSelection() {
    int n = (int)g_items.size();
    if (n == 0) return;
    if (g_selected >= n) g_selected = n - 1;
    if (g_selected < 0)  g_selected = 0;
    if (!g_items[g_selected].isHeader) return;
    // Find the nearest selectable row below, then above.
    for (int i = g_selected + 1; i < n; i++) {
        if (!g_items[i].isHeader) { g_selected = i; return; }
    }
    for (int i = g_selected - 1; i >= 0; i--) {
        if (!g_items[i].isHeader) { g_selected = i; return; }
    }
}

void moveSelection(int dir) {
    if (g_items.empty()) return;
    int n = (int)g_items.size();
    int next = g_selected;
    for (int attempts = 0; attempts < n; attempts++) {
        next = (next + dir + n) % n;
        if (!g_items[next].isHeader) break;
    }
    g_selected = next;
    g_dirty    = true;
}

// ── rendering ────────────────────────────────────────────────────────────────

void renderScrollable(const char* title, uint16_t titleColor,
                      const std::string& body, uint16_t bodyColor) {
    auto& d = ui::display();
    ui::beginFrame();
    ui::statusbar::draw();

    d.setTextSize(1);
    d.setTextColor(titleColor);
    d.setCursor(6, ui::statusbar::HEIGHT + 3);
    d.print(title);

    int y        = ui::statusbar::HEIGHT + 16;
    int maxChars = 38;
    int lineH    = 9;
    int linesShown = 0;
    int maxLines = (124 - y) / lineH;
    int line = 0;
    d.setTextColor(bodyColor);
    for (size_t i = 0; i < body.size(); i += maxChars) {
        if (line >= g_pubkeyScroll && linesShown < maxLines) {
            std::string chunk = body.substr(i, maxChars);
            d.setCursor(4, y + linesShown * lineH);
            d.print(chunk.c_str());
            linesShown++;
        }
        line++;
    }

    d.fillRect(0, 124, SCREEN_W, 11, 0x1082);
    d.drawFastHLine(0, 124, SCREEN_W, 0x2945);
    d.setTextColor(0x8C71);
    d.setCursor(4, 127);
    d.print(";/  scroll   enter back");
    ui::flush();
}

void renderUpdateProgress() {
    auto& d = ui::display();
    ui::beginFrame();
    ui::statusbar::draw();

    updater::Status status = updater::status();
    const char* statusText = updater::statusText();
    std::string latest = updater::latestVersion();
    
    d.setTextSize(2);
    d.setTextColor(0xFFFF);
    d.setCursor(8, ui::statusbar::HEIGHT + 8);
    d.print("UPDATE");

    d.setTextSize(1);
    d.setTextColor(0x07E0);
    d.setCursor(8, ui::statusbar::HEIGHT + 28);
    
    if (status == updater::Status::Checking) {
        d.print("Checking for updates…");
    } else if (status == updater::Status::Downloading) {
        d.printf("Downloading %s", latest.c_str());
    } else if (status == updater::Status::UpToDate) {
        d.setTextColor(0x07E0);
        d.print("✓ Already up to date");
        d.setTextSize(0);
        d.setTextColor(0x8C71);
        d.setCursor(8, ui::statusbar::HEIGHT + 55);
        d.printf("Current: %s", updater::currentVersion());
    } else if (status == updater::Status::Failed) {
        d.setTextColor(0xF800);
        d.print("✗ Update failed");
        d.setTextSize(0);
        d.setTextColor(0x8C71);
        d.setCursor(8, ui::statusbar::HEIGHT + 55);
        std::string err = updater::lastError();
        if (err.size() > 38) err = err.substr(0, 35) + "…";
        d.print(err.c_str());
    } else {
        // Idle
        d.print("Ready");
    }

    d.fillRect(0, 124, SCREEN_W, 11, 0x1082);
    d.drawFastHLine(0, 124, SCREEN_W, 0x2945);
    d.setTextSize(0);
    d.setTextColor(0x8C71);
    d.setCursor(4, 127);
    if (status == updater::Status::UpToDate || status == updater::Status::Failed) {
        d.print("enter back to settings");
    } else {
        d.print("monitoring…  don't unplug");
    }
    ui::flush();
}

void render() {
    if (g_monitoringUpdate) {
        renderUpdateProgress();
        return;
    }
    if (g_showPubkey) {
        renderScrollable("SSH pubkey (paste to authorized_keys)", 0xFFFF,
                         identity::publicKeyOpenSsh(), 0x07E0);
        return;
    }
    if (g_showSealKey) {
        renderScrollable("seal key (export CLAWD_SEAL_KEY)", 0xFFE0,
                         identity::sealKeyBase64(), 0xFFE0);
        return;
    }
    if (g_showInstall) {
        renderScrollable("run on any Mac to install bridge", 0xFFFF,
                         std::string("curl -fsSL https://github.com/")
                            + "CHaerem/clawdputer/raw/main/host/install/online.sh"
                            + " | bash",
                         0x07E0);
        return;
    }

    auto& d = ui::display();
    ui::beginFrame();
    ui::statusbar::draw();

    d.setTextSize(1);
    int y          = ui::statusbar::HEIGHT + 2;
    int row_h      = 11;
    int footer_y   = 124;
    int viewport_h = footer_y - y;
    int max_rows   = viewport_h / row_h;

    int top = 0;
    if (g_selected >= max_rows) top = g_selected - max_rows + 1;

    for (int i = top; i < (int)g_items.size() && i < top + max_rows; i++) {
        const auto& item  = g_items[i];
        bool selected     = (i == g_selected);

        if (item.isHeader) {
            // Filled dark band — visually separates sections without ambiguity.
            d.fillRect(0, y, SCREEN_W, row_h, 0x0861);
            d.setCursor(6, y + 2);
            d.setTextColor(0x5AEB);
            d.print(item.label.c_str());
            y += row_h;
            continue;
        }

        bool isAction     = item.action != nullptr;
        bool isToggle     = isAction && !item.value.empty();
        bool isPureAction = isAction && item.value.empty();
        bool isToggleOn   = isToggle && (item.value == "on" || item.value == "every 5 min");

        if (selected) d.fillRoundRect(2, y - 1, SCREEN_W - 4, row_h, 2, 0x18E3);

        d.setCursor(6, y + 1);
        d.setTextColor(selected ? 0xFFE0 : 0xC618);
        d.print(item.label.c_str());

        int valX = SCREEN_W - 88;
        if (isToggle) {
            uint16_t pillBg = isToggleOn ? 0x0420 : 0x2104;
            uint16_t pillFg = isToggleOn ? 0x07E0 : 0x8C71;
            int pillW = (int)item.value.size() * 6 + 8;
            if (pillW < 28) pillW = 28;
            d.fillRoundRect(valX, y, pillW, row_h - 1, 3, pillBg);
            d.setCursor(valX + 4, y + 1);
            d.setTextColor(pillFg);
            d.print(item.value.c_str());
        } else if (isPureAction) {
            d.setCursor(valX, y + 1);
            d.setTextColor(selected ? 0xFFE0 : 0x8C71);
            d.print(selected ? "press \xC3\xAB" : "      \xC3\xAB");
        } else {
            // Read-only info value.
            d.setCursor(valX, y + 1);
            d.setTextColor(selected ? 0xFFFF : WHITE);
            std::string v = item.value;
            if (v.size() > 14) v = v.substr(0, 13) + "…";
            d.print(v.c_str());
        }
        y += row_h;
    }

    if (!g_toast.empty() && millis() < g_toastUntil) {
        d.fillRect(0, 108, SCREEN_W, 16, 0x4800);
        d.setTextColor(WHITE);
        d.setCursor(6, 113);
        d.print(g_toast.c_str());
    }

    d.fillRect(0, footer_y, SCREEN_W, SCREEN_H - footer_y, 0x1082);
    d.drawFastHLine(0, footer_y, SCREEN_W, 0x2945);
    d.setTextColor(0x8C71);
    d.setCursor(4, footer_y + 3);
    d.print(";/  move   enter invoke   tab home");

    ui::flush();
}

// ── lifecycle ────────────────────────────────────────────────────────────────

void onEnter() {
    rebuild();
    clampSelection();
    g_dirty = true;
}

void onExit() {}

void onTick() {
    static uint32_t lastRefresh = 0;
    uint32_t now = millis();
    
    // While monitoring updates, periodically refresh to show live status
    if (g_monitoringUpdate) {
        updater::Status status = updater::status();
        if (now - lastRefresh >= 500) {  // Refresh every 500ms max during update
            lastRefresh = now;
            g_dirty = true;
        }
        return;
    }

    if (now - lastRefresh >= 1000) {
        lastRefresh = now;
        rebuild();
        g_dirty = true;
    }
    if (!g_toast.empty() && now >= g_toastUntil) {
        g_toast.clear();
        g_dirty = true;
    }

    // Show result when update check completes (if not already monitoring)
    updater::Status status = updater::status();
    if (status != g_lastStatus) {
        g_lastStatus = status;
        if (status == updater::Status::UpToDate) {
            toast("✓ up to date");
        } else if (status == updater::Status::Failed) {
            std::string msg = "✗ update failed: ";
            msg += updater::lastError();
            if (msg.size() > 32) msg = msg.substr(0, 32);
            toast(msg);
        }
    }
}

void onKey(char ch) {
    if (g_monitoringUpdate) {
        updater::Status status = updater::status();
        // Allow exit if check completed (success or failure)
        if ((status == updater::Status::UpToDate || status == updater::Status::Failed) 
            && (ch == '\n' || ch == key::Left || ch == '\b')) {
            g_monitoringUpdate = false;
            g_dirty = true;
        }
        return;
    }
    if (g_showPubkey || g_showSealKey || g_showInstall) {
        if      (ch == key::Up   && g_pubkeyScroll > 0) { g_pubkeyScroll--; g_dirty = true; }
        else if (ch == key::Down)                        { g_pubkeyScroll++; g_dirty = true; }
        else if (ch == '\n') {
            g_showPubkey = g_showSealKey = g_showInstall = false;
            g_dirty = true;
        }
        return;
    }
    if      (ch == key::Up)   moveSelection(-1);
    else if (ch == key::Down) moveSelection(+1);
    else if (ch == '\n') {
        if (g_selected >= 0 && g_selected < (int)g_items.size()) {
            auto& item = g_items[g_selected];
            if (!item.isHeader && item.action) item.action();
        }
    }
}

void onDraw() {
    if (!g_dirty) return;
    render();
    g_dirty = false;
}

App settings_app = {
    .id          = "settings",
    .name        = "Settings",
    .description = "Info & actions",
    .services    = SVC_NONE,
    .onEnter     = onEnter,
    .onExit      = onExit,
    .onTick      = onTick,
    .onKey       = onKey,
    .onDraw      = onDraw,
    .onEvent     = nullptr,
};

}  // namespace

REGISTER_APP(settings_app);
