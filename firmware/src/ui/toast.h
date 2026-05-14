#pragma once

#include <string>

// Transient bottom-of-screen message. App calls show("msg") to start a
// 2.5-second timer; draw() renders the toast over whatever else is on
// screen until the timer expires.

namespace ui::toast {

void show(const std::string& msg, uint32_t durationMs = 2500);
void draw();
bool active();   // true while a toast is on screen

}  // namespace ui::toast
