# 敲木鱼积功德 · FoloToy AI Passport

[English](README.md) | 简体中文

<p align="center">
  <img src="docs/images/muyu-hero.png" width="400" alt="FoloToy AI Passport 上运行的敲木鱼积功德应用,背景是发光的功德广告牌">
</p>

基于 [FoloToy AI Passport](https://github.com/folotoy/ai-passport) 可穿戴设备的敲木鱼积功德应用——由 **Nick** 使用 TRAE 打造。开机直接进入应用:按下 `OK`,功德 +1。

## 功能亮点

- 功德计数,段位升级(100 → 100 000)与连击检测,NVS 掉电保存
- 四种合成音色:经典木鱼「笃」· 铜磬「叮」· 梆子「哒」· 水滴「咚」
- 电量显示(CW2017),每 30 秒刷新,低电量变红提醒
- 2 分钟自动息屏;息屏期间自动敲击继续计数、发声("听声攒功德")
- 静音、自动敲击、音色、亮度档均持久化保存

| 按键 | 操作 | 功能 |
| --- | --- | --- |
| `OK` | 按下 | 敲木鱼:功德 +1、音效、动画、飘出 "+1" |
| `OK` | 长按 | 静音 / 取消静音 |
| `UP` | 短按 | 保存并退出,返回上一级(菜单中:息屏/亮屏切换) |
| `UP` | 长按 1.5 秒 | 任何页面关机(深度睡眠;再按 `UP` 开机) |
| `DOWN` | 短按 | 循环切换音色 CLASSIC / BELL / BLOCK / DROP |
| `DOWN` | 长按 | 每 0.7 秒自动敲击 |

## 网页刷机

打开刷机网页,用 USB 连接设备:

**https://ai-passport.folotoy.cn/tools/web-flasher/**

选择合并后的单文件镜像(`woodfish-flash-all.bin`,写入偏移 `0x0`),建议勾选"清除设备数据",开始写入即可。自己构建合并镜像:

```bash
idf.py build
python -m esptool --chip esp32c3 merge_bin -o woodfish-flash-all.bin \
  --flash_mode dio --flash_size 8MB --flash_freq 80m \
  0x0 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0x10000 build/FoloToy-AI-Passport.bin
```

## 从源码构建

ESP-IDF 5.5.x(已知可用:5.5.3):

```bash
idf.py set-target esp32c3     # 仅全新检出时需要
idf.py build
idf.py flash monitor
```

主机侧逻辑测试(无需硬件):

```bash
cc -std=c11 -Wall -Wextra -Werror -Imain \
  tests/test_woodfish_model.c main/woodfish_model.c \
  -o /tmp/test_woodfish_model
/tmp/test_woodfish_model
```

## 作者

**Nick**([nistudyc](https://github.com/nistudyc))——基于 [folotoy/ai-passport](https://github.com/folotoy/ai-passport) 开发基线,使用 TRAE 构建。

---

# 开发者部分 —— FoloToy AI Passport 基线

本仓库同时也是 FoloToy AI Passport(面向 AI agent 的开放式可穿戴 AI 硬件)的开发基线。`main` 是最小但完整的可运行基线;`components/bsp` 隔离板级细节并提供稳定 API;`demo/*` 分支展示从需求到成品的不同实现路径。在仓库中工作前请先阅读 `AGENTS.md`;构建结果与真机结果分开记录——禁止把"编译通过"描述成"硬件验证通过"。

## 硬件能力契约

| 能力 | 已确认实现 | 应用接口 | 必须遵守的边界 |
| --- | --- | --- | --- |
| 显示 | ST7789P3,240 × 320 竖屏 RGB565,SPI2 40 MHz;LEDC 背光 | `bsp_display_*`、`bsp_lvgl_*` | ESP32-C3 无 PSRAM;小单 DMA 缓冲;无 LCD MISO、触摸、已知 TE 接口 |
| 输入 | `UP`/`DOWN`/`OK` 共用 GPIO0 上的 ADC 分压电阻梯 | `bsp_button_init()`、`bsp_button_read_mv()` | 回调运行在按键组件任务里,不得阻塞;不要创建第二个 ADC1 unit |
| 音频 | ES8311,I2S0 全双工 PCM | `bsp_audio_*` | PCM 读写阻塞,应在 worker 任务中;改格式必须保留 BSP 的关闭/打开顺序 |
| 电池 | CW2017 电量与电压 | `bsp_battery_*` | 运行时可选;精度取决于电芯与 profile |
| 共享总线 | ES8311 与 CW2017 共享 I2C0 | `bsp_i2c_*` | 复用 BSP 拥有的总线;不要在同端口另建总线 |
| 日志/刷机 | 原生 ESP32-C3 USB Serial/JTAG | ESP-IDF console | GPIO18/19 保留给 USB;GPIO21 的 UART0 TX 与背光冲突 |

所有引脚、地址、面板参数、按键电压窗口只在 [`components/bsp/include/bsp_pins.h`](components/bsp/include/bsp_pins.h) 中定义。板卡版本、接线、极性或寄存器行为未知时,报告未知并索要证据,不要用其他 ESP32-C3 板子的参数填补。完整引脚表、面板初始化、ADC 阈值、I2C 规则与内存细节见 [`docs/AI_HARDWARE_DEVELOPMENT_GUIDE.md`](docs/AI_HARDWARE_DEVELOPMENT_GUIDE.md)。

尚未保证的能力:触摸、显示回读、IMU、外部存储、充电控制、USB 插入检测、深睡唤醒接线、任意"空闲 GPIO"、量产级电源规格。涉及这些的需求必须从原理图、板卡版本或实测开始。

## 应用与 BSP 边界

```text
main/                     页面、状态机、动画、应用任务、资源
  └─ components/bsp/include/  稳定的板级 API
      └─ components/bsp/src/  GPIO、总线、设备与驱动细节
          └─ bsp_pins.h       引脚与硬件参数的唯一真相源
```

新增页面:创建实现 `enter`、`exit`、`key` 的 `main/demo_<feature>.c`,并更新 `main/demo.h`、`main/CMakeLists.txt` 与 `main/main.c` 的 `DEMOS[]` 注册。只有被多个应用共享的硬件能力才放进 `components/bsp`。

### 运行时不变量

- LVGL 非线程安全:LVGL 上下文之外必须持 `bsp_lvgl_lock()`。
- 按键回调只分发轻量事件;慢操作放 worker 任务。
- 离开页面前,先停掉所有可能访问其 UI 的任务/定时器,再删屏。
- 新增图片、字体、网络栈、音频缓冲、LVGL 缓冲、任务栈必须对照内部 RAM 评估(无 PSRAM;空闲堆 ≠ 连续大块)。
- 可测试的状态机与时序逻辑应与 ESP-IDF/LVGL 分离,并由主机侧测试覆盖。

## 验收与交付格式

`idf.py build` 是最低自动检查,不等于硬件验证。涉及物理外设的改动需在真机上记录:启动日志稳定、显示/背光正确、按键事件正确、音频播放正常、电池读数合理、页面切换无泄漏。交付时报告:

```text
Build: PASS / FAIL / NOT RUN
Host tests: PASS / FAIL / NOT RUN
Device tests: PASS / FAIL / NOT RUN
Unverified: 仍需要开发板、仪器或用户确认的项
```

## 项目结构

```text
components/bsp/include/  BSP 公开 API 与 bsp_pins.h 硬件事实
components/bsp/src/      显示、按键、音频、电池、共享 I2C 实现
main/                    菜单、LVGL UI、硬件演示页与木鱼应用
tests/                   可脱离硬件运行的轻量逻辑测试源
docs/                    agent 硬件开发指南与扩展文档
sdkconfig.defaults       ESP32-C3、USB console、Flash、LVGL 默认配置
AGENTS.md                agent 在本仓库的编码、验证和提交规则
```
