#pragma once

namespace ui::statusbar {

// Height in pixels — apps should start their content below this.
constexpr int HEIGHT = 18;

// Draws the global status bar at y=0..HEIGHT. Apps call this at the top of
// their render. Reads battery, BLE link state, and WiFi state from services
// directly, so the bar always reflects current truth without per-app wiring.
void draw();

}  // namespace ui::statusbar
