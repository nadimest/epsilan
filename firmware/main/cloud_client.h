#pragma once

#include "esp_err.h"

/** Queues the newest IMU sample for any active cloud acceleration subscription. */
void epsilan_cloud_client_publish_acceleration(float x, float y, float z);

/** Starts the SiLA 2 server-initiated cloud connector after Wi-Fi has an IP address. */
esp_err_t epsilan_cloud_client_start(void);
