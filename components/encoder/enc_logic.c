// enc_logic: quadrature count to detents. See enc_logic.h.
#include "enc_logic.h"

int enc_delta(int prev, int cur, int limit)
{
    int d = cur - prev;
    if (d > limit / 2) d -= limit;
    else if (d < -limit / 2) d += limit;
    return d;
}

int enc_detents(int *residue, int delta)
{
    *residue += delta;
    int detents = *residue / ENC_COUNTS_PER_DETENT;
    *residue -= detents * ENC_COUNTS_PER_DETENT;
    return detents;
}

enc_click_event_t enc_click(enc_click_t *c, bool press, bool held, uint32_t now_ms)
{
    if (press) {
        if (c->presses == 2) {
            c->presses = 0;
            c->down = false;
            return ENC_CLICK_TRIPLE;
        }
        c->presses++;
        c->down = held;
        c->last_ms = now_ms;
        return ENC_CLICK_NONE;
    }
    if (!held) c->down = false;
    if (!c->presses) return ENC_CLICK_NONE;
    uint32_t age = (uint32_t)(now_ms - c->last_ms);
    if (c->presses == 2) {
        // No third press in time: a double click, held or not.
        if (age >= ENC_DOUBLE_MS) {
            c->presses = 0;
            c->down = false;
            return ENC_CLICK_DOUBLE;
        }
        return ENC_CLICK_NONE;
    }
    if (c->down) {
        // Still held: no single click yet, it may become a long one.
        if (age >= ENC_LONG_MS) {
            c->presses = 0;
            c->down = false;
            return ENC_CLICK_LONG;
        }
        return ENC_CLICK_NONE;
    }
    if (age >= ENC_DOUBLE_MS) {
        c->presses = 0;
        return ENC_CLICK_SINGLE;
    }
    return ENC_CLICK_NONE;
}
