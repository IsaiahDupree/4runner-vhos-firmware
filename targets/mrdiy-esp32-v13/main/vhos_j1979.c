#include "vhos_j1979.h"

#include <string.h>

#define VHOS_J1979_FUNCTIONAL_REQUEST_ID 0x7DFU
#define VHOS_J1979_POSITIVE_MODE_01 0x41U

static bool valid_supported_base_pid(uint8_t pid)
{
    return (pid % 0x20U) == 0U;
}

bool vhos_j1979_decode_passive_response(
    const vhos_can_observation_t *observation,
    uint32_t capture_session_id,
    vhos_j1979_response_t *response
)
{
    if (observation == NULL || response == NULL || observation->extended ||
        observation->remote_request || observation->identifier < 0x7E8U ||
        observation->identifier > 0x7EFU || observation->data_length < 4U ||
        (observation->bitrate_bps != 250000U && observation->bitrate_bps != 500000U)) {
        return false;
    }
    uint8_t pci = observation->data[0];
    uint8_t response_length = pci & 0x0FU;
    if ((pci & 0xF0U) != 0U || response_length < 2U ||
        response_length > VHOS_J1979_MAX_RESPONSE_BYTES ||
        (uint8_t)(response_length + 1U) > observation->data_length ||
        observation->data[1] != VHOS_J1979_POSITIVE_MODE_01) {
        return false;
    }
    memset(response, 0, sizeof(*response));
    response->ecu_identifier = observation->identifier;
    response->source_sequence = observation->source_sequence;
    response->monotonic_us = observation->monotonic_us;
    response->capture_session_id = capture_session_id;
    response->transport = observation->bitrate_bps == 250000U
                              ? VHOS_J1979_TRANSPORT_ISO_15765_11_250
                              : VHOS_J1979_TRANSPORT_ISO_15765_11_500;
    response->pid = observation->data[2];
    response->response_length = response_length;
    memcpy(response->response, &observation->data[1], response_length);
    return true;
}

esp_err_t vhos_j1979_decode_supported_bitmap(
    uint8_t base_pid,
    const uint8_t bitmap[4],
    uint8_t supported[32],
    size_t *supported_count
)
{
    if (!valid_supported_base_pid(base_pid) || bitmap == NULL || supported == NULL ||
        supported_count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *supported_count = 0;
    for (uint8_t offset = 1U; offset <= 32U; offset++) {
        uint8_t bit_index = (uint8_t)(offset - 1U);
        uint8_t byte_index = bit_index / 8U;
        uint8_t mask = (uint8_t)(0x80U >> (bit_index % 8U));
        if ((bitmap[byte_index] & mask) != 0U &&
            (uint16_t)base_pid + (uint16_t)offset <= UINT8_MAX) {
            supported[(*supported_count)++] = (uint8_t)(base_pid + offset);
        }
    }
    return ESP_OK;
}

esp_err_t vhos_j1979_plan_supported_pid_request(
    uint8_t base_pid,
    const vhos_j1979_safety_context_t *safety,
    vhos_j1979_request_frame_t *request
)
{
    if (!valid_supported_base_pid(base_pid) || safety == NULL || request == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!safety->signed_plan_verified || !safety->deterministically_parked ||
        !safety->capture_idle || !safety->protocol_confirmed) {
        return ESP_ERR_INVALID_STATE;
    }
    *request = (vhos_j1979_request_frame_t){
        .identifier = VHOS_J1979_FUNCTIONAL_REQUEST_ID,
        .data_length = 8U,
        .data = {0x02U, 0x01U, base_pid, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U},
    };
    return ESP_OK;
}
