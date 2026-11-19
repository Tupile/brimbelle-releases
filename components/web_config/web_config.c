// web_config: the configuration web page and its REST endpoints.
//
// Serves an embedded page (no CDN), reads and writes config.json through
// config_store, and accepts Wi-Fi credentials during provisioning. A 404
// handler redirects unknown paths to the portal root, which together with the
// DNS hijack in net drives the captive portal. Reduced from Bugne (see
// UPSTREAM.md): no SD, podcast, library, memo, screen or stats routes.
#include "web_config.h"
#include "config_store.h"
#include "board.h"
#include "net.h"
#include "logstore.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdatomic.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_core_dump.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "cJSON.h"
#include "audio.h"
#include "playback.h"
#include "ha_mqtt.h"
#include "mbedtls/base64.h"

static const char *TAG = "web_config";

#define WEB_MAX_BODY      32768  // bound on config.json uploads, must stay >= CONFIG_FILE_MAX
#define WEB_MAX_WIFI_BODY 512    // bound on the small wifi credential post
#define SESSION_TOKEN_LEN 32     // hex chars
#define COOKIE_NAME       "brimbelle_session"
#define AUTH_FAIL_DELAY_MS 1000  // slows password guessing (login and Basic auth)

static httpd_handle_t s_server;
static char s_session[SESSION_TOKEN_LEN + 1];
static bool s_has_session;

// Cache of the last GitHub release check, read by GET /api/ghota/status.
// RAM only, no NVS: a reboot just re-checks later.
static portMUX_TYPE s_gh_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_gh_latest[32];
static bool s_gh_update;
static bool s_gh_checked;
static int64_t s_gh_check_time_us;

static void gh_cache_store(const char *latest, bool update)
{
    portENTER_CRITICAL(&s_gh_lock);
    strlcpy(s_gh_latest, latest, sizeof(s_gh_latest));
    s_gh_update = update;
    s_gh_checked = true;
    s_gh_check_time_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_gh_lock);
}

static void gh_cache_read(char *latest, size_t cap, bool *update, bool *checked,
                          long long *checked_age_s)
{
    portENTER_CRITICAL(&s_gh_lock);
    strlcpy(latest, s_gh_latest, cap);
    *update = s_gh_update;
    *checked = s_gh_checked;
    *checked_age_s = *checked ? (long long)((esp_timer_get_time() - s_gh_check_time_us) / 1000000) : -1;
    portEXIT_CRITICAL(&s_gh_lock);
}

// The config page and login HTML live in www/*.html and are embedded via
// EMBED_FILES (see CMakeLists.txt). EMBED_FILES blobs are NOT null-terminated,
// so root_get sends them with an explicit length (end - start).
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");
extern const uint8_t login_html_start[] asm("_binary_login_html_start");
extern const uint8_t login_html_end[]   asm("_binary_login_html_end");

// Generate a fresh random session token (hex).
static void new_session(void)
{
    uint8_t raw[SESSION_TOKEN_LEN / 2];
    esp_fill_random(raw, sizeof(raw));
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(raw); i++) {
        s_session[i * 2]     = hex[raw[i] >> 4];
        s_session[i * 2 + 1] = hex[raw[i] & 0x0F];
    }
    s_session[SESSION_TOKEN_LEN] = '\0';
    s_has_session = true;
}

static bool cookie_authed(httpd_req_t *req)
{
    if (!s_has_session) {
        return false;
    }
    char cookie[160];
    if (httpd_req_get_hdr_value_str(req, "Cookie", cookie, sizeof(cookie)) != ESP_OK) {
        return false;
    }
    char *t = strstr(cookie, COOKIE_NAME "=");
    if (!t) {
        return false;
    }
    t += strlen(COOKIE_NAME "=");
    return strncmp(t, s_session, SESSION_TOKEN_LEN) == 0;
}

// Stateless fallback for API clients that cannot keep a session cookie: HTTP
// Basic auth, checked on every request, no cookie set. The username is
// ignored; only the password after the first ':' matters.
static bool basic_authed(httpd_req_t *req)
{
    char hdr[128];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK) {
        return false;
    }
    static const char prefix[] = "Basic ";
    size_t plen = strlen(prefix);
    if (strncmp(hdr, prefix, plen) != 0) {
        return false;
    }
    unsigned char creds[96];
    size_t out_len = 0;
    if (mbedtls_base64_decode(creds, sizeof(creds) - 1, &out_len,
                              (const unsigned char *)hdr + plen, strlen(hdr) - plen) != 0) {
        return false;
    }
    creds[out_len] = '\0';
    char *colon = strchr((char *)creds, ':');
    if (!colon) {
        return false;
    }
    if (config_store_check_password(colon + 1) == ESP_OK) {
        return true;
    }
    vTaskDelay(pdMS_TO_TICKS(AUTH_FAIL_DELAY_MS));
    return false;
}

// A request is authorized if no password is set yet (first-run setup), it
// carries the current session cookie, or it presents valid HTTP Basic auth.
static bool is_authed(httpd_req_t *req)
{
    if (!config_store_has_password()) {
        return true;
    }
    return cookie_authed(req) || basic_authed(req);
}

