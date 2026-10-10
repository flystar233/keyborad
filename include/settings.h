// ============================================================================
//  settings.h —— 键位预设 + 配置持久化 (NVS/Preferences)
//
//  HID 键盘报文布局（8 字节，与 BLE HID boot keyboard 一致）：
//     [0] 修饰键  [1] 保留  [2..7] 最多 6 个同时按下的键码
//
//  修饰键位：
//     0x01 Left Ctrl   0x02 Left Shift  0x04 Left Alt   0x08 Left GUI(Win/Cmd)
//     0x10 Right Ctrl  0x20 Right Shift 0x40 Right Alt  0x80 Right GUI
//
//  6 组预设全部沿用原工程的定义，没有改动，保证行为一致。
// ============================================================================
#pragma once

#include <Arduino.h>

#define PRESET_COUNT     6
#define KEY_COUNT        5
#define HID_REPORT_LEN   8

typedef struct {
    uint8_t keymap[KEY_COUNT][HID_REPORT_LEN];  // 5 个键各自要发的报文
    char    name[20];                           // 预设名（显示在 OLED 上）
    char    keyDesc[KEY_COUNT][16];             // 每个键的说明文字
} KeyPreset;

extern const KeyPreset PRESETS[PRESET_COUNT];

// 全按键释放报文
extern const uint8_t HID_RELEASE[HID_REPORT_LEN];

// ---------------------------------------------------------------- 运行时配置
void     SettingsInit();                        // 挂载 NVS 并载入
uint8_t  SettingsPresetIndex();                 // 当前预设号 0..PRESET_COUNT-1
void     SettingsSetPresetIndex(uint8_t idx);   // 立即写回 NVS
uint8_t  SettingsLedBrightness();
void     SettingsSetLedBrightness(uint8_t b);
const KeyPreset& SettingsCurrentPreset();

// 取第 index 个键在当前预设下要发的报文
const uint8_t* SettingsKeyReport(uint8_t keyIndex);
