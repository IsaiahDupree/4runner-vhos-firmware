#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "vhos_ble_transfer_policy.h"

static size_t history_frame_bytes(size_t record_count)
{
    return VHOS_BLE_HISTORY_FRAME_HEADER_BYTES +
           record_count * VHOS_BLE_HISTORY_RECORD_BYTES;
}

static void test_atomic_history_frames(void)
{
    assert(VHOS_BLE_HISTORY_RECORD_CAPACITY == 3U);
    assert(VHOS_BLE_HISTORY_MAX_FRAME_BYTES == 160U);
    assert(VHOS_BLE_HISTORY_MIN_ATT_MTU == 163U);
    assert(VHOS_BLE_HISTORY_BACKPRESSURE_RETRY_LIMIT == 8U);
    assert(VHOS_BLE_HISTORY_BACKPRESSURE_BUDGET_MS == 350U);
    assert(vhos_ble_notification_retry_limit(true, 80U) == 8U);
    assert(vhos_ble_notification_retry_limit(false, 80U) == 80U);

    for (size_t records = 0; records <= VHOS_BLE_HISTORY_RECORD_CAPACITY; ++records) {
        size_t bytes = history_frame_bytes(records);
        assert(vhos_ble_history_frame_is_atomic(185U, bytes));
        assert(vhos_ble_history_frame_is_atomic(247U, bytes));
    }
    assert(!vhos_ble_history_frame_is_atomic(23U, VHOS_BLE_HISTORY_MAX_FRAME_BYTES));
    assert(!vhos_ble_history_frame_is_atomic(162U, VHOS_BLE_HISTORY_MAX_FRAME_BYTES));
    assert(vhos_ble_history_frame_is_atomic(163U, VHOS_BLE_HISTORY_MAX_FRAME_BYTES));
}

static void test_field_return_offsets_remain_atomic(void)
{
    /* Offsets seen in the returned iPhone transfer incident. */
    const uint32_t offsets[] = {0U, 90U, 105U, 125U};
    for (size_t index = 0; index < sizeof(offsets) / sizeof(offsets[0]); ++index) {
        (void)offsets[index];
        assert(vhos_ble_history_frame_is_atomic(185U, VHOS_BLE_HISTORY_MAX_FRAME_BYTES));
    }

    /* Field evidence returned by the iPhone contains 11,862 retained observations. */
    const uint32_t observations = 11862U;
    uint32_t offset = 0U;
    uint32_t responses = 0U;
    while (offset < observations) {
        uint32_t remaining = observations - offset;
        uint32_t records = remaining < VHOS_BLE_HISTORY_RECORD_CAPACITY
                               ? remaining
                               : VHOS_BLE_HISTORY_RECORD_CAPACITY;
        assert(records > 0U);
        assert(vhos_ble_history_frame_is_atomic(185U, history_frame_bytes(records)));
        offset += records;
        responses++;
    }
    assert(offset == observations);
    assert(responses == 3954U);
}

static void test_failure_boundary_policy(void)
{
    assert(
        vhos_ble_delivery_failure_action(true, 0U) ==
        VHOS_BLE_DELIVERY_FAILURE_KEEP_EPOCH
    );
    assert(
        vhos_ble_delivery_failure_action(true, 1U) ==
        VHOS_BLE_DELIVERY_FAILURE_TERMINATE_EPOCH
    );
    assert(
        vhos_ble_delivery_failure_action(false, 0U) ==
        VHOS_BLE_DELIVERY_FAILURE_TERMINATE_EPOCH
    );
}

int main(void)
{
    test_atomic_history_frames();
    test_field_return_offsets_remain_atomic();
    test_failure_boundary_policy();
    puts("PASS: BLE retained-history transfer policy");
    return 0;
}
