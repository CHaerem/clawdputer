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

}  // namespace ui