// The auth gate every handler opens with. rv is what the handler returns after
// the 401 has been sent: ESP_OK when there is no unread request body, ESP_FAIL
// for a POST whose body was not consumed (ESP_FAIL closes the socket, so
// leftover body bytes cannot be parsed as the next request).
#define REQUIRE_AUTH(req, rv) do {                                             \
    if (!is_authed(req)) {                                                     \
        httpd_resp_send_err((req), HTTPD_401_UNAUTHORIZED, "login required");   \
        return (rv);                                                           \
    }                                                                          \
} while (0)

static esp_err_t read_body(httpd_req_t *req, char *buf, size_t size, size_t *out_len)
{
    if (req->content_len >= size) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t off = 0;
    while (off < req->content_len) {
        int r = httpd_req_recv(req, buf + off, req->content_len - off);
        if (r <= 0) {
            return ESP_FAIL;
        }
        off += (size_t)r;
    }
    buf[off] = '\0';
    if (out_len) *out_len = off;
    return ESP_OK;
}

static esp_err_t root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    if (!is_authed(req)) {
        return httpd_resp_send(req, (const char *)login_html_start,
                               login_html_end - login_html_start);
    }
    return httpd_resp_send(req, (const char *)index_html_start,
                           index_html_end - index_html_start);
}

// Set the session cookie on the response. cookie_buf must outlive the send.
static void set_session_cookie(httpd_req_t *req, char *cookie_buf, size_t size)
{
    snprintf(cookie_buf, size, COOKIE_NAME "=%s; Path=/; HttpOnly; SameSite=Strict", s_session);
    httpd_resp_set_hdr(req, "Set-Cookie", cookie_buf);
}

static esp_err_t login_post(httpd_req_t *req)
{
    char body[WEB_MAX_WIFI_BODY];
    if (read_body(req, body, sizeof(body), NULL) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_FAIL;
    }
    cJSON *root = cJSON_Parse(body);
    const cJSON *pass = root ? cJSON_GetObjectItemCaseSensitive(root, "pass") : NULL;
    esp_err_t ok = (cJSON_IsString(pass) && pass->valuestring)
                   ? config_store_check_password(pass->valuestring) : ESP_FAIL;
    cJSON_Delete(root);
    if (ok != ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(AUTH_FAIL_DELAY_MS));
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "wrong password");
        return ESP_FAIL;
    }
    new_session();
    char cookie[96];
    set_session_cookie(req, cookie, sizeof(cookie));
    return httpd_resp_sendstr(req, "ok");
}

static esp_err_t password_post(httpd_req_t *req)
{
    // Changing an existing password requires an active session. Setting the
    // first password is open (first-run, behind the WPA2 AP).
    if (config_store_has_password() && !is_authed(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "login required");
        return ESP_FAIL;
    }
    char body[WEB_MAX_WIFI_BODY];
    if (read_body(req, body, sizeof(body), NULL) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_FAIL;
    }
    cJSON *root = cJSON_Parse(body);
    const cJSON *pass = root ? cJSON_GetObjectItemCaseSensitive(root, "pass") : NULL;
    const char *plain = (cJSON_IsString(pass) && pass->valuestring) ? pass->valuestring : NULL;
    if (plain && plain[0] == '\0') {
        // Empty password removes the password: the page opens without login.
        esp_err_t cerr = config_store_clear_password();
        cJSON_Delete(root);
        if (cerr != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "clear failed");
            return ESP_FAIL;
        }
        return httpd_resp_sendstr(req, "password removed");
    }
    bool valid = plain && strlen(plain) >= 4;
    esp_err_t err = valid ? config_store_set_password(plain) : ESP_ERR_INVALID_ARG;
    cJSON_Delete(root);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "password too short or save failed");
        return ESP_FAIL;
    }
    // Keep the caller logged in after setting the password.
    new_session();
    char cookie[96];
    set_session_cookie(req, cookie, sizeof(cookie));
    return httpd_resp_sendstr(req, "password set");
}

static esp_err_t scan_get(httpd_req_t *req)
{
    REQUIRE_AUTH(req, ESP_FAIL);
    net_ap_t aps[24];
    size_t n = 0;
    if (net_scan(aps, sizeof(aps) / sizeof(aps[0]), &n) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "scan failed");
        return ESP_FAIL;
    }
    cJSON *arr = cJSON_CreateArray();
    if (!arr) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_FAIL;
    }
    for (size_t i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", aps[i].ssid);
        cJSON_AddNumberToObject(o, "rssi", aps[i].rssi);
        cJSON_AddBoolToObject(o, "secure", aps[i].secure);
        cJSON_AddStringToObject(o, "bssid", aps[i].bssid);
        cJSON_AddNumberToObject(o, "channel", aps[i].channel);
        cJSON_AddItemToArray(arr, o);
    }
    char *txt = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!txt) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, txt);
    cJSON_free(txt);
    return ESP_OK;
}

static esp_err_t config_get(httpd_req_t *req)
{
    REQUIRE_AUTH(req, ESP_FAIL);
    char *buf = heap_caps_malloc(WEB_MAX_BODY, MALLOC_CAP_SPIRAM);  // keep it out of internal RAM
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_FAIL;
    }
    size_t len = 0;
    esp_err_t err = config_store_read_json(buf, WEB_MAX_BODY, &len);
    if (err != ESP_OK) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no config");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, len);
    free(buf);
    return ESP_OK;
}

