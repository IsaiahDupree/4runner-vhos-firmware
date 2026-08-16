#include "vhos_ble.h"

#include <assert.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "vhos_transport.h"

#define VHOS_BLE_TX_MAX_BYTES 1100U
#define VHOS_BLE_TX_QUEUE_DEPTH 6U

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
static QueueHandle_t tx_queue;
static SemaphoreHandle_t ready_semaphore;

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
                         BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &stream_value_handle,
            },
            {
                .uuid = &status_uuid.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC |
                         BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &status_value_handle,
            },
            {
                .uuid = &ota_status_uuid.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC |
                         BLE_GATT_CHR_F_NOTIFY,
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
        bool subscribed = item.health_channel ? status_notify_enabled : stream_notify_enabled;
        uint16_t value_handle = item.health_channel ? status_value_handle : stream_value_handle;
        uint16_t active_connection = connection_handle;
        if (!subscribed || active_connection == BLE_HS_CONN_HANDLE_NONE) {
            continue;
        }

        uint16_t mtu = ble_att_mtu(active_connection);
        size_t maximum_chunk = mtu > 3 ? (size_t)mtu - 3 : 20;
        for (size_t offset = 0; offset < item.length; offset += maximum_chunk) {
            size_t remaining = item.length - offset;
            size_t chunk_length = remaining < maximum_chunk ? remaining : maximum_chunk;
            struct os_mbuf *packet = os_msys_get_pkthdr(chunk_length, 0);
            if (packet == NULL || os_mbuf_append(packet, &item.data[offset], chunk_length) != 0) {
                if (packet != NULL) {
                    os_mbuf_free_chain(packet);
                }
                ESP_LOGE(TAG, "Unable to allocate BLE notification packet");
                break;
            }
            int result = ble_gatts_notify_custom(active_connection, value_handle, packet);
            if (result != 0) {
                ESP_LOGW(TAG, "BLE notify failed: rc=%d", result);
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    }
}

static void health_task(void *argument)
{
    (void)argument;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        if (connection_handle != BLE_HS_CONN_HANDLE_NONE && status_notify_enabled) {
            esp_err_t result = vhos_transport_send_health();
            if (result != ESP_OK && result != ESP_ERR_NO_MEM) {
                ESP_LOGW(TAG, "Health queue failed: %s", esp_err_to_name(result));
            }
        }
    }
}

static void advertise(void);

static int gap_event(struct ble_gap_event *event, void *argument)
{
    (void)argument;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            connection_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "IPHONE_LINK_CONNECTED handle=%u", connection_handle);
        } else {
            advertise();
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "IPHONE_LINK_DISCONNECTED reason=%d", event->disconnect.reason);
        connection_handle = BLE_HS_CONN_HANDLE_NONE;
        stream_notify_enabled = false;
        status_notify_enabled = false;
        vhos_transport_reset();
        advertise();
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == stream_value_handle) {
            stream_notify_enabled = event->subscribe.cur_notify;
        } else if (event->subscribe.attr_handle == status_value_handle) {
            status_notify_enabled = event->subscribe.cur_notify;
        }
        ESP_LOGI(
            TAG,
            "BLE_SUBSCRIBE handle=%u notify=%d",
            event->subscribe.attr_handle,
            event->subscribe.cur_notify
        );
        return 0;
    case BLE_GAP_EVENT_ENC_CHANGE:
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
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = (ble_uuid128_t *)&service_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
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
    result = ble_gap_adv_start(
        own_address_type,
        NULL,
        BLE_HS_FOREVER,
        &parameters,
        gap_event,
        NULL
    );
    if (result != 0) {
        ESP_LOGE(TAG, "Advertising start failed: rc=%d", result);
    } else {
        ESP_LOGI(TAG, "VHOS_BLE_ADVERTISING name=%s", name);
    }
}

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "NimBLE reset: reason=%d", reason);
}

static void on_sync(void)
{
    int result = ble_hs_util_ensure_addr(0);
    if (result == 0) {
        result = ble_hs_id_infer_auto(0, &own_address_type);
    }
    if (result != 0) {
        ESP_LOGE(TAG, "BLE address setup failed: rc=%d", result);
        return;
    }
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
    nimble_port_freertos_init(host_task);
    if (xTaskCreate(tx_task, "vhos_ble_tx", 4096, NULL, 6, NULL) != pdPASS ||
        xTaskCreate(health_task, "vhos_health", 3072, NULL, 5, NULL) != pdPASS) {
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
