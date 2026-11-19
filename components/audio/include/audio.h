// audio: the one shared output layer.
//
// Exactly one owner of the I2S bus (PCM5100A, no control interface) and of
// the amp enable (IO17, active high). All sources route 16-bit PCM through
// audio_write(), which mixes it for the output mode (mono by default), applies
// the volume gain and feeds the DAC with 32-bit stereo slots. The API matches Bugne's so the sources
// are shared, minus the codec I2C bus and the microphone path.
//
// The source arbiter lives alongside this layer, see audio_arbiter.h.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "audio_mix.h"
#include "audio_eq.h"

#ifdef __cplusplus
extern "C" {
#endif

// Bring up the I2S TX channel and the amp GPIO (muted), and start the
// arbiter. Call once at boot.
esp_err_t audio_init(void);

// Configure the output for a stream and unmute the amp. Only 16-bit PCM, 1
// or 2 channels. Call before audio_write().
esp_err_t audio_open(uint32_t sample_rate, uint8_t bits_per_sample, uint8_t channels);

// Write interleaved 16-bit PCM. Blocks until the bytes are queued to I2S.
esp_err_t audio_write(const void *pcm, size_t bytes);

// Stop the output and mute the amp.
esp_err_t audio_close(void);

// Mute the amp immediately. Used on Stop so playback goes silent at once
// instead of draining buffered audio. The next audio_open() unmutes.
void audio_output_off(void);

// Set output volume, 0 to 100. Clamped to the volume limit. Applied live.
esp_err_t audio_set_volume(int volume);
int audio_get_volume(void);

// Volume ceiling (child-ear and speaker protection), 1 to 100. Every volume
// request is clamped to it. Lowering it below the current volume lowers the
// volume immediately. Set from the audio.volume_max config by main.
void audio_set_volume_limit(int limit);
int audio_get_volume_limit(void);

// What the two outputs (OUTL, OUTR) play, see audio_out_mode_t. Mono by
// default. Applied live. Set from the audio.output config by main.
void audio_set_output_mode(audio_out_mode_t mode);

// Speaker correction: up to AUDIO_EQ_MAX_BANDS filters applied to everything
// that plays, see audio_eq.h. count 0 turns it off. Applied live. Set from
// the audio.eq config by main.
void audio_set_eq(const audio_eq_band_t *bands, int count);

// Pause or resume playback without closing the stream. While paused,
// audio_write blocks and the DAC is fed silence. Cleared on each audio_open.
void audio_set_paused(bool paused);
bool audio_is_paused(void);

// True between audio_open() and audio_close().
bool audio_is_active(void);

// Play a sine of `hz` for `ms` at the current volume, as the TONE source.
// `side` 0 plays a mono tone; 1 or 2 puts it on the left or right channel of
// a stereo stream, the other one silent, to check which speaker is which (the
// output mode still applies: in mono mode a one-sided tone plays on both
// outputs, 6 dB lower). Blocks for the duration. Fails if another source is
// active.
esp_err_t audio_test_tone(int hz, int ms, int side);

#ifdef __cplusplus
}
#endif
