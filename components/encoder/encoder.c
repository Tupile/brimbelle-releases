// encoder: KY-040 rotary encoder with push switch. See encoder.h.
//
// CLK and DT go to a hardware pulse counter in quadrature mode (4 counts per
// detent), so no edge is lost between polls. The switch is sampled by the
// poll task and debounced by buttons.c.
#include "encoder.h"

#include <string.h>

#include "audio.h"
#include "audio_arbiter.h"
#include "board_pins.h"
#include "buttons.h"
#include "driver/gpio.h"
#include "driver/pulse_cnt.h"
#include "enc_logic.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ha_mqtt.h"
#include "playback.h"
#include "source_sendspin.h"

static const char *TAG = "encoder";

#define ENC_POLL_MS      20
#define ENC_VOLUME_STEP  2     // volume points per detent
#define ENC_COUNT_LIMIT  1000  // counter wraps to 0 here; multiple of 4
#define ENC_GLITCH_NS    10000
#define ENC_BEEP_HZ      880   // long press confirmation
#define ENC_BEEP_MS      120

static pcnt_unit_handle_t s_unit;

static void on_click(void)
{
    playback_status_t st;
    playback_status(&st);
    if (!strcmp(st.source, "none")) {
        // Nothing to resume: Home Assistant decides what to start.
        ESP_LOGI(TAG, "click while idle: play_request");
        ha_mqtt_button_event("play_request");
        return;
    }
    ESP_LOGI(TAG, "click: toggle pause (%s)", st.source);
    playback_toggle();
    ha_mqtt_button_event("toggle");
}

// Next track exists only for Music Assistant: a radio has no next track.
static void on_double_click(void)
{
    if (!source_sendspin_session_active()) {
        ESP_LOGI(TAG, "double click ignored (no Music Assistant session)");
        return;
    }
    ESP_LOGI(TAG, "double click: next track");
    source_sendspin_command(SENDSPIN_CMD_NEXT);
    ha_mqtt_button_event("next");
}

// Previous track: Music Assistant only, like next.
static void on_triple_click(void)
{
    if (!source_sendspin_session_active()) {
        ESP_LOGI(TAG, "triple click ignored (no Music Assistant session)");
        return;
    }
    ESP_LOGI(TAG, "triple click: previous track");
    source_sendspin_command(SENDSPIN_CMD_PREVIOUS);
    ha_mqtt_button_event("previous");
}

// Long press: nothing happens on the speaker, Home Assistant decides (it
// opens Bluetooth pairing on the bridge). Fired whatever plays.
static void on_long_press(void)
{
    ESP_LOGI(TAG, "long press");
    ha_mqtt_button_event("long_press");
    // Confirmation beep only when the output is free: the tone is a source of
    // its own and cannot be mixed into a stream, playing or paused. This
    // blocks the poll for the beep; the pulse counter keeps counting.
    if (audio_arbiter_active() == AUDIO_SOURCE_NONE) audio_test_tone(ENC_BEEP_HZ, ENC_BEEP_MS, 0);
}

static void encoder_task(void *arg)
{
    buttons_state_t btn = { 0 };
    enc_click_t click = { 0 };
    int prev = 0, residue = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(ENC_POLL_MS));

        int cur = 0;
        if (pcnt_unit_get_count(s_unit, &cur) == ESP_OK) {
            int detents = enc_detents(&residue, enc_delta(prev, cur, ENC_COUNT_LIMIT));
            prev = cur;
            if (detents) audio_set_volume(audio_get_volume() + detents * ENC_VOLUME_STEP);
        }

        bool pressed = gpio_get_level(BOARD_ENC_SW_GPIO) == 0;  // switch to GND, active low
        uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        bool press = buttons_update(&btn, pressed, now, false) == BUTTONS_EV_PRESS;
        switch (enc_click(&click, press, btn.stable, now)) {
        case ENC_CLICK_SINGLE: on_click(); break;
        case ENC_CLICK_DOUBLE: on_double_click(); break;
        case ENC_CLICK_TRIPLE: on_triple_click(); break;
        case ENC_CLICK_LONG: on_long_press(); break;
        default: break;
        }
    }
}

esp_err_t encoder_init(void)
{
    pcnt_unit_config_t unit_cfg = { .high_limit = ENC_COUNT_LIMIT, .low_limit = -ENC_COUNT_LIMIT };
    ESP_RETURN_ON_ERROR(pcnt_new_unit(&unit_cfg, &s_unit), TAG, "pcnt unit");
    pcnt_glitch_filter_config_t filter = { .max_glitch_ns = ENC_GLITCH_NS };
    ESP_RETURN_ON_ERROR(pcnt_unit_set_glitch_filter(s_unit, &filter), TAG, "glitch filter");

    pcnt_chan_config_t a_cfg = { .edge_gpio_num = BOARD_ENC_CLK_GPIO, .level_gpio_num = BOARD_ENC_DT_GPIO };
    pcnt_chan_config_t b_cfg = { .edge_gpio_num = BOARD_ENC_DT_GPIO, .level_gpio_num = BOARD_ENC_CLK_GPIO };
    pcnt_channel_handle_t a, b;
    ESP_RETURN_ON_ERROR(pcnt_new_channel(s_unit, &a_cfg, &a), TAG, "pcnt channel a");
    ESP_RETURN_ON_ERROR(pcnt_new_channel(s_unit, &b_cfg, &b), TAG, "pcnt channel b");
    pcnt_channel_set_edge_action(a, PCNT_CHANNEL_EDGE_ACTION_DECREASE, PCNT_CHANNEL_EDGE_ACTION_INCREASE);
    pcnt_channel_set_level_action(a, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE);
    pcnt_channel_set_edge_action(b, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_DECREASE);
    pcnt_channel_set_level_action(b, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE);

    // The KY-040 has its own pull-ups on CLK and DT only when its "+" pin is
    // wired, and none on SW: the internal ones make all three pins safe.
    gpio_pullup_en(BOARD_ENC_CLK_GPIO);
    gpio_pullup_en(BOARD_ENC_DT_GPIO);
    gpio_config_t sw = {
        .pin_bit_mask = 1ULL << BOARD_ENC_SW_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&sw), TAG, "switch gpio");

    ESP_RETURN_ON_ERROR(pcnt_unit_enable(s_unit), TAG, "pcnt enable");
    ESP_RETURN_ON_ERROR(pcnt_unit_clear_count(s_unit), TAG, "pcnt clear");
    ESP_RETURN_ON_ERROR(pcnt_unit_start(s_unit), TAG, "pcnt start");

    if (xTaskCreate(encoder_task, "encoder", 4096, NULL, 5, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "encoder on CLK=%d DT=%d SW=%d", BOARD_ENC_CLK_GPIO, BOARD_ENC_DT_GPIO, BOARD_ENC_SW_GPIO);
    return ESP_OK;
}
