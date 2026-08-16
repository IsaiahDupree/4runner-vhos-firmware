/*
 * VHOS transport extension for WiCAN Pro.
 * Copyright (C) 2026 Isaiah Dupree.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "vhos_transport.h"

#include <stdio.h>
#include <string.h>
#include "can.h"
#include "esp_vfs_fat.h"
#include "esp_timer.h"
#include "hw_config.h"
#include "sleep_mode.h"

#define VHOS_HEADER_BYTES 36U
#define VHOS_MAX_PAYLOAD_BYTES 1024U
#define VHOS_RX_BYTES (VHOS_HEADER_BYTES + VHOS_MAX_PAYLOAD_BYTES)
#define VHOS_MESSAGE_HANDSHAKE 1U
#define VHOS_MESSAGE_GATEWAY_HEALTH 4U
static uint8_t rx_buffer[VHOS_RX_BYTES];
static size_t rx_length;
static uint64_t tx_sequence = 1;
static char gateway_id_value[33] = "uninitialized";
static vhos_transport_emit_fn emit_frame;

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
    if (emit_frame == NULL || payload == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t payload_length = strlen(payload);
    if (payload_length > VHOS_MAX_PAYLOAD_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t frame[VHOS_RX_BYTES] = {0};
    memcpy(frame, "VHOS", 4);
    frame[4] = 1;
    frame[5] = 0;
    frame[6] = message_type;
    frame[7] = 0;
    write_u32_le(&frame[8], (uint32_t)payload_length);
    write_u64_le(&frame[12], tx_sequence++);
    write_u64_le(&frame[20], (uint64_t)esp_timer_get_time());
    write_u32_le(&frame[28], crc32c((const uint8_t *)payload, payload_length));
    write_u32_le(&frame[32], crc32c(frame, 32));
    memcpy(&frame[VHOS_HEADER_BYTES], payload, payload_length);
    return emit_frame(frame, VHOS_HEADER_BYTES + payload_length, health_channel);
}

static esp_err_t send_handshake(void)
{
    char payload[VHOS_MAX_PAYLOAD_BYTES + 1];
    int length = snprintf(
        payload,
        sizeof(payload),
        "{\"active_config_id\":\"vhos-passive-foundation\","
        "\"active_config_version\":\"0.1.0\","
        "\"bootloader_version\":null,"
        "\"capabilities\":[\"ota.ab\",\"ota.rollback-self-test\"],"
        "\"contract\":\"gateway.handshake\","
        "\"contract_version\":\"1.0.0\","
        "\"firmware_build_id\":\"%s\","
        "\"firmware_version\":\"0.1.0-dev.1\","
        "\"gateway_id\":\"%s\","
        "\"hardware_revision\":\"WiCAN-PRO-1_53\","
        "\"listen_only\":true,"
        "\"ota_maximum_image_bytes\":5169152,"
        "\"ota_upload_url\":null,"
        "\"protocol_version\":\"1.0.0\"}",
        GIT_SHA,
        gateway_id_value
    );
    if (length < 0 || (size_t)length >= sizeof(payload)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return send_payload(VHOS_MESSAGE_HANDSHAKE, payload, false);
}

static esp_err_t send_health(void)
{
    can_health_metrics_t metrics = {0};
    esp_err_t metrics_result = can_get_health_metrics(&metrics);
    if (metrics_result != ESP_OK) {
        return metrics_result;
    }

    uint64_t storage_total = 0;
    uint64_t storage_free = 0;
    esp_err_t storage_result = esp_vfs_fat_info(FS_MOUNT_POINT, &storage_total, &storage_free);
    if (storage_result != ESP_OK) {
        return storage_result;
    }

    float supply_volts = 0.0f;
    bool supply_available = sleep_mode_get_voltage(&supply_volts) == ESP_OK && supply_volts > 0.0f;
    char supply_value[24];
    if (supply_available) {
        snprintf(supply_value, sizeof(supply_value), "%d", (int)(supply_volts * 1000.0f + 0.5f));
    } else {
        strlcpy(supply_value, "null", sizeof(supply_value));
    }

    uint64_t observed_us = (uint64_t)esp_timer_get_time();
    char payload[VHOS_MAX_PAYLOAD_BYTES + 1];
    int length = snprintf(
        payload,
        sizeof(payload),
        "{\"bus_error_count\":%llu,"
        "\"bus_off_count\":%llu,"
        "\"capture_active\":false,"
        "\"contract\":\"gateway.health\","
        "\"contract_version\":\"1.0.0\","
        "\"dropped_frames\":%llu,"
        "\"listen_only\":%s,"
        "\"observed_at\":\"monotonic_us:%llu\","
        "\"received_frames\":%llu,"
        "\"storage_free_bytes\":%llu,"
        "\"supply_millivolts\":%s,"
        "\"vehicle_motion\":\"UNKNOWN\"}",
        (unsigned long long)metrics.bus_error_count,
        (unsigned long long)metrics.bus_off_count,
        (unsigned long long)metrics.dropped_frames,
        can_is_silent() ? "true" : "false",
        (unsigned long long)observed_us,
        (unsigned long long)metrics.received_frames,
        (unsigned long long)storage_free,
        supply_value
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
        esp_err_t handshake_result = send_handshake();
        if (handshake_result != ESP_OK) {
            return handshake_result;
        }
        return send_health();
    }

    /* Default deny: no raw frame, ELM command, or arbitrary transmit message is accepted. */
    return ESP_ERR_NOT_SUPPORTED;
}

void vhos_transport_init(const char *gateway_id, vhos_transport_emit_fn emit)
{
    if (gateway_id != NULL) {
        strlcpy(gateway_id_value, gateway_id, sizeof(gateway_id_value));
    }
    emit_frame = emit;
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
