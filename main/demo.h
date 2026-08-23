// main/demo.h —— 每个演示页实现的统一接口。
// 新增一个演示页 = 实现这三个函数 + 在 main.c 的 DEMOS[] 里加一行。
#pragma once

#include <stdbool.h>

#include "bsp_button.h"

typedef struct {
    const char *name;
    void (*enter)(void);                          // 建自己的屏并载入
    void (*exit)(void);                           // 删屏、停定时器、释放资源
    void (*key)(bsp_btn_t btn, bsp_btn_ev_t ev);  // 收按键(电源键与 OK 长按由 main 统一分发)
} demo_entry_t;

// 各演示页(定义在各自的 .c 里)
void demo_display_enter(void); void demo_display_exit(void);
void demo_display_key(bsp_btn_t btn, bsp_btn_ev_t ev);

void demo_button_enter(void);  void demo_button_exit(void);
void demo_button_key(bsp_btn_t btn, bsp_btn_ev_t ev);

void demo_audio_enter(void);   void demo_audio_exit(void);
void demo_audio_key(bsp_btn_t btn, bsp_btn_ev_t ev);

void demo_battery_enter(void); void demo_battery_exit(void);
void demo_battery_key(bsp_btn_t btn, bsp_btn_ev_t ev);

void demo_woodfish_prepare(bool audio_ok, bool battery_ok);
void demo_woodfish_enter(void); void demo_woodfish_exit(void);
void demo_woodfish_key(bsp_btn_t btn, bsp_btn_ev_t ev);

// 保存当前功德并进入深度睡眠(任何页面长按电源键都走这里;不返回)。
void demo_woodfish_power_off(void);

// 翻转木鱼页息屏/亮屏(短按电源键;开机防误触 guard 期间为空操作)。
void demo_woodfish_screen_toggle(void);

// 保存退出当前演示页,返回主菜单(main.c 实现)。页面在自定义按键交互里
// 请求退出时调用;调用时必须已持有 LVGL 锁(按键回调上下文天然满足)。
void app_exit_to_menu(void);
