// main/demo_woodfish.c —— 敲木鱼积功德。
//
// 交互(电源键 UP 短按"保存退出返回菜单"与长按关机由 main.c 统一分发):
//   OK   按下瞬间  敲一下木鱼:功德 +1、播放敲击声、木鱼下沉回弹、飘出 +1
//   OK   长按      静音 / 取消静音
//   UP   单击      保存并退出,返回主菜单(main.c 拦截;息屏时只亮屏不退出)
//   UP   长按      关机(深度睡眠;按 UP 唤醒开机,或 12 小时定时兜底唤醒)
//   DOWN 单击      切换音色 CLASSIC/BELL/BLOCK/DROP,切换时预览一声
//   DOWN 长按      开 / 关自动敲击(每 700ms 敲一下)
//
// 电量:CW2017 读 SOC,右上角图标 + 百分比,每 30 秒刷新;电量计缺失时隐藏。
//
// 自动息屏:2 分钟无【手动】操作熄背光(自动敲击不算操作,否则永不息屏)。
// 息屏期间手动敲击照常计数+发声并亮屏;自动敲击继续计数+发声但不亮屏,
// 变成"听声攒功德"模式。任何按键操作都会亮屏并重置息屏倒计时。
//
// 线程模型:
//   - 按键回调与自动敲击/电量/息屏定时器都在 LVGL 上下文里改 UI,安全;
//   - 敲击声由后台音频任务播放:按键侧只投递触发事件(队列满则丢弃旧事件,
//     保证打击乐手感:宁可丢声也不排队延迟);
//   - 四种音色进页面时一次性预合成,之后指针稳定,音频任务读取无需加锁。
#include "demo.h"
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "ui_pixel.h"
#include "woodfish_model.h"
#include "woodfish_store.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include <math.h>
#include <stdlib.h>

static const char *TAG = "demo_muyu";

// ---- 敲击声合成参数 ----
#define WF_SAMPLE_RATE   16000
#define WF_PLAY_CHUNK    512        // 每次写 codec 的采样数
#define WF_VOLUME        85
#define WF_STYLE_COUNT   4

// 各音色时长(ms)。预合成缓冲合计约 29KB,C3 内部 RAM 可承受。
#define WF_MS_CLASSIC    150
#define WF_MS_BELL       450
#define WF_MS_BLOCK      90
#define WF_MS_DROP       220

#define WF_AUTO_PERIOD_MS 700
#define WF_SCREEN_TIMEOUT_MS 120000  // 无手动操作多久后息屏(2 分钟)
#define WF_FLOAT_MAX      5         // 同屏 +1 标签上限,防快速连点刷屏
#define WF_FLOAT_RISE     34        // +1 上飘像素
#define WF_FLOAT_Y0       108       // +1 出生的 y(木鱼上沿附近)
#define TWO_PI            6.28318530718f

// 深睡唤醒:按住任意键后 GPIO0 电压低于 1900mV 视为“被按住”(见 bsp_pins.h)。
#define WF_BTN_HELD_MV    1900

// 木鱼配色(木纹棕系)
#define WF_WOOD       0xB5824A
#define WF_WOOD_DARK  0xA9713C
#define WF_SLIT       0x4E3016
#define WF_HEAD       0x6B4423
#define WF_HANDLE     0x8A5A2E

typedef struct {
    const char *name;
    uint16_t ms;
    void (*synth)(int16_t *buf, int n);
} wf_style_t;

static void synth_classic(int16_t *buf, int n);
static void synth_bell(int16_t *buf, int n);
static void synth_block(int16_t *buf, int n);
static void synth_drop(int16_t *buf, int n);

static const wf_style_t STYLES[WF_STYLE_COUNT] = {
    { "CLASSIC", WF_MS_CLASSIC, synth_classic },
    { "BELL",    WF_MS_BELL,    synth_bell    },
    { "BLOCK",   WF_MS_BLOCK,   synth_block   },
    { "DROP",    WF_MS_DROP,    synth_drop    },
};

static woodfish_model_t s_model;
static bool s_audio_ok, s_battery_ok;

