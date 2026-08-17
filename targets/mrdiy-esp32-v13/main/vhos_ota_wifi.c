#include "vhos_ota_wifi.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "vhos_ble.h"
#include "vhos_can.h"
#include "vhos_capture_store.h"

#define VHOS_OTA_IPV4 "192.168.4.1"
#define VHOS_OTA_UPLOAD_PATH "/api/v1/ota/image"
#define VHOS_OTA_PASSWORD_LENGTH 20U
#define VHOS_OTA_TOKEN_LENGTH 40U
#define VHOS_OTA_HTTP_STACK_BYTES 12288U
#define VHOS_OTA_NVS_NAMESPACE "vhos_ota"
#define VHOS_OTA_NVS_STATE_KEY "last_state"
#define VHOS_OTA_NVS_PACKAGE_KEY "package_id"
#define VHOS_OTA_NVS_VERSION_KEY "version"
#define VHOS_OTA_NVS_PARTITION_KEY "partition"

static const char *TAG = "vhos_ota_wifi";
static const char SECRET_ALPHABET[] =
    "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";

static httpd_handle_t server;
static esp_netif_t *ap_netif;
static esp_event_handler_instance_t wifi_event_instance;
static bool wifi_initialized;
static bool wifi_started;
static bool service_active;
static bool upload_in_progress;
static uint8_t connected_stations;
static int64_t expiration_us;
static portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;
static char gateway_id_value[40] = "esp32-uninitialized";
static char ota_ssid[33];
static char ota_password[VHOS_OTA_PASSWORD_LENGTH + 1];
static char bearer_token[VHOS_OTA_TOKEN_LENGTH + 1];
static vhos_ota_activation_request_t approved_request;
static vhos_ota_status_emit_fn emit_status_json;

static bool constant_time_equal(
    const char *left,
    size_t left_length,
    const char *right,
    size_t right_length
)
{
    size_t maximum = left_length > right_length ? left_length : right_length;
    uint8_t difference = (uint8_t)(left_length ^ right_length);
    for (size_t index = 0; index < maximum; ++index) {
        uint8_t left_byte = index < left_length ? (uint8_t)left[index] : 0;
        uint8_t right_byte = index < right_length ? (uint8_t)right[index] : 0;
        difference |= left_byte ^ right_byte;
    }
    return difference == 0;
}

static void generate_secret(char *destination, size_t length)
{
    const size_t alphabet_length = sizeof(SECRET_ALPHABET) - 1;
    const unsigned int acceptance_limit = 256U - (256U % alphabet_length);
    for (size_t index = 0; index < length; ++index) {
        uint8_t random_byte = 0;
        do {
            esp_fill_random(&random_byte, sizeof(random_byte));
        } while ((unsigned int)random_byte >= acceptance_limit);
        destination[index] = SECRET_ALPHABET[random_byte % alphabet_length];
    }
    destination[length] = '\0';
}

static bool text_field_valid(const char *value, size_t capacity)
{
    if (value == NULL) {
        return false;
    }
    size_t length = strnlen(value, capacity);
    if (length == 0 || length >= capacity) {
        return false;
    }
    for (size_t index = 0; index < length; ++index) {
        unsigned char character = (unsigned char)value[index];
        if (!isalnum(character) && character != '-' && character != '.' && character != '+') {
            return false;
        }
    }
    return true;
}

static bool sha256_field_valid(const char *value)
{
    if (value == NULL || strlen(value) != 64) {
        return false;
    }
    for (size_t index = 0; index < 64; ++index) {
        if (!isxdigit((unsigned char)value[index])) {
            return false;
        }
    }
    return true;
}

static void digest_hex(const uint8_t digest[32], char output[65])
{
    for (size_t index = 0; index < 32; ++index) {
        snprintf(&output[index * 2], 3, "%02x", digest[index]);
    }
    output[64] = '\0';
}

static esp_err_t persist_outcome(
    const char *state,
    const char *package_id,
    const char *version,
    const char *partition
)
{
    nvs_handle_t handle;
    esp_err_t result = nvs_open(VHOS_OTA_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (result != ESP_OK) {
        return result;
    }
    result = nvs_set_str(handle, VHOS_OTA_NVS_STATE_KEY, state);
    if (result == ESP_OK) {
        result = nvs_set_str(handle, VHOS_OTA_NVS_PACKAGE_KEY, package_id);
    }
    if (result == ESP_OK) {
        result = nvs_set_str(handle, VHOS_OTA_NVS_VERSION_KEY, version);
    }
    if (result == ESP_OK) {
        result = nvs_set_str(handle, VHOS_OTA_NVS_PARTITION_KEY, partition);
    }
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }
    nvs_close(handle);
    return result;
}

