#include "vhos_transport.h"

#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "vhos_can.h"
#include "vhos_capture_store.h"
#include "vhos_ota_wifi.h"

#define VHOS_HEADER_BYTES 36U
#define VHOS_MAX_PAYLOAD_BYTES 1024U
#define VHOS_MAX_FRAME_BYTES (VHOS_HEADER_BYTES + VHOS_MAX_PAYLOAD_BYTES)
#define VHOS_MESSAGE_HANDSHAKE 1U
#define VHOS_MESSAGE_RAW_CAN_FRAME 2U
#define VHOS_MESSAGE_GATEWAY_HEALTH 4U
#define VHOS_MESSAGE_OTA_CONTROL 8U
#define VHOS_MESSAGE_CAPTURE_LOG_REQUEST 11U
#define VHOS_MESSAGE_CAPTURE_LOG_INDEX 12U
#define VHOS_MESSAGE_CAPTURE_LOG_CHUNK 13U
#define VHOS_CAPTURE_LOG_REQUEST_BYTES 8U
#define VHOS_CAPTURE_LOG_CHUNK_HEADER_BYTES 16U
#define VHOS_CAPTURE_LOG_CHUNK_RECORD_CAPACITY 24U
#define VHOS_LIVE_CAN_INTERVAL_US 500000ULL

#ifndef VHOS_BUILD_ID
#define VHOS_BUILD_ID "source-tree"
#endif

#if defined(CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT) || defined(CONFIG_SECURE_BOOT)
#define VHOS_SIGNED_OTA_CAPABILITY ",\"ota.signed-image\""
#else
#define VHOS_SIGNED_OTA_CAPABILITY ""
#endif

#ifdef CONFIG_VHOS_STATUS_SOFTAP_AUTOSTART
#define VHOS_STATUS_CAPABILITY ",\"status.softap.readonly\""
#else
#define VHOS_STATUS_CAPABILITY ""
#endif

#define VHOS_CAPABILITIES "[\"capture.passive\",\"evidence.persistent-log\",\"evidence.export\",\"ota.ab\",\"ota.rollback-self-test\"" VHOS_SIGNED_OTA_CAPABILITY VHOS_STATUS_CAPABILITY "]"

static uint8_t rx_buffer[VHOS_MAX_FRAME_BYTES];
static size_t rx_length;
static uint64_t tx_sequence = 1;
static char gateway_id_value[40] = "esp32-uninitialized";
static vhos_transport_emit_fn emit_frame;
static SemaphoreHandle_t send_lock;
static bool can_observer_registered;
static portMUX_TYPE live_lock = portMUX_INITIALIZER_UNLOCKED;
static uint64_t last_live_can_us;

static uint32_t read_u32_le(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0]) |
           ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