static esp_err_t config_post(httpd_req_t *req)
{
    REQUIRE_AUTH(req, ESP_FAIL);
    char *buf = heap_caps_malloc(WEB_MAX_BODY, MALLOC_CAP_SPIRAM);  // keep it out of internal RAM
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_FAIL;
    }
    esp_err_t err = read_body(req, buf, WEB_MAX_BODY, NULL);
    if (err == ESP_OK) {
        err = config_store_write_json(buf);
    }
    free(buf);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid config");
        return ESP_FAIL;
    }
    // Apply the new ceiling, output mode and speaker correction now, not at the next boot.
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
    return httpd_resp_sendstr(req, "config saved");
}

// ---- Crash reports (coredump partition) ----
// The panic handler writes a dump to the coredump partition (see
// sdkconfig.defaults) and these routes read its summary back: crashed task,
// PC, exception cause and backtrace. Feed the backtrace addresses to addr2line
// against the matching build to get file:line.
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
#define COREDUMP_SUPPORTED 1
// Cached: esp_core_dump_image_check reads the whole dump from flash and checks
// its CRC, while /api/status may be polled on a timer. A dump only appears
// through a panic (which reboots), so the only invalidation is our own erase.
static int s_coredump_cached = -1;  // -1 unknown, 0 none, 1 present

static bool coredump_present(void)
{
    if (s_coredump_cached < 0) {
        s_coredump_cached = (esp_core_dump_image_check() == ESP_OK) ? 1 : 0;
    }
    return s_coredump_cached == 1;
}
#else
#define COREDUMP_SUPPORTED 0
static bool coredump_present(void) { return false; }
#endif

// GET /api/coredump: {"enabled":bool,"present":bool} plus, when present,
// task/pc/cause/vaddr/backtrace/corrupted.
static esp_err_t coredump_get(httpd_req_t *req)
{
    REQUIRE_AUTH(req, ESP_OK);
    cJSON *o = cJSON_CreateObject();
    if (!o) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_OK;
    }
    cJSON_AddBoolToObject(o, "enabled", COREDUMP_SUPPORTED);
    bool present = false;
#if COREDUMP_SUPPORTED
    if (coredump_present()) {
        // Several hundred bytes: keep it off the httpd task stack.
        esp_core_dump_summary_t *s = heap_caps_malloc(sizeof(*s), MALLOC_CAP_SPIRAM);
        if (s && esp_core_dump_get_summary(s) == ESP_OK) {
            present = true;
            char hex[16], task[17];
            // exc_task is a fixed 16-byte field, not guaranteed NUL-terminated.
            snprintf(task, sizeof(task), "%.16s", s->exc_task);
            cJSON_AddStringToObject(o, "task", task);
            snprintf(hex, sizeof(hex), "0x%08x", (unsigned)s->exc_pc);
            cJSON_AddStringToObject(o, "pc", hex);
            snprintf(hex, sizeof(hex), "0x%x", (unsigned)s->ex_info.exc_cause);
            cJSON_AddStringToObject(o, "cause", hex);
            snprintf(hex, sizeof(hex), "0x%08x", (unsigned)s->ex_info.exc_vaddr);
            cJSON_AddStringToObject(o, "vaddr", hex);
            cJSON_AddBoolToObject(o, "corrupted", s->exc_bt_info.corrupted);
            cJSON *bt = cJSON_AddArrayToObject(o, "backtrace");
            uint32_t depth = s->exc_bt_info.depth;
            if (depth > 16) depth = 16;  // bt[] is a fixed 16-entry array
            for (uint32_t i = 0; bt && i < depth; i++) {
                snprintf(hex, sizeof(hex), "0x%08x", (unsigned)s->exc_bt_info.bt[i]);
                cJSON_AddItemToArray(bt, cJSON_CreateString(hex));
            }
        }
        free(s);
    }
#endif
    cJSON_AddBoolToObject(o, "present", present);
    char *txt = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!txt) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_sendstr(req, txt);
    cJSON_free(txt);
    return r;
}

// POST /api/coredump/erase: drop the saved dump so the next crash is the one
// reported.
static esp_err_t coredump_erase_post(httpd_req_t *req)
{
    REQUIRE_AUTH(req, ESP_FAIL);
#if COREDUMP_SUPPORTED
    esp_core_dump_image_erase();
    s_coredump_cached = 0;
#endif
    return httpd_resp_sendstr(req, "ok");
}

static esp_err_t logs_get(httpd_req_t *req)
{
    REQUIRE_AUTH(req, ESP_FAIL);
    char *buf = heap_caps_malloc(LOGSTORE_SIZE + 1, MALLOC_CAP_SPIRAM);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_FAIL;
    }
    size_t len = 0;
    logstore_read(buf, LOGSTORE_SIZE + 1, &len);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, buf, len);
    free(buf);
    return ESP_OK;
}

// ---- Firmware update ----
// Both the local upload and the GitHub install refuse an image whose embedded
// project name is not ours: the Bugne release channel uses the same check
// logic, and a bugne.bin flashed here would boot with the wrong pin map.
static bool image_is_ours(const esp_app_desc_t *img)
{
    return strncmp(img->project_name, esp_app_get_description()->project_name,
                   sizeof(img->project_name)) == 0;
}

// One OTA writer at a time. Taken for the whole write; released only on
// failure (a success reboots, and must not let a second OTA start meanwhile).
static atomic_flag s_ota_busy = ATOMIC_FLAG_INIT;

