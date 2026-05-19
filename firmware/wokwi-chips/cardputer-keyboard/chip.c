// Wokwi custom chip — M5Cardputer keyboard matrix passive scanner.
//
// The Cardputer keyboard is a 4×14 grid (56 keys) accessed through a
// 3→8 column demux and 7 row sense lines:
//
//   COL_A, COL_B, COL_C  (outputs from MCU)  — drive a 74HC138 selecting
//                                              1 of 8 column groups
//   ROW0..ROW6           (inputs to MCU)     — active-low, pulled up
//
// This stub keeps every row input floating (interpreted as HIGH by the
// firmware's internal pull-ups), so the firmware sees "no keys pressed"
// and boot proceeds normally. The smoke test (test.scenario.yaml) only
// checks that the launcher renders; we don't need keypresses for that.
//
// Interactive key injection isn't wired yet — the public Wokwi Custom
// Chips API (see ../wokwi-api.h: attr_init / attr_read variants) only
// exposes integer/float attributes, not strings, and no attribute
// change callbacks. Inverting the matrix from a single readable mask
// would need ≥2× uint32 attrs to cover 56 cells; an external diagram of
// `wokwi-pushbutton`s wired to the row pins is the cleaner long-term
// path. Tracked as a follow-up.

#include "wokwi-api.h"

void chip_init(void) {
  pin_init("COL_A", INPUT);
  pin_init("COL_B", INPUT);
  pin_init("COL_C", INPUT);
  pin_init("ROW0",  INPUT);
  pin_init("ROW1",  INPUT);
  pin_init("ROW2",  INPUT);
  pin_init("ROW3",  INPUT);
  pin_init("ROW4",  INPUT);
  pin_init("ROW5",  INPUT);
  pin_init("ROW6",  INPUT);
}
