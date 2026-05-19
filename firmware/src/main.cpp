// clawdputer firmware entrypoint.
//
// Boots core services and runs the active app's lifecycle. The app registry
// is populated by static initialisers in each apps/<name>/*.cpp; this file
// stays agnostic of which apps are linked in.

#include <Arduino.h>
#include <M5Cardputer.h>

#include <string>

#include "core/app.h"
#include "core/key.h"
#include "core/registry.h"
#include "services/audio.h"
#include "services/ble.h"
#include "services/bridge.h"
#include "services/crashlog.h"
#include "services/health.h"
#include "services/identity.h"
#include "services/imu.h"
#include "services/ota.h"
#include "services/power.h"
#include "services/settings.h"
#include "services/sd.h"
#include "services/updater.h"
#include "services/wifi.h"

extern "C" void clawd_bridge_tick();
#include "ui/canvas.h"

namespace {

const App* g_active  = nullptr;
const App* g_pending = nullptr;

void applyServices(uint32_t needed) {
    if (needed & SVC_WIFI) { if (wifi::isPaused()) wifi::resume(); }
    else                   { if (!wifi::isPaused()) wifi::pause(); }

    if (needed & SVC_BLE)  { if (ble::isPaused()) ble::resume(); }
    else                   { if (!ble::isPaused()) ble::pause(); }

    if (needed & SVC_SD)   { if (sd::isPaused()) sd::resume(); }
    else                   { if (!sd::isPaused()) sd::pause(); }

    // Canvas allocation order matters: do this AFTER pause() calls have freed
    // BLE/WiFi heap, so the 32 KB sprite alloc has the best chance of finding
    // a contiguous block. release before the new app's onEnter draws to it.
    if (needed & SVC_CANVAS) ui::tryAcquireCanvas();
    else                     ui::releaseCanvas();
}

void enter(const App* app) {
    if (!app) return;
    if (g_active && g_active->onExit) g_active->onExit();
    applyServices(app->services);
    g_active = app;
    crashlog::noteAppEntered(app->id);
    health::noteAppEntered(app->id);
    if (g_active->onEnter) g_active->onEnter();
    Serial.printf("[clawdputer] entered app: %s\n", g_active->id);
    Serial0.printf("[clawdputer] entered app: %s\n", g_active->id);
}

const App* findApp(const char* id) {
    for (size_t i = 0; i < registry::count(); i++) {
        const App* a = registry::at(i);
        if (strcmp(a->id, id) == 0) return a;
    }
    return nullptr;
}

void goHome() {
    const App* home = findApp("home");
    if (home) enter(home);
    else if (registry::count() > 0) enter(registry::at(0));
}

// Last keystate snapshot — read by the home app to render an on-screen
// debug line. Helpful when a key combo isn't doing what we expect.
struct LastKey {
    bool        fn, opt, shift, ctrl, alt, tab, del, enter, space;
    std::string word;
    uint32_t    at;
} g_lastKey;

void dispatchSideButton() {
    if (M5Cardputer.BtnA.wasPressed()) {
        power::noteActivity();
        goHome();
    }
}

void dispatchKeys() {
    // isChange() has a side effect (updates _last_key_size) so we MUST
    // only call it once per loop iteration. Putting it anywhere else in
    // the loop steals the edge transition.
    if (!M5Cardputer.Keyboard.isChange()) return;
    if (!M5Cardputer.Keyboard.isPressed()) return;
    power::noteActivity();
    // Any keypress also cancels a pending low-battery sleep countdown.
    if (power::lowBatteryWarning()) power::cancelLowBatteryWarning();
    auto state = M5Cardputer.Keyboard.keysState();

    g_lastKey.fn    = state.fn;
    g_lastKey.opt   = state.opt;
    g_lastKey.shift = state.shift;
    g_lastKey.ctrl  = state.ctrl;
    g_lastKey.alt   = state.alt;
    g_lastKey.tab   = state.tab;
    g_lastKey.del   = state.del;
    g_lastKey.enter = state.enter;
    g_lastKey.space = state.space;
    g_lastKey.word.clear();
    for (char c : state.word) g_lastKey.word.push_back(c);
    g_lastKey.at = millis();

    Serial.printf("[key] fn=%d opt=%d shift=%d tab=%d enter=%d del=%d word='",
                  (int)state.fn, (int)state.opt, (int)state.shift,
                  (int)state.tab, (int)state.enter, (int)state.del);
    for (char c : state.word) Serial.printf("%c", c);
    Serial.println("'");

    if (state.tab) {
        goHome();
        return;
    }

    if (!g_active || !g_active->onKey) return;

    // Translate ; . , / to arrow keys when either Fn is held (standard
    // Cardputer convention) OR the active app is menu-style. Text-input
    // apps (keysAsArrows=false) only get the arrow translation with Fn.
    bool aliasArrows = state.fn || g_active->keysAsArrows;
    for (char ch : state.word) {
        if (aliasArrows) {
            switch (ch) {
                case ';': g_active->onKey(key::Up);    continue;
                case '.': g_active->onKey(key::Down);  continue;
                case ',': g_active->onKey(key::Left);  continue;
                case '/': g_active->onKey(key::Right); continue;
                default: break;
            }
        }
        g_active->onKey(ch);
    }
    if (state.enter) g_active->onKey('\n');
    if (state.del)   g_active->onKey('\b');
}

}  // namespace

