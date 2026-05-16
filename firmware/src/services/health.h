#pragma once

#include <stdint.h>

// health: periodic on-device telemetry sampler.
//
// Every ~30s captures (freeHeap, minFreeHeap, largestBlock, loopStackHwm,
// battMv, appId). Keeps the last ~2 hours in a RAM ring buffer. When a
// threshold is breached (heap fragmentation, low heap, near-stack-limit,
// crash-loop), files a deduplicated GitHub issue with the recent samples
// attached so the loop is: device → issue → fix → OTA.
//
// Gated by settings::reportEnabled(). No-op when reporting is off or the
// sealed PAT is absent.

namespace health {

void begin();
void tick();

// Called by the framework on every app switch so samples carry the
// currently-active app id as a label.
void noteAppEntered(const char* appId);

}  // namespace health
