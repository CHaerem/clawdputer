// clawdputer firmware entrypoint.
//
// Boots core services and runs the active app's lifecycle. The app registry
// is populated by static initialisers in each apps/<name>/*.cpp; this file
// stays agnostic of which apps are linked in.

#include <Arduino.h>
#include <M5Cardputer.h>

#include "core/app.h"
#include "core/key.h"
#include "core/registry.h"
#include "services/ble.h"
#include "services/ota.h"
#include "services/wifi.h"

namespace {

const App* g_active  = nullptr;
const App* g_pending = nullptr;

void enter(const App* app) {
    if (!app) return;
    if (g_active && g_active->onExit) g_active->onExit();
    g_active = app;
    if (g_active && g_active->onEnter) g_active->onEnter();
    Serial.printf("[clawdputer] entered app: %s\n", g_active->id);
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
    if (home) {
        enter(home);
    } else if (registry::count() > 0) {
        enter(registry::at(0));
    }
}

void dispatchKeys() {
    if (!M5Cardputer.Keyboard.isChange()) return;
    if (!M5Cardputer.Keyboard.isPressed()) return;
    auto state = M5Cardputer.Keyboard.keysState();

    // Tab always takes you home — like the home button on a phone.
    if (state.tab) {
        goHome();
        return;
    }

    if (!g_active || !g_active->onKey) return;

    for (char ch : state.word) {
        if (state.fn) {
            // Cardputer's Fn modifier turns ;./,/ into arrow keys.
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

// Apps call this to ask for a switch. We defer the actual enter() until the
// next loop iteration so the calling app's current callback can return
// cleanly first.
void clawd_request_app(const App* app) { g_pending = app; }

void setup() {
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    M5Cardputer.Display.setRotation(1);

    Serial.begin(115200);
    delay(200);
    Serial.println();
    Serial.println("[clawdputer] boot");
    Serial.printf("[clawdputer] %u app(s) registered\n", (unsigned)registry::count());

    ble::begin();
    wifi::begin();

    goHome();
    if (!g_active) Serial.println("[clawdputer] no apps registered — idle");
}

void loop() {
    M5Cardputer.update();
    ota::tick();
    if (ota::isUpdating()) {
        // OTA owns the screen and the CPU; skip app ticks until reboot.
        delay(5);
        return;
    }
    if (g_pending && g_pending != g_active) {
        const App* next = g_pending;
        g_pending = nullptr;
        enter(next);
    }
    dispatchKeys();
    if (g_active && g_active->onTick) g_active->onTick();
    if (g_active && g_active->onDraw) g_active->onDraw();
    delay(20);
}
