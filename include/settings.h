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
//  5 组预设：出厂默认值写在 settings.cpp；用户可以通过上位机网页（tools/keymap.html）
//  修改，修改后的整张表存在 NVS 里，上电优先加载 NVS 里的版本。
// ============================================================================
#pragma once

#include <Arduino.h>

#define PRESET_COUNT     5
#define KEY_COUNT        5
#define HID_REPORT_LEN   8

typedef struct {
    uint8_t keymap[KEY_COUNT][HID_REPORT_LEN];  // 5 个键各自要发的报文
    char    name[20];                           // 预设名（显示在 OLED 上）
    char    keyDesc[KEY_COUNT][16];             // 每个键的说明文字
    // true = 这组是媒体键：keymap[i] 的前 2 字节是 Consumer Control 位图
    //        （见 lib/hid2ble/Hid2Ble.cpp 报告描述符），走 sendMedia2Ble() 发送。
    //        音量 / 播放这些键不在键盘页里，用普通键盘报文发是没有用的。
    bool    isMedia;
} KeyPreset;

// Consumer Control 位图（第 0 字节），与 Hid2Ble.cpp 报告描述符的顺序一致
#define MEDIA_NEXT_TRACK   0x01
#define MEDIA_PREV_TRACK   0x02
#define MEDIA_STOP         0x04
#define MEDIA_PLAY_PAUSE   0x08
#define MEDIA_MUTE         0x10
#define MEDIA_VOLUME_UP    0x20
#define MEDIA_VOLUME_DOWN  0x40


// 全按键释放报文
extern const uint8_t HID_RELEASE[HID_REPORT_LEN];

// ---------------------------------------------------------------- 运行时配置
void     SettingsInit();                        // 挂载 NVS 并载入
uint8_t  SettingsPresetIndex();                 // 当前预设号 0..PRESET_COUNT-1
// persist=true 立即写回 NVS（内容没变就不写）；false 只改内存（预设切换界面翻页用）
void     SettingsSetPresetIndex(uint8_t idx, bool persist = true);
uint8_t  SettingsSoundLevel();                  // SoundLevel，见 buzzer.h
void     SettingsSetSoundLevel(uint8_t level);  // 立即写回 NVS
const KeyPreset& SettingsCurrentPreset();
const KeyPreset& SettingsPreset(uint8_t idx);   // idx 越界时返回第 0 组

// ---------------------------------------------------------------- 改键（上位机用）
bool SettingsUpdatePreset(uint8_t idx, const KeyPreset &p);  // 只改内存，立即生效
bool SettingsSavePresets();                     // 把整张预设表写进 NVS
void SettingsResetPresets();                    // 恢复出厂预设（内存 + 删除 NVS 里的版本）

// 取第 index 个键在当前预设下要发的报文
const uint8_t* SettingsKeyReport(uint8_t keyIndex);
