#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    VHOS_CAN_SCAN_PROBING_500K = 0,
    VHOS_CAN_SCAN_PROBING_250K,
    VHOS_CAN_SCAN_LOCKED_500K,
    VHOS_CAN_SCAN_LOCKED_250K,
    VHOS_CAN_SCAN_ERROR,
} vhos_can_scan_state_t;

typedef struct {
    uint64_t received_frames;
    uint64_t dropped_frames;
    uint64_t bus_error_count;
    uint64_t bus_off_count;
    uint64_t standard_frames;
    uint64_t extended_frames;
    uint64_t frames_500k;
    uint64_t frames_250k;
    uint64_t candidate_standard_frames;
    uint64_t candidate_extended_frames;
    uint32_t scan_cycles;
    bool listen_only;
    bool controller_running;
    bool passive_lock;
    uint32_t bitrate_bps;
    vhos_can_scan_state_t scan_state;
} vhos_can_health_t;

esp_err_t vhos_can_start(void);
esp_err_t vhos_can_get_health(vhos_can_health_t *health);
const char *vhos_can_scan_state_name(vhos_can_scan_state_t state);
const char *vhos_can_passive_candidate(const vhos_can_health_t *health);
