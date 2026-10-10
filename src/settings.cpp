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
//  出厂默认的 5 组预设。运行时用的是 sPresets（可被上位机修改并存进 NVS）
// ---------------------------------------------------------------------------
static const KeyPreset kDefaultPresets[PRESET_COUNT] = {
    // ---- 0: Daily（日常编辑）----
    {
        {
            { 0x01, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00 },  // Ctrl+C
            { 0x01, 0x00, 0x19, 0x00, 0x00, 0x00, 0x00, 0x00 },  // Ctrl+V
            { 0x00, 0x00, 0x46, 0x00, 0x00, 0x00, 0x00, 0x00 },  // PrtSc 截图
            { 0x01, 0x00, 0x1D, 0x00, 0x00, 0x00, 0x00, 0x00 },  // Ctrl+Z 撤销
            { 0x01, 0x00, 0x16, 0x00, 0x00, 0x00, 0x00, 0x00 }   // Ctrl+S 保存
        },
        "Daily",
        { "Ctrl+C", "Ctrl+V", "PrtSc", "Ctrl+Z", "Ctrl+S" }
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
    // ---- 4: Media ----
    // 原工程把 0xE9/0xEA/0x6F/0x70 塞进键盘报文，但这几个是 Consumer 页的用法码，
    // 而且超出了键盘报告描述符的 LOGICAL_MAXIMUM(0x65)，主机会直接丢弃 -> 一直是 N/A。
    // 现在改走 Consumer Control 报文（isMedia = true），前 2 字节是位图。
    // 亮度 +/- 不在报告描述符的 16 个用法里（加进去要改描述符、主机得重新配对），
    // 所以换成了所有系统都认的 播放/暂停、静音、下一曲。
    {
        {
            { MEDIA_VOLUME_DOWN, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
            { MEDIA_VOLUME_UP,   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
            { MEDIA_PLAY_PAUSE,  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
            { MEDIA_MUTE,        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
            { MEDIA_NEXT_TRACK,  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }
        },
        "Media",
        { "Volume -", "Volume +", "Play/Pause", "Mute", "Next Track" },
        true
    }
};

// ---------------------------------------------------------------------------
//  运行时状态
// ---------------------------------------------------------------------------
static Preferences sPrefs;
static KeyPreset   sPresets[PRESET_COUNT];      // 运行时预设表
static uint8_t     sPresetIdx = 0;
static uint8_t     sSoundLevel = 0;

// NVS 命名空间与键名（和原工程用同一个命名空间，方便沿用旧配置）
#define NVS_NS          "KEY_CONFIG"
#define NVS_KEY_PRESET  "preset"
#define NVS_KEY_KEYMAP  "keymap"
#define NVS_KEY_SOUND   "sound"

// NVS 里存的整张预设表。magic 里带上结构体大小：以后改了 KeyPreset 的布局，
// 旧数据大小 / magic 对不上就自动忽略，回到出厂预设，不会读出一堆乱码。
struct KeymapBlob {
    uint32_t  magic;
    KeyPreset presets[PRESET_COUNT];
};
static const uint32_t KEYMAP_MAGIC = 0x4B4D0000UL | (uint32_t)sizeof(KeyPreset);

// 字符串字段保证以 0 结尾、只含屏幕字体能显示的 ASCII
static void sanitizeText(char *s, size_t cap) {
    s[cap - 1] = 0;
    for (size_t i = 0; s[i]; i++) {
        if ((uint8_t)s[i] < 0x20 || (uint8_t)s[i] > 0x7E) s[i] = '?';
    }
}

static void sanitizePreset(KeyPreset &p) {
    sanitizeText(p.name, sizeof(p.name));
    for (uint8_t k = 0; k < KEY_COUNT; k++) sanitizeText(p.keyDesc[k], sizeof(p.keyDesc[k]));
    p.isMedia = p.isMedia ? true : false;
}

static void loadPresets() {
    memcpy(sPresets, kDefaultPresets, sizeof(sPresets));

    if (sPrefs.getBytesLength(NVS_KEY_KEYMAP) != sizeof(KeymapBlob)) return;
    KeymapBlob *blob = (KeymapBlob *)malloc(sizeof(KeymapBlob));
    if (blob == nullptr) return;
    if (sPrefs.getBytes(NVS_KEY_KEYMAP, blob, sizeof(KeymapBlob)) == sizeof(KeymapBlob) &&
        blob->magic == KEYMAP_MAGIC) {
        for (uint8_t i = 0; i < PRESET_COUNT; i++) {
            sanitizePreset(blob->presets[i]);
            sPresets[i] = blob->presets[i];
        }
        usbLog::print("[cfg] custom keymap loaded from NVS\n");
    }
    free(blob);
}

void SettingsInit() {
    sPrefs.begin(NVS_NS, false);    // 读写模式
    loadPresets();

    sPresetIdx = sPrefs.getUChar(NVS_KEY_PRESET, 0);
    if (sPresetIdx >= PRESET_COUNT) sPresetIdx = 0;

    sSoundLevel = sPrefs.getUChar(NVS_KEY_SOUND, 0);
    if (sSoundLevel > 2) sSoundLevel = 0;


    usbLog::printf("[cfg] preset=%u/%u ('%s'), sound=%u\n",
                   (unsigned)(sPresetIdx + 1), (unsigned)PRESET_COUNT,
                   sPresets[sPresetIdx].name, (unsigned)sSoundLevel);
}

uint8_t SettingsPresetIndex() { return sPresetIdx; }

void SettingsSetPresetIndex(uint8_t idx, bool persist) {
    if (idx >= PRESET_COUNT) return;
    sPresetIdx = idx;
    if (persist && sPrefs.getUChar(NVS_KEY_PRESET, 0xFF) != idx) {
        sPrefs.putUChar(NVS_KEY_PRESET, idx);
    }
    usbLog::printf("[cfg] preset -> %u ('%s')%s\n", (unsigned)(idx + 1), sPresets[idx].name,
                   persist ? " saved" : "");
}

uint8_t SettingsSoundLevel() { return sSoundLevel; }

void SettingsSetSoundLevel(uint8_t level) {
    if (level > 2) level = 0;
    sSoundLevel = level;
    if (sPrefs.getUChar(NVS_KEY_SOUND, 0xFF) != level) sPrefs.putUChar(NVS_KEY_SOUND, level);
}

const KeyPreset& SettingsCurrentPreset() { return sPresets[sPresetIdx]; }

const KeyPreset& SettingsPreset(uint8_t idx) {
    return sPresets[idx < PRESET_COUNT ? idx : 0];
}

bool SettingsUpdatePreset(uint8_t idx, const KeyPreset &p) {
    if (idx >= PRESET_COUNT) return false;
    sPresets[idx] = p;
    sanitizePreset(sPresets[idx]);
    return true;
}

bool SettingsSavePresets() {
    KeymapBlob *blob = (KeymapBlob *)malloc(sizeof(KeymapBlob));
    if (blob == nullptr) return false;
    blob->magic = KEYMAP_MAGIC;
    memcpy(blob->presets, sPresets, sizeof(sPresets));
    bool ok = sPrefs.putBytes(NVS_KEY_KEYMAP, blob, sizeof(KeymapBlob)) == sizeof(KeymapBlob);
    free(blob);
    usbLog::printf("[cfg] keymap saved: %s\n", ok ? "ok" : "FAILED");
    return ok;
}

void SettingsResetPresets() {
    memcpy(sPresets, kDefaultPresets, sizeof(sPresets));
    sPrefs.remove(NVS_KEY_KEYMAP);
    usbLog::print("[cfg] keymap reset to factory defaults\n");
}

const uint8_t* SettingsKeyReport(uint8_t keyIndex) {
    if (keyIndex >= KEY_COUNT) return HID_RELEASE;
    return sPresets[sPresetIdx].keymap[keyIndex];
}
