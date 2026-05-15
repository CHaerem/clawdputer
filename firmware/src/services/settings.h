#pragma once

#include <stdint.h>

// Persistent on/off flags for optional features. Stored in NVS under the
// "settings" namespace so they survive reboots and firmware updates.

namespace settings {

void begin();

bool audioEnabled();
bool petEnabled();
bool shakeEnabled();
bool autoUpdateEnabled();

void setAudioEnabled(bool v);
void setPetEnabled(bool v);
void setShakeEnabled(bool v);
void setAutoUpdateEnabled(bool v);

}  // namespace settings
