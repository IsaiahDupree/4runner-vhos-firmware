#include "vhos_status_web.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "nvs.h"
#include "vhos_ble.h"
#include "vhos_can.h"

#define VHOS_STATUS_PASSWORD_LENGTH 20U
#define VHOS_STATUS_USERNAME "vhos"
#define VHOS_STATUS_NVS_NAMESPACE "vhos_status"
#define VHOS_STATUS_NVS_PASSWORD_KEY "password"
#define VHOS_STATUS_IPV4 "192.168.4.1"
#define VHOS_STATUS_HTTP_STACK_BYTES 8192U
#define VHOS_STATUS_LIFECYCLE_STACK_BYTES 4096U

#ifndef VHOS_BUILD_ID
#define VHOS_BUILD_ID "source-tree"
#endif

extern const uint8_t status_page_html_start[] asm("_binary_status_page_html_start");
extern const uint8_t status_page_html_end[] asm("_binary_status_page_html_end");

static const char *TAG = "vhos_status_web";
static const char PASSWORD_ALPHABET[] =
    "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";

static httpd_handle_t server;
static esp_netif_t *ap_netif;
static esp_event_handler_instance_t wifi_event_instance;
static bool wifi_initialized;
static bool wifi_started;
static bool service_active;
static uint8_t connected_stations;
static int64_t expiration_us;
static portMUX_TYPE status_lock = portMUX_INITIALIZER_UNLOCKED;
static char gateway_id_value[40] = "esp32-uninitialized";
static char status_ssid[33];
static char status_password[VHOS_STATUS_PASSWORD_LENGTH + 1];
static char expected_authorization[128];

static bool constant_time_equal(const char *left, size_t left_length, const char *right, size_t right_length)
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

static bool credential_character_valid(char value)
{
    return strchr(PASSWORD_ALPHABET, value) != NULL;
}

static bool credential_valid(const char *credential)
{
    if (credential == NULL || strlen(credential) != VHOS_STATUS_PASSWORD_LENGTH) {
        return false;
    }
    for (size_t index = 0; index < VHOS_STATUS_PASSWORD_LENGTH; ++index) {
        if (!credential_character_valid(credential[index])) {
            return false;
        }
    }
    return true;
}

static void generate_credential(char *destination)
{
    const size_t alphabet_length = sizeof(PASSWORD_ALPHABET) - 1;
    const unsigned int acceptance_limit = 256U - (256U % alphabet_length);
    for (size_t index = 0; index < VHOS_STATUS_PASSWORD_LENGTH; ++index) {
        uint8_t random_byte = 0;
        do {
            esp_fill_random(&random_byte, sizeof(random_byte));
        } while ((unsigned int)random_byte >= acceptance_limit);
        destination[index] = PASSWORD_ALPHABET[random_byte % alphabet_length];
    }
    destination[VHOS_STATUS_PASSWORD_LENGTH] = '\0';
}