static bool load_outcome(
    char *state,
    size_t state_capacity,
    char *package_id,
    size_t package_capacity,
    char *version,
    size_t version_capacity,
    char *partition,
    size_t partition_capacity
)
{
    nvs_handle_t handle;
    if (nvs_open(VHOS_OTA_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    size_t state_length = state_capacity;
    size_t package_length = package_capacity;
    size_t version_length = version_capacity;
    size_t partition_length = partition_capacity;
    bool loaded = nvs_get_str(handle, VHOS_OTA_NVS_STATE_KEY, state, &state_length) == ESP_OK &&
                  nvs_get_str(handle, VHOS_OTA_NVS_PACKAGE_KEY, package_id, &package_length) == ESP_OK &&
                  nvs_get_str(handle, VHOS_OTA_NVS_VERSION_KEY, version, &version_length) == ESP_OK &&
                  nvs_get_str(handle, VHOS_OTA_NVS_PARTITION_KEY, partition, &partition_length) == ESP_OK;
    nvs_close(handle);
    return loaded;
}

static esp_err_t emit_status(
    const char *state,
    const char *detail,
    bool include_credentials
)
{
    if (emit_status_json == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    bool active;
    int64_t deadline;
    portENTER_CRITICAL(&state_lock);
    active = service_active;
    deadline = expiration_us;
    portEXIT_CRITICAL(&state_lock);
    int64_t remaining_us = deadline - esp_timer_get_time();
    uint32_t remaining_seconds = active && remaining_us > 0
        ? (uint32_t)((remaining_us + 999999) / 1000000)
        : 0;

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "contract", "gateway.ota-status");
    cJSON_AddStringToObject(root, "contract_version", "1.0.0");
    cJSON_AddStringToObject(root, "gateway_id", gateway_id_value);
    cJSON_AddStringToObject(root, "state", state);
    cJSON_AddStringToObject(root, "detail", detail);
    cJSON_AddStringToObject(root, "package_id", approved_request.package_id);
    cJSON_AddStringToObject(root, "firmware_version", approved_request.firmware_version);
    cJSON_AddBoolToObject(root, "session_active", active);
    cJSON_AddNumberToObject(root, "expires_in_seconds", remaining_seconds);
    cJSON_AddNumberToObject(root, "maximum_image_bytes", VHOS_OTA_MAX_IMAGE_BYTES);
    if (include_credentials) {
        cJSON_AddStringToObject(root, "ssid", ota_ssid);
        cJSON_AddStringToObject(root, "passphrase", ota_password);
        cJSON_AddStringToObject(root, "upload_url", "http://" VHOS_OTA_IPV4 VHOS_OTA_UPLOAD_PATH);
        cJSON_AddStringToObject(root, "bearer_token", bearer_token);
    } else {
        cJSON_AddNullToObject(root, "ssid");
        cJSON_AddNullToObject(root, "passphrase");
        cJSON_AddNullToObject(root, "upload_url");
        cJSON_AddNullToObject(root, "bearer_token");
    }
    char *encoded = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (encoded == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t result = emit_status_json(encoded);
    cJSON_free(encoded);
    return result;
}

static bool authorize(httpd_req_t *request)
{
    size_t header_length = httpd_req_get_hdr_value_len(request, "Authorization");
    if (header_length == 0 || header_length >= 96) {
        return false;
    }
    char supplied[96];
    if (httpd_req_get_hdr_value_str(request, "Authorization", supplied, sizeof(supplied)) != ESP_OK) {
        return false;
    }
    char expected[96];
    int expected_length = snprintf(expected, sizeof(expected), "Bearer %s", bearer_token);
    if (expected_length < 0 || (size_t)expected_length >= sizeof(expected)) {
        memset(supplied, 0, sizeof(supplied));
        return false;
    }
    bool accepted = constant_time_equal(
        supplied,
        strlen(supplied),
        expected,
        (size_t)expected_length
    );
    memset(supplied, 0, sizeof(supplied));
    memset(expected, 0, sizeof(expected));
    return accepted;
}

static void set_common_headers(httpd_req_t *request)
{
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(request, "Connection", "close");
}

static void wifi_event_handler(
    void *argument,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data
)
{
    (void)argument;
    (void)event_base;
    (void)event_data;
    portENTER_CRITICAL(&state_lock);
    if (event_id == WIFI_EVENT_AP_STACONNECTED && connected_stations < UINT8_MAX) {
        connected_stations++;
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED && connected_stations > 0) {
        connected_stations--;
    }
    portEXIT_CRITICAL(&state_lock);
}

static void clear_secrets(void)
{
    memset(ota_password, 0, sizeof(ota_password));
    memset(bearer_token, 0, sizeof(bearer_token));
}

static void shutdown_service(bool resume_capture)
{
    httpd_handle_t active_server;
    portENTER_CRITICAL(&state_lock);
    service_active = false;
    upload_in_progress = false;
    connected_stations = 0;
    active_server = server;
    server = NULL;
    portEXIT_CRITICAL(&state_lock);

    if (active_server != NULL) {
        httpd_stop(active_server);
    }
    if (wifi_started) {
        esp_wifi_stop();
        wifi_started = false;
    }
    if (wifi_event_instance != NULL) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_instance);
        wifi_event_instance = NULL;
    }
    if (wifi_initialized) {
        esp_wifi_deinit();
        wifi_initialized = false;
    }
    if (ap_netif != NULL) {
        esp_netif_destroy_default_wifi(ap_netif);
        ap_netif = NULL;
    }
    clear_secrets();
    if (resume_capture) {
        vhos_capture_store_set_logging(true);
    }
}

static void restart_task(void *argument)
{
    (void)argument;
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

static void failure_shutdown_task(void *argument)
{
    (void)argument;
    vTaskDelay(pdMS_TO_TICKS(750));
    shutdown_service(true);
    vTaskDelete(NULL);
}

static esp_err_t reject_upload(httpd_req_t *request, const char *status, const char *message)
{
    set_common_headers(request);
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request, message);
}

static esp_err_t reject_and_shutdown(
    httpd_req_t *request,
    const char *status,
    const char *message
)
{
    esp_err_t result = reject_upload(request, status, message);
    if (xTaskCreate(failure_shutdown_task, "vhos_ota_fail", 3072, NULL, 4, NULL) != pdPASS) {
        shutdown_service(true);
    }
    return result;
}

static esp_err_t upload_handler(httpd_req_t *request)
{
    int64_t now = esp_timer_get_time();
    bool active;
    bool already_uploading;
    int64_t deadline;
    portENTER_CRITICAL(&state_lock);
    active = service_active && expiration_us > now;
    already_uploading = upload_in_progress;
    deadline = expiration_us;
    if (active && !already_uploading) {
        upload_in_progress = true;
    }
    portEXIT_CRITICAL(&state_lock);

    if (!active || deadline <= now) {
        return reject_upload(request, "410 Gone", "{\"error\":\"ota_session_expired\"}");
    }
    if (already_uploading) {
        return reject_upload(request, "409 Conflict", "{\"error\":\"upload_in_progress\"}");
    }
    if (!authorize(request)) {
        portENTER_CRITICAL(&state_lock);
        upload_in_progress = false;
        portEXIT_CRITICAL(&state_lock);
        return reject_upload(request, "401 Unauthorized", "{\"error\":\"invalid_session_token\"}");
    }
    if (request->content_len <= 0 ||
        (uint32_t)request->content_len != approved_request.firmware_size_bytes ||
        (uint32_t)request->content_len > VHOS_OTA_MAX_IMAGE_BYTES) {
        portENTER_CRITICAL(&state_lock);
        upload_in_progress = false;
        portEXIT_CRITICAL(&state_lock);
        return reject_upload(request, "422 Unprocessable Entity", "{\"error\":\"image_size_mismatch\"}");
    }

    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (next == NULL || running == NULL || next == running ||
        (uint32_t)request->content_len > next->size) {
        emit_status("FAILED", "No valid inactive OTA partition is available.", false);
        return reject_and_shutdown(
            request,
            "507 Insufficient Storage",
            "{\"error\":\"inactive_partition_unavailable\"}"
        );
    }

    esp_ota_handle_t update_handle = 0;
    esp_err_t result = esp_ota_begin(next, (size_t)request->content_len, &update_handle);
    if (result != ESP_OK) {
        emit_status("FAILED", "The inactive OTA partition could not be opened.", false);
        return reject_and_shutdown(
            request,
            "500 Internal Server Error",
            "{\"error\":\"ota_begin_failed\"}"
        );
    }

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    uint8_t buffer[2048];
    int remaining = request->content_len;
    bool receive_failed = false;
    emit_status("UPLOADING", "Writing the verified package to the inactive partition.", false);
    while (remaining > 0) {
        if (esp_timer_get_time() >= deadline) {
            receive_failed = true;
            break;
        }
        int read = httpd_req_recv(
            request,
            (char *)buffer,
            remaining < (int)sizeof(buffer) ? remaining : (int)sizeof(buffer)
        );
        if (read == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (read <= 0 || esp_ota_write(update_handle, buffer, (size_t)read) != ESP_OK) {
            receive_failed = true;
            break;
        }
        mbedtls_sha256_update(&sha, buffer, (size_t)read);
        remaining -= read;
    }
    memset(buffer, 0, sizeof(buffer));

    uint8_t digest[32];
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);
    char actual_sha256[65];
    digest_hex(digest, actual_sha256);
    memset(digest, 0, sizeof(digest));

    if (receive_failed || remaining != 0) {
        esp_ota_abort(update_handle);
        emit_status("FAILED", "The firmware transfer was interrupted.", false);
        return reject_and_shutdown(
            request,
            "400 Bad Request",
            "{\"error\":\"transfer_interrupted\"}"
        );
    }
    if (!constant_time_equal(actual_sha256, 64, approved_request.firmware_sha256, 64)) {
        esp_ota_abort(update_handle);
        emit_status("FAILED", "The received image SHA-256 did not match the approved package.", false);
        return reject_and_shutdown(
            request,
            "422 Unprocessable Entity",
            "{\"error\":\"sha256_mismatch\"}"
        );
    }

    /* With CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT, esp_ota_end rejects
     * any image that lacks the embedded ESP-IDF ECDSA signature. */
    result = esp_ota_end(update_handle);
    if (result != ESP_OK) {
        emit_status("FAILED", "ESP-IDF rejected the application signature or image format.", false);
        return reject_and_shutdown(
            request,
            "422 Unprocessable Entity",
            "{\"error\":\"signed_image_rejected\"}"
        );
    }
    result = persist_outcome(
        "PREPARED",
        approved_request.package_id,
        approved_request.firmware_version,
        next->label
    );
    if (result != ESP_OK) {
        emit_status("FAILED", "The verified update outcome could not be persisted safely.", false);
        return reject_and_shutdown(
            request,
            "500 Internal Server Error",
            "{\"error\":\"ota_outcome_persistence_failed\"}"
        );
    }
    result = esp_ota_set_boot_partition(next);
    if (result != ESP_OK) {
        persist_outcome(
            "BOOT_SELECTION_FAILED",
            approved_request.package_id,
            approved_request.firmware_version,
            next->label
        );
        emit_status("FAILED", "The verified image could not be selected for probationary boot.", false);
        return reject_and_shutdown(
            request,
            "500 Internal Server Error",
            "{\"error\":\"boot_partition_rejected\"}"
        );
    }
    /* PREPARED is itself recoverable if this final NVS update fails or power is lost. */
    persist_outcome(
        "PENDING_REBOOT",
        approved_request.package_id,
        approved_request.firmware_version,
        next->label
    );
    emit_status("REBOOTING", "Signed image accepted; starting probationary boot and POST.", false);
    set_common_headers(request);
    httpd_resp_set_type(request, "application/json");
    httpd_resp_sendstr(request, "{\"status\":\"accepted\",\"next\":\"probationary_boot\"}");
    xTaskCreate(restart_task, "vhos_ota_restart", 3072, NULL, 5, NULL);
    return ESP_OK;
}

static const httpd_uri_t upload_uri = {
    .uri = VHOS_OTA_UPLOAD_PATH,
    .method = HTTP_POST,
    .handler = upload_handler,
};

static esp_err_t initialize_network(void)
{
    esp_err_t result = esp_netif_init();
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        return result;
    }
    result = esp_event_loop_create_default();
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        return result;
    }
    ap_netif = esp_netif_create_default_wifi_ap();
    if (ap_netif == NULL) {
        return ESP_ERR_NO_MEM;
    }
    wifi_init_config_t initialization = WIFI_INIT_CONFIG_DEFAULT();
    result = esp_wifi_init(&initialization);
    if (result != ESP_OK) {
        return result;
    }
    wifi_initialized = true;
    result = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (result != ESP_OK) {
        return result;
    }
    result = esp_event_handler_instance_register(
        WIFI_EVENT,
        ESP_EVENT_ANY_ID,
        wifi_event_handler,
        NULL,
        &wifi_event_instance
    );
    if (result != ESP_OK) {
        return result;
    }

    wifi_config_t configuration = {0};
    strlcpy((char *)configuration.ap.ssid, ota_ssid, sizeof(configuration.ap.ssid));
    configuration.ap.ssid_len = strlen(ota_ssid);
    strlcpy((char *)configuration.ap.password, ota_password, sizeof(configuration.ap.password));
    configuration.ap.channel = 6;
    configuration.ap.authmode = WIFI_AUTH_WPA2_PSK;
    configuration.ap.max_connection = 1;
    configuration.ap.ssid_hidden = 1;
    configuration.ap.pmf_cfg.required = true;
    result = esp_wifi_set_mode(WIFI_MODE_AP);
    if (result == ESP_OK) {
        result = esp_wifi_set_config(WIFI_IF_AP, &configuration);
    }
    if (result == ESP_OK) {
        result = esp_wifi_start();
    }
    if (result == ESP_OK) {
        wifi_started = true;
    }
    return result;
}

