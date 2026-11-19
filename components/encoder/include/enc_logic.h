// enc_logic: quadrature count to detents for the rotary encoder. Pure C, no
// ESP-IDF, host-tested.
//
// The hardware counter counts 4 edges per detent and resets to 0 when it
// reaches +limit or -limit. The poll task reads it every 20 ms and feeds the
// readings here.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define ENC_COUNTS_PER_DETENT 4

// Signed number of counts between two readings of a counter that wraps to 0
// at +limit and -limit. Valid while fewer than limit / 2 counts pass between
// two readings.
int enc_delta(int prev, int cur, int limit);

// Add delta counts to *residue and return the whole detents reached (signed).
// The remainder stays in *residue, so contact bounce around a detent (one
// count back and forth) never makes a step.
int enc_detents(int *residue, int delta);

// Single, double, triple and long click. Each press within ENC_DOUBLE_MS of
// the previous one adds to the click: the third one makes a triple click,
// reported at once. A single or double click is reported only once that
// window has passed with no further press, so it is late by ENC_DOUBLE_MS; a
// single click also waits for the button to be released. A press held for
// ENC_LONG_MS makes a long click, reported once while the button is still
// down; its release reports nothing. The second press of a double click
// never makes a long click.
#define ENC_DOUBLE_MS 400
#define ENC_LONG_MS   1500

typedef struct {
    uint8_t  presses;   // presses seen so far (0 to 2), waiting for one more
    bool     down;      // still held since the first press
    uint32_t last_ms;   // time of the last press
} enc_click_t;

typedef enum {
    ENC_CLICK_NONE = 0,
    ENC_CLICK_SINGLE,
    ENC_CLICK_DOUBLE,
    ENC_CLICK_TRIPLE,
    ENC_CLICK_LONG,
} enc_click_event_t;

// Call on every poll. press is true on a debounced press edge, held is the
// debounced level (true = pressed). Zeroed state = nothing pending. now_ms
// may wrap.
enc_click_event_t enc_click(enc_click_t *c, bool press, bool held, uint32_t now_ms);
