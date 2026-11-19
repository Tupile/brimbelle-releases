// Host unit tests for enc_logic.c (counter wrap, counts to detents, clicks,
// long press).
// Build and run with test/host/run.sh. No ESP-IDF needed.
#include "enc_logic.h"

#include <stdio.h>

static int g_fail;
#define CHECK(cond, ...) do { \
    if (!(cond)) { g_fail++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static void test_delta(void)
{
    CHECK(enc_delta(0, 4, 1000) == 4, "plain up");
    CHECK(enc_delta(4, 0, 1000) == -4, "plain down");
    CHECK(enc_delta(998, 2, 1000) == 4, "wrap at +limit");
    CHECK(enc_delta(-998, -2, 1000) == -4, "wrap at -limit");
    CHECK(enc_delta(2, 998, 1000) == -4, "back across +limit");
    CHECK(enc_delta(7, 7, 1000) == 0, "no move");
}

static void test_detents(void)
{
    int r = 0;
    CHECK(enc_detents(&r, 4) == 1 && r == 0, "one detent up");
    CHECK(enc_detents(&r, -4) == -1 && r == 0, "one detent down");
    CHECK(enc_detents(&r, 10) == 2 && r == 2, "fast turn keeps the remainder");
    CHECK(enc_detents(&r, 2) == 1 && r == 0, "remainder completes a detent");
    // Bounce around a detent: one count back and forth makes no step.
    int steps = 0;
    for (int i = 0; i < 20; i++) steps += enc_detents(&r, (i & 1) ? -1 : 1);
    CHECK(steps == 0 && r == 0, "bounce steps=%d r=%d", steps, r);
    // A detent seen in pieces across polls is still one step.
    steps = 0;
    for (int i = 0; i < 4; i++) steps += enc_detents(&r, -1);
    CHECK(steps == -1 && r == 0, "piecewise down steps=%d r=%d", steps, r);
}

static int g_long;    // long clicks seen by poll() and hold()
static int g_triple;  // triple clicks seen by poll() and hold()

// Poll every 20 ms from *t for dur ms, the button held for the first hold ms
// (press edge on the first poll if hold is not 0); count the events.
static void hold(enc_click_t *c, uint32_t *t, uint32_t hold, uint32_t dur, int *single, int *dbl)
{
    uint32_t start = *t;
    for (uint32_t end = start + dur; (int32_t)(*t - end) < 0; *t += 20) {
        bool held = (uint32_t)(*t - start) < hold;
        enc_click_event_t e = enc_click(c, held && *t == start, held, *t);
        if (e == ENC_CLICK_SINGLE) (*single)++;
        if (e == ENC_CLICK_DOUBLE) (*dbl)++;
        if (e == ENC_CLICK_LONG) g_long++;
        if (e == ENC_CLICK_TRIPLE) g_triple++;
    }
}

// Same with a short press (60 ms) if press is set.
static void poll(enc_click_t *c, uint32_t *t, bool press, uint32_t dur, int *single, int *dbl)
{
    hold(c, t, press ? 60 : 0, dur, single, dbl);
}

static void test_click(void)
{
    enc_click_t c = { 0 };
    uint32_t t = 1000; int s = 0, d = 0;
    poll(&c, &t, false, 1000, &s, &d);
    CHECK(s == 0 && d == 0, "idle fired s=%d d=%d", s, d);
    // One press: nothing during the window, one single click after it.
    poll(&c, &t, true, 380, &s, &d);
    CHECK(s == 0 && d == 0, "single fired early s=%d d=%d", s, d);
    poll(&c, &t, false, 600, &s, &d);
    CHECK(s == 1 && d == 0, "single s=%d d=%d", s, d);
    // Two presses 200 ms apart: one double click, no single.
    s = d = 0;
    poll(&c, &t, true, 200, &s, &d);
    poll(&c, &t, true, 1000, &s, &d);
    CHECK(s == 0 && d == 1, "double s=%d d=%d", s, d);
    // Two presses 600 ms apart: two single clicks.
    s = d = 0;
    poll(&c, &t, true, 600, &s, &d);
    poll(&c, &t, true, 600, &s, &d);
    CHECK(s == 2 && d == 0, "two singles s=%d d=%d", s, d);
    // Two presses 200 ms apart: the double waits for the window after the second.
    s = d = 0;
    poll(&c, &t, true, 200, &s, &d);
    poll(&c, &t, true, 380, &s, &d);
    CHECK(s == 0 && d == 0, "double fired early s=%d d=%d", s, d);
    poll(&c, &t, false, 600, &s, &d);
    CHECK(s == 0 && d == 1, "late double s=%d d=%d", s, d);
    // Three quick presses: one triple click, at the third press, nothing else.
    s = d = 0;
    poll(&c, &t, true, 150, &s, &d);
    poll(&c, &t, true, 150, &s, &d);
    CHECK(g_triple == 0, "triple fired early n=%d", g_triple);
    poll(&c, &t, true, 20, &s, &d);
    CHECK(g_triple == 1, "triple not at the third press n=%d", g_triple);
    poll(&c, &t, false, 1000, &s, &d);
    CHECK(g_triple == 1 && s == 0 && d == 0, "triple n=%d s=%d d=%d", g_triple, s, d);
    // Four quick presses: a triple, then the fourth is a single click.
    g_triple = 0;
    for (int i = 0; i < 4; i++) poll(&c, &t, true, i < 3 ? 150 : 1000, &s, &d);
    CHECK(g_triple == 1 && s == 1 && d == 0, "four presses n=%d s=%d d=%d", g_triple, s, d);
    // Two quick presses, a third one 600 ms later: a double and a single.
    g_triple = 0; s = d = 0;
    poll(&c, &t, true, 150, &s, &d);
    poll(&c, &t, true, 600, &s, &d);
    poll(&c, &t, true, 1000, &s, &d);
    CHECK(g_triple == 0 && s == 1 && d == 1, "double then single n=%d s=%d d=%d", g_triple, s, d);
    // Wrap-around of the ms clock.
    enc_click_t w = { 0 }; t = 0xFFFFFF00u; s = d = 0;
    poll(&w, &t, true, 1000, &s, &d);
    CHECK(s == 1 && d == 0, "wrap s=%d d=%d", s, d);
    CHECK(g_long == 0, "short presses made %d long click(s)", g_long);
}

static void test_long(void)
{
    enc_click_t c = { 0 };
    uint32_t t = 1000; int s = 0, d = 0;
    g_long = 0;
    // Held 1480 ms: not long yet, and no single click while held.
    hold(&c, &t, 3000, 1480, &s, &d);
    CHECK(g_long == 0 && s == 0 && d == 0, "long fired early l=%d s=%d d=%d", g_long, s, d);
    // Still held (no new press edge): one long click at 1.5 s, only once.
    for (uint32_t end = t + 1500; t < end; t += 20)
        if (enc_click(&c, false, true, t) == ENC_CLICK_LONG) g_long++;
    CHECK(g_long == 1, "long l=%d", g_long);
    // Release: nothing more.
    hold(&c, &t, 0, 1000, &s, &d);
    CHECK(g_long == 1 && s == 0 && d == 0, "after release l=%d s=%d d=%d", g_long, s, d);
    // Long press, then a click 100 ms after the release: one long, one single.
    g_long = 0;
    hold(&c, &t, 2000, 2100, &s, &d);
    CHECK(g_long == 1 && s == 0 && d == 0, "long before click l=%d s=%d d=%d", g_long, s, d);
    poll(&c, &t, true, 1000, &s, &d);
    CHECK(g_long == 1 && s == 1 && d == 0, "long then click l=%d s=%d d=%d", g_long, s, d);
    // Held 1 s (past the double window, short of long): a single click, on release.
    g_long = 0; s = d = 0;
    hold(&c, &t, 1000, 980, &s, &d);
    CHECK(s == 0, "single fired while held s=%d", s);
    hold(&c, &t, 0, 500, &s, &d);
    CHECK(g_long == 0 && s == 1 && d == 0, "slow click l=%d s=%d d=%d", g_long, s, d);
    // Double click with the second press held 3 s: a double click only.
    s = d = 0;
    poll(&c, &t, true, 200, &s, &d);
    hold(&c, &t, 3000, 4000, &s, &d);
    CHECK(g_long == 0 && s == 0 && d == 1, "double then hold l=%d s=%d d=%d", g_long, s, d);
    // Wrap-around of the ms clock during a long press.
    enc_click_t w = { 0 }; t = 0xFFFFFF00u; s = d = 0;
    hold(&w, &t, 2000, 3000, &s, &d);
    CHECK(g_long == 1 && s == 0 && d == 0, "wrap l=%d s=%d d=%d", g_long, s, d);
}

int main(void)
{
    test_delta();
    test_detents();
    test_click();
    test_long();
    if (g_fail) { printf("%d failure(s)\n", g_fail); return 1; }
    printf("enc_logic: delta, detents and clicks passed\n");
    return 0;
}
