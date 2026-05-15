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
bool reportEnabled();

void setAudioEnabled(bool v);
void setPetEnabled(bool v);
void setShakeEnabled(bool v);
void setAutoUpdateEnabled(bool v);
void setReportEnabled(bool v);

// True when CLAWD_GITHUB_PAT is compiled in. Used by the Settings UI to
// decide whether the "crash reports" toggle is meaningful at all.
bool reportAvailable();

}  // namespace settings