static esp_err_t ota_post_locked(httpd_req_t *req);

// Upload stalls: tolerate this many consecutive 5 s receive timeouts per chunk.
#define OTA_RECV_TIMEOUTS_MAX 6

// POST /api/ota: the body is the raw firmware image, streamed into the
// inactive OTA partition, then booted.
static esp_err_t ota_post(httpd_req_t *req)
{
    REQUIRE_AUTH(req, ESP_FAIL);
    if (atomic_flag_test_and_set(&s_ota_busy)) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "update already running");
        return ESP_FAIL;
    }
    esp_err_t err = ota_post_locked(req);  // reboots on success
    atomic_flag_clear(&s_ota_busy);
    return err;
}

// Stop playback before touching flash (OTA, reboot): esp_ota_begin/write
// disable the flash cache, and a non-IRAM ISR from active I2S playback firing
// in that window crashes (Bugne #32). Waits up to ~3 s for the sources to
// unwind.
static void stop_playback_and_wait(void)
{
    playback_stop();
    for (int i = 0; i < 30 && playback_active(); i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    vTaskDelay(pdMS_TO_TICKS(200));  // let the worker finish tearing the source down
}

static esp_err_t ota_post_locked(httpd_req_t *req)
{
    stop_playback_and_wait();
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no OTA partition");
        return ESP_FAIL;
    }
    // content_len is unsigned: a chunked request (0) or an image bigger than
    // the slot is rejected here.
    if (req->content_len == 0 || req->content_len > part->size) {
        httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "bad firmware size");
        return ESP_FAIL;
    }
    esp_ota_handle_t handle = 0;
    if (esp_ota_begin(part, OTA_SIZE_UNKNOWN, &handle) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota begin failed");
        return ESP_FAIL;
    }
    char *buf = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    if (!buf) {
        esp_ota_abort(handle);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_FAIL;
    }
    int remaining = (int)req->content_len;
    esp_err_t err = ESP_OK;
    bool first = true;
    while (remaining > 0) {
        int want = remaining < 4096 ? remaining : 4096;
        int got = 0, timeouts = 0;
        while (got < want) {  // fill the chunk so the descriptor check below sees it whole
            int r = httpd_req_recv(req, buf + got, want - got);
            if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= OTA_RECV_TIMEOUTS_MAX) continue;  // Wi-Fi stall
            if (r <= 0) { err = ESP_FAIL; break; }
            got += r;
        }
        if (err != ESP_OK) break;
        if (first) {
            // The app descriptor follows the image header and the first
            // segment header (24 + 8 bytes), inside this first chunk.
            const esp_app_desc_t *d = (const esp_app_desc_t *)(buf + sizeof(esp_image_header_t) +
                                                               sizeof(esp_image_segment_header_t));
            if (got < (int)(sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(*d)) ||
                d->magic_word != ESP_APP_DESC_MAGIC_WORD || !image_is_ours(d)) {
                ESP_LOGW(TAG, "OTA upload refused: not a %s image",
                         esp_app_get_description()->project_name);
                err = ESP_ERR_INVALID_ARG;
                break;
            }
            first = false;
        }
        err = esp_ota_write(handle, buf, got);
        if (err != ESP_OK) {
            break;
        }
        remaining -= got;
    }
    free(buf);
    if (err != ESP_OK) {
        esp_ota_abort(handle);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            err == ESP_ERR_INVALID_ARG ? "not a Brimbelle image" : "ota write failed");
        return ESP_FAIL;
    }
    if (esp_ota_end(handle) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid firmware image");
        return ESP_FAIL;
    }
    if (esp_ota_set_boot_partition(part) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "set boot failed");
        return ESP_FAIL;
    }
    httpd_resp_sendstr(req, "update ok, rebooting");
    ESP_LOGI(TAG, "OTA written to %s, rebooting", part->label);
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

// Firmware update from the latest GitHub release. The stable
// /releases/latest/download/<asset> URL always redirects to the newest
// release's asset, so the device needs no GitHub API and no JSON parsing.
// Brimbelle has its own release repo: NEVER point this at bugne-releases.
#define GH_OTA_URL "https://github.com/Tupile/brimbelle-releases/releases/latest/download/brimbelle.bin"

static esp_err_t gh_ota_begin(esp_https_ota_handle_t *out)
{
    esp_http_client_config_t http = {
        .url = GH_OTA_URL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,  // the signed asset redirect URL is long
    };
    esp_https_ota_config_t cfg = { .http_config = &http };
    return esp_https_ota_begin(&cfg, out);
}

