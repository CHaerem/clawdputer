#pragma once

#include <stdint.h>

struct Event;

using AppLifecycleFn = void (*)();
using AppKeyFn       = void (*)(char ch);
using AppEventFn     = void (*)(const Event& e);

enum AppService : uint32_t {
    SVC_NONE = 0,
    SVC_BLE  = 1u << 0,
    SVC_WIFI = 1u << 1,
    SVC_SD   = 1u << 2,
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
};

void registerApp(const App* app);

struct AppRegistrar {
    explicit AppRegistrar(const App* app) { registerApp(app); }
};

#define REGISTER_APP(app_var) \
    static AppRegistrar _clawd_reg_##app_var(&app_var)
