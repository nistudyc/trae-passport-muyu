// main/woodfish_model.h —— 敲木鱼积功德的纯逻辑状态机,不依赖 ESP-IDF/LVGL,可主机测试。
#pragma once

#include <stdbool.h>
#include <stdint.h>

// 连击窗口:两次敲击间隔不超过该值时连击数 +1。
#define WOODFISH_COMBO_WINDOW_MS 2000

typedef struct {
    uint32_t merit;          // 累计功德(掉电保存)
    uint32_t session_merit;  // 本次开机新增功德
    uint32_t combo;          // 当前连击数
    uint32_t best_combo;     // 本次会话最佳连击
    uint32_t last_knock_ms;  // 上次敲击的时间戳(不持久化)
    bool muted;              // 静音
    bool auto_mode;          // 自动敲击
} woodfish_model_t;

typedef struct {
    const char *name;      // 段位名(UI 文本按仓库规范使用英文)
    uint32_t threshold;    // 达到该功德值升入此段位
} woodfish_rank_t;

void woodfish_model_init(woodfish_model_t *m);

// 敲一下:功德 +1,并按时间窗口维护连击。now_ms 用单调时钟毫秒值。
void woodfish_model_knock(woodfish_model_t *m, uint32_t now_ms);

// 功德对应的当前段位(表驱动,末项兜底)。
const woodfish_rank_t *woodfish_model_rank(uint32_t merit);

// 下一档段位;已是最高档返回 NULL。
const woodfish_rank_t *woodfish_model_next_rank(uint32_t merit);

// 距下一档的进度,0..1000(千分比);最高档恒为 1000。
uint32_t woodfish_model_rank_progress(uint32_t merit);
