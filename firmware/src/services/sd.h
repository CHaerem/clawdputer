#pragma once

namespace sd {

// Initialise SPI and mount the SD card. Safe no-op if no card is inserted.
void begin();

// True iff the card is mounted and accessible.
bool isAvailable();

// Power-management hooks. Framework calls pause() when the active app
// declares no SVC_SD; resume() when an app with SVC_SD is entered.
void pause();
void resume();
bool isPaused();

}  // namespace sd
