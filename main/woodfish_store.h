// main/woodfish_store.h —— 功德计数 NVS 持久化。
// 保存走后台任务并做 3 秒防抖:连续敲击只在停手后写一次 Flash,减轻磨损。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "woodfish_model.h"

// 初始化 NVS 并载入上次功德;失败时回退默认值且 has_error() 为真,应用照常可玩。
bool woodfish_store_init(woodfish_model_t *model);

// 请求保存当前模型快照(非阻塞,可从按键回调/定时器上下文调用)。
void woodfish_store_request_save(const woodfish_model_t *model);

// 同步立即落盘(关机前用,阻塞直到写入完成)。返回是否成功。
bool woodfish_store_save_now(const woodfish_model_t *model);

// 音色偏好:独立小键 u8,单独读写(不与功德 blob 混排,老数据可无损升级)。
uint8_t woodfish_store_tone(void);
void woodfish_store_set_tone(uint8_t tone);

// 屏幕亮度(0..100,默认 100):Display 页调的档位全局生效并持久化。
uint8_t woodfish_store_brightness(void);
void woodfish_store_set_brightness(uint8_t percent);

// 自 init 以来是否发生过读/写失败。
bool woodfish_store_has_error(void);
