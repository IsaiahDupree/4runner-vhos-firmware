#pragma once

#include "esp_err.h"

#define VHOS_STATUS_WINDOW_SECONDS 900U

esp_err_t vhos_status_web_start(const char *gateway_id);
