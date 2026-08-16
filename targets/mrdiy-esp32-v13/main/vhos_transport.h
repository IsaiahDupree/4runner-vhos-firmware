#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef esp_err_t (*vhos_transport_emit_fn)(
    const uint8_t *data,
    size_t length,
    bool health_channel
);

void vhos_transport_init(const char *gateway_id, vhos_transport_emit_fn emit);
void vhos_transport_reset(void);
esp_err_t vhos_transport_ingest(const uint8_t *data, size_t length);
esp_err_t vhos_transport_send_health(void);