static lv_obj_t *s_scr, *s_merit_lbl, *s_rank_lbl, *s_hint_lbl, *s_status_lbl;
static lv_obj_t *s_batt_lbl, *s_bar;
static lv_obj_t *s_fish, *s_striker;

static TaskHandle_t s_audio_task;
static QueueHandle_t s_knock_q;
static lv_timer_t *s_auto_timer, *s_batt_timer, *s_guard_timer, *s_bl_timer;
static volatile int s_style;
static int16_t *s_pcm[WF_STYLE_COUNT];   // 四种音色的预合成 PCM
static int s_pcm_samples[WF_STYLE_COUNT];
static int s_float_count;
static int s_float_slot;                 // +1 出现的水平位置轮换,避免完全重叠
static bool s_wake_guard;                // 开机时仍按着键:忽略其事件直到松开
static int s_guard_ticks;                // guard 总时长(tick),10 秒兜底
static int s_guard_loose;                // 连续松开 tick 数,吃掉松键后的 CLICK
static bool s_screen_off;                // 息屏态(仅背光灭,系统照常运行)

// ---- 四种音色合成 ----

// 木鱼“笃”:三个非谐波分音 + 快衰减,接近真木鱼。
static void synth_classic(int16_t *buf, int n) {
    const float tau = 0.030f;
    for (int i = 0; i < n; i++) {
        float t = (float)i / WF_SAMPLE_RATE;
        float env = expf(-t / tau);
        float attack = t < 0.0015f ? t / 0.0015f : 1.0f;   // 1.5ms 起振,消除爆音
        float s = 0.62f * sinf(TWO_PI * 640.0f * t)
                + 0.27f * sinf(TWO_PI * 1250.0f * t)
                + 0.11f * sinf(TWO_PI * 2090.0f * t);
        buf[i] = (int16_t)(s * env * attack * 13500.0f);
    }
}

// 铜磬“叮”:520Hz 基频 + 类钟非谐波分音,慢衰减;尾部 30ms 线性淡出防爆音。
static void synth_bell(int16_t *buf, int n) {
    const float tau = 0.150f;
    for (int i = 0; i < n; i++) {
        float t = (float)i / WF_SAMPLE_RATE;
        float env = expf(-t / tau);
        float attack = t < 0.003f ? t / 0.003f : 1.0f;
        float s = 0.55f * sinf(TWO_PI * 520.0f * t)
                + 0.28f * sinf(TWO_PI * 1040.0f * t)
                + 0.12f * sinf(TWO_PI * 1549.6f * t)
                + 0.05f * sinf(TWO_PI * 2111.2f * t);
        buf[i] = (int16_t)(s * env * attack * 12500.0f);
    }
    int fade = WF_SAMPLE_RATE * 30 / 1000;
    for (int i = 0; i < fade && i < n; i++) {
        float k = 1.0f - (float)i / fade;
        buf[n - 1 - i] = (int16_t)(buf[n - 1 - i] * k);
    }
}

// 梆子“哒”:高频 + 极快衰减,打击感最硬、最短促。
static void synth_block(int16_t *buf, int n) {
    const float tau = 0.012f;
    for (int i = 0; i < n; i++) {
        float t = (float)i / WF_SAMPLE_RATE;
        float env = expf(-t / tau);
        float attack = t < 0.0005f ? t / 0.0005f : 1.0f;
        float s = 0.7f * sinf(TWO_PI * 1100.0f * t)
                + 0.3f * sinf(TWO_PI * 2300.0f * t);
        buf[i] = (int16_t)(s * env * attack * 14000.0f);
    }
}

// 水滴“咚”:500→1600Hz 对数上扫频 + 中速衰减,卡通水滴声。
static void synth_drop(int16_t *buf, int n) {
    const float tau = 0.090f;
    const float sweep_s = 0.070f;
    float phase = 0.0f;
    for (int i = 0; i < n; i++) {
        float t = (float)i / WF_SAMPLE_RATE;
        float f = (t < sweep_s)
            ? 500.0f * powf(3.2f, t / sweep_s)      // 500 × 3.2 = 1600
            : 1600.0f;
        phase += TWO_PI * f / WF_SAMPLE_RATE;
        if (phase >= TWO_PI) phase -= TWO_PI;
        buf[i] = (int16_t)(sinf(phase) * expf(-t / tau) * 12500.0f);
    }
}

