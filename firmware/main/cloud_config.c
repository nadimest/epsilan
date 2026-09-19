#include "cloud_config.h"

#include <string.h>
#include <ctype.h>

#include "nvs.h"

void epsilan_cloud_config_defaults(epsilan_cloud_config_t *config)
{
    memset(config, 0, sizeof(*config));
    config->port = 443;
    config->tls = true;
}

bool epsilan_cloud_config_valid(const epsilan_cloud_config_t *config)
{
    return config && config->port != 0 &&
           (!config->enabled || config->endpoint[0] != '\0');
}

void epsilan_cloud_config_normalize(epsilan_cloud_config_t *config)
{
    if (!config) return;
    char *start = config->endpoint;
    while (*start && isspace((unsigned char)*start)) ++start;
    if (start != config->endpoint) memmove(config->endpoint, start, strlen(start) + 1);

    size_t length = strlen(config->endpoint);
    while (length && isspace((unsigned char)config->endpoint[length - 1])) {
        config->endpoint[--length] = '\0';
    }
    if (length >= 2 && config->endpoint[0] == '"' && config->endpoint[length - 1] == '"') {
        memmove(config->endpoint, config->endpoint + 1, length - 2);
        config->endpoint[length - 2] = '\0';
    }
}

esp_err_t epsilan_cloud_config_load(epsilan_cloud_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    epsilan_cloud_config_defaults(config);

    nvs_handle_t nvs;
    esp_err_t err = nvs_open("epsilan", NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;

    uint8_t enabled = 0;
    uint8_t tls = 1;
    uint16_t port = config->port;
    size_t endpoint_size = sizeof(config->endpoint);
    if (nvs_get_u8(nvs, "cloud_enabled", &enabled) == ESP_OK) config->enabled = enabled != 0;
    if (nvs_get_str(nvs, "cloud_endpoint", config->endpoint, &endpoint_size) != ESP_OK) {
        config->endpoint[0] = '\0';
    }
    if (nvs_get_u16(nvs, "cloud_port", &port) == ESP_OK) config->port = port;
    if (nvs_get_u8(nvs, "cloud_tls", &tls) == ESP_OK) config->tls = tls != 0;
    nvs_close(nvs);
    epsilan_cloud_config_normalize(config);
    return epsilan_cloud_config_valid(config) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t epsilan_cloud_config_save(const epsilan_cloud_config_t *config)
{
    epsilan_cloud_config_t normalized = *config;
    epsilan_cloud_config_normalize(&normalized);
    config = &normalized;
    if (!epsilan_cloud_config_valid(config)) return ESP_ERR_INVALID_ARG;

    nvs_handle_t nvs;
    esp_err_t err = nvs_open("epsilan", NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(nvs, "cloud_enabled", config->enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_str(nvs, "cloud_endpoint", config->endpoint);
    if (err == ESP_OK) err = nvs_set_u16(nvs, "cloud_port", config->port);
    if (err == ESP_OK) err = nvs_set_u8(nvs, "cloud_tls", config->tls ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}
