// ============================================================================
//  system.h —— 系统层：模式状态机、OLED 界面、按键事件分发
// ============================================================================
#pragma once

#include <Arduino.h>
#include "buttons.h"
#include "led_effects.h"

// 注意：这几个值同时用于 LED_EffectsSetMode()，改动要一起改
enum SystemMode {
    MODE_NORMAL     = 0,    // 正常键盘模式：按键打字
    MODE_TIMER_SET  = 1,    // 倒计时设置
    MODE_METRONOME  = 2,    // 节拍器
    MODE_KEY_CONFIG = 3,    // 键位预设切换
};

void SystemInit();
void SystemUpdate();                    // 主循环调用：长按组合判定、节拍器/定时器推进、息屏

// 按键事件入口（main 把它注册给 ButtonsInit）
void SystemHandleButtonEvent(ButtonId id, bool pressed, bool longPress, bool repeat);

// 键盘报文的 press/release 时序：main 每轮调一次
void    SystemTick();
uint8_t SystemPeekPendingKey();         // 0xFF = 当前没有待发键
void    SystemCompletePendingKey();
bool    SystemTakeReleaseRequest();     // true = 需要发一条"全键释放"

uint8_t SystemModeGet();
bool    SystemKeyEnabled();

// BLE 连接状态同步给显示层（main 在状态变化时调用）
void SystemSetLink(LedLinkState st);

// 绘制当前界面到 OLED 帧缓冲（不刷屏，刷屏由 main 调 DisplayFlush）
void SystemDrawDisplay();

// 界面上的可见内容是否变了（变了才需要重画 + 刷屏）
bool SystemDisplayDirty();
void SystemDisplayMarkClean();

// 屏幕是否被串口诊断冻结（冻结时 main 不应重画）
bool SystemDisplayFrozen();

// 只画两个图标（给串口诊断用）
void SystemDrawIconsOnly();

// 发送一个键的按下（内部记账，由 SystemTick 负责释放）
void SystemSendKeyPress(uint8_t keyIndex);
void SystemSendRelease();

// 有按键活动 -> 唤醒屏幕、重置息屏计时
void SystemNoteActivity();

// 串口诊断命令（单键，不需要回车）
void SystemHandleSerial();
