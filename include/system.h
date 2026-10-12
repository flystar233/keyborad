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
    MODE_KEY_CONFIG = 1,    // 键位预设切换
};

void SystemInit();
// 蓝牙 NUS 串口：读一个字节（没有返回 -1）/ 发数据。由 main.cpp 接到 BleKeyboard 上
void SystemSetBleSerial(int (*readFn)(), size_t (*writeFn)(const uint8_t *, size_t));
// 串口命令 b：打印蓝牙状态。由 main.cpp 接到 BleKeyboard::dumpStatus
void SystemSetBleDump(void (*dumpFn)());
void SystemUpdate();                    // 主循环调用：SW1 短按/长按、预设确认、息屏

// 按键事件入口（main 把它注册给 ButtonsInit）
void SystemHandleButtonEvent(ButtonId id, bool pressed, bool longPress, bool repeat);

// 键盘报文的 press/release 时序：main 每轮调一次
uint8_t SystemPeekPendingKey();         // 0xFF = 当前没有待发键
void    SystemCompletePendingKey();
bool    SystemTakeReleaseRequest();     // true = 需要发一条"全键释放"

// BLE 连接状态同步给显示层（main 在状态变化时调用）
void SystemSetLink(LedLinkState st);

// 绘制当前界面到 OLED 帧缓冲（不刷屏，刷屏由 main 调 DisplayFlush）
void SystemDrawDisplay();

// 界面上的可见内容是否变了（变了才需要重画 + 刷屏）
bool SystemDisplayDirty();

// 内容变了就重画并刷屏；被刷屏限流挡回来时保留脏标记，下一轮自动重试
void SystemRefreshDisplay();

// 发送一个键的按下；松开时调 SystemSendKeyRelease，主机按住期间自己连发
void SystemSendKeyPress(uint8_t keyIndex);
void SystemSendKeyRelease(uint8_t keyIndex);
void SystemSendRelease();

// 有按键活动 -> 唤醒屏幕、重置息屏计时
void SystemNoteActivity();

// 串口诊断命令（单键，不需要回车）
void SystemHandleSerial();