static esp_err_t initialize_http(void)
{
    httpd_config_t configuration = HTTPD_DEFAULT_CONFIG();
    configuration.max_uri_handlers = 1;
    configuration.stack_size = VHOS_OTA_HTTP_STACK_BYTES;
    configuration.recv_wait_timeout = 20;
    configuration.send_wait_timeout = 10;
    esp_err_t result = httpd_start(&server, &configuration);
    if (result == ESP_OK) {
        result = httpd_register_uri_handler(server, &upload_uri);
    }
    if (result != ESP_OK && server != NULL) {
        httpd_stop(server);
        server = NULL;
    }
    return result;
}

static void lifecycle_task(void *argument)
{
    (void)argument;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        bool active;
        bool uploading;
        int64_t deadline;
        portENTER_CRITICAL(&state_lock);
        active = service_active;
        uploading = upload_in_progress;
        deadline = expiration_us;
        portEXIT_CRITICAL(&state_lock);
        if (!active) {
            break;
        }
        if (!uploading && esp_timer_get_time() >= deadline) {
            emit_status("EXPIRED", "The temporary OTA network expired without changing firmware.", false);
            shutdown_service(true);
            ESP_LOGI(TAG, "VHOS_OTA_WIFI_STOPPED reason=session-expired");
            break;
        }
    }
    vTaskDelete(NULL);
}

