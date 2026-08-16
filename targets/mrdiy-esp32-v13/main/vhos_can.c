#include "vhos_can.h"

#include "driver/gpio.h"
#include "driver/twai.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define VHOS_CAN_RX_GPIO GPIO_NUM_4
#define VHOS_CAN_TX_GPIO GPIO_NUM_5
#define VHOS_CAN_BITRATE_BPS 500000U

static const char *TAG = "vhos_can";
static portMUX_TYPE metrics_lock = portMUX_INITIALIZER_UNLOCKED;
static uint64_t received_frames;
static uint64_t bus_off_count;
static bool bus_off_observed;
static bool controller_running;

static void receive_task(void *argument)
{
    (void)argument;
    twai_message_t message = {0};
    while (true) {
        if (twai_receive(&message, pdMS_TO_TICKS(1000)) == ESP_OK) {
            portENTER_CRITICAL(&metrics_lock);
            received_frames++;
            portEXIT_CRITICAL(&metrics_lock);
        }
    }
}

esp_err_t vhos_can_start(void)
{
    twai_general_config_t general = TWAI_GENERAL_CONFIG_DEFAULT(
        VHOS_CAN_TX_GPIO,
        VHOS_CAN_RX_GPIO,
        TWAI_MODE_LISTEN_ONLY
    );
    general.tx_queue_len = 0;
    general.rx_queue_len = 128;
    general.alerts_enabled = TWAI_ALERT_BUS_OFF | TWAI_ALERT_BUS_ERROR |
                             TWAI_ALERT_RX_QUEUE_FULL;

    twai_timing_config_t timing = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t filter = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    esp_err_t result = twai_driver_install(&general, &timing, &filter);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "TWAI install failed: %s", esp_err_to_name(result));
        return result;
    }

    result = twai_start();
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "TWAI start failed: %s", esp_err_to_name(result));
        twai_driver_uninstall();
        return result;
    }

    controller_running = true;
    BaseType_t task_result = xTaskCreate(
        receive_task,
        "vhos_can_rx",
        3072,
        NULL,
        8,
        NULL
    );
    if (task_result != pdPASS) {
        controller_running = false;
        twai_stop();
        twai_driver_uninstall();
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(
        TAG,
        "PASSIVE_CAN_READY mode=listen-only bitrate=%u rx_gpio=%d tx_gpio=%d",
        VHOS_CAN_BITRATE_BPS,
        VHOS_CAN_RX_GPIO,
        VHOS_CAN_TX_GPIO
    );
    return ESP_OK;
}

esp_err_t vhos_can_get_health(vhos_can_health_t *health)
{
    if (health == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    twai_status_info_t status = {0};
    esp_err_t result = controller_running
        ? twai_get_status_info(&status)
        : ESP_ERR_INVALID_STATE;

    portENTER_CRITICAL(&metrics_lock);
    health->received_frames = received_frames;
    if (result == ESP_OK) {
        bool bus_off_now = status.state == TWAI_STATE_BUS_OFF;
        if (bus_off_now && !bus_off_observed) {
            bus_off_count++;
        }
        bus_off_observed = bus_off_now;
        health->dropped_frames = (uint64_t)status.rx_missed_count + status.rx_overrun_count;
        health->bus_error_count = status.bus_error_count;
    } else {
        bus_off_observed = false;
        health->dropped_frames = 0;
        health->bus_error_count = 0;
    }
    health->bus_off_count = bus_off_count;
    portEXIT_CRITICAL(&metrics_lock);

    health->listen_only = true;
    health->controller_running = controller_running && result == ESP_OK;
    health->bitrate_bps = VHOS_CAN_BITRATE_BPS;
    return result;
}
