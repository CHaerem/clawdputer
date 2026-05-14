#pragma once

namespace ui {

// Shared content-area constants so apps stop hardcoding magic offsets.
constexpr int CONTENT_TOP    = 14 + 2;   // below statusbar
constexpr int FOOTER_TOP     = 124;       // start of footer hint strip
constexpr int FOOTER_H       = 11;
constexpr int CONTENT_BOTTOM = FOOTER_TOP - 1;
constexpr int CONTENT_H      = CONTENT_BOTTOM - CONTENT_TOP;

// Renders the standard 11-pixel hint strip at the bottom of the screen.
void footer(const char* hint);

// Title bar just under the statusbar: bold-ish title left, optional sub
// (smaller, dim) on the right.
void title(const char* text, const char* sub = nullptr);

}  // namespace ui
