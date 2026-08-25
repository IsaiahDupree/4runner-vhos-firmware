#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * A retained-history response must fit in one ATT notification.  Keeping the
 * VHOS frame atomic means a temporary controller-buffer shortage can defer the
 * response without leaving the mobile decoder stranded behind a partial frame.
 */
#define VHOS_BLE_HISTORY_RECORD_CAPACITY 3U
#define VHOS_BLE_HISTORY_FRAME_HEADER_BYTES 52U
#define VHOS_BLE_HISTORY_RECORD_BYTES 36U
#define VHOS_BLE_HISTORY_MAX_FRAME_BYTES                                      \
    (VHOS_BLE_HISTORY_FRAME_HEADER_BYTES +                                    \
     VHOS_BLE_HISTORY_RECORD_CAPACITY * VHOS_BLE_HISTORY_RECORD_BYTES)
#define VHOS_BLE_HISTORY_MIN_ATT_MTU (VHOS_BLE_HISTORY_MAX_FRAME_BYTES + 3U)
#define VHOS_BLE_HISTORY_BACKPRESSURE_RETRY_LIMIT 8U
#define VHOS_BLE_HISTORY_BACKPRESSURE_RETRY_DELAY_MS 50U
#define VHOS_BLE_HISTORY_BACKPRESSURE_BUDGET_MS                                \
    ((VHOS_BLE_HISTORY_BACKPRESSURE_RETRY_LIMIT - 1U) *                        \
     VHOS_BLE_HISTORY_BACKPRESSURE_RETRY_DELAY_MS)

typedef enum {
    VHOS_BLE_DELIVERY_FAILURE_KEEP_EPOCH = 0,
    VHOS_BLE_DELIVERY_FAILURE_TERMINATE_EPOCH = 1,
} vhos_ble_delivery_failure_action_t;

static inline size_t vhos_ble_notification_capacity(uint16_t att_mtu)
{
    return att_mtu > 3U ? (size_t)att_mtu - 3U : 20U;
}

static inline bool vhos_ble_history_frame_is_atomic(uint16_t att_mtu, size_t frame_length)
{
    return frame_length <= vhos_ble_notification_capacity(att_mtu);
}

static inline unsigned int vhos_ble_notification_retry_limit(
    bool history_frame,
    unsigned int default_retry_limit
)
{
    return history_frame ? VHOS_BLE_HISTORY_BACKPRESSURE_RETRY_LIMIT : default_retry_limit;
}

static inline vhos_ble_delivery_failure_action_t vhos_ble_delivery_failure_action(
    bool history_frame,
    size_t delivered_bytes
)
{
    /* No history byte was admitted, so the same request can be retried safely. */
    if (history_frame && delivered_bytes == 0U) {
        return VHOS_BLE_DELIVERY_FAILURE_KEEP_EPOCH;
    }
    /* Other partial logical frames need a fresh decoder boundary. */
    return VHOS_BLE_DELIVERY_FAILURE_TERMINATE_EPOCH;
}