static esp_err_t load_or_create_credential(void)
{
    nvs_handle_t handle;
    esp_err_t result = nvs_open(VHOS_STATUS_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (result != ESP_OK) {
        return result;
    }

    size_t stored_length = 0;
    result = nvs_get_str(handle, VHOS_STATUS_NVS_PASSWORD_KEY, NULL, &stored_length);
    bool should_generate = result == ESP_ERR_NVS_NOT_FOUND ||
                           stored_length != sizeof(status_password);
    if (result == ESP_OK && !should_generate) {
        size_t destination_length = sizeof(status_password);
        result = nvs_get_str(
            handle,
            VHOS_STATUS_NVS_PASSWORD_KEY,
            status_password,
            &destination_length
        );
        should_generate = result != ESP_OK || !credential_valid(status_password);
    } else if (result != ESP_OK && result != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return result;
    }

    if (should_generate) {
        generate_credential(status_password);
        result = nvs_set_str(handle, VHOS_STATUS_NVS_PASSWORD_KEY, status_password);
        if (result == ESP_OK) {
            result = nvs_commit(handle);
        }
    }
    nvs_close(handle);
    return result;
}

static esp_err_t build_expected_authorization(void)
{
    char plain[64];
    int plain_length = snprintf(
        plain,
        sizeof(plain),
        "%s:%s",
        VHOS_STATUS_USERNAME,
        status_password
    );
    if (plain_length < 0 || (size_t)plain_length >= sizeof(plain)) {
        return ESP_ERR_INVALID_SIZE;
    }

    unsigned char encoded[96];
    size_t encoded_length = 0;
    int result = mbedtls_base64_encode(
        encoded,
        sizeof(encoded),
        &encoded_length,
        (const unsigned char *)plain,
        (size_t)plain_length
    );
    memset(plain, 0, sizeof(plain));
    if (result != 0) {
        return ESP_FAIL;
    }
    int authorization_length = snprintf(
        expected_authorization,
        sizeof(expected_authorization),
        "Basic %.*s",
        (int)encoded_length,
        encoded
    );
    memset(encoded, 0, sizeof(encoded));
    if (authorization_length < 0 ||
        (size_t)authorization_length >= sizeof(expected_authorization)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static void set_common_headers(httpd_req_t *request)
{
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(request, "Referrer-Policy", "no-referrer");
}

static esp_err_t reject_unauthorized(httpd_req_t *request)
{
    httpd_resp_set_status(request, "401 Unauthorized");
    httpd_resp_set_hdr(request, "WWW-Authenticate", "Basic realm=\"VHOS local status\"");
    set_common_headers(request);
    return httpd_resp_send(request, NULL, 0);
}

static bool authorize(httpd_req_t *request)
{
    size_t header_length = httpd_req_get_hdr_value_len(request, "Authorization");
    if (header_length == 0 || header_length >= sizeof(expected_authorization)) {
        reject_unauthorized(request);
        return false;
    }
    char supplied[sizeof(expected_authorization)];
    if (httpd_req_get_hdr_value_str(
            request,
            "Authorization",
            supplied,
            sizeof(supplied)
        ) != ESP_OK) {
        reject_unauthorized(request);
        return false;
    }
    bool accepted = constant_time_equal(
        supplied,
        strlen(supplied),
        expected_authorization,
        strlen(expected_authorization)
    );
    memset(supplied, 0, sizeof(supplied));
    if (!accepted) {
        reject_unauthorized(request);
    }
    return accepted;
}

static const char *reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON:
        return "power_on";
    case ESP_RST_EXT:
        return "external_pin";
    case ESP_RST_SW:
        return "software";
    case ESP_RST_PANIC:
        return "panic";
    case ESP_RST_INT_WDT:
        return "interrupt_watchdog";
    case ESP_RST_TASK_WDT:
        return "task_watchdog";
    case ESP_RST_WDT:
        return "other_watchdog";
    case ESP_RST_DEEPSLEEP:
        return "deep_sleep";
    case ESP_RST_BROWNOUT:
        return "brownout";
    case ESP_RST_SDIO:
        return "sdio";
    case ESP_RST_UNKNOWN:
    default:
        return "unknown";
    }
}

static const char *ota_state_name(const esp_partition_t *partition)
{
    if (partition == NULL) {
        return "unavailable";
    }
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(partition, &state) != ESP_OK) {
        return "undefined";
    }
    switch (state) {
    case ESP_OTA_IMG_NEW:
        return "new";
    case ESP_OTA_IMG_PENDING_VERIFY:
        return "pending_verify";
    case ESP_OTA_IMG_VALID:
        return "valid";
    case ESP_OTA_IMG_INVALID:
        return "invalid";
    case ESP_OTA_IMG_ABORTED:
        return "aborted";
    case ESP_OTA_IMG_UNDEFINED:
    default:
        return "undefined";
    }
}

static uint32_t seconds_remaining(bool *active, uint8_t *stations)
{
    int64_t deadline;
    portENTER_CRITICAL(&status_lock);
    *active = service_active;
    *stations = connected_stations;
    deadline = expiration_us;
    portEXIT_CRITICAL(&status_lock);
    if (!*active) {
        return 0;
    }
    int64_t difference = deadline - esp_timer_get_time();
    return difference > 0 ? (uint32_t)((difference + 999999) / 1000000) : 0;
}

static void add_u64(cJSON *object, const char *key, uint64_t value)
{
    char encoded[24];
    snprintf(encoded, sizeof(encoded), "%llu", (unsigned long long)value);
    cJSON_AddRawToObject(object, key, encoded);
}

static void add_nullable_u16(cJSON *object, const char *key, bool available, uint16_t value)
{
    if (available) {
        cJSON_AddNumberToObject(object, key, value);
    } else {
        cJSON_AddNullToObject(object, key);
    }
}

