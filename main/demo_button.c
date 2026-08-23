// main/demo_button.c —— 按键事件流 + 实时 ADC 电压。
// 电压显示是本页的核心:换了分压/上拉阻值靠它重标 BSP_BTN_MV_TABLE;
// 也靠它找挂在分压链上的第 4 个键(电源键):按住目标键看电压落点。
//
// ⚠ 本页只读 ADC,不做 GPIO 扫描:ESP32-C3 的 GPIO11~17 接片上 Flash,
//   gpio_config() 一碰就死机;其余脚全被外设占用,重配会破坏外设。
#include "demo.h"
#include "bsp_button.h"
#include "bsp_pins.h"
#include "esp_log.h"
#include "ui_pixel.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "demo_btn";

static lv_obj_t   *s_scr, *s_mv, *s_band, *s_log;
static lv_timer_t *s_timer;
static int         s_prev_band;

#define LOG_LINES 5
static char s_lines[LOG_LINES][32];
static int  s_line_cnt;

static const char *BTN_NAME[]  = { "UP", "DOWN", "OK", "PWR" };
static const char *EV_NAME[]   = { "PRESS", "CLICK", "DOUBLE", "LONG" };
static const char *BAND_NAME[] = { "UP", "DOWN", "OK", "RELEASE" };

// 电压归档:0/1/2=三键窗口,3=松开(>1900mV),-1=读取失败/未知档(可能是第 4 键)。
static int band_of(int mv) {
    if (mv < 0)    return -1;
    if (mv < 150)  return 0;
    if (mv < 447)  return 1;
    if (mv < 1900) return 2;
    return 3;
}

// 每 100ms 刷新一次电压。档位变化时同步打串口日志:按住未知键(电源键)时,
// monitor 里能直接看到精确电压值,不用盯着小屏幕读数。
static void tick(lv_timer_t *t) {
    (void)t;
    int mv = bsp_button_read_mv();
    int band = band_of(mv);
    if (mv < 0) lv_label_set_text(s_mv, "ADC read failed");
    else        lv_label_set_text_fmt(s_mv, "%d mV", mv);

    if (band != s_prev_band) {
        ESP_LOGI(TAG, "ADC %d mV (%s)", mv,
                 band < 0 ? "UNKNOWN band - press & hold to identify"
                          : BAND_NAME[band]);
        s_prev_band = band;
    }
    lv_label_set_text_fmt(s_band, "band: %s",
                          band < 0 ? "?" : BAND_NAME[band]);
}

static void log_push(const char *text) {
    if (s_line_cnt < LOG_LINES) {
        snprintf(s_lines[s_line_cnt++], sizeof(s_lines[0]), "%s", text);
    } else {
        for (int i = 0; i < LOG_LINES - 1; i++)
            memcpy(s_lines[i], s_lines[i + 1], sizeof(s_lines[0]));
        snprintf(s_lines[LOG_LINES - 1], sizeof(s_lines[0]), "%s", text);
    }
    char all[LOG_LINES * 32 + 1] = { 0 };
    for (int i = 0; i < s_line_cnt; i++) {
        strcat(all, s_lines[i]);
        if (i < s_line_cnt - 1) strcat(all, "\n");
    }
    lv_label_set_text(s_log, all);
}

void demo_button_enter(void) {
    s_line_cnt = 0;
    s_prev_band = -1;
    ESP_LOGI(TAG, "ADC 观察就绪:按住电源键看电压读数,变化即在分压链上");
    s_scr = ui_pixel_screen_create("BUTTON / ADC");
    lv_obj_t *panel = ui_pixel_panel_create(s_scr, 18, 58, 204, 184, UI_PAPER);

    s_mv = lv_label_create(panel);
    lv_obj_set_style_text_font(s_mv, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_mv, lv_color_hex(UI_SKY_DARK), 0);
    lv_obj_align(s_mv, LV_ALIGN_TOP_MID, 0, 6);
    lv_label_set_text(s_mv, "-- mV");

    s_band = lv_label_create(panel);
    lv_obj_set_style_text_color(s_band, lv_color_hex(UI_SKY_DARK), 0);
    lv_obj_align(s_band, LV_ALIGN_TOP_MID, 0, 34);
    lv_label_set_text(s_band, "band: ?");

    s_log = lv_label_create(panel);
    lv_obj_set_style_text_color(s_log, lv_color_hex(UI_INK), 0);
    lv_obj_align(s_log, LV_ALIGN_TOP_LEFT, 9, 62);
    lv_label_set_text(s_log, "press any key...");

    ui_pixel_mascot_create(s_scr, 101, 238);

    s_timer = lv_timer_create(tick, 100, NULL);
    lv_screen_load(s_scr);
}

void demo_button_exit(void) {
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    if (s_scr) { lv_obj_delete(s_scr); s_scr = NULL; s_mv = s_band = s_log = NULL; }
}

void demo_button_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    char line[32];
    snprintf(line, sizeof(line), "%s: %s", BTN_NAME[btn], EV_NAME[ev]);
    log_push(line);
}
