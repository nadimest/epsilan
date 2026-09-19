#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EPSILAN_SILA_PORT 50052

typedef struct {
    float acceleration_x;
    float acceleration_y;
    float acceleration_z;
    float angular_rate_x;
    float angular_rate_y;
    float angular_rate_z;
    float ambient_lux;
    uint16_t proximity_raw;
    uint16_t battery_mv;
    uint8_t battery_percent;
    bool battery_present;
    bool external_power;
    bool charging;
} epsilan_telemetry_t;

typedef enum {
    EPSILAN_PROPERTY_NONE,
    EPSILAN_PROPERTY_ACCELERATION,
    EPSILAN_PROPERTY_ANGULAR_RATE,
    EPSILAN_PROPERTY_AMBIENT_LIGHT,
    EPSILAN_PROPERTY_PROXIMITY,
    EPSILAN_PROPERTY_BATTERY_LEVEL,
    EPSILAN_PROPERTY_BATTERY_VOLTAGE,
    EPSILAN_PROPERTY_BATTERY_PRESENT,
    EPSILAN_PROPERTY_EXTERNAL_POWER,
    EPSILAN_PROPERTY_CHARGING,
} epsilan_property_t;

esp_err_t sila_server_start(const char *server_uuid);
void sila_server_set_telemetry(const epsilan_telemetry_t *telemetry);

/** Returns the current property protobuf payload; caller frees it. */
uint8_t *sila_server_property_value(epsilan_property_t property, size_t *out_length);

/**
 * Dispatches one unobservable SiLA Cloud call through the same implementation
 * used by the LAN gRPC server.  The returned payload is the unframed protobuf
 * response and must be freed by the caller.
 */
bool sila_server_cloud_unary_call(const char *fqi, bool is_property,
                                  const uint8_t *request, size_t request_length,
                                  uint8_t **response, size_t *response_length,
                                  int *grpc_status);
