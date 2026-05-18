// Wokwi custom chip — M5Cardputer keyboard matrix scanner.
//
// Mirrors the firmware-side scanning logic in
//   m5stack/M5Cardputer src/utility/Keyboard/KeyboardReader/IOMatrix.cpp
//
// The Cardputer keyboard is a 4×14 grid (56 keys) accessed through a
// 3→8 column demux and 7 row sense lines:
//
//   COL_A, COL_B, COL_C  (outputs from MCU)  — drive a 74HC138 selecting
//                                              1 of 8 column groups
//   ROW0..ROW6           (inputs to MCU)     — active-low, pulled up
//
// On each scan step the firmware writes a 3-bit value to {COL_A,B,C}
// and reads the 7 row inputs. The (column-step i, row-bit j) pair maps
// to a (x, y) cell in the keyboard layout via:
//
//   y = 3 - ((i > 3) ? (i - 4) : i)
//   x = (i > 3) ? X_map_chart[j].x_1 : X_map_chart[j].x_2
//
// where X_map_chart[7] = {{_,0,1},{_,2,3},{_,4,5},{_,6,7},{_,8,9},
//                          {_,10,11},{_,12,13}}
//
// We invert that mapping to figure out, for any pressed (x, y) cell,
// which (i, j) the firmware will see it on, and pull ROW_j low whenever
// the current column-step matches.
//
// Pressed keys are configured via the `keys` attribute as a semicolon-
// separated list of "x,y" pairs (e.g. "0,0;1,0" presses the top-left
// two cells). The web host writes to this attribute when the user
// types on the on-screen / physical keyboard.

#include "wokwi-api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_KEYS 16

typedef struct {
  pin_t col_a, col_b, col_c;
  pin_t row[7];
  uint32_t keys_attr;

  // Each pressed key encoded as (i, j) — the column-step / row-bit
  // the firmware will observe it on.
  uint8_t pressed_count;
  struct {
    uint8_t i;       // column scan step 0..7
    uint8_t j;       // row bit 0..6
  } pressed[MAX_KEYS];
} chip_state_t;

static void on_col_change(void *user_data, pin_t pin, uint32_t value);
static void on_attr_change(void *user_data);
static void refresh_rows(chip_state_t *state);

// (x, y) → (i, j). Returns 1 on success, 0 if out-of-range.
static int cell_to_ij(int x, int y, uint8_t *out_i, uint8_t *out_j) {
  if (x < 0 || x > 13 || y < 0 || y > 3) return 0;

  // y = 3 - ((i > 3) ? (i - 4) : i)  →  base_i = 3 - y
  // i is either base_i (top half, i<=3, scans odd x) or base_i+4
  // (bottom half, i>3, scans even x).
  uint8_t base_i = (uint8_t)(3 - y);

  // x parity decides which half. Odd x → top half (i<=3, x_2 col).
  // Even x → bottom half (i>3, x_1 col).
  uint8_t i = (x & 1) ? base_i : (uint8_t)(base_i + 4);

  // j is x / 2 for both halves (X_map_chart[j] = {_, 2j, 2j+1}).
  uint8_t j = (uint8_t)(x >> 1);
  if (j > 6) return 0;

  *out_i = i;
  *out_j = j;
  return 1;
}

static void parse_keys(chip_state_t *state) {
  state->pressed_count = 0;
  char buf[256];
  uint32_t len = attr_read_string(state->keys_attr, buf, sizeof(buf) - 1);
  if (len == 0) { refresh_rows(state); return; }
  buf[len] = 0;

  char *p = buf;
  while (*p && state->pressed_count < MAX_KEYS) {
    while (*p == ' ' || *p == ';' || *p == ',') p++;
    if (!*p) break;
    int x = (int)strtol(p, &p, 10);
    while (*p == ' ' || *p == ',') p++;
    int y = (int)strtol(p, &p, 10);
    uint8_t i, j;
    if (cell_to_ij(x, y, &i, &j)) {
      state->pressed[state->pressed_count].i = i;
      state->pressed[state->pressed_count].j = j;
      state->pressed_count++;
    }
  }
  refresh_rows(state);
}

static uint8_t current_i(chip_state_t *state) {
  uint8_t a = pin_read(state->col_a) ? 1 : 0;
  uint8_t b = pin_read(state->col_b) ? 1 : 0;
  uint8_t c = pin_read(state->col_c) ? 1 : 0;
  return (uint8_t)(a | (b << 1) | (c << 2));
}

static void refresh_rows(chip_state_t *state) {
  uint8_t i = current_i(state);
  // Default: rows idle high (pulled up by MCU; we drive nothing).
  // For any pressed key matching the current column step, pull its
  // row line low.
  uint8_t low_mask = 0;
  for (uint8_t k = 0; k < state->pressed_count; k++) {
    if (state->pressed[k].i == i) {
      low_mask |= (uint8_t)(1 << state->pressed[k].j);
    }
  }
  for (uint8_t j = 0; j < 7; j++) {
    if (low_mask & (1 << j)) {
      pin_mode(state->row[j], OUTPUT);
      pin_write(state->row[j], LOW);
    } else {
      // Release — let the MCU's internal pull-up float it high.
      pin_mode(state->row[j], INPUT);
    }
  }
}

void chip_init() {
  chip_state_t *state = malloc(sizeof(chip_state_t));
  memset(state, 0, sizeof(*state));

  const pin_watch_config_t col_watch = {
    .edge = BOTH,
    .pin_change = on_col_change,
    .user_data = state,
  };

  state->col_a = pin_init("COL_A", INPUT);
  state->col_b = pin_init("COL_B", INPUT);
  state->col_c = pin_init("COL_C", INPUT);
  pin_watch(state->col_a, &col_watch);
  pin_watch(state->col_b, &col_watch);
  pin_watch(state->col_c, &col_watch);

  const char *row_names[7] = { "ROW0","ROW1","ROW2","ROW3","ROW4","ROW5","ROW6" };
  for (uint8_t j = 0; j < 7; j++) {
    state->row[j] = pin_init(row_names[j], INPUT);
  }

  const attr_watch_config_t attr_watch = {
    .user_data = state,
    .callback = on_attr_change,
  };
  state->keys_attr = attr_init("keys", 0);
  attr_watch_init(state->keys_attr, &attr_watch);

  parse_keys(state);
}

static void on_col_change(void *user_data, pin_t pin, uint32_t value) {
  (void)pin; (void)value;
  refresh_rows((chip_state_t *)user_data);
}

static void on_attr_change(void *user_data) {
  parse_keys((chip_state_t *)user_data);
}