// Check: read only the new image's app descriptor (first kilobytes), compare
// its embedded version with the running one, then abort the transfer. An
// INEQUALITY means "update available" (a downgrade release is deliberate).
// Returns ESP_ERR_NOT_FOUND when no release is reachable, ESP_FAIL when the
// header read failed or the image is not ours, ESP_OK otherwise.
static esp_err_t gh_check(char *latest, size_t cap, bool *update)
{
    const esp_app_desc_t *cur = esp_app_get_description();
    esp_https_ota_handle_t h = NULL;
    if (gh_ota_begin(&h) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    esp_app_desc_t img;
    esp_err_t err = esp_https_ota_get_img_desc(h, &img);
    esp_https_ota_abort(h);
    if (err != ESP_OK || !image_is_ours(&img)) {
        return ESP_FAIL;
    }
    bool upd = strcmp(cur->version, img.version) != 0;
    if (latest && cap) {
        strlcpy(latest, img.version, cap);
    }
    if (update) {
        *update = upd;
    }
    gh_cache_store(img.version, upd);
    return ESP_OK;
}

// Stream the latest release into the inactive slot and validate it. Does NOT
// reboot: the handler answers first. Blocking, up to a minute, on the httpd
// task (internal stack: flash writes).
static esp_err_t gh_install(void)
{
    if (atomic_flag_test_and_set(&s_ota_busy)) return ESP_ERR_NOT_FINISHED;
    esp_https_ota_handle_t h = NULL;
    esp_err_t err = gh_ota_begin(&h);
    if (err != ESP_OK) {
        atomic_flag_clear(&s_ota_busy);
        return ESP_ERR_NOT_FOUND;
    }
    esp_app_desc_t img;
    if (esp_https_ota_get_img_desc(h, &img) != ESP_OK || !image_is_ours(&img)) {
        esp_https_ota_abort(h);
        atomic_flag_clear(&s_ota_busy);
        ESP_LOGW(TAG, "GitHub OTA refused: not a %s image", esp_app_get_description()->project_name);
        return ESP_ERR_INVALID_STATE;
    }
    while ((err = esp_https_ota_perform(h)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        // Streaming the image into the inactive slot; nothing else to do here.
    }
    if (err != ESP_OK || !esp_https_ota_is_complete_data_received(h)) {
        esp_https_ota_abort(h);
        atomic_flag_clear(&s_ota_busy);
        return ESP_FAIL;
    }
    if (esp_https_ota_finish(h) != ESP_OK) {  // validates the image, sets boot
        atomic_flag_clear(&s_ota_busy);
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "GitHub OTA installed");
    return ESP_OK;  // s_ota_busy stays set: the caller reboots
}

static esp_err_t gh_check_get(httpd_req_t *req)
{
    REQUIRE_AUTH(req, ESP_FAIL);
    const esp_app_desc_t *cur = esp_app_get_description();
    char resp[176];
    httpd_resp_set_type(req, "application/json");
    char latest[32];
    bool update = false;
    esp_err_t err = gh_check(latest, sizeof(latest), &update);
    if (err == ESP_ERR_NOT_FOUND) {
        snprintf(resp, sizeof(resp), "{\"current\":\"%s\",\"error\":\"no release\"}", cur->version);
        return httpd_resp_sendstr(req, resp);
    }
    if (err != ESP_OK) {
        snprintf(resp, sizeof(resp), "{\"current\":\"%s\",\"error\":\"check failed\"}", cur->version);
        return httpd_resp_sendstr(req, resp);
    }
    snprintf(resp, sizeof(resp), "{\"current\":\"%s\",\"latest\":\"%s\",\"update\":%s}",
             cur->version, latest, update ? "true" : "false");
    return httpd_resp_sendstr(req, resp);
}

// Cached status for the web page badge. age_s is -1 when no check has run
// since boot (RAM-only cache).
static esp_err_t ghota_status_get(httpd_req_t *req)
{
    REQUIRE_AUTH(req, ESP_FAIL);
    const esp_app_desc_t *cur = esp_app_get_description();
    char latest[32];
    bool update = false, checked = false;
    long long age_s = -1;
    gh_cache_read(latest, sizeof(latest), &update, &checked, &age_s);
    char resp[224];
    httpd_resp_set_type(req, "application/json");
    snprintf(resp, sizeof(resp),
             "{\"current\":\"%s\",\"latest\":\"%s\",\"update\":%s,\"age_s\":%lld}",
             cur->version, checked ? latest : "", update ? "true" : "false", age_s);
    return httpd_resp_sendstr(req, resp);
}

// POST /api/ghota: install the latest GitHub release, then reboot.
static esp_err_t gh_ota_post(httpd_req_t *req)
{
    REQUIRE_AUTH(req, ESP_FAIL);
    stop_playback_and_wait();
    esp_err_t err = gh_install();
    if (err == ESP_ERR_NOT_FINISHED) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "update already running");
        return ESP_FAIL;
    }
    if (err == ESP_ERR_NOT_FOUND) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no release");
        return ESP_FAIL;
    }
    if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid firmware image");
        return ESP_FAIL;
    }
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "download failed");
        return ESP_FAIL;
    }
    httpd_resp_sendstr(req, "update ok, rebooting");
    ESP_LOGI(TAG, "GitHub OTA installed, rebooting");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

// GET /api/wifi: the saved networks' SSIDs only, never the passwords.
// {"networks":["Home","Office", ...]}.
static esp_err_t wifi_get(httpd_req_t *req)
{
    REQUIRE_AUTH(req, ESP_FAIL);
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = root ? cJSON_AddArrayToObject(root, "networks") : NULL;
    if (!arr) { cJSON_Delete(root); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_FAIL; }
    for (int s = 0; s < CFG_WIFI_SLOTS; s++) {
        char ss[CFG_WIFI_SSID_MAX], pp[CFG_WIFI_PASS_MAX];
        if (config_store_get_wifi_slot(s, ss, sizeof(ss), pp, sizeof(pp)) == ESP_OK && ss[0])
            cJSON_AddItemToArray(arr, cJSON_CreateString(ss));
    }
    char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, txt ? txt : "{\"networks\":[]}");
    cJSON_free(txt);
    return ESP_OK;
}