static void add_partition_label(cJSON *object, const char *key, const esp_partition_t *partition)
{
    if (partition != NULL) {
        cJSON_AddStringToObject(object, key, partition->label);
    } else {
        cJSON_AddNullToObject(object, key);
    }
}

static cJSON *create_status_json(void)
{
    vhos_ble_health_t ble = {0};
    vhos_can_health_t can = {0};
    vhos_ble_get_health(&ble);
    vhos_can_get_health(&can);

    bool active;
    uint8_t stations;
    uint32_t remaining = seconds_remaining(&active, &stations);
    uint64_t observed_us = (uint64_t)esp_timer_get_time();
    char observed_at[48];
    snprintf(observed_at, sizeof(observed_at), "monotonic_us:%llu", (unsigned long long)observed_us);

    const esp_app_desc_t *description = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    const esp_partition_t *invalid = esp_ota_get_last_invalid_partition();
    const esp_partition_t *storage = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_DATA_SPIFFS,
        "storage"
    );

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }
    cJSON_AddStringToObject(root, "contract", "vhos.status");
    cJSON_AddStringToObject(root, "contract_version", "1.0.0");
    cJSON_AddStringToObject(root, "observed_at", observed_at);

    cJSON *gateway = cJSON_AddObjectToObject(root, "gateway");
    cJSON_AddStringToObject(gateway, "gateway_id", gateway_id_value);
    cJSON_AddStringToObject(gateway, "hardware_revision", "MrDIY-CAN-SHIELD-v1.3+");
    cJSON_AddStringToObject(gateway, "firmware_version", description->version);
    cJSON_AddStringToObject(gateway, "firmware_build_id", VHOS_BUILD_ID);
    cJSON_AddStringToObject(gateway, "esp_idf_version", description->idf_ver);

    cJSON *runtime = cJSON_AddObjectToObject(root, "runtime");
    add_u64(runtime, "uptime_ms", observed_us / 1000U);
    cJSON_AddStringToObject(runtime, "reset_reason", reset_reason_name(esp_reset_reason()));
    cJSON_AddNumberToObject(runtime, "free_heap_bytes", esp_get_free_heap_size());
    cJSON_AddNumberToObject(runtime, "minimum_free_heap_bytes", esp_get_minimum_free_heap_size());

    cJSON *softap = cJSON_AddObjectToObject(root, "softap");
    cJSON_AddBoolToObject(softap, "active", active);
    cJSON_AddStringToObject(softap, "ssid", status_ssid);
    cJSON_AddStringToObject(softap, "ipv4", VHOS_STATUS_IPV4);
    cJSON_AddBoolToObject(softap, "authenticated", true);
    cJSON_AddNumberToObject(softap, "connected_stations", stations);
    cJSON_AddNumberToObject(softap, "expires_in_seconds", remaining);
    cJSON_AddNumberToObject(softap, "window_seconds", VHOS_STATUS_WINDOW_SECONDS);

    cJSON *ble_json = cJSON_AddObjectToObject(root, "ble");
    cJSON_AddBoolToObject(ble_json, "ready", ble.ready);
    cJSON_AddBoolToObject(ble_json, "advertising", ble.advertising);
    cJSON_AddBoolToObject(ble_json, "connected", ble.connected);
    cJSON_AddBoolToObject(ble_json, "encrypted", ble.encrypted);
    cJSON_AddBoolToObject(ble_json, "stream_subscribed", ble.stream_subscribed);
    cJSON_AddBoolToObject(ble_json, "health_subscribed", ble.health_subscribed);
    add_nullable_u16(ble_json, "att_mtu", ble.connected && ble.att_mtu > 0, ble.att_mtu);
    add_nullable_u16(
        ble_json,
        "connection_interval_units",
        ble.connected && ble.connection_parameters_available,
        ble.connection_interval_units
    );
    add_nullable_u16(
        ble_json,
        "connection_latency",
        ble.connected && ble.connection_parameters_available,
        ble.connection_latency
    );
    add_nullable_u16(
        ble_json,
        "supervision_timeout_units",
        ble.connected && ble.connection_parameters_available,
        ble.supervision_timeout_units
    );

    cJSON *bus = cJSON_AddObjectToObject(root, "vehicle_bus");
    cJSON_AddBoolToObject(bus, "controller_running", can.controller_running);
    cJSON_AddBoolToObject(bus, "listen_only", can.listen_only);
    cJSON_AddNumberToObject(bus, "bitrate_bps", can.bitrate_bps);
    cJSON_AddBoolToObject(bus, "bus_detected", can.received_frames > 0);
    add_u64(bus, "received_frames", can.received_frames);
    add_u64(bus, "dropped_frames", can.dropped_frames);
    add_u64(bus, "bus_error_count", can.bus_error_count);
    add_u64(bus, "bus_off_count", can.bus_off_count);
    cJSON_AddBoolToObject(bus, "obd_protocol_confirmed", false);
    cJSON_AddNullToObject(bus, "obd_protocol");

    cJSON *storage_json = cJSON_AddObjectToObject(root, "storage");
    cJSON_AddBoolToObject(storage_json, "partition_present", storage != NULL);
    cJSON_AddBoolToObject(storage_json, "mounted", false);
    if (storage != NULL) {
        cJSON_AddNumberToObject(storage_json, "total_bytes", storage->size);
    } else {
        cJSON_AddNullToObject(storage_json, "total_bytes");
    }
    cJSON_AddNullToObject(storage_json, "free_bytes");
    cJSON_AddStringToObject(
        storage_json,
        "reason",
        storage != NULL ? "filesystem accounting is not mounted" : "storage partition not found"
    );

    cJSON *power = cJSON_AddObjectToObject(root, "power");
    cJSON_AddBoolToObject(power, "supply_available", false);
    cJSON_AddNullToObject(power, "supply_millivolts");
    cJSON_AddStringToObject(power, "reason", "no calibrated supply source is implemented");

    cJSON *ota = cJSON_AddObjectToObject(root, "ota");
    add_partition_label(ota, "running_partition", running);
    add_partition_label(ota, "boot_partition", boot);
    add_partition_label(ota, "next_update_partition", next);
    cJSON_AddStringToObject(ota, "running_image_state", ota_state_name(running));