static void audio_task(void *arg) {
    (void)arg;
    uint8_t trig;
    for (;;) {
        if (xQueueReceive(s_knock_q, &trig, portMAX_DELAY) != pdTRUE) continue;
        int idx = s_style;
        if (idx < 0 || idx >= WF_STYLE_COUNT) idx = 0;
        int16_t *pcm = s_pcm[idx];
        int total = s_pcm_samples[idx];
        if (!pcm || total <= 0) continue;
        if (bsp_audio_set_format(WF_SAMPLE_RATE, 16, 1) != ESP_OK) continue;
        bsp_audio_set_volume(WF_VOLUME);
        for (int off = 0; off < total; off += WF_PLAY_CHUNK) {
            int n = total - off;
            if (n > WF_PLAY_CHUNK) n = WF_PLAY_CHUNK;
            bsp_audio_write(pcm + off, (size_t)n * sizeof(int16_t));
        }
    }
}

// ---- UI 刷新(均要求 LVGL 上下文) ----
static void refresh_merit(void) {
    lv_label_set_text_fmt(s_merit_lbl, "%lu", (unsigned long)s_model.merit);

    const woodfish_rank_t *rank = woodfish_model_rank(s_model.merit);
    if (s_model.combo >= 3) {
        lv_label_set_text_fmt(s_rank_lbl, "%s · x%lu COMBO",
                              rank->name, (unsigned long)s_model.combo);
    } else {
        lv_label_set_text(s_rank_lbl, rank->name);
    }
    lv_bar_set_value(s_bar, (int32_t)woodfish_model_rank_progress(s_model.merit),
                     LV_ANIM_OFF);
}

// 状态行用图标压缩宽度:♪ 音色名 + 音量符号 + 播放/暂停符号。
static void refresh_status(void) {
    lv_label_set_text_fmt(s_status_lbl,
        LV_SYMBOL_AUDIO " %s   %s   %s",
        STYLES[s_style].name,
        s_model.muted ? LV_SYMBOL_MUTE : LV_SYMBOL_VOLUME_MAX,
        s_model.auto_mode ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
}

static void batt_refresh(void) {
    if (!s_battery_ok || !s_batt_lbl) return;
    int soc = bsp_battery_soc();
    if (soc < 0 || soc > 100) {          // 读失败:隐藏而不是显示错误值
        lv_obj_add_flag(s_batt_lbl, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(s_batt_lbl, LV_OBJ_FLAG_HIDDEN);
    const char *icon = soc > 75 ? LV_SYMBOL_BATTERY_FULL :
                       soc > 50 ? LV_SYMBOL_BATTERY_3 :
                       soc > 25 ? LV_SYMBOL_BATTERY_2 :
                       soc > 5  ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
    lv_label_set_text_fmt(s_batt_lbl, "%s %d%%", icon, soc);
    lv_obj_set_style_text_color(s_batt_lbl,
        lv_color_hex(soc <= 20 ? UI_RED : UI_INK), 0);
}

static void batt_tick(lv_timer_t *timer) {
    (void)timer;
    batt_refresh();
}

static void move_y_cb(void *obj, int32_t v) {
    lv_obj_set_y((lv_obj_t *)obj, v);
}

// 木鱼被敲:整体下沉 4px 再回弹(打击感)。
static void animate_fish(void) {
    if (!s_fish) return;
    int y = lv_obj_get_y(s_fish);
    lv_anim_delete(s_fish, move_y_cb);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_fish);
    lv_anim_set_exec_cb(&a, move_y_cb);
    lv_anim_set_values(&a, y, y + 4);
    lv_anim_set_duration(&a, 70);
    lv_anim_set_playback_duration(&a, 120);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

// 木槌下击 14px 再抬起。
static void animate_striker(void) {
    if (!s_striker) return;
    int y = lv_obj_get_y(s_striker);
    lv_anim_delete(s_striker, move_y_cb);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_striker);
    lv_anim_set_exec_cb(&a, move_y_cb);
    lv_anim_set_values(&a, y, y + 14);
    lv_anim_set_duration(&a, 70);
    lv_anim_set_playback_duration(&a, 100);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_start(&a);
}

static void float_step_cb(void *label, int32_t v) {
    lv_obj_set_y((lv_obj_t *)label, WF_FLOAT_Y0 - v);
    lv_obj_set_style_opa((lv_obj_t *)label,
        (lv_opa_t)(LV_OPA_COVER - v * LV_OPA_COVER / WF_FLOAT_RISE), 0);
}

static void float_done_cb(lv_anim_t *a) {
    lv_obj_delete((lv_obj_t *)a->var);
    if (s_float_count > 0) s_float_count--;
}

// 飘出 “+1”:水平位置在木鱼上方轮换,上飘并淡出。
static void spawn_float(void) {
    if (s_float_count >= WF_FLOAT_MAX) return;
    static const int slots[] = { -22, 0, 22, -11, 11 };

    lv_obj_t *lbl = lv_label_create(s_scr);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(UI_RED), 0);
    lv_label_set_text(lbl, "+1");
    int x = 120 + slots[s_float_slot % 5] - 8;
    s_float_slot++;
    lv_obj_set_pos(lbl, x, WF_FLOAT_Y0);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, lbl);
    lv_anim_set_exec_cb(&a, float_step_cb);
    lv_anim_set_values(&a, 0, WF_FLOAT_RISE);
    lv_anim_set_duration(&a, 650);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a, float_done_cb);
    lv_anim_start(&a);
    s_float_count++;
}

