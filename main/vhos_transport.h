/*
 * VHOS transport extension for WiCAN Pro.
 * Copyright (C) 2026 Isaiah Dupree.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef esp_err_t (*vhos_transport_emit_fn)(const uint8_t *data, size_t length, bool health_channel);

void vhos_transport_init(const char *gateway_id, vhos_transport_emit_fn emit);
void vhos_transport_reset(void);
esp_err_t vhos_transport_ingest(const uint8_t *data, size_t length);
