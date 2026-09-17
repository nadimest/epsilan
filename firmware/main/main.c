#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/usb_serial_jtag.h"
#include "driver/i2c_master.h"
#include "esp_chip_info.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_psram.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_ble.h"
#include "bsp/esp-bsp.h"
#include "bmi270.h"
#include "lvgl.h"

static const char *TAG = "epsilan";
static lv_obj_t *readings;
static lv_obj_t *status;
static lv_obj_t *setup_qr;
static lv_obj_t *setup_details;
static lv_obj_t *setup_button;
static unsigned brightness = 60;
static char server_uuid[37];
static char network_status[128] = "Wi-Fi starting...";
static char setup_name[16];
static char setup_pop[16];
static bool wifi_ready;
static bool provisioning_active;
static volatile bool setup_requested;
static volatile bool runtime_screen_requested;

static void setup_button_event(lv_event_t *event);
static esp_err_t set_wifi_credentials(const char *ssid, const char *password);
static esp_err_t load_or_create_token(const char *key, char *value, size_t value_size, const char *prefix);

static void set_network_status(const char *value)
{
    strlcpy(network_status, value, sizeof(network_status));
}

static esp_err_t load_settings(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("epsilan", NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;

    size_t uuid_size = sizeof(server_uuid);
    err = nvs_get_str(nvs, "server_uuid", server_uuid, &uuid_size);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        uint32_t random[4];
        esp_fill_random(random, sizeof(random));
        snprintf(server_uuid, sizeof(server_uuid),
                 "%08" PRIx32 "-%04" PRIx32 "-4%03" PRIx32 "-%04" PRIx32 "-%08" PRIx32 "%04" PRIx32,
                 random[0], random[1] >> 16, random[1] & 0x0fff,
                 (random[2] >> 16 & 0x3fff) | 0x8000, random[2] & 0xffff,
                 random[3] >> 16);
        err = nvs_set_str(nvs, "server_uuid", server_uuid);
        if (err == ESP_OK) err = nvs_commit(nvs);
    }
    if (err == ESP_OK) {
        uint8_t saved_brightness;
        if (nvs_get_u8(nvs, "brightness", &saved_brightness) == ESP_OK &&
            saved_brightness >= 1 && saved_brightness <= 100) {
            brightness = saved_brightness;
        }
    }
    nvs_close(nvs);
    return err;
}

static esp_err_t save_brightness(unsigned value)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("epsilan", NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, "brightness", (uint8_t)value);
        if (err == ESP_OK) err = nvs_commit(nvs);
        nvs_close(nvs);
    }
    return err;
}

static bool epsilan_wifi_is_configured(void)
{
    nvs_handle_t nvs;
    uint8_t configured = 0;
    if (nvs_open("epsilan", NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u8(nvs, "wifi_configured", &configured);
        nvs_close(nvs);
    }
    return configured == 1;
}

static esp_err_t mark_epsilan_wifi_configured(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("epsilan", NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, "wifi_configured", 1);
        if (err == ESP_OK) err = nvs_commit(nvs);
        nvs_close(nvs);
    }
    return err;
}

static void show_runtime_screen(void)
{
    if (readings) return;
    lv_obj_t *screen = lv_scr_act();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101b27), 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(0xe5eff9), 0);
    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "EPSILAN / C firmware");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 16, 12);
    readings = lv_label_create(screen);
    lv_obj_align(readings, LV_ALIGN_TOP_LEFT, 16, 46);
    status = lv_label_create(screen);
    lv_obj_align(status, LV_ALIGN_BOTTOM_LEFT, 16, -12);
    setup_button = lv_button_create(screen);
    lv_obj_set_size(setup_button, 132, 30);
    lv_obj_align(setup_button, LV_ALIGN_TOP_RIGHT, -12, 8);
    lv_obj_t *label = lv_label_create(setup_button);
    lv_label_set_text(label, "Hold: reset Wi-Fi");
    lv_obj_center(label);
    lv_obj_add_event_cb(setup_button, setup_button_event, LV_EVENT_LONG_PRESSED, NULL);
    setup_qr = NULL;
    setup_details = NULL;
}

