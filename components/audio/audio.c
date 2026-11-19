// audio: shared output layer over the PCM5100A (pure I2S) and the TPA3118
// amp enable. See audio.h for the contract. The volume is a software gain
// applied in audio_mix.c; the DAC has no control interface.
#include <math.h>
#include <string.h>

#include "audio.h"
#include "audio_arbiter.h"
#include "audio_mix.h"
#include "audio_eq.h"
#include "board_pins.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"

static const char *TAG = "audio";

#define AUDIO_I2S_PORT       I2S_NUM_0
#define AUDIO_DEFAULT_RATE   48000

// Hardware ceiling, in dBFS, applied at volume 100. The TPA3118 gain is fixed
// at 20 dB with no limiter and the PCM5100A outputs 2.1 Vrms full scale, so
// 0 dBFS would be 21 Vrms, far past clipping (about 13 Vrms on 19 V, near
// -4 dBFS) and the speaker's 30 W. -9 dB (about 7 W on a sine) was set on the
// bench on 2026-11-09 and raised to -6 dB (about 14 W) on 2026-11-11 at the
// user's request; never raise it above -6 dB (see docs/hardware.md).
#define AUDIO_GAIN_MAX_DB   (-6.0f)

// Amp enable (IO17, active high through a MOSFET to the TPA3118 MUTE pin).
// Kept low except while a stream is open, so the speaker is silent at idle
// and never amplifies the DAC's start-up transient.
#define AMP_SETTLE_MS  80
static inline void amp_on(bool on) { gpio_set_level(BOARD_AMP_EN_GPIO, on ? 1 : 0); }

// Conversion buffer: 16-bit input frames become 8-byte output frames.
#define MIX_FRAMES 1024

// DMA: 6 x 384 frames of 8 bytes = 18 KB internal RAM, 48 ms at 48 kHz.
#define DMA_DESC_NUM   6
#define DMA_FRAME_NUM  384

static i2s_chan_handle_t s_tx;
static int32_t *s_mix;                 // MIX_FRAMES * 2 int32, in PSRAM
static bool s_open;                    // a source holds the output
static bool s_enabled;                 // I2S channel enabled (>= s_open)
static uint32_t s_rate;                // rate the channel is clocked at
static uint8_t s_channels;             // 1 or 2, format of the open stream
static volatile bool s_paused;
static int s_volume = 50;
static int s_volume_limit = 100;
static volatile int32_t s_gain;        // Q16, derived from s_volume
static volatile audio_out_mode_t s_out_mode = AUDIO_OUT_MONO;

// Speaker correction. The bands are set from any task; the filter itself
// belongs to the task that writes audio, which redesigns it when the bands or
// the sample rate change.
static portMUX_TYPE s_eq_mux = portMUX_INITIALIZER_UNLOCKED;
static audio_eq_band_t s_eq_bands[AUDIO_EQ_MAX_BANDS];
static int s_eq_count;
static volatile bool s_eq_dirty;
static audio_eq_t s_eq;
static uint32_t s_eq_rate;
static int32_t s_eq_limit;             // AUDIO_GAIN_MAX_DB as an int32 sample value

// Lazy close: audio_close keeps the I2S channel running (zeros, auto_clear)
// for a short window so an immediate next track at the same rate reopens
// with no clock reconfiguration. The amp is muted through the window.
#define IDLE_CLOSE_MS 2000
static SemaphoreHandle_t s_lock;
static esp_timer_handle_t s_close_timer;

static void idle_close_cb(void *arg)
{
    (void)arg;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_open && s_enabled) {
        i2s_channel_disable(s_tx);
        s_enabled = false;
    }
    xSemaphoreGive(s_lock);
}

static esp_err_t init_i2s(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(AUDIO_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = DMA_DESC_NUM;
    chan_cfg.dma_frame_num = DMA_FRAME_NUM;
    // Send zeros when nothing feeds the DMA instead of looping the last
    // buffer: the end of a stream must be silence, not a stuck tone.
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, NULL), TAG, "i2s_new_channel failed");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_DEFAULT_RATE),
        // 32-bit slots: the 16-bit input times the gain keeps its full
        // resolution at low volume (the amp adds 20 dB after the DAC).
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,  // the PCM5100A clocks itself from BCK
            .bclk = BOARD_I2S_BCLK_GPIO,
            .ws   = BOARD_I2S_WS_GPIO,
            .dout = BOARD_I2S_DOUT_GPIO,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {0},
        },
    };
    // Inverted WS: with the standard polarity the left slot came out of the
    // terminal marked R (bench, 2026-11-13). This puts the left slot on OUTL.
    std_cfg.slot_cfg.ws_pol = true;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std_cfg), TAG, "i2s init std failed");
    s_rate = AUDIO_DEFAULT_RATE;
    return ESP_OK;
}

