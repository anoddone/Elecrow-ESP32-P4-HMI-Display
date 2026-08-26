 /**
 * Elecrow ESP32-P4 HMI Display — starter sketch.
 *
 * Brings up the MIPI-DSI panel (EK79007) + GT911 touch via Espressif's
 * ESP32_Display_Panel/LVGL stack, then dumps chip/PSRAM/flash info to
 * Serial as a jumping-off point for exploring the P4's expanded memory.
 */

#include "board_config.h"
#include <Arduino.h>

#include <esp_chip_info.h>
#include <esp_err.h>
#include <esp_flash.h>
#include <esp_heap_caps.h>
#include <esp_ldo_regulator.h>
#include <esp_log.h>

#include "esp_panel_board_custom_conf.h"
#include "esp_panel_drivers_conf.h"
#include "ESP_Panel_Library.h"

#include "lvgl_v8_port.h"
#include <lvgl.h>

#include "esp_hosted.h"
#if __has_include("esp_hosted_api_types.h")
#include "esp_hosted_api_types.h"
#endif
#include "nvs_flash.h"
#include "esp_event.h"

using namespace esp_panel::board;
using namespace esp_panel::drivers;

static const char *TAG = "main";

static void print_system_info() {
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    Serial.printf("Chip: %d cores, rev v%d.%d\n", chip.cores, chip.revision / 100, chip.revision % 100);

    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);
    Serial.printf("Flash: %lu MB\n", flash_size / (1024 * 1024));

    Serial.printf("Internal RAM free: %u / %u bytes\n",
                   heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                   heap_caps_get_total_size(MALLOC_CAP_INTERNAL));
    Serial.printf("PSRAM free: %u / %u bytes\n",
                   heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                   heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
}

// One-shot diagnostic: bring up the esp-hosted SDIO transport to the
// companion ESP32-C6 and query its firmware version, before touching the
// display at all. This board has no WiFi radio on the P4 itself - it talks
// to a separate C6 chip over SDIO (see platformio.ini's custom_sdkconfig
// block for the pin config, which is unverified against this specific unit).
// A version response here means the SDIO link and pin mapping are correct;
// a failure/hang here is isolated from the display/touch bring-up that
// follows, so it's obvious which subsystem is at fault.
//
// esp_hosted_get_coprocessor_fwversion() alone isn't enough - the hosted
// transport isn't up until nvs_flash_init() -> esp_event_loop_create_default()
// -> esp_hosted_init() -> esp_hosted_connect_to_slave() have all run (this
// sequence is from the community reference project's C6-upgrade tool,
// github.com/mverch67/crowpanel-advanced-p4-c6-upgrade).
static void test_wifi_hosted_link() {
    esp_err_t ret = nvs_flash_init();
    if (ret != ESP_OK) {
        Serial.printf("nvs_flash_init failed: %s\n", esp_err_to_name(ret));
        return;
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK) {
        Serial.printf("esp_event_loop_create_default failed: %s\n", esp_err_to_name(ret));
        return;
    }

    Serial.println("Initializing esp-hosted SDIO transport...");
    ret = esp_hosted_init();
    if (ret != ESP_OK) {
        Serial.printf("esp_hosted_init failed: %s (0x%x) - check SDIO pin config\n",
                       esp_err_to_name(ret), ret);
        return;
    }

    Serial.println("Connecting to C6 co-processor...");
    ret = esp_hosted_connect_to_slave();
    if (ret != ESP_OK) {
        Serial.printf("esp_hosted_connect_to_slave failed: %s (0x%x) - SDIO handshake with C6 failed\n",
                       esp_err_to_name(ret), ret);
        return;
    }

    esp_hosted_coprocessor_fwver_t ver = {0};
    ret = esp_hosted_get_coprocessor_fwversion(&ver);
    if (ret == ESP_OK) {
        Serial.printf("C6 firmware: %lu.%lu.%lu - SDIO link OK\n",
                       (unsigned long)ver.major1, (unsigned long)ver.minor1, (unsigned long)ver.patch1);
    } else {
        Serial.printf("C6 version query failed: %s (0x%x)\n", esp_err_to_name(ret), ret);
    }
}

// Power up the MIPI D-PHY (LDO3) and the touch/I2C pull-up rail (LDO4).
// Required on ESP32-P4 boards before the DSI bus or GT911 will respond.
static void power_up_display_rails() {
    esp_ldo_channel_handle_t ldo3 = NULL;
    esp_ldo_channel_config_t ldo3_cfg = {.chan_id = 3, .voltage_mv = 2500};
    esp_err_t err = esp_ldo_acquire_channel(&ldo3_cfg, &ldo3);
    if (err != ESP_OK) {
        Serial.printf("LDO3 (MIPI D-PHY) power error: %s\n", esp_err_to_name(err));
    }

    esp_ldo_channel_handle_t ldo4 = NULL;
    esp_ldo_channel_config_t ldo4_cfg = {.chan_id = 4, .voltage_mv = 3300};
    err = esp_ldo_acquire_channel(&ldo4_cfg, &ldo4);
    if (err != ESP_OK) {
        Serial.printf("LDO4 (touch/I2C) power error: %s\n", esp_err_to_name(err));
    }
}

