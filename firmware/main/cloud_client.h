#pragma once

#include "esp_err.h"

/** Starts the SiLA 2 server-initiated cloud connector after Wi-Fi has an IP address. */
esp_err_t epsilan_cloud_client_start(void);