// POST /api/wifi: {"networks":[{"ssid":"..","pass":".."}, ...]}. Stores up to
// CFG_WIFI_SLOTS networks and clears the rest. A blank pass keeps the password
// already stored for that SSID (the UI never receives passwords). Reboots to
// apply.
static esp_err_t wifi_post(httpd_req_t *req)
{
    REQUIRE_AUTH(req, ESP_FAIL);
    char *buf = malloc(4096);  // up to 16 networks; off the small httpd stack
    if (!buf) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_FAIL; }
    esp_err_t rb = read_body(req, buf, 4096, NULL);
    cJSON *root = (rb == ESP_OK) ? cJSON_Parse(buf) : NULL;
    free(buf);
    cJSON *nets = root ? cJSON_GetObjectItemCaseSensitive(root, "networks") : NULL;
    if (!cJSON_IsArray(nets)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing networks");
        return ESP_FAIL;
    }

    // Snapshot the currently-stored SSID->password pairs so a blank password
    // keeps the existing one. Static to stay off the handler stack.
    static char ex_ssid[CFG_WIFI_SLOTS][CFG_WIFI_SSID_MAX];
    static char ex_pass[CFG_WIFI_SLOTS][CFG_WIFI_PASS_MAX];
    int ex_n = 0;
    for (int s = 0; s < CFG_WIFI_SLOTS; s++) {
        char ss[CFG_WIFI_SSID_MAX], pp[CFG_WIFI_PASS_MAX];
        if (config_store_get_wifi_slot(s, ss, sizeof(ss), pp, sizeof(pp)) == ESP_OK && ss[0]) {
            strlcpy(ex_ssid[ex_n], ss, CFG_WIFI_SSID_MAX);
            strlcpy(ex_pass[ex_n], pp, CFG_WIFI_PASS_MAX);
            ex_n++;
        }
    }

    // Validate every row before writing any slot: the boot-time read uses
    // fixed buffers and FAILS on an oversized value, so a too-long SSID or
    // password would be "saved" and then silently vanish at the reboot below.
    int count = cJSON_GetArraySize(nets);
    for (int i = 0; i < count; i++) {
        const cJSON *n = cJSON_GetArrayItem(nets, i);
        const cJSON *js = cJSON_GetObjectItemCaseSensitive(n, "ssid");
        const cJSON *jp = cJSON_GetObjectItemCaseSensitive(n, "pass");
        if ((cJSON_IsString(js) && js->valuestring && strlen(js->valuestring) >= CFG_WIFI_SSID_MAX) ||
            (cJSON_IsString(jp) && jp->valuestring && strlen(jp->valuestring) >= CFG_WIFI_PASS_MAX)) {
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                "SSID max 32 bytes, password max 63 characters");
            return ESP_OK;
        }
    }
    esp_err_t err = ESP_OK;
    int slot = 0;
    for (int i = 0; i < count && slot < CFG_WIFI_SLOTS && err == ESP_OK; i++) {
        const cJSON *n = cJSON_GetArrayItem(nets, i);
        const cJSON *js = cJSON_GetObjectItemCaseSensitive(n, "ssid");
        const cJSON *jp = cJSON_GetObjectItemCaseSensitive(n, "pass");
        if (!cJSON_IsString(js) || !js->valuestring || !js->valuestring[0]) continue;  // skip blank rows
        const char *ssid = js->valuestring;
        const char *pass = (cJSON_IsString(jp) && jp->valuestring) ? jp->valuestring : "";
        if (pass[0] == '\0') {
            for (int e = 0; e < ex_n; e++)
                if (strcmp(ex_ssid[e], ssid) == 0) { pass = ex_pass[e]; break; }
        }
        err = config_store_set_wifi_slot(slot++, ssid, pass);
    }
    for (; slot < CFG_WIFI_SLOTS && err == ESP_OK; slot++)
        err = config_store_set_wifi_slot(slot, "", "");  // clear unused slots
    cJSON_Delete(root);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
        return ESP_FAIL;
    }
    httpd_resp_sendstr(req, "saved, rebooting");
    ESP_LOGI(TAG, "wifi networks saved, rebooting to apply");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

// Redirect any unknown path to the portal root. With the DNS hijack this is
// what makes phones and laptops pop the captive portal. Outside provisioning
// the AP address is unreachable, so an unknown path is a plain 404.
static esp_err_t captive_redirect(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    if (net_state() != NET_STATE_PROVISIONING) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        return ESP_FAIL;  // close: a POST body may be unread
    }
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// GET /api/mqtt: the broker url and user, never the password.
static esp_err_t mqtt_get(httpd_req_t *req)
{
    REQUIRE_AUTH(req, ESP_OK);
    config_mqtt_t m;
    config_store_get_mqtt(&m);
    cJSON *o = cJSON_CreateObject();
    if (!o) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_OK;
    }
    cJSON_AddStringToObject(o, "url", m.url);
    cJSON_AddStringToObject(o, "user", m.user);
    char *txt = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, txt ? txt : "{}");
    cJSON_free(txt);
    return ESP_OK;
}

