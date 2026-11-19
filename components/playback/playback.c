#include "playback.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "audio.h"
#include "audio_arbiter.h"
#include "config_store.h"
#include "source_stream.h"
#include "source_sendspin.h"

static const char *TAG = "playback";

typedef struct {
    uint32_t generation;
    int      radio_id;
    bool     decoy;              // webradio skip_preroll: arm the decoy connection
    char     url[CFG_URL_MAX];
} play_req_t;

static QueueHandle_t s_q;
static volatile uint32_t s_generation;   // bumped by every play or stop request
static volatile bool s_busy;             // worker is running a request
static volatile bool s_retrying;         // reconnect wait in progress
static volatile int  s_radio_id;         // radio of the running request
static volatile bool s_stopped;          // stop requested: report idle at once, before the source unwinds

// The request the worker holds is current until a newer play or stop arrives.
static bool play_current(uint32_t generation)
{
    return s_generation == generation && uxQueueMessagesWaiting(s_q) == 0;
}

static bool sendspin_owns_output(void)
{
    return source_sendspin_session_active() || audio_arbiter_active() == AUDIO_SOURCE_SENDSPIN;
}

// Same reconnect policy as Bugne's play worker: a stream that dies is retried
// with backoff, up to 6 attempts over about 2 minutes; an attempt that played
// more than 60 s opens a fresh retry window. Any newer request, a stop, or a
// Music Assistant session engaging meanwhile ends the retries at once.
static void play_task(void *arg)
{
    (void)arg;
    static const uint16_t retry_delay_s[] = { 2, 5, 10, 20, 30, 45 };
    play_req_t req;
    for (;;) {
        if (xQueueReceive(s_q, &req, portMAX_DELAY) != pdTRUE) continue;
        if (!play_current(req.generation)) continue;
        ESP_LOGI(TAG, "play request: radio %d %s", req.radio_id, req.url);
        s_busy = true;
        s_stopped = false;
        s_radio_id = req.radio_id;

        // A local play ends the Music Assistant session (playback_play_radio
        // sent the stop command): wait for the output to be released.
        int64_t takeover_until = esp_timer_get_time() + 8000000LL;
        bool takeover_timed_out = false;
        while (play_current(req.generation) && sendspin_owns_output()) {
            if (esp_timer_get_time() >= takeover_until) {
                takeover_timed_out = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (takeover_timed_out) ESP_LOGW(TAG, "Sendspin takeover timed out");

        int attempt = 0;
        while (!takeover_timed_out) {
            if (!play_current(req.generation) || sendspin_owns_output()) break;
            source_stream_set_preroll_decoy(req.decoy);  // always set, even false
            int64_t t0 = esp_timer_get_time();
            esp_err_t err = source_stream_play(req.url);
            // Stop, newer request, or Music Assistant engaged meanwhile (a
            // reconnect must not steal the output back): not a failure.
            if (!play_current(req.generation) || source_sendspin_session_active()) break;
            // A live radio never ends by itself: any return is an abnormal
            // end (dead connection, unreachable stream). Retry with backoff.
            if (esp_timer_get_time() - t0 > 60 * 1000000LL) attempt = 0;
            if (attempt >= (int)(sizeof(retry_delay_s) / sizeof(retry_delay_s[0]))) {
                ESP_LOGW(TAG, "stream gave up after %d attempts (%s)", attempt, esp_err_to_name(err));
                break;
            }
            s_retrying = true;
            ESP_LOGW(TAG, "stream died (%s), reconnect attempt %d in %u s",
                     esp_err_to_name(err), attempt + 1, (unsigned)retry_delay_s[attempt]);
            int64_t until = esp_timer_get_time() + (int64_t)retry_delay_s[attempt] * 1000000LL;
            while (esp_timer_get_time() < until && play_current(req.generation) &&
                   !source_sendspin_session_active()) {
                vTaskDelay(pdMS_TO_TICKS(200));
            }
            s_retrying = false;
            attempt++;
        }
        s_retrying = false;
        s_radio_id = 0;
        s_busy = false;
        ESP_LOGI(TAG, "play finished");
    }
}

// A pause that lasts longer than the playback.pause_timeout_min setting ends
// in a stop, so a forgotten pause does not hold the speaker (and its Music
// Assistant session) forever. Checked every 30 s; the setting is read each
// time, so a change applies to a pause already running.
#define PAUSE_WATCH_PERIOD_S 30
static int64_t s_paused_since;  // esp_timer time when the pause was first seen, 0 = not paused

static void pause_watch_cb(void *arg)
{
    (void)arg;
    playback_status_t st;
    playback_status(&st);
    int limit_min = config_store_get()->pause_timeout_min;
    if (!st.active || !st.paused || limit_min <= 0) {
        s_paused_since = 0;
        return;
    }
    int64_t now = esp_timer_get_time();
    if (!s_paused_since) {
        s_paused_since = now;
    } else if (now - s_paused_since >= (int64_t)limit_min * 60 * 1000000LL) {
        ESP_LOGI(TAG, "paused for %d min: stopping", limit_min);
        s_paused_since = 0;
        playback_stop();
    }
}

esp_err_t playback_init(void)
{
    s_q = xQueueCreate(1, sizeof(play_req_t));
    if (!s_q) return ESP_ERR_NO_MEM;
    const esp_timer_create_args_t watch = { .callback = pause_watch_cb, .name = "pause_watch" };
    esp_timer_handle_t timer;
    if (esp_timer_create(&watch, &timer) != ESP_OK ||
        esp_timer_start_periodic(timer, (uint64_t)PAUSE_WATCH_PERIOD_S * 1000000) != ESP_OK) {
        return ESP_FAIL;
    }
    // Internal stack: the stream source runs TLS and the decoders on this task.
    // Same size and priority as Bugne's play worker.
    if (xTaskCreatePinnedToCore(play_task, "playback", 16384, NULL, 5, NULL, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

// Tear down whatever plays now so the next source starts from silence.
static void stop_sources(void)
{
    s_generation++;
    audio_set_paused(false);   // release a paused decode loop so it can unwind
    source_stream_stop();
    // Only one source plays at a time: an explicit local play or stop ends the
    // Music Assistant session too, otherwise it lingers across stream-end.
    if (source_sendspin_session_active()) {
        source_sendspin_command(SENDSPIN_CMD_STOP);
    }
}

esp_err_t playback_play_radio(int id)
{
    const config_t *c = config_store_get();
    const config_webradio_t *w = NULL;
    for (size_t i = 0; c && i < c->webradio_count; i++) {
        if (c->webradios[i].id == id) { w = &c->webradios[i]; break; }
    }
    if (!w || !w->url[0]) return ESP_ERR_NOT_FOUND;
    if (!s_q) return ESP_ERR_INVALID_STATE;
    stop_sources();
    play_req_t req = {
        .generation = s_generation,
        .radio_id = id,
        .decoy = w->skip_preroll != 0,
    };
    strlcpy(req.url, w->url, sizeof(req.url));
    xQueueOverwrite(s_q, &req);
    return ESP_OK;
}

void playback_stop(void)
{
    s_stopped = true;
    audio_output_off();  // mute now, not after the buffers drain
    stop_sources();
}

void playback_toggle(void)
{
    if (source_sendspin_session_active()) {
        source_sendspin_command(source_sendspin_active() ? SENDSPIN_CMD_PAUSE : SENDSPIN_CMD_PLAY);
    } else if (s_busy) {
        audio_set_paused(!audio_is_paused());
    }
}

bool playback_active(void)
{
    return s_busy || source_sendspin_session_active() ||
           audio_arbiter_active() != AUDIO_SOURCE_NONE;
}

void playback_status(playback_status_t *out)
{
    memset(out, 0, sizeof(*out));
    strlcpy(out->source, "none", sizeof(out->source));
    if (source_sendspin_session_active()) {
        out->active = true;
        out->paused = !source_sendspin_active();
        strlcpy(out->source, "sendspin", sizeof(out->source));
        source_sendspin_title(out->title, sizeof(out->title));
        source_sendspin_artist(out->artist, sizeof(out->artist));
    } else if (s_busy && !s_stopped) {
        out->active = true;
        out->paused = audio_is_paused();
        out->retrying = s_retrying;
        strlcpy(out->source, "radio", sizeof(out->source));
        out->radio_id = s_radio_id;
        source_stream_title(out->title, sizeof(out->title));
    }
}
