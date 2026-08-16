#include "vhos_transport.h"

#include <stdio.h>
#include <string.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "vhos_can.h"

#define VHOS_HEADER_BYTES 36U
#define VHOS_MAX_PAYLOAD_BYTES 1024U
#define VHOS_MAX_FRAME_BYTES (VHOS_HEADER_BYTES + VHOS_MAX_PAYLOAD_BYTES)
#define VHOS_MESSAGE_HANDSHAKE 1U
#define VHOS_MESSAGE_GATEWAY_HEALTH 4U

#ifndef VHOS_BUILD_ID
#define VHOS_BUILD_ID "source-tree"
#endif

static uint8_t rx_buffer[VHOS_MAX_FRAME_BYTES];
static size_t rx_length;
static uint64_t tx_sequence = 1;
static char gateway_id_value[40] = "esp32-uninitialized";
static vhos_transport_emit_fn emit_frame;
static SemaphoreHandle_t send_lock;

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

static esp_err_t send_payload(uint8_t message_type, const char *payload, bool health_channel)
{
    if (emit_frame == NULL || payload == NULL || send_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t payload_length = strlen(payload);
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
    memcpy(&frame[VHOS_HEADER_BYTES], payload, payload_length);
    esp_err_t result = emit_frame(frame, VHOS_HEADER_BYTES + payload_length, health_channel);
    xSemaphoreGive(send_lock);
    return result;
}

static esp_err_t send_handshake(void)
{
    char payload[VHOS_MAX_PAYLOAD_BYTES + 1];
    int length = snprintf(
        payload,
        sizeof(payload),
        "{\"active_config_id\":\"mrdiy-v13-passive-500k\","
        "\"active_config_version\":\"0.1.0\","
        "\"bootloader_version\":\"esp-idf-5.5.3\","
        "\"capabilities\":[\"ota.ab\",\"ota.rollback-self-test\"],"
        "\"contract\":\"gateway.handshake\","
        "\"contract_version\":\"1.0.0\","
        "\"firmware_build_id\":\"%s\","
        "\"firmware_version\":\"0.1.0-dev.2\","
        "\"gateway_id\":\"%s\","
        "\"hardware_revision\":\"MrDIY-CAN-SHIELD-v1.3+\","
        "\"listen_only\":true,"
        "\"ota_maximum_image_bytes\":null,"
        "\"ota_upload_url\":null,"
        "\"protocol_version\":\"1.0.0\"}",
        VHOS_BUILD_ID,
        gateway_id_value
    );
    if (length < 0 || (size_t)length >= sizeof(payload)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return send_payload(VHOS_MESSAGE_HANDSHAKE, payload, false);
}

esp_err_t vhos_transport_send_health(void)
{
    vhos_can_health_t health = {0};
    esp_err_t can_result = vhos_can_get_health(&health);
    uint64_t observed_us = (uint64_t)esp_timer_get_time();

    char payload[VHOS_MAX_PAYLOAD_BYTES + 1];
    int length = snprintf(
        payload,
        sizeof(payload),
        "{\"bus_error_count\":%llu,"
        "\"bus_off_count\":%llu,"
        "\"can_bitrate_bps\":%lu,"
        "\"can_controller_running\":%s,"
        "\"capture_active\":false,"
        "\"contract\":\"gateway.health\","
        "\"contract_version\":\"1.0.0\","
        "\"dropped_frames\":%llu,"
        "\"listen_only\":true,"
        "\"observed_at\":\"monotonic_us:%llu\","
        "\"received_frames\":%llu,"
        "\"storage_free_bytes\":0,"
        "\"supply_millivolts\":null,"
        "\"vehicle_motion\":\"UNKNOWN\"}",
        (unsigned long long)health.bus_error_count,
        (unsigned long long)health.bus_off_count,
        (unsigned long)health.bitrate_bps,
        (can_result == ESP_OK && health.controller_running) ? "true" : "false",
        (unsigned long long)health.dropped_frames,
        (unsigned long long)observed_us,
        (unsigned long long)health.received_frames
    );
    if (length < 0 || (size_t)length >= sizeof(payload)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return send_payload(VHOS_MESSAGE_GATEWAY_HEALTH, payload, true);
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

    /* Default deny: this target has no raw-CAN or arbitrary diagnostic transmit command. */
    return ESP_ERR_NOT_SUPPORTED;
}

void vhos_transport_init(const char *gateway_id, vhos_transport_emit_fn emit)
{
    if (gateway_id != NULL) {
        strlcpy(gateway_id_value, gateway_id, sizeof(gateway_id_value));
    }
    emit_frame = emit;
    if (send_lock == NULL) {
        send_lock = xSemaphoreCreateMutex();
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