// Touch feed timer.
//
// This is the ONLY code that may call Touch::readPoints(). LVGL's indev read
// (lvgl_v8_port.cpp) used to poll the touch controller independently in
// parallel with the app's own reads, and the two raced over the same I2C
// bus closely enough that the indev only ever won that race for a touch's
// very first sample - every touch looked like an instant tap, with drag/
// swipe/long-press (and by extension slider dragging) never seen. This
// timer is the single source of truth instead: it polls once, then pushes
// the result to the indev via lvgl_port_touch_update(). Do not add a second
// caller of readPoints() without routing it through here too.
static esp_panel::drivers::Touch *g_touch = nullptr;

static void touch_feed_timer_cb(lv_timer_t *timer) {
    if (g_touch == nullptr) {
        return;
    }
    esp_panel::drivers::TouchPoint point;
    int n = g_touch->readPoints(&point, 1, 0);
    lvgl_port_touch_update(n > 0 ? point.x : 0, n > 0 ? point.y : 0, n > 0);
}

// Dial + slider demo: the slider drives the LCD backlight brightness (real
// hardware, via the board's PWM-capable Backlight driver) and the meter
// mirrors that value as a dial readout, so dragging the slider is
// immediately visible as the actual screen brightness changing.
#define BRIGHTNESS_INITIAL_PERCENT 80

static esp_panel::drivers::Backlight *g_backlight = nullptr;
static lv_obj_t *brightness_meter;
static lv_meter_indicator_t *brightness_indicator;
static lv_obj_t *brightness_label;

static void set_brightness(int percent) {
    if (g_backlight != nullptr) {
        g_backlight->setBrightness(percent);
    }
    lv_meter_set_indicator_value(brightness_meter, brightness_indicator, percent);
    lv_label_set_text_fmt(brightness_label, "Brightness: %d%%", percent);
}

static void brightness_slider_cb(lv_event_t *e) {
    lv_obj_t *slider = lv_event_get_target(e);
    set_brightness(lv_slider_get_value(slider));
}

static void build_ui() {
    if (!lvgl_port_lock(-1)) {
        ESP_LOGE(TAG, "LVGL lock failed");
        return;
    }

    lv_obj_t *screen = lv_scr_act();
    lv_obj_set_style_bg_color(screen, lv_color_make(0x10, 0x14, 0x1c), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "Backlight Dial & Slider Demo");
    static lv_style_t title_style;
    lv_style_init(&title_style);
    lv_style_set_text_font(&title_style, &lv_font_montserrat_28);
    lv_style_set_text_color(&title_style, lv_color_white());
    lv_obj_add_style(title, &title_style, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

    brightness_meter = lv_meter_create(screen);
    lv_obj_set_size(brightness_meter, 280, 280);
    lv_obj_align(brightness_meter, LV_ALIGN_CENTER, 0, -30);
    lv_obj_set_style_bg_color(brightness_meter, lv_color_make(0x1a, 0x20, 0x2c), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(brightness_meter, LV_OPA_COVER, LV_PART_MAIN);

    lv_meter_scale_t *scale = lv_meter_add_scale(brightness_meter);
    lv_meter_set_scale_range(brightness_meter, scale, 0, 100, 270, 135);
    lv_meter_set_scale_ticks(brightness_meter, scale, 21, 2, 10, lv_palette_lighten(LV_PALETTE_GREY, 1));
    lv_meter_set_scale_major_ticks(brightness_meter, scale, 5, 3, 16, lv_color_white(), 14);

    lv_meter_indicator_t *arc = lv_meter_add_arc(brightness_meter, scale, 8, lv_palette_main(LV_PALETTE_ORANGE), 0);
    lv_meter_set_indicator_start_value(brightness_meter, arc, 0);
    lv_meter_set_indicator_end_value(brightness_meter, arc, BRIGHTNESS_INITIAL_PERCENT);

    brightness_indicator = lv_meter_add_needle_line(brightness_meter, scale, 4, lv_color_white(), -14);

    brightness_label = lv_label_create(screen);
    static lv_style_t value_style;
    lv_style_init(&value_style);
    lv_style_set_text_font(&value_style, &lv_font_montserrat_28);
    lv_style_set_text_color(&value_style, lv_color_white());
    lv_obj_add_style(brightness_label, &value_style, LV_PART_MAIN);
    lv_obj_align(brightness_label, LV_ALIGN_CENTER, 0, 110);

    lv_obj_t *slider = lv_slider_create(screen);
    lv_obj_set_size(slider, 600, 20);
    lv_obj_align(slider, LV_ALIGN_BOTTOM_MID, 0, -50);
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, BRIGHTNESS_INITIAL_PERCENT, LV_ANIM_OFF);
    lv_obj_add_event_cb(slider, brightness_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);

    set_brightness(BRIGHTNESS_INITIAL_PERCENT);

    lv_timer_create(touch_feed_timer_cb, 30, NULL);

    lvgl_port_unlock();
}

void setup() {
    Serial.begin(115200);
    delay(200);

    print_system_info();
    test_wifi_hosted_link();
    power_up_display_rails();

    Board *board = new Board();
    Serial.println("Initializing panel (EK79007 + GT911)...");
    assert(board->init());

#if LVGL_PORT_AVOID_TEARING_MODE
    board->getLCD()->configFrameBufferNumber(LVGL_PORT_DISP_BUFFER_NUM);
#endif

    assert(board->begin());
    Serial.println("Display and touch online.");

    g_touch = board->getTouch();
    g_backlight = board->getBacklight();
    lvgl_port_init(board->getLCD(), g_touch);

    build_ui();
}

void loop() {
    delay(1000);
}
