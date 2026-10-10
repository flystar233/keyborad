// ============================================================================
//  battery.h —— 电池电压检测
//
//  硬件：BAT+ --(R9 100K)--> GPIO3 --(R15 100K)--> GND，C6 100nF 滤波
//        分压比 = 100K/(100K+100K) = 0.5
//        4.20V 满电 -> GPIO3 = 2.10V（ADC 量程内，安全）
//
//  注意：GPIO3 是模拟输入专用，代码里任何地方都不能把它当输出驱动。
//        （原工程用 A0/GPIO0 是错的：GPIO0 在本板上是 K1。）
//
//  没装电池时 BAT+ 也有 4.2V（TP4056 恒压空载），所以"有没有电池"用一个
//  明确开关（board_config.h 的 BATTERY_PRESENT / 运行时 BatterySetPresent），
//  不靠电压去猜 —— 没装电池和电池充满，BAT+ 电压是完全一样的。
// ============================================================================
#pragma once

#include <Arduino.h>

void BatteryInit();

// 主循环里调用，内部按 BATTERY_READ_INTERVAL_MS 限流，非阻塞
void BatteryUpdate();

// 运行时开关"有没有装电池"（装上 / 拆下后不用重新烧录）
void BatterySetPresent(bool present);
bool BatteryIsPresent();

float   BatteryVoltage();       // 电池电压（V），未装电池时返回 0
uint8_t BatteryPercent();       // 0..100，未装电池时返回 0
bool    BatteryIsLow();         // 低于 BAT_LOW_V