// Apps call this to queue a switch. Applied at the start of the next loop
// iteration so the caller's onKey/onTick can return cleanly first.
void clawd_request_app(const App* app) { g_pending = app; }

extern "C" const char* clawd_last_key_summary() {
    static char buf[80];
    snprintf(buf, sizeof(buf),
             "fn=%d opt=%d sh=%d en=%d del=%d w='%.20s'",
             (int)g_lastKey.fn, (int)g_lastKey.opt, (int)g_lastKey.shift,
             (int)g_lastKey.enter, (int)g_lastKey.del, g_lastKey.word.c_str());
    return buf;
}

void setup() {
    // UART0 ping as the very first action — used by the Wokwi smoke
    // test to disambiguate "Wokwi can see UART0" from "M5Cardputer.begin
    // hung." On real hardware UART0 is unused outside the dock header,
    // so the extra bytes are harmless.
    Serial0.begin(115200);
    Serial0.println();
    Serial0.println("[clawdputer] pre-init");

    auto cfg = M5.config();
    // Without this, autodetect in Wokwi (where the ST7789 doesn't reply
    // to panel-ID reads) lands on board_M5PowerHub via the I2C 0x50
    // probe on GPIO 45/48, then watchdog-panics in PowerHub-specific
    // init. On real Cardputer hardware autodetect always succeeds, so
    // this fallback is never consulted.
    cfg.fallback_board = m5::board_t::board_M5Cardputer;
    M5Cardputer.begin(cfg, true);
    M5Cardputer.Display.setRotation(1);

    Serial.begin(115200);
    delay(200);
    Serial.println();
    Serial.println("[clawdputer] boot");
    Serial.printf("[clawdputer] %u app(s) registered\n", (unsigned)registry::count());
    Serial0.println("[clawdputer] boot");
    Serial0.printf("[clawdputer] %u app(s) registered\n", (unsigned)registry::count());

    // Touching ui::display() triggers the canvas allocation so we know up
    // front whether the backbuffer is available. Allocated first because
    // the sprite needs a single large contiguous heap block — once BLE
    // and WiFi have run their begin(), the heap is fragmented enough
    // that the canvas alloc may fail even with plenty of total bytes.
    (void)ui::display();

    power::begin();
    settings::begin();
    audio::begin();
    imu::begin();
    updater::begin();
    identity::begin();
    // ble::begin() is deferred — applyServices() will reinit on first SVC_BLE
    // app entry. Saves ~90 KB of heap during boot so the canvas alloc can
    // grab a contiguous block.
    wifi::begin();
    sd::begin();
    // crashlog needs sd::isAvailable() so init order is sd → crashlog.
    crashlog::begin();
    health::begin();
    bridge::begin();

    goHome();
    if (!g_active) Serial.println("[clawdputer] no apps registered — idle");

    // Auto-prompt the report app when the previous boot was a crash. Done
    // after goHome() so home renders first and the user sees the device
    // boot normally before the prompt comes up.
    if (crashlog::hasPriorCrash()) {
        const App* report = findApp("report");
        if (report) clawd_request_app(report);
    }
}

void loop() {
    M5Cardputer.update();
    ota::tick();
    updater::tick();
    crashlog::tick();
    health::tick();
    clawd_bridge_tick();
    if (ota::isUpdating()) {
        delay(5);
        return;
    }
    if (g_pending && g_pending != g_active) {
        const App* next = g_pending;
        g_pending = nullptr;
        enter(next);
    }
    dispatchSideButton();
    dispatchKeys();
    // IMU polled at ~10 Hz instead of the full loop rate — shake events
    // are still caught reliably and we save a noticeable bit of current.
    static uint32_t lastImuTick = 0;
    uint32_t now = millis();
    if (now - lastImuTick >= 100) {
        lastImuTick = now;
        imu::tick();
    }
    if (g_active && g_active->onTick) g_active->onTick();
    if (g_active && g_active->onDraw) g_active->onDraw();

    // Overlay the low-battery countdown on top of whatever just rendered.
    if (power::lowBatteryWarning()) {
        auto& d = ui::display();
        int boxY = 50;
        d.fillRoundRect(20, boxY, SCREEN_W - 40, 38, 6, 0x4800);
        d.drawRoundRect(20, boxY, SCREEN_W - 40, 38, 6, 0xFD20);
        d.setTextSize(1);
        d.setTextColor(0xFFFF);
        d.setCursor(28, boxY + 6);
        d.print("low battery — sleeping in");
        d.setTextSize(2);
        d.setTextColor(0xF800);
        d.setCursor(28, boxY + 18);
        char buf[16];
        snprintf(buf, sizeof(buf), "%d s", power::lowBatterySecondsLeft());
        d.print(buf);
        d.setTextSize(1);
        d.setTextColor(0xC618);
        d.setCursor(120, boxY + 24);
        d.print("any key cancels");
        ui::flush();
    }

    power::tick();
    delay(power::loopDelayMs());
}
