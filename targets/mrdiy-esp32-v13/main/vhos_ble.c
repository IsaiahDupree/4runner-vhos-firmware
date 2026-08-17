#include "vhos_ble.h"

#include <assert.h>
#include <string.h>
#include "esp_bt.h"
#include "esp_log.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_id.h"
#include "host/ble_store.h"
#include "host/ble_uuid.h"
#include "nimble/nimble_npl.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "vhos_transport.h"

#define VHOS_BLE_TX_MAX_BYTES 1100U
#define VHOS_BLE_TX_QUEUE_DEPTH 6U
#define VHOS_BLE_NOTIFICATION_PACE_MS 15U
#define VHOS_BLE_MBUF_RETRY_LIMIT 20U
#define VHOS_BLE_CONN_INTERVAL_MIN 24U
#define VHOS_BLE_CONN_INTERVAL_MAX 40U
#define VHOS_BLE_CONN_LATENCY 0U
#define VHOS_BLE_SUPERVISION_TIMEOUT 600U
#define VHOS_BLE_IDENTITY_NAMESPACE "vhos_ble_id"
#define VHOS_BLE_IDENTITY_KEY "identity_v1"

typedef struct {
    size_t length;
    bool health_channel;
    uint8_t data[VHOS_BLE_TX_MAX_BYTES];
} vhos_ble_tx_item_t;

static const char *TAG = "vhos_ble";
static uint8_t own_address_type;
static uint16_t connection_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t command_value_handle;
static uint16_t stream_value_handle;
static uint16_t status_value_handle;
static uint16_t ota_status_value_handle;
static bool stream_notify_enabled;
static bool status_notify_enabled;
static bool link_encrypted;
static bool host_ready;
static bool advertising_active;
static bool connection_parameters_available;
static uint16_t active_att_mtu;
static uint16_t active_connection_interval;
static uint16_t active_connection_latency;
static uint16_t active_supervision_timeout;
static portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t tx_queue;
static SemaphoreHandle_t ready_semaphore;
static struct ble_npl_callout advertising_retry_callout;

static const ble_uuid128_t service_uuid = BLE_UUID128_INIT(
    0x23, 0xf1, 0xb3, 0x12, 0x8f, 0xa1, 0xfa, 0x83,
    0xd1, 0x42, 0xca, 0xff, 0xb3, 0x3e, 0x61, 0x33
);
static const ble_uuid128_t command_uuid = BLE_UUID128_INIT(
    0x0a, 0xfc, 0xa5, 0x47, 0xab, 0xa1, 0xab, 0xa2,
    0x54, 0x4d, 0x44, 0x02, 0x9b, 0x27, 0xd3, 0xb3
);
static const ble_uuid128_t stream_uuid = BLE_UUID128_INIT(
    0xcc, 0x49, 0x1c, 0x41, 0xda, 0x5c, 0xbd, 0xbb,
    0x59, 0x46, 0x00, 0xa6, 0xc0, 0x90, 0x5b, 0x26
);
static const ble_uuid128_t status_uuid = BLE_UUID128_INIT(
    0xa9, 0x41, 0x9b, 0xf1, 0xdf, 0xd2, 0x9b, 0xb6,
    0xb8, 0x49, 0xb4, 0xa9, 0x9a, 0x69, 0xb5, 0xbc
);
static const ble_uuid128_t ota_status_uuid = BLE_UUID128_INIT(
    0x74, 0x58, 0x35, 0xfc, 0xbb, 0x27, 0x3c, 0x92,
    0xb3, 0x4d, 0x90, 0xd1, 0x8e, 0x1f, 0xd2, 0x18
);

