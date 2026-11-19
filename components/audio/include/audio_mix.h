// audio_mix: pure PCM helpers for the output (no ESP-IDF, host-tested).
//
// The output is a stereo I2S frame of two 32-bit slots (left, right). The
// output mode decides what each slot carries: by default the same mono
// sample, so a single speaker on OUTL hears everything. Gain is a Q16
// fixed-point factor (65536 = 0 dB), always <= 0 dB so a 16-bit input scaled
// to 32 bits can never overflow.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_MIX_GAIN_0DB   65536   // Q16 unity gain
#define AUDIO_MIX_DB_PER_STEP 0.5f   // volume 1..100 spans 49.5 dB, like a codec

// Volume 0..100 to a Q16 gain. 0 is mute (0). 100 is max_db (which must be
// <= 0, the hardware ceiling of the board). Each step below 100 is 0.5 dB.
int32_t audio_mix_gain_q16(int volume, float max_db);

// What the two output slots carry for a stereo input. A mono input goes to
// both slots in every mode.
typedef enum {
    AUDIO_OUT_MONO = 0,  // (L + R) / 2 on both slots: one speaker (default)
    AUDIO_OUT_STEREO,    // L on the left slot, R on the right: two speakers on one board
    AUDIO_OUT_LEFT,      // L on both slots: the left speaker of a pair of units
    AUDIO_OUT_RIGHT,     // R on both slots: the right speaker of a pair of units
} audio_out_mode_t;

// Convert `frames` frames of interleaved 16-bit PCM (`channels` = 1 or 2) into
// `frames` output frames of two int32 slots (left, right) scaled by gain_q16,
// filled according to `mode`. Returns the number of int32 values written
// (frames * 2).
size_t audio_mix_frames(const int16_t *in, size_t frames, int channels,
                        audio_out_mode_t mode, int32_t gain_q16, int32_t *out);

// Sine generator for the test tone: writes `n` 16-bit mono samples at `hz`,
// continuing from *phase (0..1, updated). amp is the peak (e.g. 16000).
// fade_in / fade_out are sample counts applied at `pos` relative to `total`
// samples, so the tone starts and ends without a click.
void audio_mix_tone(int16_t *out, size_t n, uint32_t rate, float hz, int16_t amp,
                    float *phase, size_t pos, size_t total, size_t fade);

#ifdef __cplusplus
}
#endif
