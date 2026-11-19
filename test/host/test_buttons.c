// Host unit tests for buttons.c (debounce, auto-repeat, volume step), copied
// from Bugne's test_buttons.c without the pin and action tests.
// Build and run with test/host/run.sh. No ESP-IDF needed.
#include "buttons.h"

#include <stdio.h>
#include <string.h>

static int g_fail;

#define CHECK(cond, ...) do { \
    if (!(cond)) { g_fail++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

// Feed a level for dur ms, sampling every 50 ms like the UI tick; count events.
static void feed(buttons_state_t *s, uint32_t *t, bool pressed, uint32_t dur, bool rep,
                 int *press, int *repeat)
{
    for (uint32_t end = *t + dur; (int32_t)(*t - end) < 0; *t += 50) {
        buttons_event_t e = buttons_update(s, pressed, *t, rep);
        if (e == BUTTONS_EV_PRESS) (*press)++;
        if (e == BUTTONS_EV_REPEAT) (*repeat)++;
    }
}

static void test_debounce(void)
{
    buttons_state_t s; memset(&s, 0, sizeof(s));
    uint32_t t = 1000; int p = 0, r = 0;
    feed(&s, &t, false, 500, false, &p, &r);
    CHECK(p == 0 && r == 0, "idle fired p=%d r=%d", p, r);
    // A single-sample glitch is ignored.
    feed(&s, &t, true, 50, false, &p, &r);
    feed(&s, &t, false, 200, false, &p, &r);
    CHECK(p == 0, "glitch fired %d", p);
    // Bouncy press: alternating samples, then held: exactly one press.
    for (int i = 0; i < 4; i++) feed(&s, &t, (i & 1) == 0, 50, false, &p, &r);
    feed(&s, &t, true, 2000, false, &p, &r);
    CHECK(p == 1 && r == 0, "toggle held: p=%d r=%d (want 1, 0)", p, r);
    feed(&s, &t, false, 300, false, &p, &r);
    feed(&s, &t, true, 200, false, &p, &r);
    CHECK(p == 2, "second press: %d", p);
}

static void test_repeat(void)
{
    buttons_state_t s; memset(&s, 0, sizeof(s));
    uint32_t t = 0; int p = 0, r = 0;
    feed(&s, &t, true, 400, true, &p, &r);   // shorter than the repeat delay
    CHECK(p == 1 && r == 0, "short hold p=%d r=%d", p, r);
    feed(&s, &t, false, 200, true, &p, &r);
    p = r = 0;
    // Press seen at +50 ms; repeats at +550, +850, +1150, +1450, +1750 within 2 s.
    feed(&s, &t, true, 2000, true, &p, &r);
    CHECK(p == 1 && r == 5, "long hold p=%d r=%d (want 1, 5)", p, r);
    // Wrap-around of the ms clock.
    memset(&s, 0, sizeof(s)); t = 0xFFFFFF00u; p = r = 0;
    feed(&s, &t, true, 1000, true, &p, &r);
    CHECK(p == 1 && r >= 1, "wrap p=%d r=%d", p, r);
}

static void test_volume(void)
{
    CHECK(buttons_volume_step(50, 1, 100) == 55, "50 up");
    CHECK(buttons_volume_step(50, -1, 100) == 45, "50 down");
    CHECK(buttons_volume_step(98, 1, 100) == 100, "clamp 100");
    CHECK(buttons_volume_step(2, -1, 100) == 0, "clamp 0");
    CHECK(buttons_volume_step(58, 1, 60) == 60, "limit 60");
    CHECK(buttons_volume_step(80, 1, 60) == 60, "above limit goes to limit");
    CHECK(buttons_volume_step(80, -1, 60) == 60, "down from above limit");
}

int main(void)
{
    test_debounce();
    test_repeat();
    test_volume();
    if (g_fail) { printf("%d failure(s)\n", g_fail); return 1; }
    printf("buttons: debounce, repeat and volume step passed\n");
    return 0;
}
