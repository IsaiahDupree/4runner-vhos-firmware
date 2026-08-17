#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define VHOS_OTA_SESSION_WINDOW_SECONDS 300U
#define VHOS_OTA_MAX_IMAGE_BYTES 0x180000U

typedef struct {
    char package_id[40];
    char firmware_version[32];
    char firmware_sha256[65];
    uint32_t firmware_size_bytes;
} vhos_ota_activation_request_t;

typedef esp_err_t (*vhos_ota_status_emit_fn)(const char *json);

void vhos_ota_wifi_init(const char *gateway_id, vhos_ota_status_emit_fn emit_status);
esp_err_t vhos_ota_wifi_activate(const vhos_ota_activation_request_t *request);
esp_err_t vhos_ota_wifi_cancel(void);
bool vhos_ota_wifi_active(void);
void vhos_ota_wifi_reconcile_boot(void);
esp_err_t vhos_ota_wifi_send_last_status(void);
