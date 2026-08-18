#include "vhos_ble.h"

#include <assert.h>
#include <string.h>
#include "esp_bt.h"
#include "esp_log.h"
#include "esp_system.h"
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

#define VHOS_BLE_TX_MAX_BYTES 1600U
#define VHOS_BLE_FRAME_HEADER_BYTES 36U
#define VHOS_BLE_FRAME_MESSAGE_HANDSHAKE 1U
#define VHOS_BLE_TX_QUEUE_DEPTH 6U
#define VHOS_BLE_NOTIFICATION_PACE_MS 15U
#define VHOS_BLE_MBUF_RETRY_LIMIT 20U
#define VHOS_BLE_SECURITY_START_DELAY_MS 150U
#define VHOS_BLE_CONN_INTERVAL_MIN 24U
#define VHOS_BLE_CONN_INTERVAL_MAX 40U
#define VHOS_BLE_CONN_LATENCY 0U
#define VHOS_BLE_SUPERVISION_TIMEOUT 600U
#define VHOS_BLE_IDENTITY_NAMESPACE "vhos_ble_id"
#define VHOS_BLE_IDENTITY_KEY "identity_v1"
#define VHOS_BLE_GATT_SCHEMA_KEY "gatt_schema"
#define VHOS_BLE_GATT_SCHEMA_VERSION 6U
#define VHOS_BLE_GATT_SCHEMA_BOND_COMPATIBLE_VERSION 2U
#define VHOS_BLE_BOND_POLICY_KEY "bond_policy"
#define VHOS_BLE_BONDED_ONCE_KEY "paired_once"
#define VHOS_BLE_ROTATE_PENDING_KEY "rotate_pending"
#define VHOS_BLE_BOND_POLICY_VERSION 1U

typedef struct {
    size_t length;
    vhos_transport_channel_t channel;
    vhos_transport_emit_scope_t scope;
    uint16_t connection_handle;
    uint32_t connection_epoch;
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
static bool ota_status_notify_enabled;
static bool link_encrypted;
static bool application_session_ready;
static bool application_handshake_pending;
static bool initial_session_publish_pending;
static uint32_t connection_epoch;
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
static TaskHandle_t health_task_handle;
static struct ble_npl_callout advertising_retry_callout;
static struct ble_npl_callout security_start_callout;
static uint32_t security_start_epoch;
static bool identity_recovery_restart_scheduled;

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

static const char *host_status_name(int status)
{
    switch (status) {
    case 0:
        return "success";
    case BLE_HS_EALREADY:
        return "already-in-progress";
    case BLE_HS_ENOTCONN:
        return "not-connected";
    case BLE_HS_ETIMEOUT:
        return "host-procedure-timeout";
    case BLE_HS_EAUTHEN:
        return "authentication-failed";
    case BLE_HS_EENCRYPT:
        return "encryption-failed";
    case BLE_HS_EENCRYPT_KEY_SZ:
        return "invalid-encryption-key-size";
    case BLE_HS_ESTORE_CAP:
        return "security-store-capacity";
    case BLE_HS_ESTORE_FAIL:
        return "security-store-failure";
    default:
        break;
    }
    if (status > BLE_HS_ERR_SM_US_BASE && status < BLE_HS_ERR_SM_PEER_BASE) {
        return "local-security-manager-error";
    }
    if (status > BLE_HS_ERR_SM_PEER_BASE && status < BLE_HS_ERR_HW_BASE) {
        return "peer-security-manager-error";
    }
    if (status > BLE_HS_ERR_HCI_BASE && status < BLE_HS_ERR_L2C_BASE) {
        return "controller-hci-error";
    }
    return "other-host-error";
}

static const char *sm_error_name(unsigned int code)
{
    switch (code) {
    case BLE_SM_ERR_PASSKEY:
        return "passkey-entry-failed";
    case BLE_SM_ERR_OOB:
        return "oob-unavailable";
    case BLE_SM_ERR_AUTHREQ:
        return "authentication-requirements";
    case BLE_SM_ERR_CONFIRM_MISMATCH:
        return "confirm-value-mismatch";
    case BLE_SM_ERR_PAIR_NOT_SUPP:
        return "pairing-not-supported";
    case BLE_SM_ERR_ENC_KEY_SZ:
        return "encryption-key-size";
    case BLE_SM_ERR_CMD_NOT_SUPP:
        return "security-command-not-supported";
    case BLE_SM_ERR_UNSPECIFIED:
        return "unspecified-security-error";
    case BLE_SM_ERR_REPEATED:
        return "repeated-attempts";
    case BLE_SM_ERR_INVAL:
        return "invalid-security-parameters";
    case BLE_SM_ERR_DHKEY:
        return "dhkey-check-failed";
    case BLE_SM_ERR_NUMCMP:
        return "numeric-comparison-failed";
    case BLE_SM_ERR_ALREADY:
        return "bond-already-exists";
    case BLE_SM_ERR_CROSS_TRANS:
        return "cross-transport-key-derivation";
    case BLE_SM_ERR_KEY_REJ:
        return "key-rejected";
    default:
        return "unknown-security-error";
    }
}

static const char *hci_error_name(unsigned int code)
{
    switch (code) {
    case BLE_ERR_AUTH_FAIL:
        return "authentication-failure";
    case BLE_ERR_PINKEY_MISSING:
        return "pin-or-key-missing";
    case BLE_ERR_CONN_SPVN_TMO:
        return "connection-supervision-timeout";
    case BLE_ERR_CMD_DISALLOWED:
        return "command-disallowed";
    case BLE_ERR_REM_USER_CONN_TERM:
        return "remote-user-terminated";
    case BLE_ERR_CONN_TERM_LOCAL:
        return "local-host-terminated";
    case BLE_ERR_REPEATED_ATTEMPTS:
        return "repeated-attempts";
    case BLE_ERR_NO_PAIRING:
        return "pairing-not-allowed";
    case BLE_ERR_ENCRYPTION_MODE:
        return "unsupported-encryption-mode";
    case BLE_ERR_INSUFFICIENT_SEC:
        return "insufficient-security";
    default:
        return "unknown-hci-error";
    }
}

static const char *subscribe_reason_name(uint8_t reason)
{
    switch (reason) {
    case BLE_GAP_SUBSCRIBE_REASON_WRITE:
        return "cccd-write";
    case BLE_GAP_SUBSCRIBE_REASON_TERM:
        return "link-termination";
    case BLE_GAP_SUBSCRIBE_REASON_RESTORE:
        return "bond-restore";
    default:
        return "unknown";
    }
}

static const char *passkey_action_name(uint8_t action)
{
    switch (action) {
    case BLE_SM_IOACT_NONE:
        return "just-works-none";
    case BLE_SM_IOACT_OOB:
        return "legacy-oob";
    case BLE_SM_IOACT_INPUT:
        return "passkey-input";
    case BLE_SM_IOACT_DISP:
        return "passkey-display";
    case BLE_SM_IOACT_NUMCMP:
        return "numeric-comparison";
    case BLE_SM_IOACT_OOB_SC:
        return "secure-connections-oob";
    case BLE_SM_IOACT_STATIC:
        return "static-passkey";
    default:
        return "unknown";
    }
}

static bool read_bond_store_counts(int *our_records, int *peer_records)
{
    if (our_records == NULL || peer_records == NULL) {
        return false;
    }
    *our_records = 0;
    *peer_records = 0;
    int our_result = ble_store_util_count(BLE_STORE_OBJ_TYPE_OUR_SEC, our_records);
    int peer_result = ble_store_util_count(BLE_STORE_OBJ_TYPE_PEER_SEC, peer_records);
    return our_result == 0 && peer_result == 0;
}

static void log_bond_store(const char *phase)
{
    int our_security_records = 0;
    int peer_security_records = 0;
    int our_result = ble_store_util_count(BLE_STORE_OBJ_TYPE_OUR_SEC, &our_security_records);
    int peer_result = ble_store_util_count(BLE_STORE_OBJ_TYPE_PEER_SEC, &peer_security_records);
    ESP_LOGI(
        TAG,
        "BLE_BOND_STORE phase=%s our_records=%d peer_records=%d our_rc=%d peer_rc=%d",
        phase,
        our_security_records,
        peer_security_records,
        our_result,
        peer_result
    );
}

static esp_err_t persist_bond_policy_state(bool paired_once, bool rotate_pending)
{
    nvs_handle_t handle;
    esp_err_t result = nvs_open(VHOS_BLE_IDENTITY_NAMESPACE, NVS_READWRITE, &handle);
    if (result != ESP_OK) {
        return result;
    }
    result = nvs_set_u32(handle, VHOS_BLE_BOND_POLICY_KEY, VHOS_BLE_BOND_POLICY_VERSION);
    if (result == ESP_OK) {
        result = nvs_set_u8(handle, VHOS_BLE_BONDED_ONCE_KEY, paired_once ? 1U : 0U);
    }
    if (result == ESP_OK) {
        result = nvs_set_u8(
            handle,
            VHOS_BLE_ROTATE_PENDING_KEY,
            rotate_pending ? 1U : 0U
        );
    }
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }
    nvs_close(handle);
    return result;
}

