#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    VHOS_TRANSPORT_CHANNEL_STREAM = 0,
    VHOS_TRANSPORT_CHANNEL_HEALTH = 1,
    VHOS_TRANSPORT_CHANNEL_OTA = 2,
} vhos_transport_channel_t;

typedef enum {
    VHOS_TRANSPORT_EMIT_SESSION_REQUIRED = 0,
    VHOS_TRANSPORT_EMIT_BOOTSTRAP_HANDSHAKE = 1,
} vhos_transport_emit_scope_t;

typedef esp_err_t (*vhos_transport_emit_fn)(
    const uint8_t *data,
    size_t length,
    vhos_transport_channel_t channel,
    vhos_transport_emit_scope_t scope
);

typedef struct {
    /* Zero is the expected result while an incremental frame is still incomplete. */
    size_t completed_frames;
    /* True only after a valid handshake request and its response enter the TX queue. */
    bool handshake_accepted;
} vhos_transport_ingest_result_t;

void vhos_transport_init(const char *gateway_id, vhos_transport_emit_fn emit);
void vhos_transport_reset(void);
esp_err_t vhos_transport_ingest(
    const uint8_t *data,
    size_t length,
    bool session_ready,
    vhos_transport_ingest_result_t *ingest_result
);
esp_err_t vhos_transport_send_health(void);
esp_err_t vhos_transport_send_ota_status(const char *json);
esp_err_t vhos_transport_send_session_status(void);
bool vhos_transport_history_transfer_active(void);
