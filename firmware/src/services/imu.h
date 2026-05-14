#pragma once

namespace imu {

void begin();
void tick();  // call from main loop; publishes shake events via consumeShake()

// Returns true once for each detected shake (rate-limited to ~1/s) and
// clears the flag. Buddy's pet state machine consumes this for "dizzy".
bool consumeShake();

}  // namespace imu
