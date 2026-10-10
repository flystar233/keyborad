// ============================================================================
//  buzzer.h —— 蜂鸣器（GPIO10 -> R10 1K -> Q1 S9013 基极，NPN 低边）
//
//  GPIO10 输出高 -> Q1 导通 -> 蜂鸣器 BUZ1 负极接地 -> 响
//  蜂鸣器是 2.7kHz 无源型，用 LEDC 出方波；不用 Arduino 的 tone()
//  是因为 LEDC 是硬件定时器，出音期间完全不占 CPU、也不会被 BLE 中断影响音准。
// ============================================================================
#pragma once

#include <Arduino.h>

void BuzzerInit();

// 开始 / 停止发声
void BuzzerTone(uint32_t freqHz);
void BuzzerStop();

// 在接下来的 ms 毫秒内响 freqHz，之后由 BuzzerUpdate() 自动停
void BuzzerBeep(uint32_t freqHz, uint32_t ms);

// 常用提示音
void BuzzerClick();         // 按键确认：很短的一声
void BuzzerOk();            // 两连升调：操作成功
void BuzzerError();         // 低音一声：操作失败 / 低电
void BuzzerModeChange();    // 模式切换

// 节拍器重音 / 弱音
void BuzzerBeatAccent();
void BuzzerBeatNormal();

// 主循环调用，处理 Beep 的自动停止
void BuzzerUpdate();

bool BuzzerEnabled();
