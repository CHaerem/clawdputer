#pragma once

#include <M5Cardputer.h>

// Screen dimensions for Cardputer (1.14" IPS) in landscape orientation
// (setRotation(1)). The whole UI is sized around these numbers.
constexpr int SCREEN_W = 240;
constexpr int SCREEN_H = 135;

namespace ui {

LovyanGFX& display();
void       beginFrame();
void       flush();

// Allocate the 8bpp double-buffer sprite. Returns true if heap permitted
// the allocation; false → callers fall back silently to direct draw.
// Framework calls these from applyServices() based on SVC_CANVAS.
bool tryAcquireCanvas();
void releaseCanvas();
bool canvasActive();

// Resize/reorient the canvas + physical display. Used by apps that toggle
// landscape/portrait at runtime (snake, tetris). Pass {240,135,1} to
// restore framework defaults; callers MUST do so in onExit so the next
// app sees a correctly-sized canvas.
bool reconfigureCanvas(int w, int h, int rotation);

}  // namespace ui
