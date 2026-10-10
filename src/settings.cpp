// ============================================================================
//  settings.cpp —— 键位预设表 + NVS 持久化
// ============================================================================
#include <Arduino.h>
#include <Preferences.h>
#include <string.h>
#include "board_config.h"
#include "settings.h"
#include "usb_log.h"

// 全按键释放
const uint8_t HID_RELEASE[HID_REPORT_LEN] = { 0, 0, 0, 0, 0, 0, 0, 0 };

// ---------------------------------------------------------------------------
//  6 组预设（数值与原工程完全一致）
// ---------------------------------------------------------------------------
const KeyPreset PRESETS[PRESET_COUNT] = {
    // ---- 0: Ctrl CVX ----
    {
        {
            { 0x01, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00 },  // Ctrl+C
            { 0x01, 0x00, 0x19, 0x00, 0x00, 0x00, 0x00, 0x00 },  // Ctrl+V
            { 0x01, 0x00, 0x1B, 0x00, 0x00, 0x00, 0x00, 0x00 },  // Ctrl+X
            { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },  // 空
            { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }   // 空
        },
        "Ctrl CVX",
        { "Ctrl+C", "Ctrl+V", "Ctrl+X", "N/A", "N/A" }
    },
    // ---- 1: Win Combos ----
    {
        {
            { 0x04, 0x00, 0x2B, 0x00, 0x00, 0x00, 0x00, 0x00 },  // Alt+Tab
            { 0x08, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00 },  // Win+D
            { 0x05, 0x00, 0x4C, 0x00, 0x00, 0x00, 0x00, 0x00 },  // Ctrl+Alt+Del
            { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
            { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }
        },
        "Win Combos",
        { "Alt+Tab", "Win+D", "Ctrl+Alt+Del", "N/A", "N/A" }
    },
    // ---- 2: VSCode ----
    {
        {
            { 0x01, 0x00, 0x0E, 0x06, 0x00, 0x00, 0x00, 0x00 },  // Ctrl+K+C 注释
            { 0x01, 0x00, 0x0E, 0x18, 0x00, 0x00, 0x00, 0x00 },  // Ctrl+K+U 取消注释
            { 0x01, 0x00, 0x2F, 0x00, 0x00, 0x00, 0x00, 0x00 },  // Ctrl+[   左对齐
            { 0x01, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00 },  // Ctrl+]   右对齐
            { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }
        },
        "VSCode",
        { "(Comment)", "(Uncomment)", "(Left Align)", "(Right Align)", "N/A" }
    },
    // ---- 3: LCEDA Tools（立创EDA 快捷键）----
    {
        {
            { 0x04, 0x00, 0x1A, 0x00, 0x00, 0x00, 0x00, 0x00 },  // Alt+W 布线
            { 0x04, 0x00, 0x19, 0x00, 0x00, 0x00, 0x00, 0x00 },  // Alt+V 过孔
            { 0x04, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00 },  // Alt+E 铺铜
            { 0x04, 0x00, 0x17, 0x00, 0x00, 0x00, 0x00, 0x00 },  // Alt+T 顶层
            { 0x04, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00 }   // Alt+B 底层
        },
        "LCEDA Tools",
        { "Alt+W(Wire)", "Alt+V(Via)", "Alt+E(Copper)", "Alt+T(Top)", "Alt+B(Bottom)" }
    },
    // ---- 4: Tab x5 ----
    {
        {
            { 0x00, 0x00, 0x2B, 0x00, 0x00, 0x00, 0x00, 0x00 },  // Tab
            { 0x00, 0x00, 0x2B, 0x00, 0x00, 0x00, 0x00, 0x00 },
            { 0x00, 0x00, 0x2B, 0x00, 0x00, 0x00, 0x00, 0x00 },
            { 0x00, 0x00, 0x2B, 0x00, 0x00, 0x00, 0x00, 0x00 },
            { 0x00, 0x00, 0x2B, 0x00, 0x00, 0x00, 0x00, 0x00 }
        },
        "Tab.b.b.b.b. (?",
        { "Tab", "Tab", "Tab", "Tab", "Tab" }
    },
    // ---- 5: MediaCtrls ----
    // 注意：这几个键码在多数系统上不生效（原工程里也标注了 N/A），
    // 保留是为了和原工程行为一致，方便以后改成真正的媒体键。
    {
        {
            { 0x00, 0x00, 0xE9, 0x00, 0x00, 0x00, 0x00, 0x00 },  // Volume Up
            { 0x00, 0x00, 0xEA, 0x00, 0x00, 0x00, 0x00, 0x00 },  // Volume Down
            { 0x00, 0x00, 0x6F, 0x00, 0x00, 0x00, 0x00, 0x00 },  // Brightness Up
            { 0x00, 0x00, 0x70, 0x00, 0x00, 0x00, 0x00, 0x00 },  // Brightness Down
            { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }
        },
        "MediaCtrls(N/A)",
        { "Volume +", "Volume -", "Brightness +", "Brightness -", "N/A" }
    }
};

// ---------------------------------------------------------------------------
//  运行时状态
// ---------------------------------------------------------------------------
static Preferences sPrefs;
static uint8_t     sPresetIdx = 0;
static uint8_t     sLedBright = LED_BRIGHTNESS;

// NVS 命名空间与键名（和原工程用同一个命名空间，方便沿用旧配置）
#define NVS_NS          "KEY_CONFIG"
#define NVS_KEY_PRESET  "preset"
#define NVS_KEY_BRIGHT  "bright"

void SettingsInit() {
    sPrefs.begin(NVS_NS, false);    // 读写模式

    sPresetIdx = sPrefs.getUChar(NVS_KEY_PRESET, 0);
    if (sPresetIdx >= PRESET_COUNT) sPresetIdx = 0;

    sLedBright = sPrefs.getUChar(NVS_KEY_BRIGHT, LED_BRIGHTNESS);
    if (sLedBright < 8) sLedBright = 8;

    usbLog::printf("[cfg] preset=%u/%u ('%s'), led brightness=%u\n",
                   (unsigned)(sPresetIdx + 1), (unsigned)PRESET_COUNT,
                   PRESETS[sPresetIdx].name, (unsigned)sLedBright);
}

uint8_t SettingsPresetIndex() { return sPresetIdx; }

void SettingsSetPresetIndex(uint8_t idx) {
    if (idx >= PRESET_COUNT) return;
    sPresetIdx = idx;
    sPrefs.putUChar(NVS_KEY_PRESET, idx);
    usbLog::printf("[cfg] preset -> %u ('%s')\n", (unsigned)(idx + 1), PRESETS[idx].name);
}

uint8_t SettingsLedBrightness() { return sLedBright; }

void SettingsSetLedBrightness(uint8_t b) {
    if (b < 8) b = 8;
    sLedBright = b;
    sPrefs.putUChar(NVS_KEY_BRIGHT, b);
}

const KeyPreset& SettingsCurrentPreset() { return PRESETS[sPresetIdx]; }

const uint8_t* SettingsKeyReport(uint8_t keyIndex) {
    if (keyIndex >= KEY_COUNT) return HID_RELEASE;
    return PRESETS[sPresetIdx].keymap[keyIndex];
}
