#include "vhos_can.h"

#include <string.h>
#include "driver/gpio.h"
#include "driver/twai.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define VHOS_CAN_RX_GPIO GPIO_NUM_4
#define VHOS_CAN_TX_GPIO GPIO_NUM_5
#define VHOS_CAN_BITRATE_500K_BPS 500000U
#define VHOS_CAN_BITRATE_250K_BPS 250000U
#define VHOS_CAN_PROBE_WINDOW_MS 10000U
#define VHOS_CAN_LOCK_MINIMUM_FRAMES 3U
#define VHOS_CAN_RECEIVE_POLL_MS 250U
#define VHOS_CAN_MAX_OBSERVERS 3U
#define VHOS_CAN_TWAI_RX_QUEUE_DEPTH 512U
#define VHOS_CAN_OBSERVATION_QUEUE_DEPTH 256U
#define VHOS_CAN_RX_BATCH_LIMIT 64U
#define VHOS_CAN_RX_TASK_STACK_BYTES 3072U
#define VHOS_CAN_DISPATCH_TASK_STACK_BYTES 4096U
#define VHOS_CAN_RX_TASK_PRIORITY 12U
#define VHOS_CAN_DISPATCH_TASK_PRIORITY 9U

#if CONFIG_FREERTOS_UNICORE
#define VHOS_CAN_RX_TASK_CORE tskNO_AFFINITY
#define VHOS_CAN_DISPATCH_TASK_CORE tskNO_AFFINITY
#else
#define VHOS_CAN_RX_TASK_CORE 1
#define VHOS_CAN_DISPATCH_TASK_CORE 0
#endif

typedef struct {
    vhos_can_observer_fn function;
    void *context;
} vhos_can_observer_t;

static const char *TAG = "vhos_can";
static portMUX_TYPE metrics_lock = portMUX_INITIALIZER_UNLOCKED;
static uint64_t received_frames;
static uint64_t standard_frames;
static uint64_t extended_frames;
static uint64_t frames_500k;
static uint64_t frames_250k;
static uint64_t candidate_standard_frames;
static uint64_t candidate_extended_frames;
static uint64_t completed_twai_missed_frames;
static uint64_t completed_twai_overrun_frames;
static uint64_t observer_queue_dropped_frames;
static uint32_t observer_queue_high_water;
static uint64_t completed_bus_error_count;
static uint64_t bus_off_count;
static bool bus_off_observed;
static bool controller_running;
static bool passive_lock;
static uint32_t current_bitrate_bps = VHOS_CAN_BITRATE_500K_BPS;
static uint32_t scan_cycles;
static vhos_can_scan_state_t scan_state = VHOS_CAN_SCAN_PROBING_500K;
static SemaphoreHandle_t controller_lock;
static QueueHandle_t observation_queue;
static portMUX_TYPE observer_lock = portMUX_INITIALIZER_UNLOCKED;
static vhos_can_observer_t observers[VHOS_CAN_MAX_OBSERVERS];
static uint64_t source_sequence;

static void dispatch_observation(const vhos_can_observation_t *observation)
{
    vhos_can_observer_t snapshot[VHOS_CAN_MAX_OBSERVERS];
    portENTER_CRITICAL(&observer_lock);
    memcpy(snapshot, observers, sizeof(snapshot));
    portEXIT_CRITICAL(&observer_lock);
    for (size_t index = 0; index < VHOS_CAN_MAX_OBSERVERS; index++) {
        if (snapshot[index].function != NULL) {
            snapshot[index].function(observation, snapshot[index].context);
        }
    }
}

static void observation_dispatch_task(void *argument)
{
    (void)argument;
    vhos_can_observation_t observation = {0};
    while (true) {
        if (xQueueReceive(observation_queue, &observation, portMAX_DELAY) == pdTRUE) {
            dispatch_observation(&observation);
        }
    }
}

