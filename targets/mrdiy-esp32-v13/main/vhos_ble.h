#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

esp_err_t vhos_ble_start(const char *device_name, const char *gateway_id);
esp_err_t vhos_ble_wait_ready(TickType_t timeout);
