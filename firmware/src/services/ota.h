#pragma once

namespace ota {

// Should be called from the main loop. Starts the ArduinoOTA listener the
// first time WiFi is up and then polls for incoming firmware updates each
// tick. Cheap when no update is in progress.
void tick();

bool isUpdating();

}  // namespace ota
