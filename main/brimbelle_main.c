// Brimbelle app entry point.
//
// Boots the foundation in dependency order. config_store and board are
// required (a failure there is fatal). The rest are best-effort: a failure is
// logged but the device keeps running, so a broken network never reboot-loops
// the whole unit.

#include <stdbool.h>
#include <stdint.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "esp_pm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"

#include "logstore.h"
#include "config_store.h"
#include "board.h"
#include "audio.h"
#include "net.h"
#include "web_config.h"
#include "source_stream.h"
#include "source_sendspin.h"
#include "playback.h"
#include "encoder.h"
#include "ha_mqtt.h"

static const char *TAG = "brimbelle";

// cJSON has no PSRAM allocator by default: route its heap to PSRAM so config
// saves and web handlers never eat the scarce internal RAM.
static void *json_psram_malloc(size_t sz) { return heap_caps_malloc(sz, MALLOC_CAP_SPIRAM); }
static void  json_psram_free(void *p)     { heap_caps_free(p); }

// After an OTA update the new image boots as "pending verify". Confirm it
// healthy only after a stable uptime, so an image that crash-loops or dies a
// few seconds in is rolled back to the previous slot by the bootloader. The
// speaker has no screen: an image that cannot be reached over Wi-Fi
// could only be replaced over USB, so it is also confirmed only once the web
// server runs and the network is up (station connected, or setup AP). Until
// then it stays pending and the next reset rolls back.
#define OTA_VALIDATE_DELAY_MS 30000
static bool s_web_ok;

static void ota_validate_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(OTA_VALIDATE_DELAY_MS));
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        if (!s_web_ok) {
            ESP_LOGW(TAG, "web server down: OTA image left unconfirmed");
            vTaskDelete(NULL);
        }
        while (net_state() != NET_STATE_CONNECTED && net_state() != NET_STATE_PROVISIONING) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
            ESP_LOGI(TAG, "OTA image confirmed healthy, rollback canceled");
        }
    }
    vTaskDelete(NULL);
}

// Periodic memory telemetry, retrievable over /api/logs. Logged every
// HEAP_LOG_PERIOD_MIN minutes, or at once when the internal low-water mark
// drops by HEAP_LOG_LOW_STEP or more.
#define HEAP_LOG_PERIOD_MIN 15
#define HEAP_LOG_LOW_STEP   1024
static void heap_log_task(void *arg)
{
    (void)arg;
    size_t last_min = SIZE_MAX;
    unsigned minutes = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(60000));
        size_t min_free = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        bool new_low = last_min == SIZE_MAX || min_free + HEAP_LOG_LOW_STEP <= last_min;
        if (!new_low && ++minutes < HEAP_LOG_PERIOD_MIN) continue;
        minutes = 0;
        last_min = min_free;
        ESP_LOGI(TAG, "internal RAM: %u free, %u min free (PSRAM %u free)%s",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)min_free,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 new_low ? " [new low]" : "");
    }
}

// Run a best-effort init: log on failure but keep going.
#define TRY(call) do {                                            \
    esp_err_t _err = (call);                                      \
    if (_err != ESP_OK) {                                         \
        ESP_LOGE(TAG, #call " failed: %s", esp_err_to_name(_err));\
    }                                                             \
} while (0)

void app_main(void)
{
    cJSON_Hooks json_hooks = { .malloc_fn = json_psram_malloc, .free_fn = json_psram_free };
    cJSON_InitHooks(&json_hooks);

    // Capture logs into a PSRAM ring buffer first, so boot logs are retrievable
    // over Wi-Fi (/api/logs). Best-effort: the console still works without it.
    TRY(logstore_init());

    ESP_LOGI(TAG, "Brimbelle booting (reset reason: %s)", board_reset_reason_name());

#if CONFIG_PM_ENABLE
    esp_pm_config_t pm = {
        .max_freq_mhz = 240,
        .min_freq_mhz = 160,
        .light_sleep_enable = false,
    };
    TRY(esp_pm_configure(&pm));
#endif

    // Required: configuration and the device identity.
    ESP_ERROR_CHECK(config_store_init());
    ESP_ERROR_CHECK(board_init());

    // Best-effort from here. The output stays silent (amp off) until a source
    // opens it; the volume ceiling from config protects the speaker.
    TRY(audio_init());
    audio_set_volume_limit(config_store_get()->audio.volume_max);
    audio_set_output_mode((audio_out_mode_t)config_store_get()->audio.output);
    {
        const config_audio_t *a = &config_store_get()->audio;
        audio_eq_band_t bands[CFG_MAX_EQ_BANDS];
        for (size_t i = 0; i < a->eq_count; i++) {
            bands[i] = (audio_eq_band_t){ a->eq[i].type, a->eq[i].hz, a->eq[i].db, a->eq[i].q };
        }
        audio_set_eq(bands, (int)a->eq_count);
    }
    audio_set_volume(config_store_get()->audio.volume);
    TRY(net_start());
    esp_err_t web_err = web_config_start();
    s_web_ok = (web_err == ESP_OK);
    TRY(web_err);
    TRY(source_stream_init());
    TRY(playback_init());
    TRY(source_sendspin_init());  // advertises over mDNS once the station is up
    TRY(ha_mqtt_start());         // connects once the station is up
    TRY(encoder_init());

    ESP_LOGI(TAG, "Brimbelle boot complete");

    xTaskCreate(ota_validate_task, "ota_valid", 3072, NULL, 1, NULL);
    xTaskCreate(heap_log_task, "heap_log", 3072, NULL, 1, NULL);  // vprintf needs headroom
}