#ifdef CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    cJSON_AddBoolToObject(ota, "rollback_enabled", true);
#else
    cJSON_AddBoolToObject(ota, "rollback_enabled", false);
#endif
    add_partition_label(ota, "last_invalid_partition", invalid);
    cJSON_AddBoolToObject(ota, "upload_supported", false);

    cJSON *safety = cJSON_AddObjectToObject(root, "safety");
    cJSON_AddBoolToObject(safety, "read_only_http", true);
    cJSON_AddBoolToObject(safety, "arbitrary_can_transmit_available", false);
    cJSON_AddBoolToObject(safety, "diagnostic_command_available", false);
    cJSON_AddBoolToObject(safety, "configuration_mutation_available", false);
    return root;
}

static esp_err_t page_handler(httpd_req_t *request)
{
    if (!authorize(request)) {
        return ESP_OK;
    }
    set_common_headers(request);
    httpd_resp_set_hdr(
        request,
        "Content-Security-Policy",
        "default-src 'none'; style-src 'unsafe-inline'; script-src 'unsafe-inline'; "
        "connect-src 'self'; img-src 'self' data:; frame-ancestors 'none'; "
        "base-uri 'none'; form-action 'none'"
    );
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    return httpd_resp_send(
        request,
        (const char *)status_page_html_start,
        (ssize_t)(status_page_html_end - status_page_html_start)
    );
}

static esp_err_t status_handler(httpd_req_t *request)
{
    if (!authorize(request)) {
        return ESP_OK;
    }
    cJSON *root = create_status_json();
    if (root == NULL) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "status unavailable");
    }
    char *encoded = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (encoded == NULL) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "status unavailable");
    }
    set_common_headers(request);
    httpd_resp_set_type(request, "application/json");
    esp_err_t result = httpd_resp_send(request, encoded, HTTPD_RESP_USE_STRLEN);
    cJSON_free(encoded);
    return result;
}

static esp_err_t health_handler(httpd_req_t *request)
{
    if (!authorize(request)) {
        return ESP_OK;
    }
    set_common_headers(request);
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_send(request, "{\"status\":\"ok\",\"read_only\":true}", HTTPD_RESP_USE_STRLEN);
}

static const httpd_uri_t page_uri = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = page_handler,
};

static const httpd_uri_t status_uri = {
    .uri = "/api/v1/status",
    .method = HTTP_GET,
    .handler = status_handler,
};

