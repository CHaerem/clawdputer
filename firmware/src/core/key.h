#pragma once

// Special key codes apps receive through onKey alongside printable ASCII.
// Main.cpp translates Cardputer's Fn-modified keys to these so apps don't
// have to know the physical layout.

namespace key {

constexpr char Up    = 0x11;
constexpr char Down  = 0x12;
constexpr char Left  = 0x13;
constexpr char Right = 0x14;
constexpr char Esc   = 0x1B;  // standard

}  // namespace key