static void write_u32_le(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

static void write_u16_le(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

static void write_u64_le(uint8_t *bytes, uint64_t value)
{
    for (size_t index = 0; index < 8; index++) {
        bytes[index] = (uint8_t)(value >> (index * 8));
    }
}

static uint32_t crc32c(const uint8_t *bytes, size_t length)
{
    uint32_t crc = UINT32_MAX;
    for (size_t index = 0; index < length; index++) {
        crc ^= bytes[index];
        for (uint8_t bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ ((crc & 1U) ? 0x82F63B78U : 0U);
        }
    }
    return ~crc;
}

static esp_err_t send_payload(
    uint8_t message_type,
    const uint8_t *payload,
    size_t payload_length,
    vhos_transport_channel_t channel
)
{
    if (emit_frame == NULL || (payload == NULL && payload_length > 0) || send_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (payload_length > VHOS_MAX_PAYLOAD_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (xSemaphoreTake(send_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    uint8_t frame[VHOS_MAX_FRAME_BYTES] = {0};
    memcpy(frame, "VHOS", 4);
    frame[4] = 1;
    frame[5] = 0;
    frame[6] = message_type;
    write_u32_le(&frame[8], (uint32_t)payload_length);
    write_u64_le(&frame[12], tx_sequence++);
    write_u64_le(&frame[20], (uint64_t)esp_timer_get_time());
    write_u32_le(&frame[28], crc32c((const uint8_t *)payload, payload_length));
    write_u32_le(&frame[32], crc32c(frame, 32));
    if (payload_length > 0) {
        memcpy(&frame[VHOS_HEADER_BYTES], payload, payload_length);
    }
    esp_err_t result = emit_frame(frame, VHOS_HEADER_BYTES + payload_length, channel);
    xSemaphoreGive(send_lock);
    return result;
}

static esp_err_t send_json(
    uint8_t message_type,
    const char *payload,
    vhos_transport_channel_t channel
)
{
    if (payload == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return send_payload(
        message_type,
        (const uint8_t *)payload,
        strlen(payload),
        channel
    );
}

static esp_err_t send_handshake(void)
{
    char payload[VHOS_MAX_PAYLOAD_BYTES + 1];
    int length = snprintf(
        payload,
        sizeof(payload),
        "{\"active_config_id\":\"mrdiy-v13-passive-can-scan\","
        "\"active_config_version\":\"0.4.0\","
        "\"bootloader_version\":\"esp-idf-5.5.3\","
        "\"capabilities\":%s,"
        "\"contract\":\"gateway.handshake\","
        "\"contract_version\":\"1.0.0\","
        "\"firmware_build_id\":\"%s\","
        "\"firmware_version\":\"0.1.0-dev.12\","
        "\"gateway_id\":\"%s\","
        "\"hardware_revision\":\"MrDIY-CAN-SHIELD-v1.3+\","
        "\"listen_only\":true,"
        "\"ota_maximum_image_bytes\":%u,"
        "\"ota_upload_url\":\"http://192.168.4.1/api/v1/ota/image\","
        "\"protocol_version\":\"1.0.0\"}",
        VHOS_CAPABILITIES,
        VHOS_BUILD_ID,
        gateway_id_value,
        VHOS_OTA_MAX_IMAGE_BYTES
    );
    if (length < 0 || (size_t)length >= sizeof(payload)) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t result = send_json(
        VHOS_MESSAGE_HANDSHAKE,
        payload,
        VHOS_TRANSPORT_CHANNEL_STREAM
    );
    if (result == ESP_OK) {
        esp_err_t status_result = vhos_ota_wifi_send_last_status();
        if (status_result != ESP_OK && status_result != ESP_ERR_NOT_FOUND) {
            return status_result;
        }
    }
    return result;
}

esp_err_t vhos_transport_send_ota_status(const char *json)
{
    return send_json(VHOS_MESSAGE_OTA_CONTROL, json, VHOS_TRANSPORT_CHANNEL_OTA);
}

esp_err_t vhos_transport_send_health(void)
{
    vhos_can_health_t health = {0};
    esp_err_t can_result = vhos_can_get_health(&health);
    uint64_t observed_us = (uint64_t)esp_timer_get_time();
    const char *candidate = vhos_can_passive_candidate(&health);
    vhos_capture_store_status_t capture = {0};
    bool capture_available = vhos_capture_store_get_status(&capture) == ESP_OK;
    char candidate_json[32];
    if (candidate == NULL) {
        strlcpy(candidate_json, "null", sizeof(candidate_json));
    } else {
        snprintf(candidate_json, sizeof(candidate_json), "\"%s\"", candidate);
    }

    char payload[VHOS_MAX_PAYLOAD_BYTES + 1];
    int length = snprintf(
        payload,
        sizeof(payload),
        "{\"bus_error_count\":%llu,"
        "\"bus_off_count\":%llu,"
        "\"can_bitrate_bps\":%lu,"
        "\"can_controller_running\":%s,"
        "\"can_extended_frames\":%llu,"
        "\"can_frames_250k\":%llu,"
        "\"can_frames_500k\":%llu,"
        "\"can_passive_lock\":%s,"
        "\"can_scan_cycles\":%lu,"
        "\"can_scan_state\":\"%s\","
        "\"can_standard_frames\":%llu,"
        "\"capture_active\":%s,"
        "\"capture_queue_dropped_records\":%llu,"
        "\"capture_retained_records\":%llu,"
        "\"capture_session_id\":%lu,"
        "\"capture_storage_write_failures\":%llu,"
        "\"contract\":\"gateway.health\","
        "\"contract_version\":\"1.0.0\","
        "\"dropped_frames\":%llu,"
        "\"listen_only\":true,"
        "\"observed_at\":\"monotonic_us:%llu\","
        "\"passive_can_candidate\":%s,"
        "\"received_frames\":%llu,"
        "\"storage_free_bytes\":%lu,"
        "\"supply_millivolts\":null,"
        "\"vehicle_motion\":\"UNKNOWN\"}",
        (unsigned long long)health.bus_error_count,
        (unsigned long long)health.bus_off_count,
        (unsigned long)health.bitrate_bps,
        (can_result == ESP_OK && health.controller_running) ? "true" : "false",
        (unsigned long long)health.extended_frames,
        (unsigned long long)health.frames_250k,
        (unsigned long long)health.frames_500k,
        health.passive_lock ? "true" : "false",
        (unsigned long)health.scan_cycles,
        vhos_can_scan_state_name(health.scan_state),
        (unsigned long long)health.standard_frames,
        capture_available && capture.logging ? "true" : "false",
        (unsigned long long)capture.queue_dropped_records,
        (unsigned long long)capture.retained_records,
        (unsigned long)capture.current_session_id,
        (unsigned long long)capture.storage_write_failures,
        (unsigned long long)health.dropped_frames,
        (unsigned long long)observed_us,
        candidate_json,
        (unsigned long long)health.received_frames,
        (unsigned long)capture.free_bytes
    );
    if (length < 0 || (size_t)length >= sizeof(payload)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return send_json(
        VHOS_MESSAGE_GATEWAY_HEALTH,
        payload,
        VHOS_TRANSPORT_CHANNEL_HEALTH
    );
}

static esp_err_t send_capture_log_index(void)
{
    vhos_capture_store_status_t status = {0};
    esp_err_t result = vhos_capture_store_get_status(&status);
    char payload[VHOS_MAX_PAYLOAD_BYTES + 1];
    int length = snprintf(
        payload,
        sizeof(payload),
        "{\"contract\":\"gateway.capture-log-index\","
        "\"contract_version\":\"1.0.0\","
        "\"current_bytes\":%lu,\"current_records\":%lu,\"current_session_id\":%lu,"
        "\"free_bytes\":%lu,\"logging\":%s,\"mounted\":%s,"
        "\"observed_frames\":%llu,\"previous_bytes\":%lu,"
        "\"previous_records\":%lu,\"previous_session_id\":%lu,"
        "\"queue_dropped_records\":%llu,\"record_bytes\":%u,"
        "\"retained_records\":%llu,\"sample_suppressed_frames\":%llu,"
        "\"sampled_frames\":%llu,\"storage_write_failures\":%llu,"
        "\"total_bytes\":%lu}",
        (unsigned long)status.current_bytes,
        (unsigned long)status.current_records,
        (unsigned long)status.current_session_id,
        (unsigned long)status.free_bytes,
        status.logging ? "true" : "false",
        status.mounted ? "true" : "false",
        (unsigned long long)status.observed_frames,
        (unsigned long)status.previous_bytes,
        (unsigned long)status.previous_records,
        (unsigned long)status.previous_session_id,
        (unsigned long long)status.queue_dropped_records,
        VHOS_CAPTURE_RECORD_BYTES,
        (unsigned long long)status.retained_records,
        (unsigned long long)status.sample_suppressed_frames,
        (unsigned long long)status.sampled_frames,
        (unsigned long long)status.storage_write_failures,
        (unsigned long)status.total_bytes
    );
    if (result != ESP_OK || length < 0 || (size_t)length >= sizeof(payload)) {
        return result == ESP_OK ? ESP_ERR_INVALID_SIZE : result;
    }
    return send_json(
        VHOS_MESSAGE_CAPTURE_LOG_INDEX,
        payload,
        VHOS_TRANSPORT_CHANNEL_STREAM
    );
}

static esp_err_t send_capture_log_chunk(uint8_t slot, uint32_t offset)
{
    uint8_t payload[VHOS_CAPTURE_LOG_CHUNK_HEADER_BYTES +
                    VHOS_CAPTURE_LOG_CHUNK_RECORD_CAPACITY * VHOS_CAPTURE_RECORD_BYTES] = {0};
    size_t data_length = 0;
    uint32_t record_count = 0;
    bool end = false;
    esp_err_t result = vhos_capture_store_read_records(
        slot,
        offset,
        &payload[VHOS_CAPTURE_LOG_CHUNK_HEADER_BYTES],
        sizeof(payload) - VHOS_CAPTURE_LOG_CHUNK_HEADER_BYTES,
        &data_length,
        &record_count,
        &end
    );
    if (result == ESP_ERR_NOT_FOUND) {
        result = ESP_OK;
        end = true;
    }
    if (result != ESP_OK) {
        return result;
    }
    vhos_capture_store_status_t status = {0};
    vhos_capture_store_get_status(&status);
    payload[0] = 1;
    payload[1] = slot;
    payload[2] = end ? 1 : 0;
    write_u32_le(&payload[4], offset);
    write_u16_le(&payload[8], (uint16_t)record_count);
    write_u16_le(&payload[10], VHOS_CAPTURE_RECORD_BYTES);
    write_u32_le(
        &payload[12],
        slot == VHOS_CAPTURE_SLOT_CURRENT
            ? status.current_session_id
            : status.previous_session_id
    );
    return send_payload(
        VHOS_MESSAGE_CAPTURE_LOG_CHUNK,
        payload,
        VHOS_CAPTURE_LOG_CHUNK_HEADER_BYTES + data_length,
        VHOS_TRANSPORT_CHANNEL_STREAM
    );
}

static void send_live_can_observation(
    const vhos_can_observation_t *observation,
    void *context
)
{
    (void)context;
    bool eligible = false;
    portENTER_CRITICAL(&live_lock);
    if (observation->monotonic_us - last_live_can_us >= VHOS_LIVE_CAN_INTERVAL_US) {
        last_live_can_us = observation->monotonic_us;
        eligible = true;
    }
    portEXIT_CRITICAL(&live_lock);
    if (!eligible) {
        return;
    }
    uint8_t payload[36] = {0};
    payload[0] = 1;
    payload[1] = (observation->extended ? 0x01 : 0) |
                 (observation->remote_request ? 0x02 : 0) |
                 (observation->listen_only ? 0x04 : 0);
    payload[2] = observation->data_length;
    payload[3] = observation->bitrate_bps == 250000U ? 2 : 1;
    write_u32_le(&payload[4], observation->identifier);
    write_u64_le(&payload[8], observation->source_sequence);
    write_u64_le(&payload[16], observation->monotonic_us);
    write_u32_le(&payload[24], vhos_capture_store_current_session_id());
    memcpy(&payload[28], observation->data, 8);
    send_payload(
        VHOS_MESSAGE_RAW_CAN_FRAME,
        payload,
        sizeof(payload),
        VHOS_TRANSPORT_CHANNEL_STREAM
    );
}

static bool copy_json_string(
    const cJSON *root,
    const char *key,
    char *destination,
    size_t capacity
)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsString(item) || item->valuestring == NULL ||
        strlcpy(destination, item->valuestring, capacity) >= capacity) {
        return false;
    }
    return true;
}

static esp_err_t process_ota_control(const uint8_t *payload, size_t payload_length)
{
#if !defined(CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT) && !defined(CONFIG_SECURE_BOOT)
    (void)payload;
    (void)payload_length;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (payload == NULL || payload_length == 0 || payload_length > VHOS_MAX_PAYLOAD_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *root = cJSON_ParseWithLength((const char *)payload, payload_length);
    if (root == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    char contract[48];
    char operation[16];
    bool valid = copy_json_string(root, "contract", contract, sizeof(contract)) &&
                 copy_json_string(root, "operation", operation, sizeof(operation)) &&
                 strcmp(contract, "gateway.ota-control-request") == 0;
    if (!valid) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(operation, "CANCEL") == 0) {
        cJSON_Delete(root);
        return vhos_ota_wifi_cancel();
    }
    if (strcmp(operation, "ACTIVATE") != 0) {
        cJSON_Delete(root);
        return ESP_ERR_NOT_SUPPORTED;
    }

    vhos_ota_activation_request_t request = {0};
    const cJSON *size = cJSON_GetObjectItemCaseSensitive(root, "firmware_size_bytes");
    valid = copy_json_string(
                root,
                "package_id",
                request.package_id,
                sizeof(request.package_id)
            ) &&
            copy_json_string(
                root,
                "firmware_version",
                request.firmware_version,
                sizeof(request.firmware_version)
            ) &&
            copy_json_string(
                root,
                "firmware_sha256",
                request.firmware_sha256,
                sizeof(request.firmware_sha256)
            ) &&
            cJSON_IsNumber(size) && size->valuedouble > 0 &&
            size->valuedouble <= VHOS_OTA_MAX_IMAGE_BYTES &&
            size->valuedouble == (double)(uint32_t)size->valuedouble;
    if (valid) {
        request.firmware_size_bytes = (uint32_t)size->valuedouble;
    }
    cJSON_Delete(root);
    return valid ? vhos_ota_wifi_activate(&request) : ESP_ERR_INVALID_ARG;
#endif
}

static esp_err_t process_frame(const uint8_t *frame, size_t length)
{
    if (length < VHOS_HEADER_BYTES || memcmp(frame, "VHOS", 4) != 0 || frame[4] != 1) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t payload_length = read_u32_le(&frame[8]);
    if (payload_length > VHOS_MAX_PAYLOAD_BYTES || length != VHOS_HEADER_BYTES + payload_length) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (read_u32_le(&frame[32]) != crc32c(frame, 32) ||
        read_u32_le(&frame[28]) != crc32c(&frame[VHOS_HEADER_BYTES], payload_length)) {
        return ESP_ERR_INVALID_CRC;
    }

    if (frame[6] == VHOS_MESSAGE_HANDSHAKE) {
        esp_err_t result = send_handshake();
        return result == ESP_OK ? vhos_transport_send_health() : result;
    }

    if (frame[6] == VHOS_MESSAGE_OTA_CONTROL) {
        return process_ota_control(&frame[VHOS_HEADER_BYTES], payload_length);
    }

    if (frame[6] == VHOS_MESSAGE_CAPTURE_LOG_REQUEST &&
        payload_length == VHOS_CAPTURE_LOG_REQUEST_BYTES) {
        const uint8_t *payload = &frame[VHOS_HEADER_BYTES];
        if (payload[0] != 1) {
            return ESP_ERR_NOT_SUPPORTED;
        }
        uint8_t operation = payload[1];
        if (operation == 0) {
            return send_capture_log_index();
        }
        if (operation == 1) {
            return send_capture_log_chunk(payload[2], read_u32_le(&payload[4]));
        }
        if (operation == 2) {
            esp_err_t result = vhos_capture_store_rotate();
            return result == ESP_OK ? send_capture_log_index() : result;
        }
        if (operation == 3 || operation == 4) {
            esp_err_t result = vhos_capture_store_set_logging(operation == 4);
            return result == ESP_OK ? send_capture_log_index() : result;
        }
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* Default deny: this target has no raw-CAN or arbitrary diagnostic transmit command. */
    return ESP_ERR_NOT_SUPPORTED;
}

void vhos_transport_init(const char *gateway_id, vhos_transport_emit_fn emit)
{
    if (gateway_id != NULL) {
        strlcpy(gateway_id_value, gateway_id, sizeof(gateway_id_value));
    }
    emit_frame = emit;
    vhos_ota_wifi_init(gateway_id_value, vhos_transport_send_ota_status);
    if (send_lock == NULL) {
        send_lock = xSemaphoreCreateMutex();
    }
    if (!can_observer_registered &&
        vhos_can_register_observer(send_live_can_observation, NULL) == ESP_OK) {
        can_observer_registered = true;
    }
    vhos_transport_reset();
}

void vhos_transport_reset(void)
{
    rx_length = 0;
    memset(rx_buffer, 0, sizeof(rx_buffer));
}

esp_err_t vhos_transport_ingest(const uint8_t *data, size_t length)
{
    if (data == NULL || length == 0 || length > sizeof(rx_buffer) - rx_length) {
        vhos_transport_reset();
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(&rx_buffer[rx_length], data, length);
    rx_length += length;

    while (rx_length >= VHOS_HEADER_BYTES) {
        if (memcmp(rx_buffer, "VHOS", 4) != 0) {
            vhos_transport_reset();
            return ESP_ERR_INVALID_ARG;
        }
        uint32_t payload_length = read_u32_le(&rx_buffer[8]);
        if (payload_length > VHOS_MAX_PAYLOAD_BYTES) {
            vhos_transport_reset();
            return ESP_ERR_INVALID_SIZE;
        }
        size_t frame_length = VHOS_HEADER_BYTES + payload_length;
        if (rx_length < frame_length) {
            return ESP_OK;
        }

        esp_err_t result = process_frame(rx_buffer, frame_length);
        size_t remaining = rx_length - frame_length;
        if (remaining > 0) {
            memmove(rx_buffer, &rx_buffer[frame_length], remaining);
        }
        rx_length = remaining;
        if (result != ESP_OK) {
            return result;
        }
    }
    return ESP_OK;
}
