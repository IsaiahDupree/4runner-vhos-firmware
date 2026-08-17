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
} vhos_ble_health_t;

esp_err_t vhos_ble_start(const char *device_name, const char *gateway_id);
esp_err_t vhos_ble_wait_ready(TickType_t timeout);
esp_err_t vhos_ble_get_health(vhos_ble_health_t *health);
