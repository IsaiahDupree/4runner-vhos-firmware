#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define VHOS_CAPTURE_RECORD_BYTES 36U
#define VHOS_CAPTURE_SLOT_CURRENT 0U
#define VHOS_CAPTURE_SLOT_PREVIOUS 1U

typedef struct {
    bool mounted;
    bool logging;
    uint32_t current_session_id;
    uint32_t previous_session_id;
    uint32_t current_records;
    uint32_t previous_records;
    uint32_t current_bytes;
    uint32_t previous_bytes;
    uint32_t total_bytes;
    uint32_t free_bytes;
    uint64_t observed_frames;
    uint64_t sampled_frames;
    uint64_t retained_records;
    uint64_t sample_suppressed_frames;
    uint64_t queue_dropped_records;
    uint64_t storage_write_failures;
} vhos_capture_store_status_t;

esp_err_t vhos_capture_store_start(void);
esp_err_t vhos_capture_store_get_runtime_status(vhos_capture_store_status_t *status);
esp_err_t vhos_capture_store_get_status(vhos_capture_store_status_t *status);
esp_err_t vhos_capture_store_read_records(
    uint8_t slot,
    uint32_t record_offset,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length,
    uint32_t *record_count,
    bool *end_of_file,
    uint32_t *session_id
);
esp_err_t vhos_capture_store_rotate(void);
esp_err_t vhos_capture_store_set_logging(bool enabled);
bool vhos_capture_store_export_ready(void);
uint32_t vhos_capture_store_current_session_id(void);
