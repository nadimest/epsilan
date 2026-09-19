#include "onboard_sensors.h"

#include <math.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "onboard_sensors";

#define LTR553_ADDRESS       0x23
#define LTR553_ALS_CONTR     0x80
#define LTR553_PS_CONTR      0x81
#define LTR553_PART_ID       0x86
#define LTR553_ALS_DATA_CH1  0x88

#define AXP2101_ADDRESS      0x34
#define AXP2101_STATUS0      0x00
#define AXP2101_STATUS1      0x01
#define AXP2101_CHIP_ID      0x03
#define AXP2101_ADC_ENABLE   0x30
#define AXP2101_VBAT_H       0x34
#define AXP2101_BAT_PERCENT  0xA4

static esp_err_t add_device(i2c_master_bus_handle_t bus, uint8_t address,
                            i2c_master_dev_handle_t *device)
{
    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = 400000,
    };
    return i2c_master_bus_add_device(bus, &config, device);
}

static esp_err_t read_register(i2c_master_dev_handle_t device, uint8_t reg,
                               uint8_t *data, size_t size)
{
    return i2c_master_transmit_receive(device, &reg, 1, data, size, 100);
}

static esp_err_t write_register(i2c_master_dev_handle_t device, uint8_t reg, uint8_t value)
{
    const uint8_t data[] = { reg, value };
    return i2c_master_transmit(device, data, sizeof(data), 100);
}

static float ltr553_counts_to_lux(uint16_t ch0, uint16_t ch1)
{
    const float total = ch0 + ch1;
    if (total <= 0.0f) return 0.0f;

    const float ratio = ch1 / total;
    float lux;
    if (ratio < 0.45f) {
        lux = 1.7743f * ch0 + 1.1059f * ch1;
    } else if (ratio < 0.64f) {
        lux = 4.2785f * ch0 - 1.9548f * ch1;
    } else if (ratio < 0.85f) {
        lux = 0.5926f * ch0 + 0.1185f * ch1;
    } else {
        lux = 0.0f;
    }
    return fmaxf(lux, 0.0f); /* 1x gain and the default 100 ms integration time. */
}

esp_err_t onboard_sensors_init(onboard_sensors_t *sensors, i2c_master_bus_handle_t bus)
{
    if (!sensors || !bus) return ESP_ERR_INVALID_ARG;
    memset(sensors, 0, sizeof(*sensors));
    esp_err_t result = ESP_OK;

    esp_err_t err = add_device(bus, LTR553_ADDRESS, &sensors->optical);
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(100));
        uint8_t part_id = 0;
        err = read_register(sensors->optical, LTR553_PART_ID, &part_id, 1);
        if (err == ESP_OK && (part_id & 0xf0) == 0x90) {
            err = write_register(sensors->optical, LTR553_ALS_CONTR, 0x01);
            if (err == ESP_OK) err = write_register(sensors->optical, LTR553_PS_CONTR, 0x03);
            if (err == ESP_OK) {
                sensors->optical_ready = true;
                ESP_LOGI(TAG, "LTR-553 ready (part ID 0x%02x)", part_id);
            } else {
                ESP_LOGW(TAG, "Could not enable LTR-553: %s", esp_err_to_name(err));
                result = err;
            }
        } else {
            ESP_LOGW(TAG, "LTR-553 unavailable: %s, part ID 0x%02x",
                     esp_err_to_name(err), part_id);
            result = err == ESP_OK ? ESP_ERR_NOT_FOUND : err;
        }
    } else {
        ESP_LOGW(TAG, "Could not register LTR-553: %s", esp_err_to_name(err));
        result = err;
    }

    err = add_device(bus, AXP2101_ADDRESS, &sensors->power);
    if (err == ESP_OK) {
        uint8_t chip_id = 0;
        err = read_register(sensors->power, AXP2101_CHIP_ID, &chip_id, 1);
        if (err == ESP_OK && chip_id == 0x4a) {
            uint8_t adc_enable = 0;
            if (read_register(sensors->power, AXP2101_ADC_ENABLE, &adc_enable, 1) == ESP_OK) {
                esp_err_t adc_err = write_register(sensors->power, AXP2101_ADC_ENABLE,
                                                   adc_enable | 0x0f);
                if (adc_err != ESP_OK) {
                    ESP_LOGW(TAG, "Could not enable AXP2101 ADCs: %s", esp_err_to_name(adc_err));
                }
            }
            sensors->power_ready = true;
            ESP_LOGI(TAG, "AXP2101 telemetry ready");
        } else {
            ESP_LOGW(TAG, "AXP2101 telemetry unavailable: %s, chip ID 0x%02x",
                     esp_err_to_name(err), chip_id);
            if (result == ESP_OK) result = err == ESP_OK ? ESP_ERR_NOT_FOUND : err;
        }
    } else {
        ESP_LOGW(TAG, "Could not register AXP2101 telemetry: %s", esp_err_to_name(err));
        if (result == ESP_OK) result = err;
    }

    /* Partial initialization is useful: callers inspect the per-reading validity flags. */
    return sensors->optical_ready || sensors->power_ready ? ESP_OK : result;
}

esp_err_t onboard_sensors_read(onboard_sensors_t *sensors, onboard_sensor_reading_t *reading)
{
    if (!sensors || !reading) return ESP_ERR_INVALID_ARG;
    memset(reading, 0, sizeof(*reading));
    esp_err_t result = ESP_ERR_NOT_FOUND;

    if (sensors->optical_ready) {
        uint8_t data[7];
        esp_err_t err = read_register(sensors->optical, LTR553_ALS_DATA_CH1, data, sizeof(data));
        if (err == ESP_OK) {
            const uint16_t ch1 = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
            const uint16_t ch0 = (uint16_t)data[2] | ((uint16_t)data[3] << 8);
            reading->proximity_raw = (uint16_t)data[5] | (((uint16_t)data[6] & 0x07) << 8);
            reading->ambient_lux = ltr553_counts_to_lux(ch0, ch1);
            reading->optical_valid = true;
            result = ESP_OK;
        } else {
            result = err;
        }
    }

    if (sensors->power_ready) {
        uint8_t battery[2];
        uint8_t percent;
        uint8_t status0;
        uint8_t status1;
        esp_err_t err = read_register(sensors->power, AXP2101_VBAT_H, battery, sizeof(battery));
        if (err == ESP_OK) err = read_register(sensors->power, AXP2101_BAT_PERCENT, &percent, 1);
        if (err == ESP_OK) err = read_register(sensors->power, AXP2101_STATUS0, &status0, 1);
        if (err == ESP_OK) err = read_register(sensors->power, AXP2101_STATUS1, &status1, 1);
        if (err == ESP_OK) {
            reading->battery_mv = ((uint16_t)(battery[0] & 0x3f) << 8) | battery[1];
            reading->battery_percent = percent <= 100 ? percent : 100;
            reading->battery_present = (status0 & 0x08) != 0;
            reading->external_power = (status0 & 0x20) != 0;
            reading->charging = (status1 & 0x60) == 0x20;
            reading->battery_valid = true;
            result = ESP_OK;
        } else if (result != ESP_OK) {
            result = err;
        }
    }

    return result;
}
