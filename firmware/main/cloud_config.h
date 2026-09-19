#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define EPSILAN_CLOUD_ENDPOINT_MAX 128

typedef struct {
    bool enabled;
    char endpoint[EPSILAN_CLOUD_ENDPOINT_MAX];
    uint16_t port;
    bool tls;
} epsilan_cloud_config_t;

void epsilan_cloud_config_defaults(epsilan_cloud_config_t *config);
esp_err_t epsilan_cloud_config_load(epsilan_cloud_config_t *config);
esp_err_t epsilan_cloud_config_save(const epsilan_cloud_config_t *config);
bool epsilan_cloud_config_valid(const epsilan_cloud_config_t *config);
void epsilan_cloud_config_normalize(epsilan_cloud_config_t *config);
