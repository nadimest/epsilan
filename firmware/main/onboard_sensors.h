#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

typedef struct {
    float ambient_lux;
    uint16_t proximity_raw;
    uint16_t battery_mv;
    uint8_t battery_percent;
    bool charging;
    bool battery_present;
    bool external_power;
    bool optical_valid;
    bool battery_valid;
} onboard_sensor_reading_t;

typedef struct {
    i2c_master_dev_handle_t optical;
    i2c_master_dev_handle_t power;
    bool optical_ready;
    bool power_ready;
} onboard_sensors_t;

/** Initialize the CoreS3 LTR-553 optical sensor and AXP2101 telemetry. */
esp_err_t onboard_sensors_init(onboard_sensors_t *sensors, i2c_master_bus_handle_t bus);

/** Read all available values. Individual validity flags indicate partial failures. */
esp_err_t onboard_sensors_read(onboard_sensors_t *sensors, onboard_sensor_reading_t *reading);