esp_err_t audio_init(void)
{
    // Lock, timer and arbiter first: audio_init is best-effort at boot, and the
    // rest of the firmware keeps calling in after a hardware failure.
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "audio lock alloc failed");
    }
    if (!s_close_timer) {
        const esp_timer_create_args_t tca = { .callback = idle_close_cb, .name = "audio_idle" };
        ESP_RETURN_ON_ERROR(esp_timer_create(&tca, &s_close_timer), TAG, "close timer failed");
    }
    ESP_RETURN_ON_ERROR(audio_arbiter_init(), TAG, "arbiter init failed");

    // Amp off before anything else can toggle it.
    gpio_config_t amp = {
        .pin_bit_mask = 1ULL << BOARD_AMP_EN_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&amp);
    amp_on(false);

    if (!s_mix) {
        s_mix = heap_caps_malloc(MIX_FRAMES * 2 * sizeof(int32_t), MALLOC_CAP_SPIRAM);
        ESP_RETURN_ON_FALSE(s_mix, ESP_ERR_NO_MEM, TAG, "mix buffer alloc failed");
    }
    s_gain = audio_mix_gain_q16(s_volume, AUDIO_GAIN_MAX_DB);
    s_eq_limit = (int32_t)(powf(10.0f, AUDIO_GAIN_MAX_DB / 20.0f) * 2147483648.0f);
    ESP_RETURN_ON_ERROR(init_i2s(), TAG, "i2s setup failed");

    ESP_LOGI(TAG, "audio output ready (I2S port %d, 32-bit slots, amp IO%d, max %.0f dB)",
             AUDIO_I2S_PORT, BOARD_AMP_EN_GPIO, (double)AUDIO_GAIN_MAX_DB);
    return ESP_OK;
}