// 触发一次当前音色的播放(不加分功德,敲击与音色预览共用)。
static void preview_tone(void) {
    if (s_model.muted || !s_audio_ok || !s_knock_q) return;
    xQueueReset(s_knock_q);              // 丢弃积压,只播最新一次
    uint8_t trig = 1;
    xQueueSend(s_knock_q, &trig, 0);
}

// ---- 敲击核心:计数 + 发声 + 存盘,不碰 UI(息屏时自动敲击走这条路) ----
static void do_knock_core(void) {
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    woodfish_model_knock(&s_model, now_ms);
    preview_tone();
    woodfish_store_request_save(&s_model);
}

// ---- 敲击主流程(LVGL 上下文,亮屏时的完整表现) ----
static void do_knock(void) {
    do_knock_core();
    refresh_merit();
    animate_fish();
    animate_striker();
    spawn_float();
}

static void auto_tick(lv_timer_t *timer) {
    (void)timer;
    // 息屏时自动敲击只计数+发声,不做任何绘制,成为"听声攒功德"模式
    if (s_screen_off) do_knock_core();
    else do_knock();
}

// ---- 息屏 / 亮屏 ----
static void screen_sleep(void) {
    if (s_screen_off) return;
    s_screen_off = true;
    bsp_display_backlight(0);
    ESP_LOGI(TAG, "背光熄灭(自动息屏)");
}

static void screen_wake(void) {
    bsp_display_backlight(woodfish_store_brightness());   // 用户保存的亮度档
    if (s_screen_off) {
        s_screen_off = false;
        refresh_merit();             // 息屏期间跳过的功德变化,亮屏补上
        batt_refresh();
    }
    if (s_bl_timer) lv_timer_reset(s_bl_timer);
}

static void bl_tick(lv_timer_t *timer) {
    (void)timer;
    screen_sleep();
}

static void set_auto(bool on) {
    s_model.auto_mode = on;
    if (on && !s_auto_timer) {
        s_auto_timer = lv_timer_create(auto_tick, WF_AUTO_PERIOD_MS, NULL);
    } else if (!on && s_auto_timer) {
        lv_timer_delete(s_auto_timer);
        s_auto_timer = NULL;
    }
    refresh_status();
    woodfish_store_request_save(&s_model);
}

