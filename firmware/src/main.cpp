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
#include "services/ble.h"
#include "services/identity.h"
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

// Last keystate snapshot — read by the home app to render an on-screen
// debug line. Helpful when a key combo isn't doing what we expect.
struct LastKey {
    bool        fn, opt, shift, ctrl, alt, tab, del, enter, space;
    std::string word;
    uint32_t    at;
} g_lastKey;

void dispatchSideButton() {
    if (M5Cardputer.BtnA.wasPressed()) goHome();
}

void dispatchKeys() {
    if (!M5Cardputer.Keyboard.isChange()) return;
    if (!M5Cardputer.Keyboard.isPressed()) return;
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
    identity::begin();
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
    dispatchSideButton();
    dispatchKeys();
    if (g_active && g_active->onTick) g_active->onTick();
    if (g_active && g_active->onDraw) g_active->onDraw();
    delay(20);
}
