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
#include "services/updater.h"
#include "services/wifi.h"
#include "ui/canvas.h"

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
    if (home) enter(home);
    else if (registry::count() > 0) enter(registry::at(0));
}

void dispatchKeys() {
    if (!M5Cardputer.Keyboard.isChange()) return;
    if (!M5Cardputer.Keyboard.isPressed()) return;
    auto state = M5Cardputer.Keyboard.keysState();

    if (state.tab) {
        goHome();
        return;
    }

    if (!g_active || !g_active->onKey) return;

    // Fn + ;./, → arrow keys. Translate first; if any of those keys are
    // present in this keystate, suppress the corresponding literal char.
    if (state.fn) {
        bool handled = false;
        for (char ch : state.word) {
            switch (ch) {
                case ';': g_active->onKey(key::Up);    handled = true; break;
                case '.': g_active->onKey(key::Down);  handled = true; break;
                case ',': g_active->onKey(key::Left);  handled = true; break;
                case '/': g_active->onKey(key::Right); handled = true; break;
                default: break;
            }
        }
        if (handled) {
            if (state.enter) g_active->onKey('\n');
            if (state.del)   g_active->onKey('\b');
            return;
        }
    }

    for (char ch : state.word) g_active->onKey(ch);
    if (state.enter) g_active->onKey('\n');
    if (state.del)   g_active->onKey('\b');
}

}  // namespace

// Apps call this to queue a switch. Applied at the start of the next loop
// iteration so the caller's onKey/onTick can return cleanly first.
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

    // Touching ui::display() triggers the canvas allocation so we know up
    // front whether the backbuffer is available.
    (void)ui::display();

    updater::begin();
    ble::begin();
    wifi::begin();

    goHome();
    if (!g_active) Serial.println("[clawdputer] no apps registered — idle");
}

void loop() {
    M5Cardputer.update();
    ota::tick();
    updater::tick();
    if (ota::isUpdating()) {
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