static int gatt_access(
    uint16_t conn_handle,
    uint16_t attr_handle,
    struct ble_gatt_access_ctxt *context,
    void *argument
)
{
    (void)conn_handle;
    (void)argument;
    if (context->op == BLE_GATT_ACCESS_OP_WRITE_CHR && attr_handle == command_value_handle) {
        uint16_t length = OS_MBUF_PKTLEN(context->om);
        uint8_t fragment[512];
        if (length == 0 || length > sizeof(fragment)) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        uint16_t flattened = 0;
        int result = ble_hs_mbuf_to_flat(context->om, fragment, sizeof(fragment), &flattened);
        if (result != 0 || flattened != length) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        esp_err_t ingest_result = vhos_transport_ingest(fragment, flattened);
        if (ingest_result != ESP_OK && ingest_result != ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGW(TAG, "Rejected command fragment: %s", esp_err_to_name(ingest_result));
            return BLE_ATT_ERR_UNLIKELY;
        }
        return 0;
    }

    if (context->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &service_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &command_uuid.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP |
                         BLE_GATT_CHR_F_WRITE_ENC,
                .val_handle = &command_value_handle,
            },
            {
                .uuid = &stream_uuid.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC |
                         BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC,
                .val_handle = &stream_value_handle,
            },
            {
                .uuid = &status_uuid.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC |
                         BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC,
                .val_handle = &status_value_handle,
            },
            {
                .uuid = &ota_status_uuid.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC |
                         BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC,
                .val_handle = &ota_status_value_handle,
            },
            {0},
        },
    },
    {0},
};

