#pragma once

#include <stdint.h>

struct Event;

using AppLifecycleFn = void (*)();
using AppKeyFn       = void (*)(char ch);
using AppEventFn     = void (*)(const Event& e);

enum AppService : uint32_t {
    SVC_NONE   = 0,
    SVC_BLE    = 1u << 0,
    SVC_WIFI   = 1u << 1,
    SVC_SD     = 1u << 2,
    // Double-buffered drawing via a 240×135 8bpp sprite (~32 KB heap).
    // Apps that opt in get flicker-free updates; framework allocates the
    // backbuffer on entry and frees it on exit. Acquisition can fail
    // silently when there's not enough heap (NimBLE + WiFi competing).
    SVC_CANVAS = 1u << 3,
};

struct App {
    const char*    id;
    const char*    name;
    const char*    description;  // one-line hint shown in the launcher; may be nullptr
    uint32_t       services;
    AppLifecycleFn onEnter;
    AppLifecycleFn onExit;
    AppLifecycleFn onTick;
    AppKeyFn       onKey;
    AppLifecycleFn onDraw;
    AppEventFn     onEvent;

    // Menu-style apps don't accept arbitrary text input, so the four physical
    // keys printed with arrow glyphs (; . , /) get translated to key::Up/
    // Down/Left/Right even without Fn held. Set to false for text-input apps
    // (chat) where ;./,/ must reach onKey as literal characters.
    bool           keysAsArrows = true;

    // Hidden apps don't appear in the home launcher. They're still in the
    // registry and reachable via clawd_request_app() — useful for settings
    // sub-flows launched from another app's action.
    bool           hidden       = false;
};

void registerApp(const App* app);

struct AppRegistrar {
    explicit AppRegistrar(const App* app) { registerApp(app); }
};

#define REGISTER_APP(app_var) \
    static AppRegistrar _clawd_reg_##app_var(&app_var)