// ---- 关机:同步落盘 → 等松键 → 熄屏深睡 ----
// 唤醒:按 UP(GPIO0 直连 GND,按下即低电平,GPIO 唤醒可靠触发)或 12 小时
// 定时兜底;深睡唤醒等效复位重启,app_main 会再次直进木鱼页。
// 任何页面长按电源键都关机(main.c 分发);必须持 LVGL 锁调用(内部改 UI)。
void demo_woodfish_power_off(void) {
    if (s_wake_guard) return;            // 开机瞬间按住的键不算长按
    if (s_hint_lbl) lv_label_set_text(s_hint_lbl, "POWERING OFF...");
    woodfish_store_save_now(&s_model);

    // 等 UP 松开再睡:否则按住的低电平会立刻触发唤醒,变成“没关掉”。
    for (int i = 0; i < 150 && bsp_button_read_mv() < WF_BTN_HELD_MV; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));   // 最多 3 秒
    }

    if (s_knock_q) xQueueReset(s_knock_q);
    bsp_display_backlight(0);
    vTaskDelay(pdMS_TO_TICKS(150));
    ESP_LOGI(TAG, "进入深度睡眠: merit=%lu muted=%d auto=%d tone=%d",
             (unsigned long)s_model.merit, s_model.muted,
             s_model.auto_mode, s_style);
    // C3 无 ext0,用 GPIO 唤醒:GPIO0(UP 键)低电平唤醒
    esp_deep_sleep_enable_gpio_wakeup(1ULL << 0, ESP_GPIO_WAKEUP_GPIO_LOW);
    esp_sleep_enable_timer_wakeup(12ULL * 3600ULL * 1000000ULL);
    esp_deep_sleep_start();              // 不返回
}

// 开机时若仍按着键(如深睡唤醒),轮询到松开后再缓冲 300ms,期间忽略一切
// 按键事件,防止唤醒键松开瞬间的 CLICK 误触发、以及按住唤醒又立刻长按关机。
static void guard_tick(lv_timer_t *timer) {
    int mv = bsp_button_read_mv();
    s_guard_ticks++;
    bool loose = (mv < 0 || mv >= WF_BTN_HELD_MV);
    if (loose) s_guard_loose++;
    else s_guard_loose = 0;
    if (s_guard_loose >= 3 || s_guard_ticks >= 100) {   // 松开300ms 或 10s 兜底
        s_wake_guard = false;
        lv_timer_delete(timer);
        s_guard_timer = NULL;
    }
}

// ---- 画面搭建 ----
static lv_obj_t *wf_block(lv_obj_t *parent, int x, int y, int w, int h,
                          uint32_t color) {
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    return obj;
}

static lv_obj_t *wf_circle(lv_obj_t *parent, int x, int y, int d,
                           uint32_t color, int border) {
    lv_obj_t *obj = wf_block(parent, x, y, d, d, color);
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, 0);
    if (border > 0) {
        lv_obj_set_style_border_color(obj, lv_color_hex(UI_INK), 0);
        lv_obj_set_style_border_width(obj, border, 0);
    }
    return obj;
}

