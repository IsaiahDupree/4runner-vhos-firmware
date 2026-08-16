#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint64_t received_frames;
    uint64_t dropped_frames;
    uint64_t bus_error_count;
    uint64_t bus_off_count;
    bool listen_only;
    bool controller_running;
    uint32_t bitrate_bps;
} vhos_can_health_t;

esp_err_t vhos_can_start(void);
esp_err_t vhos_can_get_health(vhos_can_health_t *health);
