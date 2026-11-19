// Host unit tests for audio_mix.c (gain curve, output modes, test tone).
// Build and run with test/host/run.sh. No ESP-IDF needed.
#include "audio_mix.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int g_fail;
#define CHECK(cond, ...) do { \
    if (!(cond)) { g_fail++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static float gain_db(int32_t q16) { return 20.0f * log10f((float)q16 / AUDIO_MIX_GAIN_0DB); }

static void test_gain(void)
{
    CHECK(audio_mix_gain_q16(0, -12.0f) == 0, "volume 0 is mute");
    CHECK(audio_mix_gain_q16(-5, -12.0f) == 0, "negative volume is mute");
    int32_t g100 = audio_mix_gain_q16(100, -12.0f);
    CHECK(fabsf(gain_db(g100) + 12.0f) < 0.01f, "volume 100 = max_db (%f)", gain_db(g100));
    int32_t g50 = audio_mix_gain_q16(50, -12.0f);
    CHECK(fabsf(gain_db(g50) + 37.0f) < 0.01f, "volume 50 = max_db - 25 dB (%f)", gain_db(g50));
    int32_t g1 = audio_mix_gain_q16(1, -12.0f);
    CHECK(fabsf(gain_db(g1) + 61.5f) < 0.05f, "volume 1 = max_db - 49.5 dB, Q16 rounding (%f)", gain_db(g1));
    CHECK(g1 > 0, "volume 1 is not mute");
    // Monotonic and never above unity, whatever max_db.
    int32_t prev = 0;
    for (int v = 1; v <= 100; v++) {
        int32_t g = audio_mix_gain_q16(v, 0.0f);
        CHECK(g > prev, "monotonic at %d", v);
        CHECK(g <= AUDIO_MIX_GAIN_0DB, "<= unity at %d", v);
        prev = g;
    }
    CHECK(audio_mix_gain_q16(100, 0.0f) == AUDIO_MIX_GAIN_0DB, "0 dB cap is unity");
    CHECK(audio_mix_gain_q16(100, 6.0f) == AUDIO_MIX_GAIN_0DB, "positive max_db is clamped to unity");
    CHECK(audio_mix_gain_q16(120, -12.0f) == g100, "volume above 100 clamps to 100");
}

static void test_downmix(void)
{
    int32_t out[8];
    // Stereo: opposite samples cancel, equal samples pass, both slots equal.
    int16_t st[6] = { 1000, -1000,  2000, 2000,  -32768, -32768 };
    size_t n = audio_mix_frames(st, 3, 2, AUDIO_OUT_MONO, AUDIO_MIX_GAIN_0DB, out);
    CHECK(n == 6, "3 stereo frames give 6 words (%zu)", n);
    CHECK(out[0] == 0 && out[1] == 0, "L+R cancel to 0 (%d %d)", out[0], out[1]);
    CHECK(out[2] == 2000 * 65536 && out[3] == out[2], "equal L/R pass at unity, both slots");
    CHECK(out[4] == -32768 * 65536 && out[5] == out[4], "full-scale negative does not overflow (%d)", out[4]);
    // Mono: copied to both slots, scaled.
    int16_t mo[2] = { 100, 32767 };
    n = audio_mix_frames(mo, 2, 1, AUDIO_OUT_MONO, AUDIO_MIX_GAIN_0DB / 2, out);
    CHECK(n == 4, "2 mono frames give 4 words");
    CHECK(out[0] == 100 * 32768 && out[1] == out[0], "mono scaled by -6 dB (%d)", out[0]);
    CHECK(out[2] == 32767 * 32768, "mono full scale at -6 dB (%d)", out[2]);
    // Mute.
    n = audio_mix_frames(mo, 2, 1, AUDIO_OUT_MONO, 0, out);
    CHECK(out[0] == 0 && out[3] == 0, "gain 0 gives silence");
    // Stereo rounding toward zero keeps odd sums in range.
    int16_t odd[2] = { 32767, 32766 };
    audio_mix_frames(odd, 1, 2, AUDIO_OUT_MONO, AUDIO_MIX_GAIN_0DB, out);
    CHECK(out[0] == 32766 * 65536, "odd sum truncates (%d)", out[0]);
}

static void test_modes(void)
{
    int32_t out[4];
    int16_t st[4] = { 1000, -3000,  -32768, 32767 };
    const int32_t g = AUDIO_MIX_GAIN_0DB;
    size_t n = audio_mix_frames(st, 2, 2, AUDIO_OUT_STEREO, g, out);
    CHECK(n == 4, "2 stereo frames give 4 words (%zu)", n);
    CHECK(out[0] == 1000 * g && out[1] == -3000 * g, "stereo keeps L and R apart (%d %d)", out[0], out[1]);
    CHECK(out[2] == -32768 * g && out[3] == 32767 * g, "stereo full scale does not overflow (%d %d)", out[2], out[3]);
    audio_mix_frames(st, 2, 2, AUDIO_OUT_LEFT, g, out);
    CHECK(out[0] == 1000 * g && out[1] == out[0], "left: L on both slots (%d %d)", out[0], out[1]);
    CHECK(out[2] == -32768 * g && out[3] == out[2], "left: full-scale negative (%d)", out[2]);
    audio_mix_frames(st, 2, 2, AUDIO_OUT_RIGHT, g / 2, out);
    CHECK(out[0] == -3000 * (g / 2) && out[1] == out[0], "right: R on both slots, scaled (%d %d)", out[0], out[1]);
    CHECK(out[2] == 32767 * (g / 2) && out[3] == out[2], "right: full scale at -6 dB (%d)", out[2]);
    // A mono input plays on both slots whatever the mode.
    int16_t mo[1] = { -1234 };
    for (int m = AUDIO_OUT_MONO; m <= AUDIO_OUT_RIGHT; m++) {
        audio_mix_frames(mo, 1, 1, (audio_out_mode_t)m, g, out);
        CHECK(out[0] == -1234 * g && out[1] == out[0], "mono input in mode %d (%d %d)", m, out[0], out[1]);
    }
}

static void test_tone(void)
{
    enum { RATE = 48000, N = 4800 };  // 100 ms
    static int16_t buf[N];
    float phase = 0;
    size_t fade = RATE / 100;         // 10 ms
    // Generate in two chunks to prove the phase continues across calls.
    audio_mix_tone(buf, N / 2, RATE, 440.0f, 16000, &phase, 0, N, fade);
    audio_mix_tone(buf + N / 2, N / 2, RATE, 440.0f, 16000, &phase, N / 2, N, fade);
    CHECK(buf[0] == 0, "starts at zero (%d)", buf[0]);
    CHECK(abs(buf[N - 1]) < 50, "ends near zero (%d)", buf[N - 1]);
    int16_t peak = 0;
    for (size_t i = 0; i < N; i++) if (abs(buf[i]) > peak) peak = abs(buf[i]);
    CHECK(peak >= 15990 && peak <= 16000, "peak is amp (%d)", peak);
    // Max step between samples stays small: no click at the chunk boundary.
    int maxstep = 0;
    for (size_t i = 1; i < N; i++) { int d = abs(buf[i] - buf[i - 1]); if (d > maxstep) maxstep = d; }
    // 440 Hz at 48 kHz: max slope = 2*pi*440/48000 * 16000 = 921 per sample.
    CHECK(maxstep < 1000, "no discontinuity (max step %d)", maxstep);
    // Zero crossings: 440 Hz over 100 ms = 44 cycles = 88 crossings (about).
    int zc = 0;
    for (size_t i = 1; i < N; i++) if ((buf[i] >= 0) != (buf[i - 1] >= 0)) zc++;
    CHECK(zc >= 86 && zc <= 90, "frequency is 440 Hz (%d crossings)", zc);
}

int main(void)
{
    test_gain();
    test_downmix();
    test_modes();
    test_tone();
    if (g_fail) { printf("%d failure(s)\n", g_fail); return 1; }
    printf("audio_mix: all tests passed\n");
    return 0;
}
