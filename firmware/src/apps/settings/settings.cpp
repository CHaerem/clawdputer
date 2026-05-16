// Settings app — organised into sections. Up/Down to move, Enter to invoke.

#include <Arduino.h>
#include <M5Cardputer.h>
#include <time.h>

#include <functional>
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
    uint16_t valueColor = 0;   // 0 = default; otherwise overrides info-row colour
};

std::vector<Item> g_items;
int  g_selected = 0;
bool g_dirty    = true;
std::string g_toast;
uint32_t    g_toastUntil = 0;

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

void actInstallUpdate() {
    // Triggers a recovery-boot OTA: device reboots, fetches the manifest in
    // a minimal env (where the heap isn't fragmented enough to block the
    // mbedTLS handshake), and flashes if a newer SHA is published. If
    // there's nothing newer, recovery just reboots back to normal mode.
    toast("rebooting to install…");
    delay(600);
    updater::installNow();   // never returns
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

void actReconnectWifi() {
    if (wifi::reconnect()) toast("WiFi reconnecting…");
    else toast("no saved WiFi");
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

void actToggleReport() {
    if (!settings::reportAvailable()) {
        toast("no PAT compiled in");
        return;
    }
    bool v = !settings::reportEnabled();
    settings::setReportEnabled(v);
    toast(v ? "crash reports: on" : "crash reports: off");
}

void actReportIssue() {
    const App* a = registry::find("report");
    if (a) clawd_request_app(a);
    else toast("report app not found");
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

std::string relativeTime(uint32_t epoch) {
    if (epoch == 0) return "never";
    time_t now = time(nullptr);
    if (now < 1704067200 || now < (time_t)epoch) return "just now";
    uint32_t diff = (uint32_t)(now - epoch);
    char buf[24];
    if (diff < 45)             snprintf(buf, sizeof(buf), "just now");
    else if (diff < 3600)      snprintf(buf, sizeof(buf), "%um ago", diff / 60);
    else if (diff < 86400)     snprintf(buf, sizeof(buf), "%uh ago", diff / 3600);
    else if (diff < 86400 * 7) snprintf(buf, sizeof(buf), "%ud ago", diff / 86400);
    else                       snprintf(buf, sizeof(buf), "%uw ago", diff / (86400 * 7));
    return buf;
}

uint16_t statusColor(updater::Status s) {
    switch (s) {
        case updater::Status::UpToDate:        return 0x07E0;  // green
        case updater::Status::UpdateAvailable: return 0xFFE0;  // yellow
        case updater::Status::Checking:
        case updater::Status::Downloading:     return 0x07FF;  // cyan
        case updater::Status::Failed:          return 0xF800;  // red
        default:                               return 0x8C71;  // gray
    }
}

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
    auto cinfo = [](const char* label, const std::string& value, uint16_t color) {
        Item it;
        it.label      = label;
        it.value      = value;
        it.valueColor = color;
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

    // ── UPDATES (promoted: this is what users come to Settings for) ──
    g_items.push_back(hdr("UPDATES"));
    {
        auto s = updater::status();
        g_items.push_back(cinfo("status", updater::statusText(), statusColor(s)));

        std::string installed = updater::currentVersion();
        const char* curDate   = updater::currentBuiltAt();
        if (curDate && curDate[0]) {
            installed += "  ";
            installed += curDate;
        }
        g_items.push_back(info("installed", installed));

        std::string latest = updater::latestVersion();
        if (latest.empty()) {
            g_items.push_back(cinfo("latest", "unknown", 0x8C71));
        } else {
            std::string row = latest;
            std::string lbd = updater::latestBuiltAt();
            if (!lbd.empty()) {
                row += "  ";
                row += lbd;
            }
            // Highlight the latest row if it differs from installed.
            bool newer = (s == updater::Status::UpdateAvailable);
            g_items.push_back(cinfo("latest", row, newer ? 0xFFE0 : 0x07E0));
        }

        g_items.push_back(info("last check", relativeTime(updater::lastCheckEpoch())));

        if (s == updater::Status::Failed && !updater::lastError().empty()) {
            std::string err = updater::lastError();
            if (err.size() > 28) err = err.substr(0, 27) + "…";
            g_items.push_back(cinfo("error", err, 0xF800));
        }

        g_items.push_back(act("check & install →", actInstallUpdate));
    }

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
    g_items.push_back(act("reconnect WiFi",    actReconnectWifi));
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
    if (settings::reportAvailable()) {
        g_items.push_back(tog("crash reports",
            settings::reportEnabled() ? std::string("on") : std::string("off"),
            actToggleReport));
    }

    // ── SYSTEM ──
    g_items.push_back(hdr("SYSTEM"));
    if (settings::reportEnabled()) {
        g_items.push_back(act("report a problem →", actReportIssue));
    }
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
            // Read-only info value. Use a wider column than toggles/actions
            // so timestamps and SHAs fit without ugly truncation.
            int infoValX = 90;
            d.setCursor(infoValX, y + 1);
            uint16_t col = item.valueColor
                ? item.valueColor
                : (selected ? 0xFFFF : WHITE);
            d.setTextColor(col);
            std::string v = item.value;
            int maxChars = (SCREEN_W - infoValX - 6) / 6;
            if ((int)v.size() > maxChars) v = v.substr(0, maxChars - 1) + "…";
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

updater::Status g_lastSeenStatus = updater::Status::Idle;

void onEnter() {
    g_lastSeenStatus = updater::status();
    rebuild();
    clampSelection();
    g_dirty = true;
}

void onExit() {}

void onTick() {
    static uint32_t lastPoll = 0;
    static uint32_t lastFingerprint = 0;
    uint32_t now = millis();
    if (now - lastPoll >= 1000) {
        lastPoll = now;
        auto cur = updater::status();
        if (cur != g_lastSeenStatus) {
            switch (cur) {
                case updater::Status::UpToDate:
                    toast("up to date");
                    break;
                case updater::Status::UpdateAvailable:
                    toast(std::string("update: ") + updater::latestVersion());
                    break;
                case updater::Status::Failed: {
                    std::string err = updater::lastError();
                    if (err.empty()) err = "unknown";
                    if (err.size() > 24) err = err.substr(0, 23) + "…";
                    toast(std::string("check failed: ") + err);
                    break;
                }
                default:
                    break;
            }
            g_lastSeenStatus = cur;
        }
        // Cheap content hash — only rebuild + redraw when something visible
        // actually changed. Stops the per-second full repaint from causing
        // flicker (especially when the canvas had to be released for a TLS
        // handshake and couldn't be reacquired afterwards).
        uint32_t epoch = updater::lastCheckEpoch();
        uint32_t fp = (uint32_t)cur * 1315423911u;
        fp ^= std::hash<std::string>{}(updater::latestVersion()) & 0xFFFFu;
        fp ^= (epoch / 60u) << 8;   // bucket to one tick per minute
        fp ^= (uint32_t)settings::autoUpdateEnabled() << 1;
        fp ^= (uint32_t)settings::audioEnabled()      << 2;
        fp ^= (uint32_t)settings::petEnabled()        << 3;
        fp ^= (uint32_t)settings::shakeEnabled()      << 4;
        fp ^= (uint32_t)settings::reportEnabled()     << 5;
        if (fp != lastFingerprint) {
            lastFingerprint = fp;
            rebuild();
            g_dirty = true;
        }
    }
    if (!g_toast.empty() && now >= g_toastUntil) {
        g_toast.clear();
        g_dirty = true;
    }
}

void onKey(char ch) {
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
    .services    = SVC_WIFI | SVC_CANVAS,
    .onEnter     = onEnter,
    .onExit      = onExit,
    .onTick      = onTick,
    .onKey       = onKey,
    .onDraw      = onDraw,
    .onEvent     = nullptr,
};

}  // namespace

REGISTER_APP(settings_app);
