#pragma once

#include <M5Cardputer.h>

// Backbuffer wrapper. Apps draw onto ui::display() and call ui::flush() once
// per frame — the entire sprite is pushed to the physical display in one
// atomic SPI transfer, eliminating the per-element flicker you get from
// drawing straight to M5Cardputer.Display.
//
// Falls back to direct display drawing if the sprite can't be allocated
// (low heap conditions), so apps can use display() unconditionally.

namespace ui {

// Returns the active drawing target: a backbuffer sprite if allocation
// succeeded, otherwise the physical display.
LovyanGFX& display();
void   beginFrame();
void   flush();

}  // namespace ui