// POST /api/mqtt: {"url":"mqtt://host:1883","user":"..","pass":".."}. A blank
// pass keeps the stored password while the user is unchanged. An empty url
// turns MQTT off. Applied at once, no reboot.
static esp_err_t mqtt_post(httpd_req_t *req)
{
    REQUIRE_AUTH(req, ESP_FAIL);
    char body[512];
    if (read_body(req, body, sizeof(body), NULL) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_FAIL;
    }
    cJSON *j = cJSON_Parse(body);
    const cJSON *url = cJSON_GetObjectItemCaseSensitive(j, "url");
    const cJSON *user = cJSON_GetObjectItemCaseSensitive(j, "user");
    const cJSON *pass = cJSON_GetObjectItemCaseSensitive(j, "pass");
    config_mqtt_t old, m = { 0 };
    config_store_get_mqtt(&old);
    bool ok = cJSON_IsString(url) && strlen(url->valuestring) < sizeof(m.url)
           && (!url->valuestring[0] || !strncmp(url->valuestring, "mqtt://", 7))
           && (!user || (cJSON_IsString(user) && strlen(user->valuestring) < sizeof(m.user)))
           && (!pass || (cJSON_IsString(pass) && strlen(pass->valuestring) < sizeof(m.pass)));
    if (ok) {
        strlcpy(m.url, url->valuestring, sizeof(m.url));
        if (user) strlcpy(m.user, user->valuestring, sizeof(m.user));
        if (pass && pass->valuestring[0]) strlcpy(m.pass, pass->valuestring, sizeof(m.pass));
        else if (!strcmp(m.user, old.user)) strlcpy(m.pass, old.pass, sizeof(m.pass));
    }
    cJSON_Delete(j);
    if (!ok) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "url must be empty or start with mqtt://");
        return ESP_OK;
    }
    if (config_store_set_mqtt(&m) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
        return ESP_OK;
    }
    if (ha_mqtt_restart() != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "saved, but the client did not start");
        return ESP_OK;
    }
    return httpd_resp_sendstr(req, m.url[0] ? "saved, connecting" : "saved, MQTT off");
}

// GET /api/status: a compact device snapshot.
static esp_err_t status_get(httpd_req_t *req)
{
    REQUIRE_AUTH(req, ESP_OK);
    const config_t *c = config_store_get();
    wifi_ap_record_t ap;
    int rssi = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0;
    char ip[16];
    if (!net_ip(ip, sizeof(ip))) {
        ip[0] = '\0';
    }

    cJSON *o = cJSON_CreateObject();
    if (!o) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_OK;
    }
    cJSON_AddStringToObject(o, "id", board_device_id());
    cJSON_AddStringToObject(o, "name", (c && c->device_name[0]) ? c->device_name : "");
    cJSON_AddStringToObject(o, "version", esp_app_get_description()->version);
    cJSON_AddNumberToObject(o, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(o, "heap_free", (double)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(o, "heap_min_free", (double)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(o, "psram_free", (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddStringToObject(o, "reset_reason", board_reset_reason_name());
    cJSON_AddBoolToObject(o, "coredump", coredump_present());
    cJSON_AddNumberToObject(o, "rssi", rssi);
    cJSON_AddStringToObject(o, "ip", ip);
    cJSON_AddBoolToObject(o, "mqtt_connected", ha_mqtt_connected());
    char *txt = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!txt) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, txt);
    cJSON_free(txt);
    return ESP_OK;
}

// POST /api/test: play a test tone. Optional JSON body {"hz":440,"ms":1000,
// "volume":30,"side":"left"}; side "left" or "right" plays on that channel
// only; a volume sets the live volume (not persisted) before playing,
// which is how the output ceiling is calibrated on the bench. Blocks for ms.
static esp_err_t test_post(httpd_req_t *req)
{
    REQUIRE_AUTH(req, ESP_FAIL);  // body not read yet
    int hz = 440, ms = 1000, side = 0;
    char body[128] = {0};
    if (req->content_len > 0 && read_body(req, body, sizeof(body), NULL) == ESP_OK) {
        cJSON *j = cJSON_Parse(body);
        if (j) {
            cJSON *v;
            if (cJSON_IsNumber(v = cJSON_GetObjectItem(j, "hz"))) hz = v->valueint;
            if (cJSON_IsNumber(v = cJSON_GetObjectItem(j, "ms"))) ms = v->valueint;
            if (cJSON_IsNumber(v = cJSON_GetObjectItem(j, "volume"))) audio_set_volume(v->valueint);
            if (cJSON_IsString(v = cJSON_GetObjectItem(j, "side"))) {
                side = strcmp(v->valuestring, "left") == 0 ? 1 : strcmp(v->valuestring, "right") == 0 ? 2 : 0;
            }
            cJSON_Delete(j);
        }
    }
    esp_err_t err = audio_test_tone(hz, ms, side);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, esp_err_to_name(err));
        return ESP_FAIL;
    }
    char reply[48];
    snprintf(reply, sizeof(reply), "played at volume %d", audio_get_volume());
    httpd_resp_sendstr(req, reply);
    return ESP_OK;
}

