#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "vhos_can.h"

#define VHOS_J1979_MAX_RESPONSE_BYTES 7U

typedef enum {
    VHOS_J1979_TRANSPORT_ISO_15765_11_500 = 1,
    VHOS_J1979_TRANSPORT_ISO_15765_11_250 = 2,
} vhos_j1979_transport_t;

typedef struct {
    uint32_t ecu_identifier;
    uint64_t source_sequence;
    uint64_t monotonic_us;
    uint32_t capture_session_id;
    vhos_j1979_transport_t transport;
    uint8_t pid;
    uint8_t response_length;
    uint8_t response[VHOS_J1979_MAX_RESPONSE_BYTES];
} vhos_j1979_response_t;

typedef struct {
    bool signed_plan_verified;
    bool deterministically_parked;
    bool capture_idle;
    bool protocol_confirmed;
} vhos_j1979_safety_context_t;

typedef struct {
    uint32_t identifier;
    uint8_t data_length;
    uint8_t data[8];
} vhos_j1979_request_frame_t;

bool vhos_j1979_decode_passive_response(
    const vhos_can_observation_t *observation,
    uint32_t capture_session_id,
    vhos_j1979_response_t *response
);

esp_err_t vhos_j1979_decode_supported_bitmap(
    uint8_t base_pid,
    const uint8_t bitmap[4],
    uint8_t supported[32],
    size_t *supported_count
);

/*
 * Constructs only the fixed functional Mode 01 supported-PID request. It does not transmit.
 * Every safety predicate must be true; the production target currently supplies no caller that
 * can satisfy deterministic PARKED proof, so active diagnostic transmission remains unavailable.
 */
esp_err_t vhos_j1979_plan_supported_pid_request(
    uint8_t base_pid,
    const vhos_j1979_safety_context_t *safety,
    vhos_j1979_request_frame_t *request
);