static void build_screen(void) {
    s_scr = ui_pixel_screen_create("MUYU");

    // 电量(右上角,云朵下方):图标 + 百分比
    s_batt_lbl = ui_pixel_label(s_scr, "", &lv_font_montserrat_14, UI_INK);
    lv_obj_set_pos(s_batt_lbl, 188, 28);
    if (!s_battery_ok) lv_obj_add_flag(s_batt_lbl, LV_OBJ_FLAG_HIDDEN);

    // 功德面板
    lv_obj_t *panel = ui_pixel_panel_create(s_scr, 18, 46, 204, 54, UI_PAPER);
    lv_obj_t *cap = ui_pixel_label(panel, "MERIT", &lv_font_montserrat_14, UI_INK);
    lv_obj_align(cap, LV_ALIGN_TOP_MID, 0, 4);
    s_merit_lbl = ui_pixel_label(panel, "0", &lv_font_montserrat_20, UI_INK);
    lv_obj_align(s_merit_lbl, LV_ALIGN_TOP_MID, 0, 20);

    // 木鱼:圆形鱼身 + 横向鱼口 + 眼点
    s_fish = lv_obj_create(s_scr);
    lv_obj_remove_flag(s_fish, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_fish, 60, 118);
    lv_obj_set_size(s_fish, 108, 108);
    lv_obj_set_style_bg_opa(s_fish, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_fish, 0, 0);
    lv_obj_set_style_pad_all(s_fish, 0, 0);
    wf_circle(s_fish, 0, 0, 108, WF_WOOD, 4);              // 鱼身
    wf_circle(s_fish, 12, 16, 84, WF_WOOD_DARK, 0);         // 内圈(厚壁感)
    lv_obj_t *slit = wf_block(s_fish, 14, 46, 80, 16, WF_SLIT);   // 鱼口
    lv_obj_set_style_radius(slit, 8, 0);
    lv_obj_set_style_border_color(slit, lv_color_hex(UI_INK), 0);
    lv_obj_set_style_border_width(slit, 3, 0);
    wf_circle(s_fish, 26, 22, 10, UI_INK, 0);               // 眼点

    // 木槌:竖柄 + 圆槌头,悬在木鱼右上
    s_striker = lv_obj_create(s_scr);
    lv_obj_remove_flag(s_striker, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_striker, 146, 102);
    lv_obj_set_size(s_striker, 40, 64);
    lv_obj_set_style_bg_opa(s_striker, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_striker, 0, 0);
    lv_obj_set_style_pad_all(s_striker, 0, 0);
    wf_block(s_striker, 16, 0, 8, 36, WF_HANDLE);           // 柄
    wf_circle(s_striker, 5, 30, 30, WF_HEAD, 3);            // 槌头

    // 段位与进度条
    s_rank_lbl = ui_pixel_label(s_scr, "Novice", &lv_font_montserrat_14, UI_INK);
    lv_obj_set_style_text_align(s_rank_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_rank_lbl, 20, 230);
    lv_obj_set_width(s_rank_lbl, 200);

    s_bar = lv_bar_create(s_scr);
    lv_obj_set_pos(s_bar, 60, 250);
    lv_obj_set_size(s_bar, 120, 10);
    lv_bar_set_range(s_bar, 0, 1000);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(UI_MUTED), 0);
    lv_obj_set_style_border_color(s_bar, lv_color_hex(UI_INK), 0);
    lv_obj_set_style_border_width(s_bar, 2, 0);
    lv_obj_set_style_radius(s_bar, 0, 0);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(UI_ORANGE), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_bar, 0, LV_PART_INDICATOR);

    // 状态行:♪ 音色 + 音量 + 播放状态(草地上沿)
    s_status_lbl = ui_pixel_label(s_scr, "", &lv_font_montserrat_14, UI_INK);
    lv_obj_set_style_text_align(s_status_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_status_lbl, 10, 264);
    lv_obj_set_width(s_status_lbl, 220);

    // 操作提示(草地前)
    s_hint_lbl = ui_pixel_label(s_scr, "OK:KNOCK UP:BACK DN:TONE",
                                &lv_font_montserrat_14, UI_INK);
    lv_obj_set_style_text_align(s_hint_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_hint_lbl, 10, 283);
    lv_obj_set_width(s_hint_lbl, 220);

    // 作者署名(草地右下角)
    lv_obj_t *credit = ui_pixel_label(s_scr, "BY NICK",
                                      &lv_font_montserrat_14, UI_INK);
    lv_obj_set_pos(credit, 172, 302);
}

// ---- demo 页接口 ----
void demo_woodfish_prepare(bool audio_ok, bool battery_ok) {
    s_audio_ok = audio_ok;
    s_battery_ok = battery_ok;
    if (!woodfish_store_init(&s_model)) {
        ESP_LOGW(TAG, "功德存储不可用,本次会话从零开始");
    }
    s_style = (int)woodfish_store_tone();
    if (s_style < 0 || s_style >= WF_STYLE_COUNT) s_style = 0;
    s_float_count = 0;
    s_float_slot = 0;
    s_screen_off = false;
}