static void show_setup_screen(void)
{
    lv_obj_t *screen = lv_scr_act();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101b27), 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(0xe5eff9), 0);
    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "SET UP EPSILAN");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 14, 10);
    setup_qr = lv_qrcode_create(screen);
    lv_qrcode_set_size(setup_qr, 112);
    lv_qrcode_set_dark_color(setup_qr, lv_color_hex(0x101b27));
    lv_qrcode_set_light_color(setup_qr, lv_color_hex(0xe5eff9));
    lv_qrcode_set_quiet_zone(setup_qr, true);
    char pairing_uri[160];
    snprintf(pairing_uri, sizeof(pairing_uri),
             "{\"ver\":\"v1\",\"name\":\"%s\",\"pop\":\"%s\",\"transport\":\"ble\",\"security\":1}",
             setup_name, setup_pop);
    lv_qrcode_update(setup_qr, pairing_uri, strlen(pairing_uri));
    lv_obj_align(setup_qr, LV_ALIGN_LEFT_MID, 12, 10);
    setup_details = lv_label_create(screen);
    lv_label_set_text_fmt(setup_details,
                          "Open ESP BLE\nProvisioning\n\nScan this QR\n\nDevice %s\nPoP %s",
                          setup_name, setup_pop);
    lv_obj_align(setup_details, LV_ALIGN_TOP_LEFT, 140, 48);
    status = lv_label_create(screen);
    lv_obj_align(status, LV_ALIGN_BOTTOM_LEFT, 14, -8);
    lv_label_set_text(status, network_status);
    readings = NULL;
    setup_button = NULL;
}

static void setup_button_event(lv_event_t *event)
{
    (void)event;
    setup_requested = true;
}

static esp_err_t load_or_create_token(const char *key, char *value, size_t value_size, const char *prefix)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("epsilan", NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    size_t stored_size = value_size;
    err = nvs_get_str(nvs, key, value, &stored_size);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        snprintf(value, value_size, "%s%08" PRIx32, prefix, esp_random());
        err = nvs_set_str(nvs, key, value);
        if (err == ESP_OK) err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

static void network_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START && !provisioning_active) {
        esp_wifi_connect();
        set_network_status("Wi-Fi connecting...");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED && !provisioning_active) {
        esp_wifi_connect();
        set_network_status("Wi-Fi reconnecting...");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = event_data;
        snprintf(network_status, sizeof(network_status), "Wi-Fi " IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Wi-Fi connected: " IPSTR, IP2STR(&event->ip_info.ip));
    } else if (event_base == WIFI_PROV_EVENT) {
        if (event_id == WIFI_PROV_CRED_SUCCESS) {
            ESP_ERROR_CHECK(mark_epsilan_wifi_configured());
            set_network_status("Wi-Fi credentials accepted");
            ESP_LOGI(TAG, "BLE provisioning succeeded");
        } else if (event_id == WIFI_PROV_CRED_FAIL) {
            set_network_status("Wi-Fi failed; retry in app");
            ESP_LOGW(TAG, "BLE provisioning failed; check credentials and retry");
        } else if (event_id == WIFI_PROV_END) {
            provisioning_active = false;
            wifi_prov_mgr_deinit();
            runtime_screen_requested = true;
        }
    }
}

static esp_err_t set_wifi_credentials(const char *ssid, const char *password)
{
    if (!wifi_ready || !ssid[0] || strlen(ssid) > 32 || strlen(password) > 63) return ESP_ERR_INVALID_ARG;
    if (provisioning_active) {
        wifi_prov_mgr_stop_provisioning();
        wifi_prov_mgr_deinit();
        provisioning_active = false;
    }
    esp_wifi_stop();
    wifi_config_t config = { 0 };
    strlcpy((char *)config.sta.ssid, ssid, sizeof(config.sta.ssid));
    strlcpy((char *)config.sta.password, password, sizeof(config.sta.password));
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set station mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &config), TAG, "save station credentials");
    ESP_RETURN_ON_ERROR(mark_epsilan_wifi_configured(), TAG, "mark configured");
    set_network_status("Wi-Fi connecting...");
    return esp_wifi_start();
}

