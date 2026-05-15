#include "settings.h"

#include <Arduino.h>
#include <Preferences.h>

#include "sealed.h"

namespace settings {

namespace {
bool g_audio      = true;
bool g_pet        = true;
bool g_shake      = true;
bool g_autoUpdate = true;
bool g_report     = true;

void load() {
    Preferences p;
    p.begin("settings", true);
    g_audio      = p.getBool("audio",   true);
    g_pet        = p.getBool("pet",     true);
    g_shake      = p.getBool("shake",   true);
    g_autoUpdate = p.getBool("autoupd", true);
    g_report     = p.getBool("report",  true);
    p.end();
}

void save() {
    Preferences p;
    p.begin("settings", false);
    p.putBool("audio",   g_audio);
    p.putBool("pet",     g_pet);
    p.putBool("shake",   g_shake);
    p.putBool("autoupd", g_autoUpdate);
    p.putBool("report",  g_report);
    p.end();
}
}  // namespace

void begin() {
    load();
    Serial.printf("[settings] audio=%d pet=%d shake=%d autoupd=%d report=%d\n",
                  (int)g_audio, (int)g_pet, (int)g_shake,
                  (int)g_autoUpdate, (int)g_report);
}

bool audioEnabled()      { return g_audio; }
bool petEnabled()        { return g_pet; }
bool shakeEnabled()      { return g_shake; }
bool autoUpdateEnabled() { return g_autoUpdate; }
bool reportEnabled()     { return g_report && reportAvailable(); }

bool reportAvailable() {
    return !sealed::unsealGithubPat().empty();
}

void setAudioEnabled(bool v)      { g_audio      = v; save(); }
void setPetEnabled(bool v)        { g_pet        = v; save(); }
void setShakeEnabled(bool v)      { g_shake      = v; save(); }
void setAutoUpdateEnabled(bool v) { g_autoUpdate = v; save(); }
void setReportEnabled(bool v)     { g_report     = v; save(); }

}  // namespace settings