static bool load_paired_once(void)
{
    nvs_handle_t handle;
    if (nvs_open(VHOS_BLE_IDENTITY_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    uint8_t paired_once = 0;
    esp_err_t result = nvs_get_u8(handle, VHOS_BLE_BONDED_ONCE_KEY, &paired_once);
    nvs_close(handle);
    return result == ESP_OK && paired_once == 1U;
}

static void identity_recovery_restart_task(void *argument)
{
    (void)argument;
    vTaskDelay(pdMS_TO_TICKS(250));
    esp_restart();
}

static bool schedule_missing_bond_identity_recovery(uint16_t handle)
{
    int our_records = 0;
    int peer_records = 0;
    bool counts_available = read_bond_store_counts(&our_records, &peer_records);
    if (!counts_available || our_records != 0 || peer_records != 0 || !load_paired_once()) {
        return false;
    }
    if (identity_recovery_restart_scheduled) {
        return true;
    }
    esp_err_t persist_result = persist_bond_policy_state(true, true);
    if (persist_result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "BLE_BOND_EPOCH_RECOVERY handle=%u action=arm-failed reason=%s",
            handle,
            esp_err_to_name(persist_result)
        );
        return false;
    }
    identity_recovery_restart_scheduled = true;
    BaseType_t task_result = xTaskCreate(
        identity_recovery_restart_task,
        "ble_id_recover",
        2048,
        NULL,
        7,
        NULL
    );
    if (task_result != pdPASS) {
        identity_recovery_restart_scheduled = false;
        (void)persist_bond_policy_state(true, false);
        ESP_LOGE(
            TAG,
            "BLE_BOND_EPOCH_RECOVERY handle=%u action=restart-task-failed",
            handle
        );
        return false;
    }
    ESP_LOGW(
        TAG,
        "BLE_BOND_EPOCH_RECOVERY handle=%u action=rotate-identity-and-restart "
        "reason=previously-paired-key-database-empty delay_ms=250",
        handle
    );
    return true;
}

static void log_security_snapshot(uint16_t handle, const char *phase)
{
    struct ble_gap_conn_desc description;
    int result = ble_gap_conn_find(handle, &description);
    if (result != 0) {
        ESP_LOGW(
            TAG,
            "BLE_SECURITY_STATE phase=%s handle=%u unavailable_rc=%d status=%s",
            phase,
            handle,
            result,
            host_status_name(result)
        );
        return;
    }
    ESP_LOGI(
        TAG,
        "BLE_SECURITY_STATE phase=%s handle=%u encrypted=%u authenticated=%u bonded=%u "
        "authorized=%u key_size=%u peer_id_type=%u peer_id=%02x:%02x:%02x:%02x:%02x:%02x "
        "peer_ota_type=%u peer_ota=%02x:%02x:%02x:%02x:%02x:%02x",
        phase,
        handle,
        description.sec_state.encrypted,
        description.sec_state.authenticated,
        description.sec_state.bonded,
        description.sec_state.authorize,
        description.sec_state.key_size,
        description.peer_id_addr.type,
        description.peer_id_addr.val[5],
        description.peer_id_addr.val[4],
        description.peer_id_addr.val[3],
        description.peer_id_addr.val[2],
        description.peer_id_addr.val[1],
        description.peer_id_addr.val[0],
        description.peer_ota_addr.type,
        description.peer_ota_addr.val[5],
        description.peer_ota_addr.val[4],
        description.peer_ota_addr.val[3],
        description.peer_ota_addr.val[2],
        description.peer_ota_addr.val[1],
        description.peer_ota_addr.val[0]
    );
}

static void security_start_event(struct ble_npl_event *event)
{
    (void)event;
    portENTER_CRITICAL(&state_lock);
    uint16_t handle = connection_handle;
    uint32_t active_epoch = connection_epoch;
    uint32_t scheduled_epoch = security_start_epoch;
    portEXIT_CRITICAL(&state_lock);
    if (handle == BLE_HS_CONN_HANDLE_NONE || active_epoch != scheduled_epoch) {
        ESP_LOGI(
            TAG,
            "BLE_SECURITY_INITIATE skipped=stale-link active_epoch=%lu scheduled_epoch=%lu",
            (unsigned long)active_epoch,
            (unsigned long)scheduled_epoch
        );
        return;
    }
    struct ble_gap_conn_desc description;
    int description_result = ble_gap_conn_find(handle, &description);
    if (description_result != 0) {
        ESP_LOGW(
            TAG,
            "BLE_SECURITY_INITIATE skipped=descriptor-unavailable handle=%u rc=%d status=%s",
            handle,
            description_result,
            host_status_name(description_result)
        );
        return;
    }
    if (description.sec_state.encrypted) {
        portENTER_CRITICAL(&state_lock);
        link_encrypted = true;
        portEXIT_CRITICAL(&state_lock);
        ESP_LOGI(
            TAG,
            "BLE_SECURITY_INITIATE skipped=already-encrypted handle=%u bonded=%u "
            "authenticated=%u key_size=%u",
            handle,
            description.sec_state.bonded,
            description.sec_state.authenticated,
            description.sec_state.key_size
        );
        log_security_snapshot(handle, "security-initiate-skipped-restored-link");
        log_bond_store("security-initiate-skipped-restored-link");
        return;
    }
    log_security_snapshot(handle, "before-initiate-unencrypted-link");
    int result = ble_gap_security_initiate(handle);
    ESP_LOGI(
        TAG,
        "BLE_SECURITY_INITIATE trigger=link-established handle=%u rc=%d status=%s",
        handle,
        result,
        host_status_name(result)
    );
}

static void schedule_security_start(uint16_t handle)
{
    portENTER_CRITICAL(&state_lock);
    security_start_epoch = connection_epoch;
    uint32_t scheduled_epoch = security_start_epoch;
    portEXIT_CRITICAL(&state_lock);
    ble_npl_error_t result = ble_npl_callout_reset(
        &security_start_callout,
        ble_npl_time_ms_to_ticks32(VHOS_BLE_SECURITY_START_DELAY_MS)
    );
    if (result != BLE_NPL_OK) {
        ESP_LOGE(TAG, "BLE_SECURITY_SCHEDULE handle=%u failed_rc=%d", handle, result);
    } else {
        ESP_LOGI(
            TAG,
            "BLE_SECURITY_SCHEDULE handle=%u delay_ms=%u epoch=%lu",
            handle,
            VHOS_BLE_SECURITY_START_DELAY_MS,
            (unsigned long)scheduled_epoch
        );
    }
}

static int gatt_access(
    uint16_t conn_handle,
    uint16_t attr_handle,
    struct ble_gatt_access_ctxt *context,
    void *argument
)
{
    (void)argument;
    if (context->op == BLE_GATT_ACCESS_OP_WRITE_CHR && attr_handle == command_value_handle) {
        portENTER_CRITICAL(&state_lock);
        uint16_t active_connection = connection_handle;
        bool encrypted = link_encrypted;
        bool subscribed = stream_notify_enabled;
        bool session_ready = application_session_ready;
        portEXIT_CRITICAL(&state_lock);
        if (conn_handle != active_connection || !encrypted || !subscribed) {
            ESP_LOGW(
                TAG,
                "BLE_COMMAND_REJECT handle=%u active_handle=%u encrypted=%u "
                "stream_notify=%u reason=bootstrap-transport-not-ready",
                conn_handle,
                active_connection,
                encrypted,
                subscribed
            );
            return BLE_ATT_ERR_UNLIKELY;
        }
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
        vhos_transport_ingest_result_t completion = {0};
        esp_err_t ingest_result = vhos_transport_ingest(
            fragment,
            flattened,
            session_ready,
            &completion
        );
        if (ingest_result != ESP_OK && ingest_result != ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGW(TAG, "Rejected command fragment: %s", esp_err_to_name(ingest_result));
            return BLE_ATT_ERR_UNLIKELY;
        }
        if (completion.handshake_accepted) {
            ESP_LOGI(
                TAG,
                "BLE_APPLICATION_HANDSHAKE_PENDING handle=%u completed_frames=%u "
                "proof=crc-valid-request-and-response-queued",
                conn_handle,
                (unsigned int)completion.completed_frames
            );
        } else if (completion.completed_frames > 0) {
            ESP_LOGI(
                TAG,
                "BLE_APPLICATION_FRAME_COMPLETE handle=%u completed_frames=%u "
                "session_ready=%u",
                conn_handle,
                (unsigned int)completion.completed_frames,
                session_ready
            );
        } else if (ingest_result == ESP_OK) {
            ESP_LOGD(
                TAG,
                "BLE_APPLICATION_FRAME_PENDING handle=%u fragment_bytes=%u",
                conn_handle,
                flattened
            );
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

static const char *emit_scope_name(vhos_transport_emit_scope_t scope)
{
    switch (scope) {
    case VHOS_TRANSPORT_EMIT_SESSION_REQUIRED:
        return "session-required";
    case VHOS_TRANSPORT_EMIT_BOOTSTRAP_HANDSHAKE:
        return "bootstrap-handshake";
    default:
        return "invalid";
    }
}

static bool emit_scope_is_bootstrap(vhos_transport_emit_scope_t scope)
{
    return scope == VHOS_TRANSPORT_EMIT_BOOTSTRAP_HANDSHAKE;
}

static bool emit_scope_matches_frame(
    vhos_transport_emit_scope_t scope,
    const uint8_t *data,
    size_t length
)
{
    if (data == NULL || length < VHOS_BLE_FRAME_HEADER_BYTES || memcmp(data, "VHOS", 4) != 0) {
        return false;
    }
    switch (scope) {
    case VHOS_TRANSPORT_EMIT_SESSION_REQUIRED:
        return true;
    case VHOS_TRANSPORT_EMIT_BOOTSTRAP_HANDSHAKE:
        return data[6] == VHOS_BLE_FRAME_MESSAGE_HANDSHAKE;
    default:
        return false;
    }
}

/* Caller holds state_lock. Bootstrap is the only pre-session exception. */
static bool emit_authorized_locked(vhos_transport_emit_scope_t scope)
{
    bool transport_ready = connection_handle != BLE_HS_CONN_HANDLE_NONE &&
                           link_encrypted && stream_notify_enabled;
    if (!transport_ready) {
        return false;
    }
    return emit_scope_is_bootstrap(scope) ? !application_session_ready
                                          : application_session_ready;
}

/* Caller holds state_lock. Items are bound to the connection epoch that admitted them. */
static bool tx_item_authorized_locked(const vhos_ble_tx_item_t *item)
{
    return item != NULL && item->connection_handle == connection_handle &&
           item->connection_epoch == connection_epoch && emit_authorized_locked(item->scope);
}

static esp_err_t emit_frame(
    const uint8_t *data,
    size_t length,
    vhos_transport_channel_t channel,
    vhos_transport_emit_scope_t scope
)
{
    if (length > VHOS_BLE_TX_MAX_BYTES || tx_queue == NULL ||
        !emit_scope_matches_frame(scope, data, length)) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&state_lock);
    bool authorized = emit_authorized_locked(scope);
    uint16_t active_connection = connection_handle;
    bool encrypted = link_encrypted;
    bool subscribed = stream_notify_enabled;
    bool session_ready = application_session_ready;
    bool handshake_pending = application_handshake_pending;
    uint32_t active_epoch = connection_epoch;
    if (authorized && scope == VHOS_TRANSPORT_EMIT_BOOTSTRAP_HANDSHAKE) {
        authorized = !application_handshake_pending;
        if (authorized) {
            application_handshake_pending = true;
        }
    }
    portEXIT_CRITICAL(&state_lock);
    if (!authorized) {
        ESP_LOGD(
            TAG,
            "BLE_TX_ADMISSION_REJECT scope=%s handle=%u encrypted=%u stream_notify=%u "
            "session_ready=%u handshake_pending=%u",
            emit_scope_name(scope),
            active_connection,
            encrypted,
            subscribed,
            session_ready,
            handshake_pending
        );
        return ESP_ERR_INVALID_STATE;
    }
    vhos_ble_tx_item_t item = {
        .length = length,
        .channel = channel,
        .scope = scope,
        .connection_handle = active_connection,
        .connection_epoch = active_epoch,
    };
    memcpy(item.data, data, length);
    if (xQueueSend(tx_queue, &item, 0) == pdTRUE) {
        return ESP_OK;
    }
    if (scope == VHOS_TRANSPORT_EMIT_BOOTSTRAP_HANDSHAKE) {
        portENTER_CRITICAL(&state_lock);
        if (connection_handle == active_connection && connection_epoch == active_epoch &&
            !application_session_ready) {
            application_handshake_pending = false;
        }
        portEXIT_CRITICAL(&state_lock);
    }
    return ESP_ERR_NO_MEM;
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
        /*
         * Every VHOS frame is self-describing and CRC protected. Multiplex all outbound frame
         * types over one encrypted notification characteristic so commissioning requires only
         * one CCCD transaction. Separate encrypted CCCD writes proved vulnerable to being lost
         * while iOS Just Works pairing was still completing.
         */
        bool authorized = tx_item_authorized_locked(&item);
        uint16_t value_handle = stream_value_handle;
        uint16_t active_connection = item.connection_handle;
        uint32_t active_epoch = item.connection_epoch;
        portEXIT_CRITICAL(&state_lock);
        if (!authorized) {
            if (item.scope == VHOS_TRANSPORT_EMIT_BOOTSTRAP_HANDSHAKE) {
                portENTER_CRITICAL(&state_lock);
                if (connection_handle == active_connection && connection_epoch == active_epoch &&
                    !application_session_ready) {
                    application_handshake_pending = false;
                }
                portEXIT_CRITICAL(&state_lock);
            }
            ESP_LOGD(
                TAG,
                "BLE_TX_DELIVERY_DROP scope=%s reason=session-or-transport-gate",
                emit_scope_name(item.scope)
            );
            continue;
        }

        uint16_t mtu = ble_att_mtu(active_connection);
        size_t maximum_chunk = mtu > 3 ? (size_t)mtu - 3 : 20;
        bool delivery_complete = true;
        size_t delivered_bytes = 0;
        for (size_t offset = 0; offset < item.length; offset += maximum_chunk) {
            portENTER_CRITICAL(&state_lock);
            bool session_current = tx_item_authorized_locked(&item);
            portEXIT_CRITICAL(&state_lock);
            if (!session_current) {
                delivery_complete = false;
                break;
            }
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
                delivery_complete = false;
                break;
            }
            if (os_mbuf_append(packet, &item.data[offset], chunk_length) != 0) {
                os_mbuf_free_chain(packet);
                ESP_LOGE(TAG, "Unable to append BLE notification packet");
                delivery_complete = false;
                break;
            }
            portENTER_CRITICAL(&state_lock);
            bool delivery_still_authorized = tx_item_authorized_locked(&item);
            portEXIT_CRITICAL(&state_lock);
            if (!delivery_still_authorized) {
                os_mbuf_free_chain(packet);
                delivery_complete = false;
                break;
            }
            int result = ble_gatts_notify_custom(active_connection, value_handle, packet);
            if (result != 0) {
                ESP_LOGW(
                    TAG,
                    "BLE_NOTIFY_FAILED scope=%s handle=%u epoch=%lu offset=%u rc=%d",
                    emit_scope_name(item.scope),
                    active_connection,
                    (unsigned long)active_epoch,
                    (unsigned int)offset,
                    result
                );
                delivery_complete = false;
                break;
            }
            delivered_bytes += chunk_length;
            /* Keep the controller pool below saturation when ATT MTU is still 23 bytes. */
            vTaskDelay(pdMS_TO_TICKS(VHOS_BLE_NOTIFICATION_PACE_MS));
        }

        if (item.scope == VHOS_TRANSPORT_EMIT_BOOTSTRAP_HANDSHAKE) {
            bool session_transition = false;
            portENTER_CRITICAL(&state_lock);
            bool same_epoch = connection_handle == active_connection &&
                              connection_epoch == active_epoch;
            if (delivery_complete && delivered_bytes == item.length && same_epoch &&
                link_encrypted && stream_notify_enabled && application_handshake_pending &&
                !application_session_ready) {
                application_handshake_pending = false;
                application_session_ready = true;
                initial_session_publish_pending = true;
                session_transition = true;
            } else if (same_epoch && !application_session_ready) {
                application_handshake_pending = false;
            }
            portEXIT_CRITICAL(&state_lock);

            if (!session_transition) {
                ESP_LOGW(
                    TAG,
                    "BLE_APPLICATION_HANDSHAKE_DELIVERY_FAILED handle=%u epoch=%lu "
                    "delivered=%u total=%u",
                    active_connection,
                    (unsigned long)active_epoch,
                    (unsigned int)delivered_bytes,
                    (unsigned int)item.length
                );
                continue;
            }

            ESP_LOGI(
                TAG,
                "BLE_APPLICATION_SESSION_READY handle=%u epoch=%lu "
                "proof=crc-valid-request-and-handshake-response-notified bytes=%u",
                active_connection,
                (unsigned long)active_epoch,
                (unsigned int)delivered_bytes
            );
            TaskHandle_t control_task = health_task_handle;
            if (control_task != NULL) {
                xTaskNotifyGive(control_task);
            } else {
                /* The periodic health timeout will still observe the pending flag. */
                ESP_LOGE(TAG, "BLE_SESSION_CONTROL_DEFER_FAILED reason=health-task-unavailable");
            }
        }
    }
}

static void health_task(void *argument)
{
    (void)argument;
    while (true) {
        uint32_t notification_count = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000));
        portENTER_CRITICAL(&state_lock);
        bool can_send = connection_handle != BLE_HS_CONN_HANDLE_NONE &&
                        link_encrypted && stream_notify_enabled && application_session_ready;
        bool initial_publish = can_send && initial_session_publish_pending;
        uint16_t active_connection = connection_handle;
        uint32_t active_epoch = connection_epoch;
        if (initial_publish) {
            initial_session_publish_pending = false;
        }
        portEXIT_CRITICAL(&state_lock);
        if (can_send) {
            if (initial_publish) {
                log_security_snapshot(active_connection, "application-session-ready");
                log_bond_store("application-session-ready");
            }
            esp_err_t health_result = vhos_transport_send_health();
            bool health_queued = health_result == ESP_OK;
            if (!health_queued && health_result != ESP_ERR_NO_MEM) {
                ESP_LOGW(TAG, "Health queue failed: %s", esp_err_to_name(health_result));
            }

            bool status_queued = true;
            esp_err_t session_status_result = ESP_OK;
            if (initial_publish) {
                session_status_result = vhos_transport_send_session_status();
                status_queued = session_status_result == ESP_OK ||
                                session_status_result == ESP_ERR_NOT_FOUND;
                if (!status_queued && session_status_result != ESP_ERR_NO_MEM) {
                    ESP_LOGW(
                        TAG,
                        "Unable to queue post-handshake status: %s",
                        esp_err_to_name(session_status_result)
                    );
                }
                ESP_LOGI(
                    TAG,
                    "BLE_SESSION_CONTROL_PUBLISH handle=%u epoch=%lu health_rc=%s "
                    "status_rc=%s notification_count=%lu stack_high_water=%u",
                    active_connection,
                    (unsigned long)active_epoch,
                    esp_err_to_name(health_result),
                    esp_err_to_name(session_status_result),
                    (unsigned long)notification_count,
                    (unsigned int)uxTaskGetStackHighWaterMark(NULL)
                );
            }

            if (initial_publish && (!health_queued || !status_queued)) {
                portENTER_CRITICAL(&state_lock);
                if (connection_handle == active_connection && connection_epoch == active_epoch &&
                    link_encrypted && stream_notify_enabled && application_session_ready) {
                    initial_session_publish_pending = true;
                }
                portEXIT_CRITICAL(&state_lock);
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
            struct ble_gap_conn_desc connect_description;
            int connect_description_result = ble_gap_conn_find(
                event->connect.conn_handle,
                &connect_description
            );
            portENTER_CRITICAL(&state_lock);
            connection_handle = event->connect.conn_handle;
            /*
             * NimBLE can restore encryption and bonded CCCDs before delivering GAP CONNECT.
             * Preserve those event-derived subscription flags; DISCONNECT and boot already
             * clear them for genuinely new links. Prefer the authoritative connection
             * descriptor for encryption, while retaining a pre-CONNECT ENC_CHANGE result if
             * descriptor lookup is momentarily unavailable.
             */
            if (connect_description_result == 0) {
                link_encrypted = connect_description.sec_state.encrypted;
            }
            application_session_ready = false;
            application_handshake_pending = false;
            initial_session_publish_pending = false;
            connection_epoch++;
            advertising_active = false;
            connection_parameters_available = false;
            active_att_mtu = 0;
            bool effective_encrypted = link_encrypted;
            bool effective_stream_notify = stream_notify_enabled;
            bool effective_status_notify = status_notify_enabled;
            bool effective_ota_notify = ota_status_notify_enabled;
            portEXIT_CRITICAL(&state_lock);
            ESP_LOGI(
                TAG,
                "BLE_CONNECT_EFFECTIVE_STATE handle=%u descriptor_rc=%d encrypted=%u "
                "stream_notify=%u status_notify=%u ota_notify=%u source=preserved-pre-connect",
                event->connect.conn_handle,
                connect_description_result,
                effective_encrypted,
                effective_stream_notify,
                effective_status_notify,
                effective_ota_notify
            );
            if (tx_queue != NULL) {
                xQueueReset(tx_queue);
            }
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
            log_security_snapshot(connection_handle, "connected");
            log_bond_store("connected");
            schedule_security_start(connection_handle);
        } else {
            ESP_LOGW(TAG, "BLE connection attempt failed: status=%d", event->connect.status);
            schedule_advertising();
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT: {
        unsigned int hci_code = event->disconnect.reason > BLE_HS_ERR_HCI_BASE &&
                                        event->disconnect.reason < BLE_HS_ERR_L2C_BASE
                                    ? (unsigned int)(event->disconnect.reason -
                                                     BLE_HS_ERR_HCI_BASE)
                                    : 0U;
        ESP_LOGI(
            TAG,
            "IPHONE_LINK_DISCONNECTED reason=%d status=%s hci_code=%u hci_name=%s",
            event->disconnect.reason,
            host_status_name(event->disconnect.reason),
            hci_code,
            hci_code == 0 ? "none" : hci_error_name(hci_code)
        );
        log_security_snapshot(event->disconnect.conn.conn_handle, "disconnecting");
        log_bond_store("disconnecting");
        portENTER_CRITICAL(&state_lock);
        connection_handle = BLE_HS_CONN_HANDLE_NONE;
        link_encrypted = false;
        application_session_ready = false;
        application_handshake_pending = false;
        initial_session_publish_pending = false;
        connection_epoch++;
        stream_notify_enabled = false;
        status_notify_enabled = false;
        ota_status_notify_enabled = false;
        connection_parameters_available = false;
        active_att_mtu = 0;
        active_connection_interval = 0;
        active_connection_latency = 0;
        active_supervision_timeout = 0;
        portEXIT_CRITICAL(&state_lock);
        if (tx_queue != NULL) {
            xQueueReset(tx_queue);
    }
        vhos_transport_reset();
        schedule_advertising();
        return 0;
        }
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
        } else if (event->subscribe.attr_handle == ota_status_value_handle) {
            ota_status_notify_enabled = event->subscribe.cur_notify;
        }
        portEXIT_CRITICAL(&state_lock);
        ESP_LOGI(
            TAG,
            "BLE_SUBSCRIBE connection=%u handle=%u reason=%u reason_name=%s "
            "notify=%u->%u indicate=%u->%u",
            event->subscribe.conn_handle,
            event->subscribe.attr_handle,
            event->subscribe.reason,
            subscribe_reason_name(event->subscribe.reason),
            event->subscribe.prev_notify,
            event->subscribe.cur_notify,
            event->subscribe.prev_indicate,
            event->subscribe.cur_indicate
        );
        log_security_snapshot(event->subscribe.conn_handle, "subscription-change");
        return 0;
    case BLE_GAP_EVENT_ENC_CHANGE: {
        struct ble_gap_conn_desc description;
        int description_result = ble_gap_conn_find(
            event->enc_change.conn_handle,
            &description
        );
        bool encrypted = event->enc_change.status == 0 && description_result == 0 &&
                         description.sec_state.encrypted;
        portENTER_CRITICAL(&state_lock);
        link_encrypted = encrypted;
        portEXIT_CRITICAL(&state_lock);
        unsigned int sm_code = 0;
        unsigned int hci_code = 0;
        const char *sm_origin = "none";
        if (event->enc_change.status > BLE_HS_ERR_SM_US_BASE &&
            event->enc_change.status < BLE_HS_ERR_SM_PEER_BASE) {
            sm_code = (unsigned int)(event->enc_change.status - BLE_HS_ERR_SM_US_BASE);
            sm_origin = "local";
        } else if (event->enc_change.status > BLE_HS_ERR_SM_PEER_BASE &&
                   event->enc_change.status < BLE_HS_ERR_HW_BASE) {
            sm_code = (unsigned int)(event->enc_change.status - BLE_HS_ERR_SM_PEER_BASE);
            sm_origin = "peer";
        } else if (event->enc_change.status > BLE_HS_ERR_HCI_BASE &&
                   event->enc_change.status < BLE_HS_ERR_L2C_BASE) {
            hci_code = (unsigned int)(event->enc_change.status - BLE_HS_ERR_HCI_BASE);
        }
        ESP_LOGI(
            TAG,
            "BLE_ENCRYPTION handle=%u status=%d status_name=%s sm_origin=%s "
            "sm_code=%u sm_name=%s hci_code=%u hci_name=%s descriptor_rc=%d",
            event->enc_change.conn_handle,
            event->enc_change.status,
            host_status_name(event->enc_change.status),
            sm_origin,
            sm_code,
            sm_code == 0 ? "none" : sm_error_name(sm_code),
            hci_code,
            hci_code == 0 ? "none" : hci_error_name(hci_code),
            description_result
        );
        log_security_snapshot(
            event->enc_change.conn_handle,
            event->enc_change.status == 0 ? "encryption-complete" : "encryption-failed"
        );
        log_bond_store(
            event->enc_change.status == 0 ? "encryption-complete" : "encryption-failed"
        );
        if (event->enc_change.status == BLE_HS_ETIMEOUT && description_result == 0) {
            /*
             * NimBLE marks a timed-out Security Manager procedure as unusable for further SMP
             * work on the same link. End that link immediately so Core Bluetooth can reconnect
             * into a fresh, proactively secured session instead of waiting for HCI supervision.
             */
            bool rotating_identity = schedule_missing_bond_identity_recovery(
                event->enc_change.conn_handle
            );
            if (!rotating_identity) {
                int terminate_result = ble_gap_terminate(
                    event->enc_change.conn_handle,
                    BLE_ERR_REM_USER_CONN_TERM
                );
                ESP_LOGW(
                    TAG,
                    "BLE_SECURITY_RECOVERY action=terminate-tainted-link handle=%u rc=%d "
                    "failed_status=%d failed_status_name=%s",
                    event->enc_change.conn_handle,
                    terminate_result,
                    event->enc_change.status,
                    host_status_name(event->enc_change.status)
                );
            }
        }
        return 0;
    }
    case BLE_GAP_EVENT_PARING_COMPLETE:
        ESP_LOGI(
            TAG,
            "BLE_PAIRING_COMPLETE handle=%u status=%d status_name=%s",
            event->pairing_complete.conn_handle,
            event->pairing_complete.status,
            host_status_name(event->pairing_complete.status)
        );
        log_security_snapshot(
            event->pairing_complete.conn_handle,
            event->pairing_complete.status == 0 ? "pairing-complete" : "pairing-failed"
        );
        log_bond_store(
            event->pairing_complete.status == 0 ? "pairing-complete" : "pairing-failed"
        );
        if (event->pairing_complete.status == 0) {
            esp_err_t policy_result = persist_bond_policy_state(true, false);
            ESP_LOGI(
                TAG,
                "BLE_BOND_POLICY phase=pairing-complete paired_once=1 persist=%s",
                esp_err_to_name(policy_result)
            );
        }
        return 0;
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        struct ble_gap_conn_desc description;
        int find_result = ble_gap_conn_find(event->repeat_pairing.conn_handle, &description);
        ESP_LOGW(
            TAG,
            "BLE_REPEAT_PAIRING handle=%u find_rc=%d policy=delete-exact-peer-and-retry",
            event->repeat_pairing.conn_handle,
            find_result
        );
        log_security_snapshot(event->repeat_pairing.conn_handle, "repeat-pairing");
        log_bond_store("before-repeat-pairing-delete");
        if (find_result == 0) {
            int delete_result = ble_store_util_delete_peer(&description.peer_id_addr);
            ESP_LOGW(
                TAG,
                "BLE_REPEAT_PAIRING_DELETE peer_type=%u "
                "peer=%02x:%02x:%02x:%02x:%02x:%02x rc=%d",
                description.peer_id_addr.type,
                description.peer_id_addr.val[5],
                description.peer_id_addr.val[4],
                description.peer_id_addr.val[3],
                description.peer_id_addr.val[2],
                description.peer_id_addr.val[1],
                description.peer_id_addr.val[0],
                delete_result
            );
            log_bond_store("after-repeat-pairing-delete");
            if (delete_result == 0 || delete_result == BLE_HS_ENOENT) {
                return BLE_GAP_REPEAT_PAIRING_RETRY;
            }
        }
        ESP_LOGE(TAG, "BLE_REPEAT_PAIRING policy=ignore reason=exact-peer-delete-failed");
        return BLE_GAP_REPEAT_PAIRING_IGNORE;
    }
    case BLE_GAP_EVENT_PASSKEY_ACTION:
        ESP_LOGI(
            TAG,
            "BLE_PASSKEY_ACTION handle=%u action=%u action_name=%s numcmp=%lu "
            "io_cap=no-input-no-output mitm=0 secure_connections=1",
            event->passkey.conn_handle,
            event->passkey.params.action,
            passkey_action_name(event->passkey.params.action),
            (unsigned long)event->passkey.params.numcmp
        );
        log_security_snapshot(event->passkey.conn_handle, "passkey-action");
        if (event->passkey.params.action == BLE_SM_IOACT_NONE) {
            return 0;
        }
        ESP_LOGE(
            TAG,
            "BLE_PASSKEY_ACTION rejected=unsupported-interactive-action action=%u",
            event->passkey.params.action
        );
        return BLE_HS_ENOTSUP;
    case BLE_GAP_EVENT_IDENTITY_RESOLVED:
        ESP_LOGI(
            TAG,
            "BLE_IDENTITY_RESOLVED handle=%u peer_type=%u "
            "peer=%02x:%02x:%02x:%02x:%02x:%02x",
            event->identity_resolved.conn_handle,
            event->identity_resolved.peer_id_addr.type,
            event->identity_resolved.peer_id_addr.val[5],
            event->identity_resolved.peer_id_addr.val[4],
            event->identity_resolved.peer_id_addr.val[3],
            event->identity_resolved.peer_id_addr.val[2],
            event->identity_resolved.peer_id_addr.val[1],
            event->identity_resolved.peer_id_addr.val[0]
        );
        log_security_snapshot(event->identity_resolved.conn_handle, "identity-resolved");
        return 0;
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
    application_session_ready = false;
    application_handshake_pending = false;
    initial_session_publish_pending = false;
    connection_epoch++;
    stream_notify_enabled = false;
    status_notify_enabled = false;
    ota_status_notify_enabled = false;
    connection_parameters_available = false;
    active_att_mtu = 0;
    portEXIT_CRITICAL(&state_lock);
    if (tx_queue != NULL) {
        xQueueReset(tx_queue);
    }
    vhos_transport_reset();
    ESP_LOGI(TAG, "BLE_HOST_EPOCH_CLEARED reason=%d rx=reset tx=reset", reason);
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

static esp_err_t persist_gatt_schema(nvs_handle_t handle)
{
    esp_err_t result = nvs_set_u32(
        handle,
        VHOS_BLE_GATT_SCHEMA_KEY,
        VHOS_BLE_GATT_SCHEMA_VERSION
    );
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }
    return result;
}

