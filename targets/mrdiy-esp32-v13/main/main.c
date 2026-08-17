#include <stdio.h>
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"
#include "vhos_ble.h"
#include "vhos_can.h"
#include "vhos_capture_store.h"
#include "vhos_ota_wifi.h"
#include "vhos_status_web.h"
#include "vhos_transport.h"

static const char *TAG = "vhos_main";

#ifndef VHOS_BUILD_ID
#define VHOS_BUILD_ID "source-tree"
#endif

static void confirm_running_image(esp_err_t capture_result)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        if (capture_result != ESP_OK) {
            ESP_LOGE(
                TAG,
                "OTA_ROLLBACK_SELF_TEST_FAIL partition=%s capture_store=%s",
                running->label,
                esp_err_to_name(capture_result)
            );
            ESP_ERROR_CHECK(esp_ota_mark_app_invalid_rollback_and_reboot());
        }
        ESP_ERROR_CHECK(esp_ota_mark_app_valid_cancel_rollback());
        ESP_LOGI(TAG, "OTA_ROLLBACK_SELF_TEST_PASS partition=%s", running->label);
    }
}

void app_main(void)
{
    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        result = nvs_flash_init();
    }
    ESP_ERROR_CHECK(result);

    uint8_t mac[6] = {0};
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
    char gateway_id[40];
    char device_name[24];
    snprintf(
        gateway_id,
        sizeof(gateway_id),
        "esp32-%02x%02x%02x%02x%02x%02x",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]
    );
    snprintf(
        device_name,
        sizeof(device_name),
        "VHOS-MRDIY-%02X%02X%02X",
        mac[3], mac[4], mac[5]
    );

    esp_err_t capture_result = vhos_capture_store_start();
    if (capture_result != ESP_OK) {
        ESP_LOGE(TAG, "CAPTURE_STORE_SELF_TEST_FAIL reason=%s", esp_err_to_name(capture_result));
    }
    ESP_ERROR_CHECK(vhos_can_start());
    ESP_ERROR_CHECK(vhos_ble_start(device_name, gateway_id));

    /* vhos_ble_start installs the transport emitter before this readiness gate. */
    ESP_ERROR_CHECK(vhos_ble_wait_ready(pdMS_TO_TICKS(5000)));
#ifdef CONFIG_VHOS_STATUS_SOFTAP_AUTOSTART
    ESP_ERROR_CHECK(vhos_status_web_start(gateway_id));
    const char *softap_status = "ready";
#else
    const char *softap_status = "disabled-explicit-activation-required";
    ESP_LOGI(
        TAG,
        "VHOS_SOFTAP_DISABLED reason=default-safe-policy activation=encrypted-ble-pending"
    );
#endif
    confirm_running_image(capture_result);
    vhos_ota_wifi_reconcile_boot();

    const esp_app_desc_t *description = esp_app_get_description();
    ESP_LOGI(
        TAG,
        "VHOS_SELF_TEST_PASS firmware=%s build=%s gateway=%s target=mrdiy-v1.3+ softap_status=%s vehicle_bus_read_only=true",
        description->version,
        VHOS_BUILD_ID,
        gateway_id,
        softap_status
    );
}
