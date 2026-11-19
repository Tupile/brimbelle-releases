// audio_mix: pure PCM helpers, see audio_mix.h. Host-tested in test/host.
#include "audio_mix.h"

#include <math.h>

int32_t audio_mix_gain_q16(int volume, float max_db)
{
    if (volume <= 0) return 0;
    if (volume > 100) volume = 100;
    if (max_db > 0.0f) max_db = 0.0f;  // never above unity: a 16-bit input must fit 32 bits
    float db = max_db - (100 - volume) * AUDIO_MIX_DB_PER_STEP;
    float g = powf(10.0f, db / 20.0f) * AUDIO_MIX_GAIN_0DB;
    if (g > AUDIO_MIX_GAIN_0DB) g = AUDIO_MIX_GAIN_0DB;
    return (int32_t)(g + 0.5f);
}

size_t audio_mix_frames(const int16_t *in, size_t frames, int channels,
                        audio_out_mode_t mode, int32_t gain_q16, int32_t *out)
{
    if (channels == 2) {
        for (size_t i = 0; i < frames; i++) {
            int32_t l = in[2 * i], r = in[2 * i + 1];
            if (mode == AUDIO_OUT_MONO) l = r = (l + r) / 2;
            else if (mode == AUDIO_OUT_LEFT) r = l;
            else if (mode == AUDIO_OUT_RIGHT) l = r;
            out[2 * i] = l * gain_q16;  // |sample| <= 32768, gain <= 65536: fits int32
            out[2 * i + 1] = r * gain_q16;
        }
    } else {
        for (size_t i = 0; i < frames; i++) {
            int32_t s = (int32_t)in[i] * gain_q16;
            out[2 * i] = s;
            out[2 * i + 1] = s;
        }
    }
    return frames * 2;
}

void audio_mix_tone(int16_t *out, size_t n, uint32_t rate, float hz, int16_t amp,
                    float *phase, size_t pos, size_t total, size_t fade)
{
    float step = hz / (float)rate;
    for (size_t i = 0; i < n; i++, pos++) {
        float env = 1.0f;
        if (fade) {
            if (pos < fade) env = (float)pos / (float)fade;
            else if (pos + fade > total) env = (float)(total - pos) / (float)fade;
            if (env < 0.0f) env = 0.0f;
        }
        out[i] = (int16_t)(sinf(6.28318530718f * *phase) * (float)amp * env);
        *phase += step;
        if (*phase >= 1.0f) *phase -= 1.0f;
    }
}