static const httpd_uri_t health_uri = {
    .uri = "/healthz",
    .method = HTTP_GET,
    .handler = health_handler,
};

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
    portENTER_CRITICAL(&status_lock);
    if (event_id == WIFI_EVENT_AP_STACONNECTED && connected_stations < UINT8_MAX) {
        connected_stations++;
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED && connected_stations > 0) {
        connected_stations--;
    }
    uint8_t count = connected_stations;
    portEXIT_CRITICAL(&status_lock);
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "VHOS_SOFTAP_STATION_JOINED count=%u", count);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        ESP_LOGI(TAG, "VHOS_SOFTAP_STATION_LEFT count=%u", count);
    }
}

static void shutdown_service(void)
{
    httpd_handle_t active_server;
    portENTER_CRITICAL(&status_lock);
    service_active = false;
    connected_stations = 0;
    active_server = server;
    server = NULL;
    portEXIT_CRITICAL(&status_lock);

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
    memset(status_password, 0, sizeof(status_password));
    memset(expected_authorization, 0, sizeof(expected_authorization));
}

static void lifecycle_task(void *argument)
{
    (void)argument;
    vTaskDelay(pdMS_TO_TICKS(VHOS_STATUS_WINDOW_SECONDS * 1000U));
    ESP_LOGI(TAG, "VHOS_SOFTAP_EXPIRING reason=boot-window-complete");
    shutdown_service();
    ESP_LOGI(TAG, "VHOS_SOFTAP_STOPPED can_and_ble_continue=true");
    vTaskDelete(NULL);
}

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
    strlcpy((char *)configuration.ap.ssid, status_ssid, sizeof(configuration.ap.ssid));
    configuration.ap.ssid_len = strlen(status_ssid);
    strlcpy(
        (char *)configuration.ap.password,
        status_password,
        sizeof(configuration.ap.password)
    );
    configuration.ap.channel = 1;
    configuration.ap.authmode = WIFI_AUTH_WPA2_PSK;
    configuration.ap.max_connection = 1;
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
    configuration.max_uri_handlers = 3;
    configuration.stack_size = VHOS_STATUS_HTTP_STACK_BYTES;
    esp_err_t result = httpd_start(&server, &configuration);
    if (result != ESP_OK) {
        return result;
    }
    result = httpd_register_uri_handler(server, &page_uri);
    if (result == ESP_OK) {
        result = httpd_register_uri_handler(server, &status_uri);
    }
    if (result == ESP_OK) {
        result = httpd_register_uri_handler(server, &health_uri);
    }
    if (result != ESP_OK) {
        httpd_stop(server);
        server = NULL;
    }
    return result;
}

esp_err_t vhos_status_web_start(const char *gateway_id)
{
    if (gateway_id == NULL || service_active || wifi_initialized || server != NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(gateway_id_value, gateway_id, sizeof(gateway_id_value));

    uint8_t mac[6] = {0};
    esp_err_t result = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (result != ESP_OK) {
        return result;
    }
    snprintf(
        status_ssid,
        sizeof(status_ssid),
        "VHOS-STATUS-%02X%02X%02X",
        mac[3],
        mac[4],
        mac[5]
    );

    result = load_or_create_credential();
    if (result == ESP_OK) {
        result = build_expected_authorization();
    }
    if (result == ESP_OK) {
        result = initialize_network();
    }
    if (result == ESP_OK) {
        result = initialize_http();
    }
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "VHOS_SOFTAP_START_FAILED error=%s", esp_err_to_name(result));
        shutdown_service();
        return result;
    }

    portENTER_CRITICAL(&status_lock);
    expiration_us = esp_timer_get_time() + (int64_t)VHOS_STATUS_WINDOW_SECONDS * 1000000LL;
    service_active = true;
    portEXIT_CRITICAL(&status_lock);
    if (xTaskCreate(
            lifecycle_task,
            "vhos_status_lifecycle",
            VHOS_STATUS_LIFECYCLE_STACK_BYTES,
            NULL,
            4,
            NULL
        ) != pdPASS) {
        shutdown_service();
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(
        TAG,
        "VHOS_SOFTAP_READY ssid=%s url=http://%s/ window_seconds=%u read_only=true",
        status_ssid,
        VHOS_STATUS_IPV4,
        VHOS_STATUS_WINDOW_SECONDS
    );
    ESP_LOGW(
        TAG,
        "VHOS_SOFTAP_CREDENTIAL ssid=%s username=%s password=%s physical_uart_secret=true",
        status_ssid,
        VHOS_STATUS_USERNAME,
        status_password
    );
    return ESP_OK;
}
