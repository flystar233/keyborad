// ============================================================================
//  display.h —— SSD1306 128×32 OLED（硬件 I2C）
//
//  和原工程最大的区别：原工程用 GPIO 软件 bit-bang I2C，而且每个字节都单独发一次
//  START/STOP + 地址，刷一屏要上千次事务，慢到会被 BLE 中断切碎。
//  这里改成：
//    - 硬件 I2C（GPIO4=SDA / GPIO5=SCL），400kHz
//    - 512 字节帧缓冲，刷屏时一次 I2C 事务把整屏推出去（约 13ms）
//    - 写入的是帧缓冲，刷屏由 DisplayFlush() 统一触发并限流
//
//  坐标约定：x = 0..127 列；y = 0..3 页（每页 8 像素高，共 32 像素）
// ============================================================================
#pragma once

#include <Arduino.h>

// 返回 false 表示 I2C 上没有应答（OLED 没插好 / 地址不对）
bool DisplayInit();
bool DisplayPresent();
bool DisplayIsDim();                            // 当前是否处于低亮度

// 探活：重新在总线上 ping 一次地址，返回是否有 ACK
// （不用重启就能确认"面板到底在不在"，用来区分硬件没接和驱动逻辑写错）
bool DisplayPing();

// 扫描整条 I2C 总线，打印所有有应答的地址
void DisplayScanBus();

void DisplayClear();
void DisplayClearRect(uint8_t x, uint8_t page, uint8_t w, uint8_t hPages);

// 6x8 字体：x、page 定位；不会画到屏幕外
void DisplayText(uint8_t x, uint8_t page, const char *str);
// 居中显示（6px/字符）
void DisplayTextCentered(uint8_t page, const char *str);

// ---------------------------------------------------------------- 图元（画图标用，不依赖字体）
// x, y 是全屏像素坐标（x 0..127，y 0..31），内部自动换算到页
void DisplayPixel(uint8_t x, uint8_t y);

// 电池图标：17x8 像素。percent 0..100 决定内部填充宽度，
// percent < 0 表示"没装电池"，画成空心（不做任何猜测）。
// 几何参数在 display_icons.h；校验脚本 tools/verify_icons.py
void DisplayIconBattery(uint8_t x, uint8_t y, int percent);

// 连接状态图标：9x11 像素，经典蓝牙"B"符文。
//   实心 = 已连接，空心 = 正在广播/配对中，两条竖线 = 蓝牙未启动
enum BtIconState { BT_ICON_OFF = 0, BT_ICON_PAIRING, BT_ICON_CONNECTED };
void DisplayIconBluetooth(uint8_t x, uint8_t y, BtIconState st);

// 把帧缓冲推到 OLED。内部限流到 OLED_FLUSH_INTERVAL_MS，返回 true 表示真的发了
bool DisplayFlush();

// 对比度：low=true 时压到最低（低电 / 待机变暗），false 恢复 OLED_CONTRAST_NORMAL
void DisplaySetDim(bool dim);
// 开关面板（SSD1306 0xAE / 0xAF），关屏时省电
void DisplaySetPower(bool on);
bool DisplayIsOn();
