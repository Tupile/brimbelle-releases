// Host unit tests for audio_eq.c (filter curves, pre-gain, bypass, clipping).
// Build and run with test/host/run.sh. No ESP-IDF needed.
#include "audio_eq.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int g_fail;
#define CHECK(cond, ...) do { \
    if (!(cond)) { g_fail++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

#define RATE 48000

static float resp(const audio_eq_t *eq, float hz) { return audio_eq_response_db(eq, hz, RATE); }

// Steady-state level change of a sine through the chain, in dB (left channel).
static float measured_db(audio_eq_t *eq, float hz)
{
    enum { N = RATE };
    int32_t *buf = malloc(N * 2 * sizeof(int32_t));
    const float amp = 1.0e8f;
    for (int i = 0; i < N; i++) buf[2 * i] = buf[2 * i + 1] = (int32_t)(amp * sinf(6.2831853f * hz * i / RATE));
    audio_eq_process(eq, buf, N, INT32_MAX);
    double sum = 0;
    for (int i = N / 2; i < N; i++) sum += (double)buf[2 * i] * buf[2 * i];
    free(buf);
    return (float)(10.0 * log10(sum / (N / 2) / (amp * amp / 2.0)));
}

static void test_bypass(void)
{
    audio_eq_t eq;
    audio_eq_design(&eq, NULL, 0, RATE);
    int32_t buf[8] = { 1, -2, 123456789, -987654321, INT32_MAX, INT32_MIN, 0, 7 }, ref[8];
    for (int i = 0; i < 8; i++) ref[i] = buf[i];
    audio_eq_process(&eq, buf, 4, 1000);
    for (int i = 0; i < 8; i++) CHECK(buf[i] == ref[i], "bypass leaves sample %d untouched", i);

    // Invalid bands are skipped, which is also a bypass.
    audio_eq_band_t bad[] = { { 0, 1000, 3, 1 }, { 9, 1000, 3, 1 }, { AUDIO_EQ_PEAK, 5, 3, 1 },
                              { AUDIO_EQ_PEAK, 30000, 3, 1 }, { AUDIO_EQ_PEAK, NAN, 3, 1 } };
    audio_eq_design(&eq, bad, 5, RATE);
    CHECK(eq.n == 0, "invalid bands skipped (%d)", eq.n);
}

static void test_highpass(void)
{
    audio_eq_band_t b = { AUDIO_EQ_HIGHPASS, 90, 0, 0.707f };
    audio_eq_t eq;
    audio_eq_design(&eq, &b, 1, RATE);
    CHECK(eq.n == 1 && eq.pre > 0.999f && eq.pre <= 1.0f, "one band, no pre-gain (%f)", eq.pre);
    CHECK(fabsf(resp(&eq, 90) + 3.0f) < 0.1f, "-3 dB at the corner (%f)", resp(&eq, 90));
    CHECK(fabsf(resp(&eq, 45) + 12.3f) < 0.5f, "12 dB/octave below (%f)", resp(&eq, 45));
    CHECK(fabsf(resp(&eq, 1000)) < 0.05f, "flat above (%f)", resp(&eq, 1000));
    CHECK(fabsf(measured_db(&eq, 45) - resp(&eq, 45)) < 0.2f, "processing matches the curve at 45 Hz");
    audio_eq_design(&eq, &b, 1, RATE);
    CHECK(fabsf(measured_db(&eq, 1000)) < 0.05f, "processing is flat at 1 kHz");
}

static void test_cut_and_boost(void)
{
    audio_eq_t eq;
    audio_eq_band_t cut = { AUDIO_EQ_HIGHSHELF, 1100, -4, 0.707f };
    audio_eq_design(&eq, &cut, 1, RATE);
    CHECK(eq.pre == 1.0f, "a cut needs no pre-gain");
    CHECK(fabsf(resp(&eq, 100)) < 0.1f, "shelf: flat below (%f)", resp(&eq, 100));
    CHECK(fabsf(resp(&eq, 10000) + 4.0f) < 0.1f, "shelf: -4 dB above (%f)", resp(&eq, 10000));

    audio_eq_band_t boost[] = { { AUDIO_EQ_PEAK, 1000, 6, 2 }, { AUDIO_EQ_LOWSHELF, 300, 3, 0.707f } };
    audio_eq_design(&eq, boost, 2, RATE);
    float top = -100;
    for (float hz = 20; hz < 20000; hz *= 1.01f) if (resp(&eq, hz) > top) top = resp(&eq, hz);
    CHECK(top <= 0.02f && top > -0.1f, "pre-gain puts the top of the curve at 0 dB (%f)", top);
    CHECK(fabsf(resp(&eq, 1000) - resp(&eq, 5000) - 6.0f) < 0.3f, "peak is 6 dB above the flat part");

    audio_eq_band_t wild = { AUDIO_EQ_PEAK, 1000, 40, 1 };
    audio_eq_design(&eq, &wild, 1, RATE);
    CHECK(fabsf(resp(&eq, 1000) - resp(&eq, 20) - 12.0f) < 0.3f, "boost clamped to 12 dB");
}

static void test_clip_and_channels(void)
{
    // A step into a high-pass overshoots on its way back: it must stay in +/- limit.
    audio_eq_band_t b = { AUDIO_EQ_HIGHPASS, 90, 0, 2.0f };
    audio_eq_t eq;
    audio_eq_design(&eq, &b, 1, RATE);
    enum { N = 4800 };
    static int32_t buf[N * 2];
    const int32_t limit = 1000000;
    for (int i = 0; i < N; i++) {
        buf[2 * i] = (i / 200) % 2 ? limit : -limit;   // 120 Hz square at the limit
        buf[2 * i + 1] = 0;                            // right stays silent
    }
    audio_eq_process(&eq, buf, N, limit);
    int over = 0, right = 0, at_limit = 0;
    for (int i = 0; i < N; i++) {
        if (buf[2 * i] > limit || buf[2 * i] < -limit) over++;
        if (buf[2 * i] == limit || buf[2 * i] == -limit) at_limit++;
        if (buf[2 * i + 1] != 0) right++;
    }
    CHECK(over == 0, "no sample beyond the limit (%d)", over);
    CHECK(at_limit > 0, "the overshoot was clipped");
    CHECK(right == 0, "channels do not leak into each other (%d)", right);
}

int main(void)
{
    test_bypass();
    test_highpass();
    test_cut_and_boost();
    test_clip_and_channels();
    if (g_fail) { printf("audio_eq: %d FAILED\n", g_fail); return 1; }
    printf("audio_eq: all tests passed\n");
    return 0;
}
