// AMOLED status display for the Waveshare ESP32-S3-Touch-AMOLED-1.64.
//
// Stack: SH8601 panel over QSPI ─ esp_lcd ─ esp_lvgl_port ─ LVGL widgets.
// esp_lvgl_port spins up its own FreeRTOS task that drives LVGL redraws;
// we just hold its lock whenever we update labels from other tasks.
//
// The whole module compiles to stubs when CONFIG_QREMOTE_HAS_DISPLAY is
// unset, so callers in main.c don't need to guard every call site.

#include "display.h"
#include "sdkconfig.h"

#ifndef CONFIG_QREMOTE_HAS_DISPLAY

void display_init(void) {}
void display_set_status(const char *msg) { (void)msg; }
void display_set_host(const char *ip) { (void)ip; }
void display_log_osc(const char *a, const char *t, const char *s) {
    (void)a; (void)t; (void)s;
}

#else  /* CONFIG_QREMOTE_HAS_DISPLAY */

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_err.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_sh8601.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "config.h"

static const char *TAG = "qremote.disp";

static lv_obj_t *s_lbl_title;
static lv_obj_t *s_lbl_host;
static lv_obj_t *s_lbl_status;
static lv_obj_t *s_ta_log;

// SH8601 init sequence for Waveshare 1.64" 280×456. MADCTL (0x36) and
// COLMOD (0x3A) are sent automatically by esp_lcd_sh8601 based on the
// panel_dev_config (bits_per_pixel + rgb_ele_order), so we only do the
// rest here. Brightness is latched high *before* display-on — some
// AMOLED modules otherwise latch "off" at wake and stay dark.
static const sh8601_lcd_init_cmd_t s_init_cmds[] = {
    {0x11, (uint8_t []){0x00},                   0, 120}, // sleep out + datasheet min wake
    {0x44, (uint8_t []){0x01, 0xD1},             2, 0},   // tear line (row 465 ≈ VFP edge)
    {0x35, (uint8_t []){0x00},                   1, 0},   // TE on
    {0x53, (uint8_t []){0x20},                   1, 10},  // brightness control enable
    {0x51, (uint8_t []){0xFF},                   1, 10},  // brightness = max BEFORE disp-on
    {0x29, (uint8_t []){0x00},                   0, 20},  // display on
};

static void build_ui(void)
{
    lv_obj_t *scr = lv_scr_act();
    // Temporary: bright red background so we can tell at a glance whether the
    // panel is being written at all (vs. dark/off). Change back to black once
    // the display is confirmed working end-to-end.
    lv_obj_set_style_bg_color(scr, lv_color_hex(0xFF0000), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 6, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    s_lbl_title = lv_label_create(scr);
    lv_label_set_text(s_lbl_title, "qremote");
    lv_obj_set_style_text_color(s_lbl_title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_lbl_title, &lv_font_montserrat_14, 0);

    s_lbl_host = lv_label_create(scr);
    lv_label_set_text(s_lbl_host, "host: —");
    lv_obj_set_style_text_color(s_lbl_host, lv_color_hex(0xAAAAAA), 0);

    s_lbl_status = lv_label_create(scr);
    lv_label_set_text(s_lbl_status, "booting…");
    lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(0x88FF88), 0);
    lv_label_set_long_mode(s_lbl_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_lbl_status, AMOLED_W - 12);

    // Scrolling OSC log. Read-only text area accumulates incoming messages;
    // we trim the head when it outgrows the buffer so memory stays bounded.
    s_ta_log = lv_textarea_create(scr);
    lv_obj_set_width(s_ta_log, AMOLED_W - 12);
    lv_obj_set_flex_grow(s_ta_log, 1);
    lv_textarea_set_text(s_ta_log, "");
    lv_obj_set_style_bg_color(s_ta_log, lv_color_hex(0x111111), 0);
    lv_obj_set_style_text_color(s_ta_log, lv_color_hex(0xDDDDDD), 0);
    lv_obj_set_style_border_width(s_ta_log, 0, 0);
    lv_textarea_set_cursor_click_pos(s_ta_log, false);
    lv_obj_clear_flag(s_ta_log, LV_OBJ_FLAG_CLICK_FOCUSABLE);
}

