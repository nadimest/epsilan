#pragma once

#include "esp_err.h"

#define EPSILAN_SILA_PORT 50052

esp_err_t sila_server_start(const char *server_uuid);
void sila_server_set_acceleration(float x, float y, float z);

