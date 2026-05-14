#include "settings.h"

#include <Arduino.h>
#include <Preferences.h>

namespace settings {

namespace {
bool g_audio = true;
bool g_pet   = true;
bool g_shake = true;

void load() {
    Preferences p;
    p.begin("settings", true);
    g_audio = p.getBool("audio", true);
    g_pet   = p.getBool("pet",   true);
    g_shake = p.getBool("shake", true);
    p.end();
}

void save() {
    Preferences p;
    p.begin("settings", false);
    p.putBool("audio", g_audio);
    p.putBool("pet",   g_pet);
    p.putBool("shake", g_shake);
    p.end();
}
}  // namespace

void begin() {
    load();
    Serial.printf("[settings] audio=%d pet=%d shake=%d\n",
                  (int)g_audio, (int)g_pet, (int)g_shake);
}

bool audioEnabled() { return g_audio; }
bool petEnabled()   { return g_pet; }
bool shakeEnabled() { return g_shake; }

void setAudioEnabled(bool v) { g_audio = v; save(); }
void setPetEnabled(bool v)   { g_pet   = v; save(); }
void setShakeEnabled(bool v) { g_shake = v; save(); }

}  // namespace settings