static void enqueue_observation(const twai_message_t *message, uint32_t bitrate_bps)
{
    vhos_can_observation_t observation = {
        .source_sequence = ++source_sequence,
        .monotonic_us = (uint64_t)esp_timer_get_time(),
        .identifier = message->identifier,
        .bitrate_bps = bitrate_bps,
        .data_length = message->data_length_code > 8 ? 8 : message->data_length_code,
        .extended = message->extd,
        .remote_request = message->rtr,
        .listen_only = true,
    };
    memcpy(observation.data, message->data, observation.data_length);

    if (observation_queue == NULL ||
        xQueueSend(observation_queue, &observation, 0) != pdTRUE) {
        portENTER_CRITICAL(&metrics_lock);
        observer_queue_dropped_frames++;
        portEXIT_CRITICAL(&metrics_lock);
        return;
    }
    UBaseType_t depth = uxQueueMessagesWaiting(observation_queue);
    portENTER_CRITICAL(&metrics_lock);
    if (depth > observer_queue_high_water) {
        observer_queue_high_water = (uint32_t)depth;
    }
    portEXIT_CRITICAL(&metrics_lock);
}

static twai_timing_config_t timing_for_bitrate(uint32_t bitrate_bps)
{
    if (bitrate_bps == VHOS_CAN_BITRATE_250K_BPS) {
        return (twai_timing_config_t)TWAI_TIMING_CONFIG_250KBITS();
    }
    return (twai_timing_config_t)TWAI_TIMING_CONFIG_500KBITS();
}

static esp_err_t install_controller(uint32_t bitrate_bps)
{
    twai_general_config_t general = TWAI_GENERAL_CONFIG_DEFAULT(
        VHOS_CAN_TX_GPIO,
        VHOS_CAN_RX_GPIO,
        TWAI_MODE_LISTEN_ONLY
    );
    general.tx_queue_len = 0;
    general.rx_queue_len = VHOS_CAN_TWAI_RX_QUEUE_DEPTH;
    general.alerts_enabled = TWAI_ALERT_BUS_OFF | TWAI_ALERT_BUS_ERROR |
                             TWAI_ALERT_RX_QUEUE_FULL;

    twai_timing_config_t timing = timing_for_bitrate(bitrate_bps);
    twai_filter_config_t filter = TWAI_FILTER_CONFIG_ACCEPT_ALL();
    esp_err_t result = twai_driver_install(&general, &timing, &filter);
    if (result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "TWAI install failed at %u bit/s: %s",
            bitrate_bps,
            esp_err_to_name(result)
        );
        return result;
    }

    result = twai_start();
    if (result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "TWAI start failed at %u bit/s: %s",
            bitrate_bps,
            esp_err_to_name(result)
        );
        twai_driver_uninstall();
        return result;
    }

    portENTER_CRITICAL(&metrics_lock);
    current_bitrate_bps = bitrate_bps;
    controller_running = true;
    bus_off_observed = false;
    portEXIT_CRITICAL(&metrics_lock);
    return ESP_OK;
}

static void accumulate_controller_status(void)
{
    twai_status_info_t status = {0};
    if (twai_get_status_info(&status) != ESP_OK) {
        return;
    }
    portENTER_CRITICAL(&metrics_lock);
    completed_twai_missed_frames += status.rx_missed_count;
    completed_twai_overrun_frames += status.rx_overrun_count;
    completed_bus_error_count += status.bus_error_count;
    bool bus_off_now = status.state == TWAI_STATE_BUS_OFF;
    if (bus_off_now && !bus_off_observed) {
        bus_off_count++;
    }
    bus_off_observed = bus_off_now;
    portEXIT_CRITICAL(&metrics_lock);
}

