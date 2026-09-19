#include "dashboard.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static const uint32_t COLOR_BG = 0x0b131d;
static const uint32_t COLOR_CARD = 0x152331;
static const uint32_t COLOR_MUTED = 0x7890a6;
static const uint32_t COLOR_TEXT = 0xeaf4ff;
static const uint32_t COLOR_CYAN = 0x26d9e8;
static const uint32_t COLOR_AMBER = 0xffb84a;
static const uint32_t COLOR_GREEN = 0x55d990;
static const uint32_t COLOR_TRACK = 0x273747;

static lv_obj_t *make_card(lv_obj_t *parent, int x, int y, int width, int height)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, width, height);
    lv_obj_set_scrollable(card, false);
    lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_CARD), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(COLOR_TRACK), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_pad_all(card, 7, 0);
    return card;
}

static lv_obj_t *make_caption(lv_obj_t *parent, const char *text)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(COLOR_MUTED), 0);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 0);
    return label;
}

static void create_dial(lv_obj_t *parent, int x, uint32_t color, const char *caption,
                        lv_obj_t **arc_out, lv_obj_t **value_out)
{
    lv_obj_t *card = make_card(parent, x, 35, 99, 174);
    make_caption(card, caption);

    lv_obj_t *arc = lv_arc_create(card);
    lv_obj_set_size(arc, 83, 83);
    lv_obj_align(arc, LV_ALIGN_TOP_MID, 0, 29);
    lv_arc_set_rotation(arc, 135);
    lv_arc_set_bg_angles(arc, 0, 270);
    lv_arc_set_range(arc, 0, 100);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_set_clickable(arc, false);
    lv_obj_set_style_arc_width(arc, 8, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(COLOR_TRACK), LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 8, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(color), LV_PART_INDICATOR);

    lv_obj_t *value = lv_label_create(card);
    lv_obj_set_width(value, 76);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(value, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_align(value, LV_ALIGN_TOP_MID, 0, 61);

    lv_obj_t *scale = lv_label_create(card);
    lv_label_set_text(scale, caption[0] == 'M' ? "0       20" : "0      250");
    lv_obj_set_style_text_color(scale, lv_color_hex(COLOR_MUTED), 0);
    lv_obj_align(scale, LV_ALIGN_TOP_MID, 0, 112);

    *arc_out = arc;
    *value_out = value;
}

void dashboard_create(dashboard_t *dashboard, lv_obj_t *screen, lv_event_cb_t setup_event)
{
    memset(dashboard, 0, sizeof(*dashboard));
    lv_obj_clean(screen);
    lv_obj_set_scrollable(screen, false);
    lv_obj_set_style_bg_color(screen, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_pad_all(screen, 0, 0);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "EPSILAN  /  LIVE");
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 10, 9);

    dashboard->wifi_dot = lv_obj_create(screen);
    lv_obj_set_size(dashboard->wifi_dot, 9, 9);
    lv_obj_set_style_radius(dashboard->wifi_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(dashboard->wifi_dot, 0, 0);
    lv_obj_set_style_pad_all(dashboard->wifi_dot, 0, 0);
    lv_obj_align(dashboard->wifi_dot, LV_ALIGN_TOP_RIGHT, -11, 12);

    lv_obj_t *live = lv_label_create(screen);
    lv_label_set_text(live, "LINK");
    lv_obj_set_style_text_color(live, lv_color_hex(COLOR_MUTED), 0);
    lv_obj_align(live, LV_ALIGN_TOP_RIGHT, -26, 8);

    create_dial(screen, 6, COLOR_CYAN, "MOTION", &dashboard->accel_arc,
                &dashboard->accel_value);
    create_dial(screen, 110, COLOR_AMBER, "ROTATION", &dashboard->gyro_arc,
                &dashboard->gyro_value);

    lv_obj_t *battery = make_card(screen, 214, 35, 100, 52);
    make_caption(battery, "BATTERY");
    dashboard->battery_value = lv_label_create(battery);
    lv_obj_align(dashboard->battery_value, LV_ALIGN_CENTER, 0, 3);
    dashboard->battery_bar = lv_bar_create(battery);
    lv_obj_set_size(dashboard->battery_bar, 84, 7);
    lv_obj_align(dashboard->battery_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(dashboard->battery_bar, lv_color_hex(COLOR_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_color(dashboard->battery_bar, lv_color_hex(COLOR_GREEN), LV_PART_INDICATOR);

    lv_obj_t *light = make_card(screen, 214, 92, 100, 60);
    make_caption(light, "AMBIENT");
    dashboard->light_value = lv_label_create(light);
    lv_obj_align(dashboard->light_value, LV_ALIGN_CENTER, 0, 3);
    dashboard->light_bar = lv_bar_create(light);
    lv_obj_set_size(dashboard->light_bar, 84, 6);
    lv_obj_align(dashboard->light_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(dashboard->light_bar, lv_color_hex(COLOR_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_color(dashboard->light_bar, lv_color_hex(COLOR_AMBER), LV_PART_INDICATOR);

    lv_obj_t *proximity = make_card(screen, 214, 157, 100, 52);
    make_caption(proximity, "PROXIMITY");
    dashboard->proximity_value = lv_label_create(proximity);
    lv_obj_align(dashboard->proximity_value, LV_ALIGN_CENTER, 0, 3);
    dashboard->proximity_bar = lv_bar_create(proximity);
    lv_obj_set_size(dashboard->proximity_bar, 84, 7);
    lv_obj_align(dashboard->proximity_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(dashboard->proximity_bar, lv_color_hex(COLOR_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_color(dashboard->proximity_bar, lv_color_hex(COLOR_CYAN), LV_PART_INDICATOR);

    dashboard->setup_button = lv_button_create(screen);
    lv_obj_set_size(dashboard->setup_button, 94, 24);
    lv_obj_align(dashboard->setup_button, LV_ALIGN_BOTTOM_RIGHT, -6, -4);
    lv_obj_set_style_bg_color(dashboard->setup_button, lv_color_hex(COLOR_CARD), 0);
    lv_obj_set_style_radius(dashboard->setup_button, 6, 0);
    lv_obj_t *button_label = lv_label_create(dashboard->setup_button);
    lv_label_set_text(button_label, "hold: Wi-Fi");
    lv_obj_center(button_label);
    lv_obj_add_event_cb(dashboard->setup_button, setup_event, LV_EVENT_LONG_PRESSED, NULL);

    dashboard->status = lv_label_create(screen);
    lv_obj_set_size(dashboard->status, 204, 22);
    lv_label_set_long_mode(dashboard->status, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_color(dashboard->status, lv_color_hex(COLOR_MUTED), 0);
    lv_obj_align(dashboard->status, LV_ALIGN_BOTTOM_LEFT, 9, -4);
}

void dashboard_update(dashboard_t *dashboard, const dashboard_reading_t *reading,
                      const char *network_status)
{
    if (reading->motion_valid) {
        int accel = (int)lroundf(fminf(reading->acceleration_ms2, 20.0f) * 5.0f);
        int gyro = (int)lroundf(fminf(reading->angular_rate_dps, 250.0f) * 0.4f);
        lv_arc_set_value(dashboard->accel_arc, accel);
        lv_arc_set_value(dashboard->gyro_arc, gyro);
        lv_label_set_text_fmt(dashboard->accel_value, "%.1f\nm/s2", reading->acceleration_ms2);
        lv_label_set_text_fmt(dashboard->gyro_value, "%.0f\ndeg/s", reading->angular_rate_dps);
    } else {
        lv_arc_set_value(dashboard->accel_arc, 0);
        lv_arc_set_value(dashboard->gyro_arc, 0);
        lv_label_set_text(dashboard->accel_value, "--\nm/s2");
        lv_label_set_text(dashboard->gyro_value, "--\ndeg/s");
    }

    if (reading->optical_valid) {
        int light_level = (int)lroundf(fminf(log10f(reading->ambient_lux + 1.0f) * 33.3f, 100.0f));
        lv_bar_set_value(dashboard->light_bar, light_level, LV_ANIM_OFF);
        lv_label_set_text_fmt(dashboard->light_value, "%.0f lux", reading->ambient_lux);
        lv_bar_set_value(dashboard->proximity_bar,
                         reading->proximity_raw > 2047 ? 100 : reading->proximity_raw * 100 / 2047,
                         LV_ANIM_OFF);
        lv_label_set_text_fmt(dashboard->proximity_value, "%s %u",
                              reading->proximity_raw > 250 ? "NEAR" : "FAR",
                              reading->proximity_raw);
    } else {
        lv_bar_set_value(dashboard->light_bar, 0, LV_ANIM_OFF);
        lv_bar_set_value(dashboard->proximity_bar, 0, LV_ANIM_OFF);
        lv_label_set_text(dashboard->light_value, "-- lux");
        lv_label_set_text(dashboard->proximity_value, "--");
    }

    if (reading->battery_valid) {
        if (reading->battery_present) {
            lv_bar_set_value(dashboard->battery_bar, reading->battery_percent, LV_ANIM_OFF);
            lv_label_set_text_fmt(dashboard->battery_value, "%s%u%%  %.2fV",
                                  reading->charging ? "+" : "", reading->battery_percent,
                                  reading->battery_mv / 1000.0f);
        } else {
            lv_bar_set_value(dashboard->battery_bar, reading->external_power ? 100 : 0, LV_ANIM_OFF);
            lv_label_set_text(dashboard->battery_value,
                              reading->external_power ? "USB POWER" : "NO BATTERY");
        }
    } else {
        lv_bar_set_value(dashboard->battery_bar, 0, LV_ANIM_OFF);
        lv_label_set_text(dashboard->battery_value, "--");
    }

    lv_obj_set_style_bg_color(dashboard->wifi_dot,
                              lv_color_hex(reading->wifi_connected ? COLOR_GREEN : COLOR_AMBER), 0);
    lv_label_set_text_fmt(dashboard->status, "UP %llds  |  %s",
                          (long long)reading->uptime_seconds, network_status);
}
