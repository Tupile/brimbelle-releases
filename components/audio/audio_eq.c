// audio_eq: biquad chain, see audio_eq.h. Host-tested in test/host.
// Coefficients follow the Audio EQ Cookbook (R. Bristow-Johnson).
#include "audio_eq.h"

#include <math.h>
#include <string.h>

#define PI 3.14159265358979323846

void audio_eq_design(audio_eq_t *eq, const audio_eq_band_t *bands, int n, uint32_t rate)
{
    memset(eq, 0, sizeof(*eq));
    eq->pre = 1.0f;
    if (n > AUDIO_EQ_MAX_BANDS) n = AUDIO_EQ_MAX_BANDS;
    for (int i = 0; i < n; i++) {
        const audio_eq_band_t *b = &bands[i];
        if (b->type < AUDIO_EQ_HIGHPASS || b->type > AUDIO_EQ_PEAK) continue;
        if (!(b->hz >= 20.0f) || b->hz > 0.45f * (float)rate) continue;
        double db = b->db < -12.0f ? -12.0 : b->db > 12.0f ? 12.0 : b->db;
        double q = b->q <= 0.0f ? 0.707 : b->q < 0.3f ? 0.3 : b->q > 10.0f ? 10.0 : b->q;
        double w = 2.0 * PI * b->hz / rate, c = cos(w), alpha = sin(w) / (2.0 * q);
        double A = pow(10.0, db / 40.0), k = 2.0 * sqrt(A) * alpha;
        double b0, b1, b2, a0, a1, a2;
        switch (b->type) {
        case AUDIO_EQ_HIGHPASS:
            b0 = (1 + c) / 2; b1 = -(1 + c); b2 = (1 + c) / 2;
            a0 = 1 + alpha; a1 = -2 * c; a2 = 1 - alpha;
            break;
        case AUDIO_EQ_LOWSHELF:
            b0 = A * ((A + 1) - (A - 1) * c + k); b1 = 2 * A * ((A - 1) - (A + 1) * c); b2 = A * ((A + 1) - (A - 1) * c - k);
            a0 = (A + 1) + (A - 1) * c + k; a1 = -2 * ((A - 1) + (A + 1) * c); a2 = (A + 1) + (A - 1) * c - k;
            break;
        case AUDIO_EQ_HIGHSHELF:
            b0 = A * ((A + 1) + (A - 1) * c + k); b1 = -2 * A * ((A - 1) + (A + 1) * c); b2 = A * ((A + 1) + (A - 1) * c - k);
            a0 = (A + 1) - (A - 1) * c + k; a1 = 2 * ((A - 1) - (A + 1) * c); a2 = (A + 1) - (A - 1) * c - k;
            break;
        default:  // AUDIO_EQ_PEAK
            b0 = 1 + alpha * A; b1 = -2 * c; b2 = 1 - alpha * A;
            a0 = 1 + alpha / A; a1 = -2 * c; a2 = 1 - alpha / A;
            break;
        }
        eq->s[eq->n].b0 = (float)(b0 / a0);
        eq->s[eq->n].b1 = (float)(b1 / a0);
        eq->s[eq->n].b2 = (float)(b2 / a0);
        eq->s[eq->n].a1 = (float)(a1 / a0);
        eq->s[eq->n].a2 = (float)(a2 / a0);
        eq->n++;
    }
    // Pre-gain: bring the highest point of the curve down to 0 dB. Scanned on
    // a 1/24 octave grid from 20 Hz to the top of the band.
    float max_db = 0.0f;
    for (float hz = 20.0f; hz < 0.45f * (float)rate; hz *= 1.0293f) {
        float db = audio_eq_response_db(eq, hz, rate);
        if (db > max_db) max_db = db;
    }
    eq->pre = powf(10.0f, -max_db / 20.0f);
}

float audio_eq_response_db(const audio_eq_t *eq, float hz, uint32_t rate)
{
    double w = 2.0 * PI * hz / rate, c1 = cos(w), s1 = sin(w), c2 = cos(2 * w), s2 = sin(2 * w);
    double db = 20.0 * log10(eq->pre);
    for (int i = 0; i < eq->n; i++) {
        double nr = eq->s[i].b0 + eq->s[i].b1 * c1 + eq->s[i].b2 * c2, ni = -(eq->s[i].b1 * s1 + eq->s[i].b2 * s2);
        double dr = 1.0 + eq->s[i].a1 * c1 + eq->s[i].a2 * c2, di = -(eq->s[i].a1 * s1 + eq->s[i].a2 * s2);
        db += 10.0 * log10((nr * nr + ni * ni) / (dr * dr + di * di) + 1e-30);
    }
    return (float)db;
}

void audio_eq_process(audio_eq_t *eq, int32_t *io, size_t frames, int32_t limit)
{
    if (eq->n == 0) return;
    const float lim = (float)limit;
    for (size_t i = 0; i < frames * 2; i++) {
        int ch = i & 1;
        float x = (float)io[i] * eq->pre;
        for (int b = 0; b < eq->n; b++) {  // transposed direct form II
            float *z = eq->s[b].z[ch];
            float y = eq->s[b].b0 * x + z[0];
            z[0] = eq->s[b].b1 * x - eq->s[b].a1 * y + z[1];
            z[1] = eq->s[b].b2 * x - eq->s[b].a2 * y;
            x = y;
        }
        io[i] = x > lim ? limit : x < -lim ? -limit : (int32_t)x;
    }
}
