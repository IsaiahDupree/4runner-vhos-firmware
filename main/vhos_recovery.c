/*
 * VHOS rollback confirmation for WiCAN Pro.
 * Copyright (C) 2026 Isaiah Dupree.
 * GPL-3.0-or-later.
 */

#include "vhos_recovery.h"

#include "ble.h"
#include "can.h"
#include "dev_status.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "VHOS_RECOVERY";

esp_err_t vhos_recovery_confirm_boot(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    if (running == NULL || running->type != ESP_PARTITION_TYPE_APP ||
        update == NULL || update == running) {
        ESP_LOGE(TAG, "A/B application partition self-test failed");
        return ESP_ERR_INVALID_STATE;
    }
    if (!can_is_silent()) {
        ESP_LOGE(TAG, "Listen-only policy self-test failed");
        return ESP_ERR_INVALID_STATE;
    }
    for (uint32_t attempt = 0; attempt < 500 && !ble_vhos_service_ready(); attempt++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!dev_status_is_bit_set(DEV_BLE_ENABLED_BIT) || !ble_vhos_service_ready()) {
        ESP_LOGE(TAG, "BLE service initialization self-test failed");
        return ESP_ERR_INVALID_STATE;
    }

    esp_ota_img_states_t state;
    esp_err_t state_result = esp_ota_get_state_partition(running, &state);
    if (state_result == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGI(TAG, "Running partition does not require rollback confirmation");
        return ESP_OK;
    }
    if (state_result != ESP_OK) {
        ESP_LOGE(TAG, "Unable to read OTA image state: %s", esp_err_to_name(state_result));
        return state_result;
    }
    if (state != ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "Running partition state %d does not require confirmation", state);
        return ESP_OK;
    }

    esp_err_t confirm_result = esp_ota_mark_app_valid_cancel_rollback();
    if (confirm_result != ESP_OK) {
        ESP_LOGE(TAG, "Unable to confirm pending image: %s", esp_err_to_name(confirm_result));
        return confirm_result;
    }
    ESP_LOGI(TAG, "Pending image confirmed after A/B, listen-only, and BLE self-tests");
    return ESP_OK;
}