void demo_woodfish_enter(void) {
    build_screen();
    refresh_merit();
    refresh_status();
    batt_refresh();

    s_float_count = 0;
    // 音频任务、队列与四种音色缓冲只在首次进入时创建并常驻:
    // 退出时若强删正在播放的任务会与 codec 写入竞态,常驻方案零风险。
    if (s_audio_ok) {
        for (int i = 0; i < WF_STYLE_COUNT; i++) {
            if (!s_pcm[i]) {
                s_pcm_samples[i] = WF_SAMPLE_RATE * STYLES[i].ms / 1000;
                s_pcm[i] = malloc(s_pcm_samples[i] * sizeof(int16_t));
                if (s_pcm[i]) STYLES[i].synth(s_pcm[i], s_pcm_samples[i]);
                else ESP_LOGE(TAG, "音色 %s 缓冲分配失败(%u 字节)",
                             STYLES[i].name,
                             (unsigned)(s_pcm_samples[i] * sizeof(int16_t)));
            }
        }
    }
    if (!s_knock_q) s_knock_q = xQueueCreate(2, sizeof(uint8_t));
    if (!s_audio_task && s_knock_q) {
        xTaskCreate(audio_task, "wf_audio", 4096, NULL, 4, &s_audio_task);
    }
    if (s_battery_ok && !s_batt_timer) {
        s_batt_timer = lv_timer_create(batt_tick, 30000, NULL);
    }
    if (!s_bl_timer) {
        s_bl_timer = lv_timer_create(bl_tick, WF_SCREEN_TIMEOUT_MS, NULL);
    }
    if (s_model.auto_mode) set_auto(true);

    // 从深睡唤醒(复位重启)时用户可能仍按着唤醒键:忽略直到松开
    s_guard_ticks = 0;
    s_guard_loose = 0;
    int mv = bsp_button_read_mv();
    if (mv >= 0 && mv < WF_BTN_HELD_MV) {
        s_wake_guard = true;
        s_guard_timer = lv_timer_create(guard_tick, 100, NULL);
    }

    lv_screen_load(s_scr);
}

void demo_woodfish_exit(void) {
    screen_wake();                           // 返回菜单时屏幕必须是亮的
    woodfish_store_request_save(&s_model);
    if (s_auto_timer) { lv_timer_delete(s_auto_timer); s_auto_timer = NULL; }
    if (s_batt_timer) { lv_timer_delete(s_batt_timer); s_batt_timer = NULL; }
    if (s_guard_timer) { lv_timer_delete(s_guard_timer); s_guard_timer = NULL; }
    if (s_bl_timer) { lv_timer_delete(s_bl_timer); s_bl_timer = NULL; }
    s_wake_guard = false;
    if (s_knock_q) xQueueReset(s_knock_q);   // 丢弃未播的敲击声
    if (s_scr) { lv_obj_delete(s_scr); s_scr = NULL; }
    s_float_count = 0;
    s_merit_lbl = s_rank_lbl = s_hint_lbl = NULL;
    s_status_lbl = s_batt_lbl = s_bar = NULL;
    s_fish = s_striker = NULL;
}

void demo_woodfish_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (s_wake_guard) return;                 // 唤醒后按键未松开期间,忽略
    screen_wake();                            // 手动操作:亮屏 + 重置息屏倒计时
    if (btn == BSP_BTN_OK && ev == BSP_BTN_PRESS) {
        do_knock();
        return;
    }
    if (ev == BSP_BTN_CLICK) {
        if (btn == BSP_BTN_DOWN) {            // 切换音色 + 预览一声
            s_style = (s_style + 1) % WF_STYLE_COUNT;
            refresh_status();
            woodfish_store_set_tone((uint8_t)s_style);
            preview_tone();
        }
        // UP 短按由 main.c 拦截为"保存退出返回菜单",息屏时只亮屏
    } else if (ev == BSP_BTN_LONG) {
        if (btn == BSP_BTN_UP) {
            demo_woodfish_power_off();        // 长按关机
        } else if (btn == BSP_BTN_DOWN) {
            set_auto(!s_model.auto_mode);     // 长按自动敲击
        } else if (btn == BSP_BTN_OK) {
            s_model.muted = !s_model.muted;   // 长按静音/取消静音
            refresh_status();
            woodfish_store_request_save(&s_model);
        }
    }
}

bool demo_woodfish_screen_off(void) {
    return s_screen_off;
}
