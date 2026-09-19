#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

typedef struct {
    float acceleration_ms2;
    float angular_rate_dps;
    float ambient_lux;
    uint16_t proximity_raw;
    uint16_t battery_mv;
    uint8_t battery_percent;
    int64_t uptime_seconds;
    bool motion_valid;
    bool optical_valid;
    bool battery_valid;
    bool charging;
    bool battery_present;
    bool external_power;
    bool wifi_connected;
} dashboard_reading_t;

typedef struct {
    lv_obj_t *accel_arc;
    lv_obj_t *accel_value;
    lv_obj_t *gyro_arc;
    lv_obj_t *gyro_value;
    lv_obj_t *light_value;
    lv_obj_t *light_bar;
    lv_obj_t *proximity_value;
    lv_obj_t *proximity_bar;
    lv_obj_t *battery_value;
    lv_obj_t *battery_bar;
    lv_obj_t *wifi_dot;
    lv_obj_t *status;
    lv_obj_t *setup_button;
} dashboard_t;

void dashboard_create(dashboard_t *dashboard, lv_obj_t *screen, lv_event_cb_t setup_event);
void dashboard_update(dashboard_t *dashboard, const dashboard_reading_t *reading,
                      const char *network_status);