static bool gatt_schema_preserves_bond(uint32_t stored_schema)
{
    /*
     * Epoch 2 and epoch 6 register the same services, UUIDs, characteristic order,
     * properties, encrypted permissions, and CCCDs. Epoch 6 was advanced while
     * diagnosing application/session behavior, not because the attribute database
     * changed. Keep this allowlist explicit; unknown epochs still fail over to the
     * one-time identity/bond migration below.
     */
    return stored_schema == VHOS_BLE_GATT_SCHEMA_VERSION ||
           stored_schema == VHOS_BLE_GATT_SCHEMA_BOND_COMPATIBLE_VERSION;
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
    bool identity_present = result == ESP_OK && identity_length == sizeof(identity.val);
    uint32_t stored_gatt_schema = 0;
    esp_err_t schema_result = nvs_get_u32(
        handle,
        VHOS_BLE_GATT_SCHEMA_KEY,
        &stored_gatt_schema
    );
    uint32_t stored_bond_policy = 0;
    esp_err_t bond_policy_result = nvs_get_u32(
        handle,
        VHOS_BLE_BOND_POLICY_KEY,
        &stored_bond_policy
    );
    uint8_t paired_once_value = 0;
    esp_err_t paired_once_result = nvs_get_u8(
        handle,
        VHOS_BLE_BONDED_ONCE_KEY,
        &paired_once_value
    );
    uint8_t rotate_pending_value = 0;
    esp_err_t rotate_pending_result = nvs_get_u8(
        handle,
        VHOS_BLE_ROTATE_PENDING_KEY,
        &rotate_pending_value
    );
    int our_security_records = 0;
    int peer_security_records = 0;
    bool bond_counts_available = read_bond_store_counts(
        &our_security_records,
        &peer_security_records
    );
    bool bond_store_empty = bond_counts_available &&
        our_security_records == 0 && peer_security_records == 0;
    bool prior_policy_missing_bond = identity_present &&
        bond_policy_result == ESP_ERR_NVS_NOT_FOUND &&
        schema_result == ESP_OK &&
        stored_gatt_schema == VHOS_BLE_GATT_SCHEMA_VERSION &&
        bond_store_empty;
    bool recorded_pairing_missing_bond = identity_present &&
        paired_once_result == ESP_OK && paired_once_value == 1U && bond_store_empty;
    bool pending_identity_recovery = identity_present &&
        rotate_pending_result == ESP_OK && rotate_pending_value == 1U;
    bool force_bond_epoch_rotation = prior_policy_missing_bond ||
        recorded_pairing_missing_bond || pending_identity_recovery;
    bool paired_once = paired_once_result == ESP_OK && paired_once_value == 1U;
    if (bond_counts_available && (our_security_records > 0 || peer_security_records > 0)) {
        paired_once = true;
    }
    bool compatible_schema_upgrade = identity_present &&
        schema_result == ESP_OK &&
        stored_gatt_schema != VHOS_BLE_GATT_SCHEMA_VERSION &&
        gatt_schema_preserves_bond(stored_gatt_schema);
    bool gatt_schema_changed = identity_present &&
        (schema_result != ESP_OK || !gatt_schema_preserves_bond(stored_gatt_schema));
    bool generated = false;
    if (force_bond_epoch_rotation) {
        const char *reason = pending_identity_recovery
            ? "runtime-security-timeout"
            : (recorded_pairing_missing_bond
                ? "previously-paired-key-database-empty"
                : "dev26-empty-bond-policy-migration");
        ESP_LOGW(
            TAG,
            "BLE_BOND_EPOCH_RECOVERY action=rotate-identity-clear-bonds reason=%s "
            "our_records=%d peer_records=%d stored_policy=%lu",
            reason,
            our_security_records,
            peer_security_records,
            bond_policy_result == ESP_OK ? (unsigned long)stored_bond_policy : 0UL
        );
        int clear_result = ble_store_clear();
        if (clear_result != 0) {
            ESP_LOGE(TAG, "Unable to clear BLE bonds for bond-epoch recovery: rc=%d", clear_result);
            result = ESP_FAIL;
        } else {
            result = generate_and_persist_identity(handle, &identity);
            generated = result == ESP_OK;
            paired_once = false;
        }
    } else if (compatible_schema_upgrade) {
        ESP_LOGI(
            TAG,
            "BLE_GATT_SCHEMA_MIGRATION stored=%lu current=%u "
            "compatibility=verified-identical-db action=preserve-identity-preserve-bonds",
            (unsigned long)stored_gatt_schema,
            VHOS_BLE_GATT_SCHEMA_VERSION
        );
    } else if (gatt_schema_changed) {
        ESP_LOGW(
            TAG,
            "BLE_GATT_SCHEMA_MIGRATION stored=%lu current=%u action=rotate-identity-clear-bonds",
            schema_result == ESP_OK ? (unsigned long)stored_gatt_schema : 0UL,
            VHOS_BLE_GATT_SCHEMA_VERSION
        );
        int clear_result = ble_store_clear();
        if (clear_result != 0) {
            ESP_LOGE(TAG, "Unable to clear BLE bonds for GATT migration: rc=%d", clear_result);
            result = ESP_FAIL;
        } else {
            result = generate_and_persist_identity(handle, &identity);
            generated = result == ESP_OK;
            paired_once = false;
        }
    } else if (result == ESP_ERR_NVS_NOT_FOUND ||
        result == ESP_ERR_NVS_INVALID_LENGTH ||
        (result == ESP_OK && identity_length != sizeof(identity.val))) {
        ESP_LOGW(
            TAG,
            "BLE identity absent or invalid; creating a new bond epoch: result=%s length=%u",
            esp_err_to_name(result),
            (unsigned int)identity_length
        );
        int clear_result = ble_store_clear();
        if (clear_result != 0) {
            ESP_LOGE(TAG, "Unable to clear BLE bonds for identity rotation: rc=%d", clear_result);
            result = ESP_FAIL;
        } else {
            result = generate_and_persist_identity(handle, &identity);
            generated = result == ESP_OK;
            paired_once = false;
        }
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
            int clear_result = ble_store_clear();
            if (clear_result != 0) {
                ESP_LOGE(TAG, "Unable to clear BLE bonds for identity recovery: rc=%d", clear_result);
                result = ESP_FAIL;
            } else {
                result = generate_and_persist_identity(handle, &identity);
                generated = result == ESP_OK;
                paired_once = false;
                if (result == ESP_OK) {
                    host_result = ble_hs_id_set_rnd(identity.val);
                }
            }
        }
        if (result == ESP_OK && host_result != 0) {
            ESP_LOGE(TAG, "Unable to install BLE identity: rc=%d", host_result);
            result = ESP_FAIL;
        }
    }

    if (result == ESP_OK &&
        (schema_result != ESP_OK || stored_gatt_schema != VHOS_BLE_GATT_SCHEMA_VERSION)) {
        result = persist_gatt_schema(handle);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "Unable to persist BLE GATT schema: %s", esp_err_to_name(result));
        }
    }

    if (result == ESP_OK) {
        result = nvs_set_u32(
            handle,
            VHOS_BLE_BOND_POLICY_KEY,
            VHOS_BLE_BOND_POLICY_VERSION
        );
    }
    if (result == ESP_OK) {
        result = nvs_set_u8(
            handle,
            VHOS_BLE_BONDED_ONCE_KEY,
            paired_once ? 1U : 0U
        );
    }
    if (result == ESP_OK) {
        result = nvs_set_u8(handle, VHOS_BLE_ROTATE_PENDING_KEY, 0U);
    }
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Unable to persist BLE bond policy: %s", esp_err_to_name(result));
    }

    nvs_close(handle);
    if (result != ESP_OK) {
        return result;
    }
    ESP_LOGI(
        TAG,
        "BLE_IDENTITY_READY type=random-static source=%s gatt_schema=%u bond_policy=%u "
        "paired_once=%u address=%02x:%02x:%02x:%02x:%02x:%02x",
        generated ? "generated" : "persisted",
        VHOS_BLE_GATT_SCHEMA_VERSION,
        VHOS_BLE_BOND_POLICY_VERSION,
        paired_once,
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
    callout_result = ble_npl_callout_init(
        &security_start_callout,
        nimble_port_get_dflt_eventq(),
        security_start_event,
        NULL
    );
    if (callout_result != 0) {
        ESP_LOGE(TAG, "Unable to initialize BLE security start: rc=%d", callout_result);
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
    log_bond_store("boot");
    nimble_port_freertos_init(host_task);
    if (xTaskCreate(
            health_task,
            "vhos_health",
            8192,
            NULL,
            5,
            &health_task_handle
        ) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(tx_task, "vhos_ble_tx", 6144, NULL, 6, NULL) != pdPASS) {
        vTaskDelete(health_task_handle);
        health_task_handle = NULL;
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
    health->health_subscribed = stream_notify_enabled;
    health->ota_subscribed = stream_notify_enabled;
    health->connection_parameters_available = connection_parameters_available;
    health->att_mtu = active_att_mtu;
    health->connection_interval_units = active_connection_interval;
    health->connection_latency = active_connection_latency;
    health->supervision_timeout_units = active_supervision_timeout;
    portEXIT_CRITICAL(&state_lock);
    return ESP_OK;
}
