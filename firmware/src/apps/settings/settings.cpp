// Settings app — actions and read-only diagnostics. Up/Down to move,
// Enter to invoke the selected action. Read-only rows are skipped during
// navigation.

#include <Arduino.h>
#include <M5Cardputer.h>

#include <string>
#include <vector>

#include "core/app.h"
#include "core/key.h"
#include "core/registry.h"
#include "services/ble.h"
#include "services/identity.h"
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
    std::string value;       // empty if action-only
    void (*action)();        // nullptr for read-only rows
};

std::vector<Item> g_items;
int  g_selected = 0;
bool g_dirty    = true;
std::string g_toast;
uint32_t    g_toastUntil = 0;

void toast(const std::string& msg) {
    g_toast       = msg;
    g_toastUntil  = millis() + 2500;
    g_dirty       = true;
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

void actCheckUpdate() {
    updater::checkNow();
    toast("checking for updates…");
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

bool g_showPubkey = false;
bool g_showSealKey = false;
int  g_pubkeyScroll = 0;

void actShowPubkey() {
    g_showPubkey   = true;
    g_showSealKey  = false;
    g_pubkeyScroll = 0;
    g_dirty        = true;
}

void actShowSealKey() {
    g_showSealKey  = true;
    g_showPubkey   = false;
    g_pubkeyScroll = 0;
    g_dirty        = true;
}

bool g_showInstall = false;

void actShowInstall() {
    g_showInstall  = true;
    g_showPubkey   = false;
    g_showSealKey  = false;
    g_pubkeyScroll = 0;
    g_dirty        = true;
}

void rebuild() {
    g_items.clear();
    g_items.push_back({"device",     ble::deviceName(),                                          nullptr});
    g_items.push_back({"buddy",      ble::isConnected(EventSource::NusLink)   ? "connected" : "—", nullptr});
    g_items.push_back({"bridge",     ble::isConnected(EventSource::BridgeLink)? "connected" : "—", nullptr});
    g_items.push_back({"wifi ssid",  wifi::ssid().empty() ? std::string("(unset)") : wifi::ssid(), nullptr});
    g_items.push_back({"wifi ip",    wifi::isConnected() ? wifi::ip() : std::string("—"),         nullptr});
    g_items.push_back({"build",      updater::currentVersion(),                                   nullptr});

    std::string ver = updater::latestVersion();
    std::string latestStr = ver.empty()
        ? std::string(updater::statusText())
        : (ver + std::string(" (") + updater::statusText() + ")");
    g_items.push_back({"latest",     latestStr,                                                   nullptr});

#ifdef CLAWD_OTA_PASSWORD
    g_items.push_back({"ota password", "set",                                                    nullptr});
#else
    g_items.push_back({"ota password", "(unset)",                                                nullptr});
#endif
    g_items.push_back({"ssh pubkey fp", identity::fingerprint().empty() ? std::string("—") : identity::fingerprint(), nullptr});
    g_items.push_back({"show SSH pubkey",  "",                                                  actShowPubkey});
    g_items.push_back({"show seal key",    "",                                                  actShowSealKey});
    g_items.push_back({"bridge install cmd","",                                                  actShowInstall});
    g_items.push_back({"audio",             settings::audioEnabled() ? std::string("on") : std::string("off"), actToggleAudio});
    g_items.push_back({"pet face",          settings::petEnabled()   ? std::string("on") : std::string("off"), actTogglePet});
    g_items.push_back({"shake → dizzy",     settings::shakeEnabled() ? std::string("on") : std::string("off"), actToggleShake});
    g_items.push_back({"configure WiFi",    "",                                                  actConfigureWifi});
    g_items.push_back({"check for updates", "",                                                  actCheckUpdate});
    g_items.push_back({"clear WiFi creds",  "",                                                  actClearWifi});
    g_items.push_back({"clear BLE bonds",   "",                                                  actClearBonds});
    g_items.push_back({"reboot device",     "",                                                  actReboot});

    if (g_selected >= (int)g_items.size()) g_selected = (int)g_items.size() - 1;
    if (g_selected < 0) g_selected = 0;
}

bool isAction(int idx) {
    return idx >= 0 && idx < (int)g_items.size() && g_items[idx].action != nullptr;
}

void moveSelection(int dir) {
    if (g_items.empty()) return;
    int n = (int)g_items.size();
    int next = g_selected;
    for (int i = 0; i < n; i++) {
        next = (next + dir + n) % n;
        // Allow stopping on any row, but prefer actions when there are actions
        // available. For simplicity: stop on every row.
        break;
    }
    g_selected = next;
    g_dirty    = true;
}

void renderScrollable(const char* title, uint16_t titleColor,
                      const std::string& body, uint16_t bodyColor) {
    auto& d = ui::display();
    ui::beginFrame();
    ui::statusbar::draw();

    d.setTextSize(1);
    d.setTextColor(titleColor);
    d.setCursor(6, ui::statusbar::HEIGHT + 3);
    d.print(title);

    int y = ui::statusbar::HEIGHT + 16;
    int maxChars = 38;
    int lineH = 9;
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

void render() {
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
        bool selected = i == g_selected;
        if (selected) d.fillRoundRect(2, y - 1, SCREEN_W - 4, row_h, 2, 0x18E3);

        d.setCursor(6, y + 1);
        d.setTextColor(selected ? 0xFFE0 : 0x8C71);
        d.print(g_items[i].label.c_str());

        d.setCursor(110, y + 1);
        bool action = g_items[i].action != nullptr;
        if (action) {
            d.setTextColor(selected ? 0xFFFF : 0xFFE0);
            d.print("enter \xC3\xAB");
        } else {
            d.setTextColor(selected ? 0xFFFF : WHITE);
            std::string v = g_items[i].value;
            if (v.size() > 22) v = v.substr(0, 21) + "…";
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

void onEnter() {
    rebuild();
    g_dirty = true;
}

void onExit() {}

void onTick() {
    static uint32_t lastRefresh = 0;
    uint32_t now = millis();
    if (now - lastRefresh >= 1000) {
        lastRefresh = now;
        rebuild();
        g_dirty = true;
    }
    if (!g_toast.empty() && now >= g_toastUntil) {
        g_toast.clear();
        g_dirty = true;
    }
}

void onKey(char ch) {
    if (g_showPubkey || g_showSealKey || g_showInstall) {
        if (ch == key::Up && g_pubkeyScroll > 0) { g_pubkeyScroll--; g_dirty = true; }
        else if (ch == key::Down) { g_pubkeyScroll++; g_dirty = true; }
        else if (ch == '\n')      { g_showPubkey = false; g_showSealKey = false; g_showInstall = false; g_dirty = true; }
        return;
    }
    if (ch == key::Up) {
        moveSelection(-1);
    } else if (ch == key::Down) {
        moveSelection(+1);
    } else if (ch == '\n') {
        if (isAction(g_selected)) g_items[g_selected].action();
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
