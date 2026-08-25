#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

typedef struct {
    bool ready;
    bool advertising;
    bool connected;
    bool encrypted;
    bool stream_subscribed;
    bool health_subscribed;
    bool ota_subscribed;
    bool connection_parameters_available;
    uint16_t att_mtu;
    uint16_t connection_interval_units;
    uint16_t connection_latency;
    uint16_t supervision_timeout_units;
    uint16_t tx_queue_depth;
    uint16_t tx_queue_high_water;
    uint16_t tx_queue_capacity;
    uint32_t connection_epoch;
    int32_t last_notification_result;
    uint64_t tx_frames_admitted;
    uint64_t tx_frames_nimble_accepted;
    uint64_t tx_frames_failed;
    uint64_t tx_frames_queue_rejected;
    uint64_t history_frames_admitted;
    uint64_t history_frames_nimble_accepted;
    uint64_t history_frames_deferred;
    uint64_t history_frames_queue_rejected;
    uint64_t notification_packet_alloc_failures;
    uint64_t notification_api_attempts;
    uint64_t notification_commands_accepted;
    uint64_t notification_attempt_events;
    uint64_t notification_attempt_errors;
    uint64_t notification_backpressure_events;
    uint64_t notification_backpressure_retries;
    uint64_t notification_backpressure_exhaustions;
} vhos_ble_health_t;

esp_err_t vhos_ble_start(const char *device_name, const char *gateway_id);
esp_err_t vhos_ble_wait_ready(TickType_t timeout);
esp_err_t vhos_ble_get_health(vhos_ble_health_t *health);