static esp_err_t start_provisioning(void)
{
    ESP_RETURN_ON_ERROR(load_or_create_token("setup_pop", setup_pop, sizeof(setup_pop), "P-"), TAG, "setup pop");
    snprintf(setup_name, sizeof(setup_name), "PROV_%.6s", server_uuid + 30);
    wifi_prov_mgr_config_t manager = {
        .scheme = wifi_prov_scheme_ble,
        .scheme_event_handler = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM,
    };
    ESP_RETURN_ON_ERROR(wifi_prov_mgr_init(manager), TAG, "BLE provisioning init");
    provisioning_active = true;
    snprintf(network_status, sizeof(network_status), "BLE ready: scan QR in app");
    ESP_LOGI(TAG, "BLE SETUP name=%s PoP=%s security=1", setup_name, setup_pop);
    return wifi_prov_mgr_start_provisioning(WIFI_PROV_SECURITY_1, setup_pop, setup_name, NULL);
}

static esp_err_t initialize_wifi(void)
{
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop init");
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "wifi init");
    wifi_ready = true;
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, network_event_handler, NULL), TAG, "wifi events");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, network_event_handler, NULL), TAG, "ip events");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, network_event_handler, NULL), TAG, "provision events");
    if (epsilan_wifi_is_configured()) {
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set station mode");
        set_network_status("Wi-Fi connecting...");
        return esp_wifi_start();
    }
    return start_provisioning();
}

/* USB bring-up commands only. SiLA transport will use the same board actions. */
static void handle_command(const char *line)
{
    unsigned requested;
    char extra;
    if (sscanf(line, "backlight %u %c", &requested, &extra) == 1 && requested >= 1 && requested <= 100) {
        esp_err_t err = bsp_display_brightness_set((int)requested);
        if (err == ESP_OK) {
            brightness = requested;
            err = save_brightness(requested);
        }
        ESP_LOGI(TAG, "COMMAND backlight=%u result=%s", requested, esp_err_to_name(err));
    } else if (!strcmp(line, "info")) {
        ESP_LOGI(TAG, "INFO firmware=0.1.0-bringup uuid=%s internal_free=%u internal_min=%u psram=%u brightness=%u",
                 server_uuid,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)esp_psram_get_size(), brightness);
    } else if (!strncmp(line, "wifi ", 5)) {
        char ssid[33];
        char password[65];
        if (sscanf(line + 5, "%32s %64s %c", ssid, password, &extra) == 2) {
            esp_err_t err = set_wifi_credentials(ssid, password);
            ESP_LOGI(TAG, "Wi-Fi USB configuration: %s", esp_err_to_name(err));
        } else {
            ESP_LOGW(TAG, "Usage: wifi <ssid-without-spaces> <password>");
        }
    } else {
        ESP_LOGW(TAG, "Commands: info | backlight 1..100 | wifi <ssid> <password>");
    }
}

