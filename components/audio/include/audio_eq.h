// audio_eq: a short chain of biquad filters that corrects the speaker and its
// enclosure (no ESP-IDF, host-tested). Runs on the 32-bit output frames,
// after the mono mix and the volume gain.
//
// The chain never raises the level: audio_eq_design() lowers everything by
// the largest boost of the combined curve, so the hardware ceiling of the
// board (see AUDIO_GAIN_MAX_DB in audio.c) still holds on a steady signal.
// Transients that overshoot are clipped at `limit` by audio_eq_process().
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_EQ_MAX_BANDS 6

typedef enum {
    AUDIO_EQ_HIGHPASS = 1,   // 12 dB/octave below hz, db unused
    AUDIO_EQ_LOWSHELF,       // db below hz
    AUDIO_EQ_HIGHSHELF,      // db above hz
    AUDIO_EQ_PEAK,           // db around hz, width set by q
} audio_eq_type_t;

typedef struct {
    int   type;   // audio_eq_type_t; anything else skips the band
    float hz;     // 20 .. 0.45 * rate, else the band is skipped
    float db;     // clamped to -12 .. +12
    float q;      // clamped to 0.3 .. 10; <= 0 means 0.707
} audio_eq_band_t;

typedef struct {
    int   n;      // active biquads, 0 = bypass (samples untouched)
    float pre;    // linear gain applied before the chain, <= 1
    struct {
        float b0, b1, b2, a1, a2;
        float z[2][2];           // state per output channel
    } s[AUDIO_EQ_MAX_BANDS];
} audio_eq_t;

// Compute the coefficients for `rate` and clear the filter state.
void audio_eq_design(audio_eq_t *eq, const audio_eq_band_t *bands, int n, uint32_t rate);

// Gain of the whole chain at `hz`, in dB, pre-gain included.
float audio_eq_response_db(const audio_eq_t *eq, float hz, uint32_t rate);

// Filter `frames` stereo frames of int32 in place, clipping at +/- limit.
void audio_eq_process(audio_eq_t *eq, int32_t *io, size_t frames, int32_t limit);

#ifdef __cplusplus
}
#endif