esp_err_t audio_open(uint32_t sample_rate, uint8_t bits_per_sample, uint8_t channels)
{
    if (!s_tx || !s_lock) return ESP_ERR_INVALID_STATE;  // audio_init failed
    if (bits_per_sample != 16 || (channels != 1 && channels != 2)) {
        ESP_LOGE(TAG, "unsupported format: %u bit, %u ch", bits_per_sample, channels);
        return ESP_ERR_NOT_SUPPORTED;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_timer_stop(s_close_timer);  // cancel a pending lazy close
    esp_err_t err = ESP_OK;
    if (s_enabled && s_rate == sample_rate) {
        // Fast reopen within the lazy-close window: the clock never stopped.
        // The amp is still muted from audio_close; unmute now, zeros are
        // flowing so nothing stale is amplified.
        s_channels = channels;
        s_open = true;
        s_paused = false;
        amp_on(true);
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    if (s_enabled) {
        i2s_channel_disable(s_tx);
        s_enabled = false;
    }
    if (s_rate != sample_rate) {
        i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
        err = i2s_channel_reconfig_std_clock(s_tx, &clk);
        if (err != ESP_OK) {
            xSemaphoreGive(s_lock);
            ESP_LOGE(TAG, "clock reconfig to %lu Hz failed: %s", (unsigned long)sample_rate, esp_err_to_name(err));
            return err;
        }
        s_rate = sample_rate;
    }
    err = i2s_channel_enable(s_tx);
    if (err != ESP_OK) {
        xSemaphoreGive(s_lock);
        ESP_LOGE(TAG, "i2s enable failed: %s", esp_err_to_name(err));
        return err;
    }
    s_enabled = true;
    s_channels = channels;
    s_open = true;
    s_paused = false;
    vTaskDelay(pdMS_TO_TICKS(AMP_SETTLE_MS));  // clocks and DAC settle on zeros, then unmute
    amp_on(true);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

static void eq_update(void)
{
    audio_eq_band_t bands[AUDIO_EQ_MAX_BANDS];
    portENTER_CRITICAL(&s_eq_mux);
    int count = s_eq_count;
    memcpy(bands, s_eq_bands, sizeof(bands));
    s_eq_dirty = false;
    portEXIT_CRITICAL(&s_eq_mux);
    audio_eq_design(&s_eq, bands, count, s_rate);
    s_eq_rate = s_rate;
    ESP_LOGI(TAG, "eq: %d band(s) at %lu Hz, pre-gain %.1f dB", s_eq.n, (unsigned long)s_rate,
             20.0 * log10f(s_eq.pre));
}

static esp_err_t write_frames(const int16_t *in, size_t frames, int channels)
{
    if (s_eq_dirty || s_eq_rate != s_rate) eq_update();
    while (frames > 0) {
        size_t n = frames < MIX_FRAMES ? frames : MIX_FRAMES;
        size_t words = audio_mix_frames(in, n, channels, s_out_mode, s_gain, s_mix);
        audio_eq_process(&s_eq, s_mix, n, s_eq_limit);
        size_t written = 0;
        esp_err_t err = i2s_channel_write(s_tx, s_mix, words * sizeof(int32_t), &written, portMAX_DELAY);
        if (err != ESP_OK) return err;
        in += n * channels;
        frames -= n;
    }
    return ESP_OK;
}

esp_err_t audio_write(const void *pcm, size_t bytes)
{
    if (!s_open) return ESP_ERR_INVALID_STATE;
    // While paused, hold this chunk and keep the DAC fed with silence. The
    // decode loop blocks here, which also stalls the byte source.
    while (s_paused && s_open) {
        static const int32_t silence[256] = {0};
        size_t written = 0;
        i2s_channel_write(s_tx, silence, sizeof(silence), &written, portMAX_DELAY);
    }
    if (!s_open) return ESP_ERR_INVALID_STATE;
    size_t frames = bytes / (2 * s_channels);
    return write_frames((const int16_t *)pcm, frames, s_channels);
}

void audio_set_paused(bool paused) { s_paused = paused; }
bool audio_is_paused(void) { return s_paused; }
int audio_get_volume(void) { return s_volume; }
bool audio_is_active(void) { return s_open; }

void audio_output_off(void)
{
    amp_on(false);  // instant silence; buffered PCM drains into a muted amp
}

esp_err_t audio_close(void)
{
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_open) {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    s_open = false;
    amp_on(false);  // instant silence; the I2S stop is deferred (lazy close)
    esp_timer_stop(s_close_timer);
    esp_timer_start_once(s_close_timer, (uint64_t)IDLE_CLOSE_MS * 1000);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t audio_set_volume(int volume)
{
    if (volume < 0) volume = 0;
    if (volume > s_volume_limit) volume = s_volume_limit;
    s_volume = volume;
    s_gain = audio_mix_gain_q16(volume, AUDIO_GAIN_MAX_DB);  // next mixed chunk uses it
    return ESP_OK;
}

void audio_set_volume_limit(int limit)
{
    if (limit < 1) limit = 1;
    if (limit > 100) limit = 100;
    s_volume_limit = limit;
    if (s_volume > limit) audio_set_volume(limit);
}

int audio_get_volume_limit(void) { return s_volume_limit; }

void audio_set_output_mode(audio_out_mode_t mode) { s_out_mode = mode; }  // next mixed chunk uses it

void audio_set_eq(const audio_eq_band_t *bands, int count)
{
    if (count < 0) count = 0;
    if (count > AUDIO_EQ_MAX_BANDS) count = AUDIO_EQ_MAX_BANDS;
    portENTER_CRITICAL(&s_eq_mux);
    if (count) memcpy(s_eq_bands, bands, (size_t)count * sizeof(*bands));
    s_eq_count = count;
    s_eq_dirty = true;  // the next written chunk uses it
    portEXIT_CRITICAL(&s_eq_mux);
}

#define TONE_RATE  48000
#define TONE_AMP   16000   // -6 dBFS peak, headroom below the mix ceiling
#define TONE_CHUNK 480     // 10 ms

esp_err_t audio_test_tone(int hz, int ms, int side)
{
    if (hz < 20 || hz > 20000 || ms < 10 || ms > 10000 || side < 0 || side > 2) return ESP_ERR_INVALID_ARG;
    int channels = side ? 2 : 1;
    ESP_RETURN_ON_ERROR(audio_arbiter_acquire(AUDIO_SOURCE_TONE), TAG, "output busy");
    int16_t *buf = malloc(TONE_CHUNK * channels * sizeof(int16_t));
    if (!buf) {
        audio_arbiter_release(AUDIO_SOURCE_TONE);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = audio_open(TONE_RATE, 16, (uint8_t)channels);
    if (err == ESP_OK) {
        size_t total = (size_t)TONE_RATE * ms / 1000, pos = 0;
        float phase = 0;
        ESP_LOGI(TAG, "test tone %d Hz for %d ms at volume %d", hz, ms, s_volume);
        while (pos < total && err == ESP_OK) {
            size_t n = total - pos < TONE_CHUNK ? total - pos : TONE_CHUNK;
            audio_mix_tone(buf, n, TONE_RATE, (float)hz, TONE_AMP, &phase, pos, total, TONE_RATE / 100);
            if (side) {
                // Spread in place, from the end: the tone on one channel, zero on the other.
                for (size_t i = n; i-- > 0;) {
                    int16_t s = buf[i];
                    buf[2 * i] = side == 1 ? s : 0;
                    buf[2 * i + 1] = side == 2 ? s : 0;
                }
            }
            err = audio_write(buf, n * channels * sizeof(int16_t));
            pos += n;
        }
        // Let the DMA drain before muting, so the fade-out is heard.
        vTaskDelay(pdMS_TO_TICKS(1000 * DMA_DESC_NUM * DMA_FRAME_NUM / TONE_RATE + 10));
        audio_close();
    }
    free(buf);
    audio_arbiter_release(AUDIO_SOURCE_TONE);
    return err;
}