static esp_err_t switch_controller(uint32_t bitrate_bps)
{
    if (xSemaphoreTake(controller_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    accumulate_controller_status();
    portENTER_CRITICAL(&metrics_lock);
    controller_running = false;
    bus_off_observed = false;
    portEXIT_CRITICAL(&metrics_lock);
    esp_err_t stop_result = twai_stop();
    esp_err_t uninstall_result = twai_driver_uninstall();
    if (stop_result != ESP_OK && stop_result != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "TWAI stop before bitrate switch: %s", esp_err_to_name(stop_result));
    }
    esp_err_t result = ESP_OK;
    if (uninstall_result != ESP_OK && uninstall_result != ESP_ERR_INVALID_STATE) {
        result = uninstall_result;
    } else {
        /*
         * ESP_ERR_INVALID_STATE means no driver is installed. Treat that as a
         * recoverable starting point so a single failed reinstall cannot leave
         * the passive probe permanently unable to retry.
         */
        result = install_controller(bitrate_bps);
    }
    xSemaphoreGive(controller_lock);
    return result;
}

static void set_probe_state(uint32_t bitrate_bps)
{
    portENTER_CRITICAL(&metrics_lock);
    passive_lock = false;
    scan_state = bitrate_bps == VHOS_CAN_BITRATE_500K_BPS
        ? VHOS_CAN_SCAN_PROBING_500K
        : VHOS_CAN_SCAN_PROBING_250K;
    portEXIT_CRITICAL(&metrics_lock);
}

static void lock_probe(
    uint32_t bitrate_bps,
    uint64_t phase_frames,
    uint64_t phase_standard_frames,
    uint64_t phase_extended_frames
)
{
    portENTER_CRITICAL(&metrics_lock);
    passive_lock = true;
    scan_state = bitrate_bps == VHOS_CAN_BITRATE_500K_BPS
        ? VHOS_CAN_SCAN_LOCKED_500K
        : VHOS_CAN_SCAN_LOCKED_250K;
    candidate_standard_frames = phase_standard_frames;
    candidate_extended_frames = phase_extended_frames;
    portEXIT_CRITICAL(&metrics_lock);
    ESP_LOGI(
        TAG,
        "PASSIVE_CAN_LOCK bitrate=%u phase_frames=%llu standard_frames=%llu extended_frames=%llu",
        bitrate_bps,
        (unsigned long long)phase_frames,
        (unsigned long long)phase_standard_frames,
        (unsigned long long)phase_extended_frames
    );
}

static void record_received_message(const twai_message_t *message)
{
    uint32_t observation_bitrate;
    portENTER_CRITICAL(&metrics_lock);
    received_frames++;
    if (message->extd) {
        extended_frames++;
    } else {
        standard_frames++;
    }
    if (current_bitrate_bps == VHOS_CAN_BITRATE_500K_BPS) {
        frames_500k++;
    } else {
        frames_250k++;
    }
    observation_bitrate = current_bitrate_bps;
    portEXIT_CRITICAL(&metrics_lock);
    enqueue_observation(message, observation_bitrate);
}

static void receive_task(void *argument)
{
    (void)argument;
    twai_message_t message = {0};
    uint64_t phase_frame_baseline = 0;
    uint64_t phase_standard_baseline = 0;
    uint64_t phase_extended_baseline = 0;
    TickType_t phase_started = xTaskGetTickCount();
    while (true) {
        if (twai_receive(&message, pdMS_TO_TICKS(VHOS_CAN_RECEIVE_POLL_MS)) == ESP_OK) {
            uint32_t batch_count = 0;
            do {
                record_received_message(&message);
                batch_count++;
            } while (batch_count < VHOS_CAN_RX_BATCH_LIMIT &&
                     twai_receive(&message, 0) == ESP_OK);
        }

        uint64_t total_frames;
        uint64_t total_standard_frames;
        uint64_t total_extended_frames;
        uint32_t bitrate_bps;
        bool locked;
        portENTER_CRITICAL(&metrics_lock);
        total_frames = received_frames;
        total_standard_frames = standard_frames;
        total_extended_frames = extended_frames;
        bitrate_bps = current_bitrate_bps;
        locked = passive_lock;
        portEXIT_CRITICAL(&metrics_lock);
        uint64_t phase_frames = total_frames - phase_frame_baseline;
        uint64_t phase_standard_frames = total_standard_frames - phase_standard_baseline;
        uint64_t phase_extended_frames = total_extended_frames - phase_extended_baseline;
        if (!locked && phase_frames >= VHOS_CAN_LOCK_MINIMUM_FRAMES) {
            lock_probe(
                bitrate_bps,
                phase_frames,
                phase_standard_frames,
                phase_extended_frames
            );
            continue;
        }
        if (locked ||
            xTaskGetTickCount() - phase_started < pdMS_TO_TICKS(VHOS_CAN_PROBE_WINDOW_MS)) {
            continue;
        }

        uint32_t next_bitrate = bitrate_bps == VHOS_CAN_BITRATE_500K_BPS
            ? VHOS_CAN_BITRATE_250K_BPS
            : VHOS_CAN_BITRATE_500K_BPS;
        ESP_LOGI(
            TAG,
            "PASSIVE_CAN_PROBE_SWITCH from_bitrate=%u to_bitrate=%u reason=no-valid-frames window_ms=%u",
            bitrate_bps,
            next_bitrate,
            VHOS_CAN_PROBE_WINDOW_MS
        );
        set_probe_state(next_bitrate);
        esp_err_t result = switch_controller(next_bitrate);
        if (result != ESP_OK) {
            portENTER_CRITICAL(&metrics_lock);
            scan_state = VHOS_CAN_SCAN_ERROR;
            portEXIT_CRITICAL(&metrics_lock);
            ESP_LOGE(TAG, "Passive CAN probe switch failed: %s", esp_err_to_name(result));
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        portENTER_CRITICAL(&metrics_lock);
        scan_cycles++;
        phase_frame_baseline = received_frames;
        phase_standard_baseline = standard_frames;
        phase_extended_baseline = extended_frames;
        portEXIT_CRITICAL(&metrics_lock);
        phase_started = xTaskGetTickCount();
    }
}

esp_err_t vhos_can_register_observer(vhos_can_observer_fn observer, void *context)
{
    if (observer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t result = ESP_ERR_NO_MEM;
    portENTER_CRITICAL(&observer_lock);
    for (size_t index = 0; index < VHOS_CAN_MAX_OBSERVERS; index++) {
        if (observers[index].function == observer && observers[index].context == context) {
            result = ESP_OK;
            break;
        }
        if (observers[index].function == NULL) {
            observers[index].function = observer;
            observers[index].context = context;
            result = ESP_OK;
            break;
        }
    }
    portEXIT_CRITICAL(&observer_lock);
    return result;
}

esp_err_t vhos_can_start(void)
{
    controller_lock = xSemaphoreCreateMutex();
    if (controller_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    observation_queue = xQueueCreate(
        VHOS_CAN_OBSERVATION_QUEUE_DEPTH,
        sizeof(vhos_can_observation_t)
    );
    if (observation_queue == NULL) {
        vSemaphoreDelete(controller_lock);
        controller_lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    esp_err_t result = install_controller(VHOS_CAN_BITRATE_500K_BPS);
    if (result != ESP_OK) {
        vQueueDelete(observation_queue);
        observation_queue = NULL;
        vSemaphoreDelete(controller_lock);
        controller_lock = NULL;
        return result;
    }
    TaskHandle_t dispatch_task_handle = NULL;
    BaseType_t task_result = xTaskCreatePinnedToCore(
        observation_dispatch_task,
        "vhos_can_dispatch",
        VHOS_CAN_DISPATCH_TASK_STACK_BYTES,
        NULL,
        VHOS_CAN_DISPATCH_TASK_PRIORITY,
        &dispatch_task_handle,
        VHOS_CAN_DISPATCH_TASK_CORE
    );
    if (task_result == pdPASS) {
        task_result = xTaskCreatePinnedToCore(
        receive_task,
        "vhos_can_rx",
        VHOS_CAN_RX_TASK_STACK_BYTES,
        NULL,
        VHOS_CAN_RX_TASK_PRIORITY,
        NULL,
        VHOS_CAN_RX_TASK_CORE
        );
    }
    if (task_result != pdPASS) {
        if (dispatch_task_handle != NULL) {
            vTaskDelete(dispatch_task_handle);
        }
        xSemaphoreTake(controller_lock, portMAX_DELAY);
        portENTER_CRITICAL(&metrics_lock);
        controller_running = false;
        scan_state = VHOS_CAN_SCAN_ERROR;
        portEXIT_CRITICAL(&metrics_lock);
        twai_stop();
        twai_driver_uninstall();
        xSemaphoreGive(controller_lock);
        vQueueDelete(observation_queue);
        observation_queue = NULL;
        vSemaphoreDelete(controller_lock);
        controller_lock = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(
        TAG,
        "PASSIVE_CAN_READY mode=listen-only initial_bitrate=%u probe_window_ms=%u lock_minimum_frames=%u twai_rx_queue=%u observer_queue=%u rx_priority=%u dispatch_priority=%u rx_gpio=%d tx_gpio=%d",
        VHOS_CAN_BITRATE_500K_BPS,
        VHOS_CAN_PROBE_WINDOW_MS,
        VHOS_CAN_LOCK_MINIMUM_FRAMES,
        VHOS_CAN_TWAI_RX_QUEUE_DEPTH,
        VHOS_CAN_OBSERVATION_QUEUE_DEPTH,
        VHOS_CAN_RX_TASK_PRIORITY,
        VHOS_CAN_DISPATCH_TASK_PRIORITY,
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

    if (controller_lock == NULL ||
        xSemaphoreTake(controller_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }

    bool running;
    portENTER_CRITICAL(&metrics_lock);
    running = controller_running;
    portEXIT_CRITICAL(&metrics_lock);
    twai_status_info_t status = {0};
    esp_err_t result = running ? twai_get_status_info(&status) : ESP_ERR_INVALID_STATE;
    UBaseType_t observer_depth = observation_queue == NULL
        ? 0
        : uxQueueMessagesWaiting(observation_queue);

    portENTER_CRITICAL(&metrics_lock);
    health->received_frames = received_frames;
    health->standard_frames = standard_frames;
    health->extended_frames = extended_frames;
    health->frames_500k = frames_500k;
    health->frames_250k = frames_250k;
    health->candidate_standard_frames = candidate_standard_frames;
    health->candidate_extended_frames = candidate_extended_frames;
    health->observer_queue_dropped_frames = observer_queue_dropped_frames;
    health->observer_queue_depth = (uint32_t)observer_depth;
    health->observer_queue_high_water = observer_queue_high_water;
    health->observer_queue_capacity = VHOS_CAN_OBSERVATION_QUEUE_DEPTH;
    health->twai_receive_queue_capacity = VHOS_CAN_TWAI_RX_QUEUE_DEPTH;
    health->scan_cycles = scan_cycles;
    if (result == ESP_OK) {
        bool bus_off_now = status.state == TWAI_STATE_BUS_OFF;
        if (bus_off_now && !bus_off_observed) {
            bus_off_count++;
        }
        bus_off_observed = bus_off_now;
        health->twai_receive_missed_frames = completed_twai_missed_frames +
                                             status.rx_missed_count;
        health->twai_receive_overrun_frames = completed_twai_overrun_frames +
                                              status.rx_overrun_count;
        health->twai_receive_queue_depth = status.msgs_to_rx;
        health->bus_error_count = completed_bus_error_count + status.bus_error_count;
    } else {
        bus_off_observed = false;
        health->twai_receive_missed_frames = completed_twai_missed_frames;
        health->twai_receive_overrun_frames = completed_twai_overrun_frames;
        health->twai_receive_queue_depth = 0;
        health->bus_error_count = completed_bus_error_count;
    }
    health->dropped_frames = health->twai_receive_missed_frames +
                             health->twai_receive_overrun_frames +
                             health->observer_queue_dropped_frames;
    health->bus_off_count = bus_off_count;
    health->controller_running = controller_running && result == ESP_OK;
    health->passive_lock = passive_lock;
    health->bitrate_bps = current_bitrate_bps;
    health->scan_state = scan_state;
    portEXIT_CRITICAL(&metrics_lock);
    xSemaphoreGive(controller_lock);

    health->listen_only = true;
    return result;
}

const char *vhos_can_scan_state_name(vhos_can_scan_state_t state)
{
    switch (state) {
    case VHOS_CAN_SCAN_PROBING_500K:
        return "PROBING_500K";
    case VHOS_CAN_SCAN_PROBING_250K:
        return "PROBING_250K";
    case VHOS_CAN_SCAN_LOCKED_500K:
        return "LOCKED_500K";
    case VHOS_CAN_SCAN_LOCKED_250K:
        return "LOCKED_250K";
    case VHOS_CAN_SCAN_ERROR:
    default:
        return "ERROR";
    }
}

const char *vhos_can_passive_candidate(const vhos_can_health_t *health)
{
    if (health == NULL || !health->passive_lock) {
        return NULL;
    }
    const char *format = health->candidate_standard_frames > 0 &&
                         health->candidate_extended_frames > 0
        ? "MIXED"
        : (health->candidate_extended_frames > 0 ? "29" : "11");
    if (health->bitrate_bps == VHOS_CAN_BITRATE_250K_BPS) {
        if (strcmp(format, "MIXED") == 0) {
            return "CAN_MIXED_250";
        }
        return strcmp(format, "29") == 0 ? "CAN_29_250" : "CAN_11_250";
    }
    if (strcmp(format, "MIXED") == 0) {
        return "CAN_MIXED_500";
    }
    return strcmp(format, "29") == 0 ? "CAN_29_500" : "CAN_11_500";
}