void display_init(void)
{
    ESP_LOGI(TAG, "init SH8601 QSPI (%dx%d)", AMOLED_W, AMOLED_H);

    const spi_bus_config_t bus_cfg = SH8601_PANEL_BUS_QSPI_CONFIG(
        AMOLED_PIN_SCLK,
        AMOLED_PIN_D0, AMOLED_PIN_D1, AMOLED_PIN_D2, AMOLED_PIN_D3,
        AMOLED_W * 80 * sizeof(uint16_t));
    ESP_ERROR_CHECK(spi_bus_initialize(AMOLED_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io = NULL;
    const esp_lcd_panel_io_spi_config_t io_cfg =
        SH8601_PANEL_IO_QSPI_CONFIG(AMOLED_PIN_CS, NULL, NULL);
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)AMOLED_SPI_HOST, &io_cfg, &io));

    sh8601_vendor_config_t vendor_cfg = {
        .init_cmds = s_init_cmds,
        .init_cmds_size = sizeof(s_init_cmds) / sizeof(s_init_cmds[0]),
        .flags = { .use_qspi_interface = 1 },
    };
    esp_lcd_panel_handle_t panel = NULL;
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = AMOLED_PIN_RESET,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config  = &vendor_cfg,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_sh8601(io, &panel_cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    // SH8601 AMOLEDs ship with inverted color mapping — without this the
    // framebuffer writes through but every pixel renders as its complement
    // against a panel that's effectively off. Found via Waveshare's demo.
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle     = io,
        .panel_handle  = panel,
        .buffer_size   = AMOLED_W * 40,
        .double_buffer = true,
        .hres          = AMOLED_W,
        .vres          = AMOLED_H,
        .monochrome    = false,
        .rotation = { .swap_xy = false, .mirror_x = false, .mirror_y = false },
        .flags = { .buff_dma = true, .buff_spiram = false },
    };
    lvgl_port_add_disp(&disp_cfg);

    if (lvgl_port_lock(0)) {
        build_ui();
        lvgl_port_unlock();
    }
}

void display_set_status(const char *msg)
{
    if (!s_lbl_status || !msg) return;
    if (lvgl_port_lock(100)) {
        lv_label_set_text(s_lbl_status, msg);
        lvgl_port_unlock();
    }
}

void display_set_host(const char *ip)
{
    if (!s_lbl_host) return;
    if (lvgl_port_lock(100)) {
        if (ip && *ip) lv_label_set_text_fmt(s_lbl_host, "host: %s", ip);
        else           lv_label_set_text(s_lbl_host, "host: —");
        lvgl_port_unlock();
    }
}

// Keep the log bounded — trim oldest lines when it grows past this many
// chars. LVGL rewraps on resize; we just cap total buffered text.
#define LOG_TRIM_BYTES  1500

void display_log_osc(const char *addr, const char *tags, const char *summary)
{
    if (!s_ta_log) return;
    char line[OSC_LOG_LINE_MAX];
    snprintf(line, sizeof(line), "%s %s\n  %s\n",
             addr ? addr : "?", tags ? tags : "", summary ? summary : "");

    if (lvgl_port_lock(100)) {
        const char *cur = lv_textarea_get_text(s_ta_log);
        size_t curlen = cur ? strlen(cur) : 0;
        if (curlen > LOG_TRIM_BYTES) {
            // Drop the oldest ~half when we exceed the cap.
            const char *trim_from = cur + curlen / 2;
            const char *nl = strchr(trim_from, '\n');
            lv_textarea_set_text(s_ta_log, nl ? nl + 1 : "");
        }
        lv_textarea_add_text(s_ta_log, line);
        lvgl_port_unlock();
    }
}

#endif  /* CONFIG_QREMOTE_HAS_DISPLAY */