void app_main(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(load_settings());
    ESP_LOGI(TAG, "EPSILAN BOOT firmware=0.1.0-bringup uuid=%s cores=%d psram=%u", server_uuid, chip.cores,
             (unsigned)esp_psram_get_size());

    usb_serial_jtag_driver_config_t usb = { .tx_buffer_size = 1024, .rx_buffer_size = 256 };
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb));
    if (!bsp_display_start()) {
        ESP_LOGE(TAG, "Display initialization failed");
        return;
    }
    ESP_ERROR_CHECK(bsp_display_brightness_set((int)brightness));
    bsp_display_lock(0);
    show_runtime_screen();
    lv_label_set_text(readings, "Starting motion sensor...");
    lv_label_set_text(status, "USB connected / SiLA next");
    bsp_display_unlock();

    bmi270_handle_t *imu = NULL;
    i2c_master_bus_handle_t i2c_bus = NULL;
    ESP_ERROR_CHECK(i2c_master_get_bus_handle(BSP_I2C_NUM, &i2c_bus));
    const bmi270_driver_config_t driver = {
        .addr = BMI270_I2C_ADDRESS_H, .interface = BMI270_USE_I2C,
        .i2c_bus = i2c_bus,
    };
    esp_err_t imu_err = bmi270_create(&driver, &imu);
    if (imu_err == ESP_OK) {
        const bmi270_config_t config = {
            .acce_odr = BMI270_ACC_ODR_100_HZ, .acce_range = BMI270_ACC_RANGE_4_G,
            .gyro_odr = BMI270_GYR_ODR_100_HZ, .gyro_range = BMI270_GYR_RANGE_1000_DPS,
        };
        imu_err = bmi270_start(imu, &config);
    }
    ESP_LOGI(TAG, "IMU initialization: %s", esp_err_to_name(imu_err));
    esp_err_t wifi_err = initialize_wifi();
    if (wifi_err != ESP_OK) {
        set_network_status("Wi-Fi startup failed");
        ESP_LOGE(TAG, "Wi-Fi initialization: %s", esp_err_to_name(wifi_err));
    }

    if (provisioning_active) {
        bsp_display_lock(0);
        show_setup_screen();
        bsp_display_unlock();
    }

    char command[64];
    size_t used = 0;
    bool discard = false;
    uint32_t sequence = 0;
    for (;;) {
        if (runtime_screen_requested) {
            runtime_screen_requested = false;
            bsp_display_lock(0);
            show_runtime_screen();
            bsp_display_unlock();
        }
        if (setup_requested && !provisioning_active) {
            setup_requested = false;
            ESP_LOGI(TAG, "Wi-Fi setup requested; rebooting into BLE provisioning");
            nvs_handle_t nvs;
            if (nvs_open("epsilan", NVS_READWRITE, &nvs) == ESP_OK) {
                nvs_set_u8(nvs, "wifi_configured", 0);
                nvs_commit(nvs);
                nvs_close(nvs);
            }
            esp_wifi_restore();
            esp_restart();
        }
        char incoming[64];
        int count = usb_serial_jtag_read_bytes(incoming, sizeof(incoming), 0);
        for (int i = 0; i < count; ++i) {
            char c = incoming[i];
            if (c == '\n' || c == '\r') {
                if (used && !discard) { command[used] = '\0'; handle_command(command); }
                used = 0;
                discard = false;
            } else if (!discard) {
                if (used < sizeof(command) - 1) command[used++] = c;
                else { discard = true; ESP_LOGW(TAG, "Command too long; discarded"); }
            }
        }

        float ax = 0, ay = 0, az = 0;
        esp_err_t sample_err = imu_err;
        if (imu_err == ESP_OK) sample_err = bmi270_get_acce_data(imu, &ax, &ay, &az);
        unsigned free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        int64_t uptime = esp_timer_get_time() / 1000000;
        if (sample_err == ESP_OK) {
            ax *= 9.80665f; ay *= 9.80665f; az *= 9.80665f;
            ESP_LOGI(TAG, "SAMPLE seq=%" PRIu32 " ax=%.3f ay=%.3f az=%.3f m/s2 internal_free=%u",
                     sequence++, ax, ay, az, free_internal);
        } else {
            ESP_LOGW(TAG, "SAMPLE unavailable: %s", esp_err_to_name(sample_err));
        }
        bsp_display_lock(0);
        if (provisioning_active) {
            if (status) lv_label_set_text(status, network_status);
        } else {
            if (sample_err == ESP_OK) {
                lv_label_set_text_fmt(readings, "Acceleration (m/s2)\n\nX  % .3f\nY  % .3f\nZ  % .3f", ax, ay, az);
            } else {
                lv_label_set_text_fmt(readings, "Motion sensor unavailable\n%s", esp_err_to_name(sample_err));
            }
            lv_label_set_text_fmt(status, "Up %" PRId64 "s / heap %u KiB\n%s", uptime, free_internal / 1024, network_status);
        }
        bsp_display_unlock();
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
