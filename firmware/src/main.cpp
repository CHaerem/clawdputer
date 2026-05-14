// clawdputer firmware entrypoint.
//
// Boots core services and runs the active app's lifecycle. The app registry
// is populated by static initialisers in each apps/<name>/*.cpp; this file
// stays agnostic of which apps are linked in.

#include <Arduino.h>
#include <M5Cardputer.h>

#include "core/app.h"
#include "core/registry.h"
#include "services/ble.h"

namespace {

const App* g_active = nullptr;

void enter(const App* app) {
    if (g_active && g_active->onExit) g_active->onExit();
    g_active = app;
    if (g_active && g_active->onEnter) g_active->onEnter();
}

void dispatchKeys() {
    if (!M5Cardputer.Keyboard.isChange()) return;
    if (!M5Cardputer.Keyboard.isPressed()) return;
    if (!g_active || !g_active->onKey) return;
    auto state = M5Cardputer.Keyboard.keysState();
    for (char ch : state.word) g_active->onKey(ch);
}

}  // namespace

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

    // First registered app wins for now. A launcher screen comes later when
    // more than one app is in the build.
    if (registry::count() > 0) {
        enter(registry::at(0));
    } else {
        Serial.println("[clawdputer] no apps registered — idle");
    }
}

void loop() {
    M5Cardputer.update();
    dispatchKeys();
    if (g_active && g_active->onTick) g_active->onTick();
    if (g_active && g_active->onDraw) g_active->onDraw();
    delay(20);
}