// GET /api/playback: what the device is playing.
static esp_err_t playback_get(httpd_req_t *req)
{
    REQUIRE_AUTH(req, ESP_OK);
    playback_status_t st;
    playback_status(&st);
    cJSON *o = cJSON_CreateObject();
    if (!o) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_OK;
    }
    cJSON_AddBoolToObject(o, "active", st.active);
    cJSON_AddBoolToObject(o, "paused", st.paused);
    cJSON_AddBoolToObject(o, "retrying", st.retrying);
    cJSON_AddStringToObject(o, "source", st.source);
    cJSON_AddNumberToObject(o, "radio_id", st.radio_id);
    cJSON_AddStringToObject(o, "title", st.title);
    cJSON_AddStringToObject(o, "artist", st.artist);
    cJSON_AddNumberToObject(o, "volume", audio_get_volume());
    char *txt = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!txt) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, txt);
    cJSON_free(txt);
    return ESP_OK;
}

// POST /api/playback: {"action":"radio|stop|toggle|volume","value":N}.
// "radio" plays the configured webradio with id N; "volume" sets the live
// volume 0..100 (not persisted, the ceiling still applies).
static esp_err_t playback_post(httpd_req_t *req)
{
    REQUIRE_AUTH(req, ESP_FAIL);  // body not read yet
    char body[128];
    if (read_body(req, body, sizeof(body), NULL) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_FAIL;  // body not consumed: close instead of parsing leftovers
    }
    cJSON *root = cJSON_Parse(body);
    const cJSON *action = root ? cJSON_GetObjectItemCaseSensitive(root, "action") : NULL;
    const cJSON *value = root ? cJSON_GetObjectItemCaseSensitive(root, "value") : NULL;
    int v = cJSON_IsNumber(value) ? value->valueint : 0;
    bool ok = cJSON_IsString(action) && action->valuestring;
    if (ok) {
        const char *a = action->valuestring;
        if      (!strcmp(a, "radio"))  ok = playback_play_radio(v) == ESP_OK;
        else if (!strcmp(a, "stop"))   playback_stop();
        else if (!strcmp(a, "toggle")) playback_toggle();
        else if (!strcmp(a, "volume")) ok = audio_set_volume(v) == ESP_OK;
        else ok = false;
    }
    cJSON_Delete(root);
    if (!ok) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad action");
        return ESP_OK;
    }
    return httpd_resp_sendstr(req, "ok");
}

// POST /api/reboot: stop playback, reply, then restart.
static esp_err_t reboot_post(httpd_req_t *req)
{
    REQUIRE_AUTH(req, ESP_OK);
    stop_playback_and_wait();
    httpd_resp_sendstr(req, "ok");
    ESP_LOGI(TAG, "reboot requested via /api/reboot");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

esp_err_t web_config_start(void)
{
    if (s_server) {
        return ESP_OK; // already running
    }
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    cfg.stack_size = 12288;    // the GitHub OTA handlers run a TLS handshake in
                               // this task; the 4 KB default overflows. Stays
                               // internal RAM: these handlers write flash.
    cfg.max_open_sockets = 3;  // keep lwip sockets free for streaming + Sendspin
    cfg.max_uri_handlers = 24; // registration past the limit fails silently
    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &cfg), TAG, "httpd start failed");

    const httpd_uri_t routes[] = {
        {.uri = "/",             .method = HTTP_GET,  .handler = root_get},
        {.uri = "/login",        .method = HTTP_POST, .handler = login_post},
        {.uri = "/api/password", .method = HTTP_POST, .handler = password_post},
        {.uri = "/api/scan",     .method = HTTP_GET,  .handler = scan_get},
        {.uri = "/api/wifi",     .method = HTTP_GET,  .handler = wifi_get},
        {.uri = "/api/wifi",     .method = HTTP_POST, .handler = wifi_post},
        {.uri = "/api/mqtt",     .method = HTTP_GET,  .handler = mqtt_get},
        {.uri = "/api/mqtt",     .method = HTTP_POST, .handler = mqtt_post},
        {.uri = "/api/config",   .method = HTTP_GET,  .handler = config_get},
        {.uri = "/api/config",   .method = HTTP_POST, .handler = config_post},
        {.uri = "/api/logs",     .method = HTTP_GET,  .handler = logs_get},
        {.uri = "/api/coredump", .method = HTTP_GET,  .handler = coredump_get},
        {.uri = "/api/coredump/erase", .method = HTTP_POST, .handler = coredump_erase_post},
        {.uri = "/api/ota",      .method = HTTP_POST, .handler = ota_post},
        {.uri = "/api/ghota/check",  .method = HTTP_GET,  .handler = gh_check_get},
        {.uri = "/api/ghota/status", .method = HTTP_GET,  .handler = ghota_status_get},
        {.uri = "/api/ghota",    .method = HTTP_POST, .handler = gh_ota_post},
        {.uri = "/api/status",   .method = HTTP_GET,  .handler = status_get},
        {.uri = "/api/test",     .method = HTTP_POST, .handler = test_post},
        {.uri = "/api/playback", .method = HTTP_GET,  .handler = playback_get},
        {.uri = "/api/playback", .method = HTTP_POST, .handler = playback_post},
        {.uri = "/api/reboot",   .method = HTTP_POST, .handler = reboot_post},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(s_server, &routes[i]);
    }
    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, captive_redirect);

    ESP_LOGI(TAG, "config web server up (device %s)", board_device_id());
    return ESP_OK;
}
