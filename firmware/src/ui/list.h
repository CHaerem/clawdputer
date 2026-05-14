#pragma once

#include <functional>
#include <string>
#include <vector>

#include "core/key.h"
#include "ui/canvas.h"

// Vertical scrolling selection list. Apps populate items, set the selected
// index, and call list::draw(state, x, y, w, h) once per render. Key dispatch
// is the app's responsibility — call list::onKey(state, ch) to update.

namespace ui::list {

struct Item {
    std::string label;
    std::string value;          // optional — shown right-aligned-ish
    bool        action = false; // renders "press enter" hint when true
};

struct State {
    std::vector<Item> items;
    int               selected  = 0;
    int               scrollTop = 0;
    int               rowH      = 11;
};

void draw(State& s, int x, int y, int w, int h);
bool onKey(State& s, char ch);  // returns true if the key advanced selection

}  // namespace ui::list