void vhos_ota_wifi_init(const char *gateway_id, vhos_ota_status_emit_fn emit_status_callback)
{
    if (gateway_id != NULL) {
        strlcpy(gateway_id_value, gateway_id, sizeof(gateway_id_value));
    }
    emit_status_json = emit_status_callback;
}

esp_err_t vhos_ota_wifi_activate(const vhos_ota_activation_request_t *request)
{
    if (request == NULL || emit_status_json == NULL ||
        !text_field_valid(request->package_id, sizeof(request->package_id)) ||
        !text_field_valid(request->firmware_version, sizeof(request->firmware_version)) ||
        !sha256_field_valid(request->firmware_sha256) ||
        request->firmware_size_bytes == 0 ||
        request->firmware_size_bytes > VHOS_OTA_MAX_IMAGE_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }
    vhos_ble_health_t ble = {0};
    vhos_can_health_t can = {0};
    if (vhos_ble_get_health(&ble) != ESP_OK || !ble.connected || !ble.encrypted) {
        return ESP_ERR_INVALID_STATE;
    }
    if (vhos_can_get_health(&can) != ESP_OK || !can.controller_running || !can.listen_only) {
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&state_lock);
    bool busy = service_active || wifi_initialized || server != NULL;
    portEXIT_CRITICAL(&state_lock);
    if (busy) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t result = vhos_capture_store_set_logging(false);
    if (result != ESP_OK) {
        return result;
    }
    approved_request = *request;
    generate_secret(ota_password, VHOS_OTA_PASSWORD_LENGTH);
    generate_secret(bearer_token, VHOS_OTA_TOKEN_LENGTH);
    uint8_t mac[6] = {0};
    result = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (result != ESP_OK) {
        vhos_capture_store_set_logging(true);
        clear_secrets();
        return result;
    }
    snprintf(ota_ssid, sizeof(ota_ssid), "VHOS-OTA-%02X%02X%02X", mac[3], mac[4], mac[5]);
    result = initialize_network();
    if (result == ESP_OK) {
        result = initialize_http();
    }
    if (result != ESP_OK) {
        shutdown_service(true);
        return result;
    }
    portENTER_CRITICAL(&state_lock);
    expiration_us = esp_timer_get_time() +
                    (int64_t)VHOS_OTA_SESSION_WINDOW_SECONDS * 1000000LL;
    service_active = true;
    upload_in_progress = false;
    portEXIT_CRITICAL(&state_lock);
    if (xTaskCreate(lifecycle_task, "vhos_ota_lifecycle", 4096, NULL, 4, NULL) != pdPASS) {
        shutdown_service(true);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(
        TAG,
        "VHOS_OTA_WIFI_READY ssid=%s hidden=true endpoint=http://%s%s window_seconds=%u",
        ota_ssid,
        VHOS_OTA_IPV4,
        VHOS_OTA_UPLOAD_PATH,
        VHOS_OTA_SESSION_WINDOW_SECONDS
    );
    result = emit_status("NETWORK_READY", "Temporary authenticated OTA network is ready.", true);
    if (result != ESP_OK) {
        shutdown_service(true);
    }
    return result;
}

esp_err_t vhos_ota_wifi_cancel(void)
{
    portENTER_CRITICAL(&state_lock);
    bool cancellable = service_active && !upload_in_progress;
    if (cancellable) {
        service_active = false;
    }
    portEXIT_CRITICAL(&state_lock);
    if (!cancellable) {
        return ESP_ERR_INVALID_STATE;
    }
    emit_status("CANCELLED", "OTA was cancelled before firmware activation.", false);
    shutdown_service(true);
    return ESP_OK;
}

bool vhos_ota_wifi_active(void)
{
    portENTER_CRITICAL(&state_lock);
    bool active = service_active;
    portEXIT_CRITICAL(&state_lock);
    return active;
}

void vhos_ota_wifi_reconcile_boot(void)
{
    char state[24];
    char package_id[40];
    char version[32];
    char partition[17];
    if (!load_outcome(
            state,
            sizeof(state),
            package_id,
            sizeof(package_id),
            version,
            sizeof(version),
            partition,
            sizeof(partition))) {
        return;
    }
    if (strcmp(state, "PENDING_REBOOT") != 0 && strcmp(state, "PREPARED") != 0) {
        return;
    }
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *invalid = esp_ota_get_last_invalid_partition();
    if (running != NULL && strcmp(running->label, partition) == 0) {
        persist_outcome("POST_PASSED", package_id, version, partition);
        ESP_LOGI(TAG, "VHOS_OTA_POST_PASS package=%s version=%s partition=%s", package_id, version, partition);
    } else if (invalid != NULL && strcmp(invalid->label, partition) == 0) {
        persist_outcome("ROLLED_BACK", package_id, version, partition);
        ESP_LOGW(TAG, "VHOS_OTA_ROLLBACK package=%s version=%s partition=%s", package_id, version, partition);
    } else if (strcmp(state, "PREPARED") == 0) {
        persist_outcome("NOT_ACTIVATED", package_id, version, partition);
        ESP_LOGW(TAG, "VHOS_OTA_NOT_ACTIVATED package=%s version=%s partition=%s", package_id, version, partition);
    }
}

esp_err_t vhos_ota_wifi_send_last_status(void)
{
    char state[24];
    char package_id[40];
    char version[32];
    char partition[17];
    if (!load_outcome(
            state,
            sizeof(state),
            package_id,
            sizeof(package_id),
            version,
            sizeof(version),
            partition,
            sizeof(partition))) {
        return ESP_ERR_NOT_FOUND;
    }
    strlcpy(approved_request.package_id, package_id, sizeof(approved_request.package_id));
    strlcpy(
        approved_request.firmware_version,
        version,
        sizeof(approved_request.firmware_version)
    );
    const char *detail = strcmp(state, "POST_PASSED") == 0
        ? "The updated image passed probationary boot and POST."
        : (strcmp(state, "ROLLED_BACK") == 0
            ? "The probationary image failed and the bootloader rolled back."
            : "An OTA result is available.");
    return emit_status(state, detail, false);
}