static esp_err_t emit_frame(const uint8_t *data, size_t length, bool health_channel)
{
    if (data == NULL || length == 0 || length > VHOS_BLE_TX_MAX_BYTES || tx_queue == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    vhos_ble_tx_item_t item = {
        .length = length,
        .health_channel = health_channel,
    };
    memcpy(item.data, data, length);
    return xQueueSend(tx_queue, &item, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

static void tx_task(void *argument)
{
    (void)argument;
    vhos_ble_tx_item_t item;
    while (true) {
        if (xQueueReceive(tx_queue, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        portENTER_CRITICAL(&state_lock);
        bool subscribed = item.health_channel ? status_notify_enabled : stream_notify_enabled;
        uint16_t value_handle = item.health_channel ? status_value_handle : stream_value_handle;
        uint16_t active_connection = connection_handle;
        portEXIT_CRITICAL(&state_lock);
        if (!subscribed || active_connection == BLE_HS_CONN_HANDLE_NONE) {
            continue;
        }

        uint16_t mtu = ble_att_mtu(active_connection);
        size_t maximum_chunk = mtu > 3 ? (size_t)mtu - 3 : 20;
        for (size_t offset = 0; offset < item.length; offset += maximum_chunk) {
            size_t remaining = item.length - offset;
            size_t chunk_length = remaining < maximum_chunk ? remaining : maximum_chunk;
            struct os_mbuf *packet = NULL;
            for (unsigned int attempt = 0; attempt < VHOS_BLE_MBUF_RETRY_LIMIT; ++attempt) {
                packet = os_msys_get_pkthdr(chunk_length, 0);
                if (packet != NULL) {
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            if (packet == NULL) {
                ESP_LOGE(TAG, "Unable to allocate BLE notification packet after retries");
                break;
            }
            if (os_mbuf_append(packet, &item.data[offset], chunk_length) != 0) {
                os_mbuf_free_chain(packet);
                ESP_LOGE(TAG, "Unable to append BLE notification packet");
                break;
            }
            int result = ble_gatts_notify_custom(active_connection, value_handle, packet);
            if (result != 0) {
                ESP_LOGW(TAG, "BLE notify failed: rc=%d", result);
                break;
            }
            /* Keep the controller pool below saturation when ATT MTU is still 23 bytes. */
            vTaskDelay(pdMS_TO_TICKS(VHOS_BLE_NOTIFICATION_PACE_MS));
        }
    }
}

static void health_task(void *argument)
{
    (void)argument;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        portENTER_CRITICAL(&state_lock);
        bool can_send = connection_handle != BLE_HS_CONN_HANDLE_NONE &&
                        link_encrypted && status_notify_enabled;
        portEXIT_CRITICAL(&state_lock);
        if (can_send) {
            esp_err_t result = vhos_transport_send_health();
            if (result != ESP_OK && result != ESP_ERR_NO_MEM) {
                ESP_LOGW(TAG, "Health queue failed: %s", esp_err_to_name(result));
            }
        }
    }
}

static void advertise(void);

static void advertising_retry_event(struct ble_npl_event *event)
{
    (void)event;
    advertise();
}

static void schedule_advertising(void)
{
    if (ble_gap_adv_active() || ble_npl_callout_is_active(&advertising_retry_callout)) {
        return;
    }
    ble_npl_error_t result = ble_npl_callout_reset(
        &advertising_retry_callout,
        ble_npl_time_ms_to_ticks32(250)
    );
    if (result != BLE_NPL_OK) {
        ESP_LOGE(TAG, "Unable to schedule BLE advertising retry: rc=%d", result);
    }
}

static void log_connection_parameters(uint16_t handle, const char *phase)
{
    struct ble_gap_conn_desc description;
    int result = ble_gap_conn_find(handle, &description);
    if (result != 0) {
        ESP_LOGW(TAG, "BLE_CONN_PARAMS_%s unavailable rc=%d", phase, result);
        return;
    }
    portENTER_CRITICAL(&state_lock);
    connection_parameters_available = true;
    active_connection_interval = description.conn_itvl;
    active_connection_latency = description.conn_latency;
    active_supervision_timeout = description.supervision_timeout;
    portEXIT_CRITICAL(&state_lock);
    ESP_LOGI(
        TAG,
        "BLE_CONN_PARAMS_%s interval_units=%u latency=%u supervision_units=%u",
        phase,
        description.conn_itvl,
        description.conn_latency,
        description.supervision_timeout
    );
}

static void request_stable_connection_parameters(uint16_t handle)
{
    const struct ble_gap_upd_params parameters = {
        .itvl_min = VHOS_BLE_CONN_INTERVAL_MIN,
        .itvl_max = VHOS_BLE_CONN_INTERVAL_MAX,
        .latency = VHOS_BLE_CONN_LATENCY,
        .supervision_timeout = VHOS_BLE_SUPERVISION_TIMEOUT,
        .min_ce_len = 0,
        .max_ce_len = 0,
    };
    int result = ble_gap_update_params(handle, &parameters);
    if (result != 0) {
        ESP_LOGW(TAG, "BLE_CONN_PARAMS_REQUEST failed rc=%d", result);
    } else {
        ESP_LOGI(
            TAG,
            "BLE_CONN_PARAMS_REQUEST interval_units=%u-%u latency=%u supervision_units=%u",
            parameters.itvl_min,
            parameters.itvl_max,
            parameters.latency,
            parameters.supervision_timeout
        );
    }
}

static int gap_event(struct ble_gap_event *event, void *argument)
{
    (void)argument;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            portENTER_CRITICAL(&state_lock);
            connection_handle = event->connect.conn_handle;
            link_encrypted = false;
            advertising_active = false;
            connection_parameters_available = false;
            active_att_mtu = 0;
            portEXIT_CRITICAL(&state_lock);
            if (connection_handle <= ESP_BLE_PWR_TYPE_CONN_HDL8) {
                esp_err_t power_result = esp_ble_tx_power_set(
                    (esp_ble_power_type_t)connection_handle,
                    ESP_PWR_LVL_P9
                );
                if (power_result != ESP_OK) {
                    ESP_LOGW(
                        TAG,
                        "Unable to raise connection TX power: %s",
                        esp_err_to_name(power_result)
                    );
                }
            }
            log_connection_parameters(connection_handle, "INITIAL");
            request_stable_connection_parameters(connection_handle);
            ESP_LOGI(TAG, "IPHONE_LINK_CONNECTED handle=%u", connection_handle);
        } else {
            ESP_LOGW(TAG, "BLE connection attempt failed: status=%d", event->connect.status);
            schedule_advertising();
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "IPHONE_LINK_DISCONNECTED reason=%d", event->disconnect.reason);
        portENTER_CRITICAL(&state_lock);
        connection_handle = BLE_HS_CONN_HANDLE_NONE;
        link_encrypted = false;
        stream_notify_enabled = false;
        status_notify_enabled = false;
        connection_parameters_available = false;
        active_att_mtu = 0;
        active_connection_interval = 0;
        active_connection_latency = 0;
        active_supervision_timeout = 0;
        portEXIT_CRITICAL(&state_lock);
        vhos_transport_reset();
        schedule_advertising();
        return 0;
    case BLE_GAP_EVENT_CONN_UPDATE:
        ESP_LOGI(TAG, "BLE_CONN_UPDATE status=%d", event->conn_update.status);
        if (event->conn_update.status == 0) {
            log_connection_parameters(event->conn_update.conn_handle, "ACTIVE");
        }
        return 0;
    case BLE_GAP_EVENT_MTU:
        portENTER_CRITICAL(&state_lock);
        active_att_mtu = event->mtu.value;
        portEXIT_CRITICAL(&state_lock);
        ESP_LOGI(TAG, "BLE_MTU value=%u", event->mtu.value);
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        portENTER_CRITICAL(&state_lock);
        advertising_active = false;
        portEXIT_CRITICAL(&state_lock);
        ESP_LOGW(TAG, "BLE advertising completed: reason=%d", event->adv_complete.reason);
        schedule_advertising();
        return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
        portENTER_CRITICAL(&state_lock);
        if (event->subscribe.attr_handle == stream_value_handle) {
            stream_notify_enabled = event->subscribe.cur_notify;
        } else if (event->subscribe.attr_handle == status_value_handle) {
            status_notify_enabled = event->subscribe.cur_notify;
        }
        portEXIT_CRITICAL(&state_lock);
        ESP_LOGI(
            TAG,
            "BLE_SUBSCRIBE handle=%u notify=%d",
            event->subscribe.attr_handle,
            event->subscribe.cur_notify
        );
        return 0;
    case BLE_GAP_EVENT_ENC_CHANGE:
        portENTER_CRITICAL(&state_lock);
        link_encrypted = event->enc_change.status == 0;
        portEXIT_CRITICAL(&state_lock);
        ESP_LOGI(TAG, "BLE_ENCRYPTION status=%d", event->enc_change.status);
        return 0;
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        struct ble_gap_conn_desc description;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &description) == 0) {
            ble_store_util_delete_peer(&description.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    default:
        return 0;
    }
}

static void advertise(void)
{
    if (ble_gap_adv_active()) {
        return;
    }
    static const uint8_t short_name[] = "VHOS";
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = (ble_uuid128_t *)&service_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    /* Keep an identity fallback in the primary packet, not only the scan response. */
    fields.name = (uint8_t *)short_name;
    fields.name_len = sizeof(short_name) - 1;
    fields.name_is_complete = 0;
    int result = ble_gap_adv_set_fields(&fields);
    if (result != 0) {
        ESP_LOGE(TAG, "Advertising data failed: rc=%d", result);
        return;
    }

    struct ble_hs_adv_fields response = {0};
    const char *name = ble_svc_gap_device_name();
    response.name = (uint8_t *)name;
    response.name_len = strlen(name);
    response.name_is_complete = 1;
    result = ble_gap_adv_rsp_set_fields(&response);
    if (result != 0) {
        ESP_LOGE(TAG, "Scan response failed: rc=%d", result);
        return;
    }

    struct ble_gap_adv_params parameters = {0};
    parameters.conn_mode = BLE_GAP_CONN_MODE_UND;
    parameters.disc_mode = BLE_GAP_DISC_MODE_GEN;
    parameters.itvl_min = 0x0030;
    parameters.itvl_max = 0x0060;
    result = ble_gap_adv_start(
        own_address_type,
        NULL,
        BLE_HS_FOREVER,
        &parameters,
        gap_event,
        NULL
    );
    if (result != 0) {
        portENTER_CRITICAL(&state_lock);
        advertising_active = false;
        portEXIT_CRITICAL(&state_lock);
        ESP_LOGE(TAG, "Advertising start failed: rc=%d", result);
        schedule_advertising();
    } else {
        portENTER_CRITICAL(&state_lock);
        advertising_active = true;
        portEXIT_CRITICAL(&state_lock);
        ESP_LOGI(
            TAG,
            "VHOS_BLE_ADVERTISING name=%s short_name=VHOS interval_units=48-96",
            name
        );
    }
}

static void on_reset(int reason)
{
    portENTER_CRITICAL(&state_lock);
    host_ready = false;
    advertising_active = false;
    connection_handle = BLE_HS_CONN_HANDLE_NONE;
    link_encrypted = false;
    stream_notify_enabled = false;
    status_notify_enabled = false;
    connection_parameters_available = false;
    active_att_mtu = 0;
    portEXIT_CRITICAL(&state_lock);
    ESP_LOGE(TAG, "NimBLE reset: reason=%d", reason);
}

static esp_err_t persist_identity(nvs_handle_t handle, const ble_addr_t *identity)
{
    esp_err_t result = nvs_set_blob(
        handle,
        VHOS_BLE_IDENTITY_KEY,
        identity->val,
        sizeof(identity->val)
    );
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }
    return result;
}

static esp_err_t generate_and_persist_identity(
    nvs_handle_t handle,
    ble_addr_t *identity
)
{
    int host_result = ble_hs_id_gen_rnd(0, identity);
    if (host_result != 0) {
        ESP_LOGE(TAG, "Unable to generate static random BLE identity: rc=%d", host_result);
        return ESP_FAIL;
    }
    esp_err_t result = persist_identity(handle, identity);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Unable to persist BLE identity: %s", esp_err_to_name(result));
    }
    return result;
}

static esp_err_t configure_persistent_identity(void)
{
    nvs_handle_t handle;
    esp_err_t result = nvs_open(
        VHOS_BLE_IDENTITY_NAMESPACE,
        NVS_READWRITE,
        &handle
    );
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Unable to open BLE identity store: %s", esp_err_to_name(result));
        return result;
    }

    ble_addr_t identity = {
        .type = BLE_ADDR_RANDOM,
    };
    size_t identity_length = sizeof(identity.val);
    result = nvs_get_blob(
        handle,
        VHOS_BLE_IDENTITY_KEY,
        identity.val,
        &identity_length
    );
    bool generated = false;
    if (result == ESP_ERR_NVS_NOT_FOUND ||
        result == ESP_ERR_NVS_INVALID_LENGTH ||
        (result == ESP_OK && identity_length != sizeof(identity.val))) {
        ESP_LOGW(
            TAG,
            "BLE identity absent or invalid; creating a new bond epoch: result=%s length=%u",
            esp_err_to_name(result),
            (unsigned int)identity_length
        );
        result = generate_and_persist_identity(handle, &identity);
        generated = result == ESP_OK;
    } else if (result != ESP_OK) {
        ESP_LOGE(TAG, "Unable to load BLE identity: %s", esp_err_to_name(result));
    }

    if (result == ESP_OK) {
        int host_result = ble_hs_id_set_rnd(identity.val);
        if (host_result != 0 && !generated) {
            ESP_LOGW(
                TAG,
                "Persisted BLE identity rejected; rotating bond epoch: rc=%d",
                host_result
            );
            result = generate_and_persist_identity(handle, &identity);
            generated = result == ESP_OK;
            if (result == ESP_OK) {
                host_result = ble_hs_id_set_rnd(identity.val);
            }
        }
        if (result == ESP_OK && host_result != 0) {
            ESP_LOGE(TAG, "Unable to install BLE identity: rc=%d", host_result);
            result = ESP_FAIL;
        }
    }

    nvs_close(handle);
    if (result != ESP_OK) {
        return result;
    }
    ESP_LOGI(
        TAG,
        "BLE_IDENTITY_READY type=random-static source=%s address=%02x:%02x:%02x:%02x:%02x:%02x",
        generated ? "generated" : "persisted",
        identity.val[5],
        identity.val[4],
        identity.val[3],
        identity.val[2],
        identity.val[1],
        identity.val[0]
    );
    return ESP_OK;
}

static void on_sync(void)
{
    esp_err_t identity_result = configure_persistent_identity();
    int result = identity_result == ESP_OK
        ? ble_hs_id_infer_auto(0, &own_address_type)
        : BLE_HS_ESTORE_CAP;
    if (result != 0) {
        ESP_LOGE(
            TAG,
            "BLE identity setup failed: identity=%s host_rc=%d",
            esp_err_to_name(identity_result),
            result
        );
        return;
    }
    portENTER_CRITICAL(&state_lock);
    host_ready = true;
    portEXIT_CRITICAL(&state_lock);
    advertise();
    xSemaphoreGive(ready_semaphore);
}

static void host_task(void *argument)
{
    (void)argument;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_store_config_init(void);

esp_err_t vhos_ble_start(const char *device_name, const char *gateway_id)
{
    if (device_name == NULL || gateway_id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    tx_queue = xQueueCreate(VHOS_BLE_TX_QUEUE_DEPTH, sizeof(vhos_ble_tx_item_t));
    ready_semaphore = xSemaphoreCreateBinary();
    if (tx_queue == NULL || ready_semaphore == NULL) {
        return ESP_ERR_NO_MEM;
    }
    vhos_transport_init(gateway_id, emit_frame);

    esp_err_t result = nimble_port_init();
    if (result != ESP_OK) {
        return result;
    }
    esp_err_t default_power_result = esp_ble_tx_power_set(
        ESP_BLE_PWR_TYPE_DEFAULT,
        ESP_PWR_LVL_P9
    );
    esp_err_t advertising_power_result = esp_ble_tx_power_set(
        ESP_BLE_PWR_TYPE_ADV,
        ESP_PWR_LVL_P9
    );
    if (default_power_result != ESP_OK || advertising_power_result != ESP_OK) {
        ESP_LOGW(
            TAG,
            "BLE TX power configuration incomplete: default=%s advertising=%s",
            esp_err_to_name(default_power_result),
            esp_err_to_name(advertising_power_result)
        );
    } else {
        ESP_LOGI(TAG, "BLE_TX_POWER_READY default_dbm=9 advertising_dbm=9");
    }
    int callout_result = ble_npl_callout_init(
        &advertising_retry_callout,
        nimble_port_get_dflt_eventq(),
        advertising_retry_event,
        NULL
    );
    if (callout_result != 0) {
        ESP_LOGE(TAG, "Unable to initialize BLE advertising retry: rc=%d", callout_result);
        return ESP_FAIL;
    }
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    result = ble_svc_gap_device_name_set(device_name);
    if (result != 0) {
        return ESP_FAIL;
    }
    result = ble_gatts_count_cfg(services);
    if (result == 0) {
        result = ble_gatts_add_svcs(services);
    }
    if (result != 0) {
        return ESP_FAIL;
    }

    ble_store_config_init();
    int our_security_records = 0;
    int peer_security_records = 0;
    int our_store_result = ble_store_util_count(
        BLE_STORE_OBJ_TYPE_OUR_SEC,
        &our_security_records
    );
    int peer_store_result = ble_store_util_count(
        BLE_STORE_OBJ_TYPE_PEER_SEC,
        &peer_security_records
    );
    if (our_store_result == 0 && peer_store_result == 0) {
        ESP_LOGI(
            TAG,
            "BLE_BOND_STORE our_security_records=%d peer_security_records=%d",
            our_security_records,
            peer_security_records
        );
    } else {
        ESP_LOGW(
            TAG,
            "Unable to inspect BLE bond store: our_rc=%d peer_rc=%d",
            our_store_result,
            peer_store_result
        );
    }
    nimble_port_freertos_init(host_task);
    if (xTaskCreate(tx_task, "vhos_ble_tx", 4096, NULL, 6, NULL) != pdPASS ||
        xTaskCreate(health_task, "vhos_health", 6144, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t vhos_ble_wait_ready(TickType_t timeout)
{
    if (ready_semaphore == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return xSemaphoreTake(ready_semaphore, timeout) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t vhos_ble_get_health(vhos_ble_health_t *health)
{
    if (health == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&state_lock);
    health->ready = host_ready;
    health->advertising = advertising_active;
    health->connected = connection_handle != BLE_HS_CONN_HANDLE_NONE;
    health->encrypted = link_encrypted;
    health->stream_subscribed = stream_notify_enabled;
    health->health_subscribed = status_notify_enabled;
    health->connection_parameters_available = connection_parameters_available;
    health->att_mtu = active_att_mtu;
    health->connection_interval_units = active_connection_interval;
    health->connection_latency = active_connection_latency;
    health->supervision_timeout_units = active_supervision_timeout;
    portEXIT_CRITICAL(&state_lock);
    return ESP_OK;
}
