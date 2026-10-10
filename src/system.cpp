// ============================================================================
//  system.cpp —— 模式状态机、OLED 界面、按键事件分发
//
//  模式与操作方式：
//    正常模式    ：K1~K5 按下即发送当前预设的键值
//    U4 短按     ：切换灯效（松开时生效）
//    U4 长按 1 秒：在 正常 <-> 预设切换 之间来回切
//    预设切换模式里：K1/K2 翻页、K3 确认、K4 声音档位、K5 播放 / 停止音乐，不发送键值
// ============================================================================
#include <Arduino.h>
#include <stdio.h>
#include <string.h>
#include "board_config.h"
#include "system.h"
#include "buttons.h"
#include "settings.h"
#include "display.h"
#include "led_effects.h"
#include "buzzer.h"
#include "battery.h"
#include "ws2812.h"
#include "usb_log.h"

#define SCREEN_DIM_AFTER_MS     20000UL
#define SCREEN_OFF_AFTER_MS     30000UL
#define MODE_HOLD_MS            1000    // U4 按住多久算长按（切模式）

// ---- 正常模式的屏幕布局（128x32 = 4 页，每页 8 像素高）----
//   第 0 页：左上电池图标 + 紧挨着的电池电压（如 3.87V），右上蓝牙图标
//            （图标 8x8、字体 6x8，都正好占满一页，不跨页）
//   第 2 页：按键弹出文字，固定左对齐，可用整行宽度
// 以前图标画在 y=12（跨第 1、2 页），而弹出文字也在第 2 页：
// 每次按键先清 x=26..121 这一条再写字，蓝牙图标（x=115..124）的下半截就被擦掉，
// 长一点的说明文字还会直接压在图标上。现在两者分在不同的页，互不干扰。
#define ICON_ROW_Y      0
#define ICON_BAT_X      1
#define BAT_VOLT_X      (ICON_BAT_X + 8 + 2)    // 电压文字：图标右边空 2px（字形自带 1 列留白）
#define ICON_BT_X       (OLED_WIDTH - 13)       // 8 宽图标 + 右侧 2px 连接点，共留 13px
#define POPUP_PAGE      2
#define POPUP_X         2

static SystemMode sMode          = MODE_NORMAL;
static bool       sKeyEnabled    = true;

// 当前 BLE 连接状态，由 main 通过 SystemSetLink() 同步过来，用于 OLED 显示
static LedLinkState sLinkState   = LED_LINK_OFF;

// ---- Claude Code 运行状态 ----
// 电脑上 Claude Code 的 hooks 运行 tools/claude_status.py，通过串口发 "@CLAUDE <状态> [工具名]"。
// 正常模式下显示在屏幕最下面一行；等你操作 / 做完时还有灯和提示音。
enum ClaudeState : uint8_t { CL_OFF = 0, CL_IDLE, CL_WORK, CL_TOOL, CL_WAIT, CL_DONE };
static uint8_t  sClaude         = CL_OFF;
static char     sClaudeTool[16] = "";
static uint32_t sClaudeStart    = 0;      // 这一轮开始的时刻（提交提示词时）
static uint32_t sClaudeEnd      = 0;      // 这一轮结束的时刻（done）
static uint32_t sClaudeAt       = 0;      // 最近一次收到状态的时刻
static uint32_t sClaudeWaitAt   = 0;      // 进入 wait 的时刻
#define CLAUDE_WAIT_GUARD_MS 1500         // 进入 wait 后这段时间内迟到的 tool/work 忽略
#define CLAUDE_PAGE          3            // 屏幕第 4 行
#define CLAUDE_DONE_HIDE_MS  600000UL     // "done" 显示 10 分钟后回到 "ready"

// 屏幕状态
static bool       sScreenOn      = true;
static bool       sScreenDim     = false;
static uint32_t   sLastActivity  = 0;

// 显示脏标记：只有可见内容变化时才重画 + 刷屏
static bool       sDispDirty     = true;

// 屏幕冻结：为 true 时不重画、不刷屏，让串口诊断画的测试图案能留在屏上观察。
// 用串口命令 g 反套路... 实际上用 'F' 开关（见 SystemHandleSerial）。
static bool       sDispFrozen    = false;

// 电池电压显示成"百分之一伏"的整数，例如 3.87V -> 387。
//   -1 = 没装电池（不显示电压），0 = 装了但读不到（分压点 < 2V，显示 -.--V）
static int batteryCentiVolts() {
    if (!BatteryIsPresent()) return -1;
    return (int)(BatteryVoltage() * 100.0f + 0.5f);
}

// 顶部状态行：电池图标 + 电压（紧贴图标右侧）+ 蓝牙图标。
// 正常界面、图标测试、串口 d 诊断都画同一行，保证位置完全一致。
static void drawStatusRow() {
    DisplayIconBattery(ICON_BAT_X, ICON_ROW_Y, BatteryIsPresent() ? (int)BatteryPercent() : -1);

    int cv = batteryCentiVolts();
    if (cv >= 0) {
        char v[8];
        if (cv == 0) snprintf(v, sizeof(v), "-.--V");
        else         snprintf(v, sizeof(v), "%d.%02dV", cv / 100, cv % 100);
        DisplayText(BAT_VOLT_X, ICON_ROW_Y / 8, v);
    }

    BtIconState bt = BT_ICON_OFF;
    if (sLinkState == LED_LINK_CONNECTED)        bt = BT_ICON_CONNECTED;
    else if (sLinkState == LED_LINK_ADVERTISING) bt = BT_ICON_PAIRING;
    DisplayIconBluetooth(ICON_BT_X, ICON_ROW_Y, bt);
}

// 屏幕最下面一行：Claude 状态 + 右侧计时（这一轮已经跑了多久 / 一共用了多久）
static void drawClaudeLine() {
    if (sClaude == CL_OFF) return;
    uint32_t now = millis();
    char label[24], tbuf[10] = "";
    bool showTime = true;
    switch (sClaude) {
        case CL_IDLE: snprintf(label, sizeof(label), "Claude ready");    showTime = false; break;
        case CL_WORK: snprintf(label, sizeof(label), "Claude thinking"); break;
        case CL_TOOL: snprintf(label, sizeof(label), "Claude %s", sClaudeTool); break;
        case CL_WAIT:
            // 闪烁：一眼能看到"该你了"
            if (((now - sClaudeAt) / 500) % 2) return;
            snprintf(label, sizeof(label), "Claude NEED YOU");
            break;
        case CL_DONE: snprintf(label, sizeof(label), "Claude done");     break;
        default: return;
    }
    if (showTime) {
        uint32_t s = ((sClaude == CL_DONE ? sClaudeEnd : now) - sClaudeStart) / 1000;
        if (s >= 3600) snprintf(tbuf, sizeof(tbuf), "%lu:%02lu:%02lu", (unsigned long)(s / 3600),
                                (unsigned long)(s / 60 % 60), (unsigned long)(s % 60));
        else           snprintf(tbuf, sizeof(tbuf), "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
    }
    // 一行 21 个字符：时间靠右，标签放不下就截断
    const size_t cols = OLED_WIDTH / 6;
    size_t tlen = strlen(tbuf);
    size_t maxLabel = tlen ? cols - tlen - 1 : cols;
    if (strlen(label) > maxLabel) label[maxLabel] = 0;
    DisplayText(0, CLAUDE_PAGE, label);
    if (tlen) DisplayText((uint8_t)(OLED_WIDTH - tlen * 6), CLAUDE_PAGE, tbuf);
}

// U4（GPIO6）：短按切灯效，长按切模式
static bool       sU4Down        = false;
static uint32_t   sU4DownAt      = 0;
static bool       sU4LongDone    = false;   // 这次按住已经触发过长按，松开时不再算短按

static bool       sK3ShortCut    = false;   // 配置模式里 K3 短按确认
static uint8_t    sNextSong      = 0;       // 配置模式里 K5 下一次放哪首

// 预设切换界面：进来时的预设号。不按 K3 确认就退出时恢复成它。
static uint8_t    sPresetBeforeConfig = 0;
static bool       sConfigConfirmed    = false;

// 滚动显示的相位
static uint8_t    sScrollPreset  = 0;
static uint32_t   sLastScroll    = 0;

// 正常模式下的"按键弹出提示"：按 K1~K5 时临时显示是哪个键、发什么，1.5 秒后自动消失。
// 这样平时屏幕只留连接状态和电量，不会一直被滚动文字占着。
#define KEY_POPUP_MS   1500
static int8_t     sKeyPopupIdx   = -1;      // -1 = 不显示
static uint32_t   sKeyPopupAt    = 0;


// ---------------------------------------------------------------- 工具
static const char* modeName(SystemMode m) {
    switch (m) {
        case MODE_NORMAL:     return "NORMAL";
        case MODE_KEY_CONFIG: return "CONFIG";
        default:              return "?";
    }
}

static void notifyModeToLed() {
    LED_EffectsSetMode((uint8_t)sMode);
}

// U4 长按：正常 <-> 预设切换
static SystemMode nextMode(SystemMode m) {
    return (m == MODE_NORMAL) ? MODE_KEY_CONFIG : MODE_NORMAL;
}

// ---------------------------------------------------------------- 初始化
void SystemInit() {
    sLastActivity = millis();
    sScreenOn     = true;
    sScreenDim    = false;
    sMode         = MODE_NORMAL;
    sKeyEnabled   = true;
    notifyModeToLed();

    BuzzerSetLevel(SettingsSoundLevel());

    sU4Down     = LED_EffectSwitchClosed();
    sU4LongDone = true;

    usbLog::printf("[sys] boot: mode=%s, preset=%u, battery=%u%%\n",
                   modeName(sMode), (unsigned)(SettingsPresetIndex() + 1),
                   (unsigned)BatteryPercent());
}

void SystemSetLink(LedLinkState st) {
    if (st == sLinkState) return;
    sLinkState = st;
    sDispDirty = true;
}


void SystemNoteActivity() {
    sLastActivity = millis();
    if (!sScreenOn) {
        DisplaySetPower(true);
        sScreenOn  = true;
        sDispDirty = true;      // 关屏期间可能有变化没刷上去，点亮后重画一次
    }
    if (sScreenDim) {
        DisplaySetDim(false);
        sScreenDim = false;
    }
}

// ---------------------------------------------------------------- 按键事件
// 按键语义：
//   · K1~K5 都是按下即生效。模式切换放在 U4 长按上，5 个键没有长按功能，
//     所以不需要等松开去判断"是不是长按"，5 个键响应速度一致。
//   · 连发（repeat）只用于预设翻页（K1/K2）。
//     正常模式下一律不连发（这是快捷键盘，按住不应该刷屏）；确认（K3）也不连发。
static void onButton(ButtonId id, bool pressed, bool longPress, bool repeat) {
    if (!pressed || longPress) return;      // 只关心按下和连发；抬起、长按都不用

    SystemNoteActivity();

    // 预设切换界面里，按键都会改变屏幕内容 -> 立刻重画
    if (sMode != MODE_NORMAL) sDispDirty = true;

    switch (sMode) {
        case MODE_NORMAL:
            if (sKeyEnabled && !repeat) SystemSendKeyPress((uint8_t)id);
            break;

        case MODE_KEY_CONFIG:
            switch (id) {
                case BTN_K1:
                case BTN_K2: {
                    uint8_t p = SettingsPresetIndex();
                    if (id == BTN_K1) p = (p == 0) ? (PRESET_COUNT - 1) : (uint8_t)(p - 1);
                    else              p = (uint8_t)((p + 1) % PRESET_COUNT);
                    SettingsSetPresetIndex(p, false);   // 只改内存，K3 确认才写 flash
                    sScrollPreset = 0;
                    sLastScroll   = millis();           // 新预设从 K1/K2 说明开始，完整停留 2 秒
                    BuzzerClick();
                    break;
                }
                case BTN_K3:
                    if (!repeat) sK3ShortCut = true;    // 确认，在 SystemUpdate 里落地
                    break;
                case BTN_K4: {
                    // 声音档位：全开 -> 关按键音 -> 静音 -> 全开，立即保存
                    if (repeat) break;
                    uint8_t lv = (uint8_t)((BuzzerLevel() + 1) % SOUND_LEVEL_COUNT);
                    BuzzerSetLevel(lv);
                    SettingsSetSoundLevel(lv);
                    if (lv == SOUND_ON) BuzzerClick();      // 让人听到"按键音回来了"
                    else                BuzzerConfirm();    // QUIET 档响一下提示；OFF 档本来就不出声
                    usbLog::printf("[buz] sound level -> %s\n", BuzzerLevelName(lv));
                    break;
                }
                case BTN_K5:
                    // 彩蛋：轮流放几首曲子；正在放就停
                    if (repeat) break;
                    if (BuzzerSongPlaying()) {
                        BuzzerStop();
                    } else {
                        BuzzerPlaySong(sNextSong);
                        sNextSong = (uint8_t)((sNextSong + 1) % BuzzerSongCount());
                    }
                    break;
                default:
                    break;
            }
            break;
    }
}

// ---------------------------------------------------------------- 模式切换
static void enterMode(SystemMode m) {
    if (m == sMode) return;

    SystemMode prev = sMode;
    sMode = m;

    // 离开正常模式必须把所有键释放掉，否则会有一个键永远卡在按下状态
    if (m != MODE_NORMAL) {
        SystemSendRelease();
    }

    // ---- 预设切换：进来时记住原预设；没按 K3 确认就退出 -> 恢复原预设 ----
    //   翻页（K1/K2）只改内存，不写 flash；K3 确认时才保存。
    if (m == MODE_KEY_CONFIG) {
        sPresetBeforeConfig = SettingsPresetIndex();
        sConfigConfirmed    = false;
    } else if (prev == MODE_KEY_CONFIG) {
        if (sConfigConfirmed) {
            SettingsSetPresetIndex(SettingsPresetIndex(), true);
        } else if (SettingsPresetIndex() != sPresetBeforeConfig) {
            SettingsSetPresetIndex(sPresetBeforeConfig, false);
            usbLog::print("[cfg] 未确认就退出 -> 恢复原预设\n");
        }
    }

    sScrollPreset = 0;

    notifyModeToLed();

    if (m == MODE_KEY_CONFIG)       BuzzerModeEnter();
    else if (sConfigConfirmed)      BuzzerConfirm();      // "叮咚"：已保存
    else                            BuzzerModeExit();

    usbLog::printf("[sys] mode -> %s (keys %s)\n", modeName(sMode), (m == MODE_NORMAL) ? "enabled" : "disabled");
}

void SystemUpdate() {
    uint32_t now = millis();

    // ---- U4（GPIO6）：短按切灯效，长按 1 秒切模式 ----
    //   按下那一刻还不知道是短按还是长按，所以短按在**松开**时才切灯效；
    //   按满 1 秒立刻切模式（会响一声），之后松开不再算短按。
    //   消抖在灯效模块里做，这里读的是消抖后的稳定状态。
#if LED_EFFECT_SWITCH_PRESENT
    {
        bool closed = LED_EffectSwitchClosed();
        if (closed && !sU4Down) {
            sU4Down     = true;
            sU4DownAt   = now;
            sU4LongDone = false;
            SystemNoteActivity();
        } else if (closed && !sU4LongDone && (now - sU4DownAt) >= MODE_HOLD_MS) {
            sU4LongDone = true;
            enterMode(nextMode(sMode));
            sDispDirty = true;
        } else if (!closed && sU4Down) {
            sU4Down = false;
            if (!sU4LongDone) LED_EffectsNext();
        }
    }
#endif

    // ---- 配置模式里 K3 短按 = 确认 ----
    if (sK3ShortCut) {
        sK3ShortCut = false;
        if (sMode == MODE_KEY_CONFIG) {
            usbLog::printf("[cfg] confirmed preset '%s'\n", SettingsCurrentPreset().name);
            sConfigConfirmed = true;            // enterMode() 离开配置模式时据此写 flash
            enterMode(MODE_NORMAL);
        }
    }

    // ---- 按键是否发往 BLE ----
    sKeyEnabled = (sMode == MODE_NORMAL);

    // ---- 息屏 / 变暗 ----
    uint32_t idle = now - sLastActivity;
    if (sScreenOn && !sScreenDim && idle > SCREEN_DIM_AFTER_MS) {
        DisplaySetDim(true);
        sScreenDim = true;
    }
    if (sScreenOn && idle > SCREEN_OFF_AFTER_MS) {
        DisplaySetPower(false);
        sScreenOn  = false;
        sScreenDim = false;
    }
    // 低电时强制变暗（省电），但不断屏
    if (sScreenOn && !sScreenDim && BatteryIsLow()) {
        DisplaySetDim(true);
        sScreenDim = true;
    }

    // ---- 按键弹出提示的到期清理 ----
    if (sKeyPopupIdx >= 0 && (now - sKeyPopupAt) > KEY_POPUP_MS) {
        sKeyPopupIdx = -1;
        sDispDirty   = true;
    }

    // ---- 滚动相位：只有预设配置模式需要滚动，正常模式不再滚 ----
    if (now - sLastScroll > 2000) {
        sLastScroll = now;
        if (sMode == MODE_KEY_CONFIG) {
            sScrollPreset = (uint8_t)((sScrollPreset + 1) % KEY_COUNT);
            sDispDirty = true;
        }
    }

    // ---- 低电提示交给灯效层 ----
    LED_EffectsSetLowBattery(BatteryIsLow());

    // ---- 只在"屏幕上真的会看到变化"时才标脏 ----
    // 正常模式平时只有两个图标，内容基本不变，所以不能无条件刷屏：
    //   · 电量档位或显示的电压数字变了（或 没装电池<->装了电池 切换了）
    //     电池每 10 秒才采样一次，所以电压最多 10 秒刷一次屏
    //   · 按键弹出提示的到期由上面的弹窗逻辑负责标脏
    static int      lastPercent = -2;
    static int      lastCentiV  = -2;
    static uint8_t  lastLink    = 255;

    int     nowPercent = BatteryIsPresent() ? (int)BatteryPercent() : -1;
    int     nowCentiV  = batteryCentiVolts();
    uint8_t nowLink    = (uint8_t)sLinkState;

    if (sClaude == CL_DONE && (now - sClaudeEnd) > CLAUDE_DONE_HIDE_MS) {
        sClaude    = CL_IDLE;
        sDispDirty = true;
    }
    if (sMode == MODE_NORMAL && sClaude >= CL_WORK) {
        static uint32_t lastClaudeTick = 0;
        if (now - lastClaudeTick >= 500) { lastClaudeTick = now; sDispDirty = true; }
    }

    if (sMode == MODE_NORMAL) {
        if (nowPercent != lastPercent || nowCentiV != lastCentiV || nowLink != lastLink) {
            lastPercent = nowPercent;
            lastCentiV  = nowCentiV;
            lastLink    = nowLink;
            sDispDirty  = true;
        }
    }
}

// ---------------------------------------------------------------- 发送
// 按键报告是"按下后隔一小段再释放"，所以这里只记账，由 SystemTick 负责释放。
// 释放延时取值：BLE HID 连接间隔一般 7.5~30ms，留 60ms 保证每次按下都能被主机收到。
#define KEY_HOLD_MS   60

static uint8_t  sPendingIdx     = 0xFF;   // 当前待发/已发的键号
static uint32_t sPendingRelease = 0;      // 该释放的时刻，0 = 无
static bool     sHolding        = false;
static bool     sReleaseNeeded  = false;  // 有一条"全键释放"报文等着发

void SystemSendKeyPress(uint8_t keyIndex) {
    if (keyIndex >= KEY_COUNT) return;

    LED_EffectsKeyFeedback(keyIndex);
    BuzzerClick();

    // 在屏幕上弹一下"是哪个键、发什么"，1.5 秒后自动消失
    sKeyPopupIdx = (int8_t)keyIndex;
    sKeyPopupAt  = millis();
    sDispDirty   = true;

    const uint8_t *rep = SettingsKeyReport(keyIndex);

    // 空报文（N/A 的键）不发，但保留灯和音反馈
    bool isEmpty = true;
    for (uint8_t i = 0; i < HID_REPORT_LEN; i++) {
        if (rep[i] != 0) { isEmpty = false; break; }
    }
    if (isEmpty) return;

    // 上一个键还没释放就先放掉，避免两个键叠在一起
    if (sHolding && sPendingIdx != 0xFF) {
        sReleaseNeeded  = true;
        sPendingIdx     = 0xFF;
        sHolding        = false;
        sPendingRelease = millis();          // 让 SystemTick 立刻收尾
    }

    sPendingIdx     = keyIndex;
    sPendingRelease = millis() + KEY_HOLD_MS;
    sHolding        = true;
}

void SystemSendRelease() {
    sPendingIdx     = 0xFF;
    sHolding        = false;
    sReleaseNeeded  = true;
    sPendingRelease = millis();              // 立刻收尾
}

void SystemTick() {
    if (sPendingRelease == 0) return;
    if ((int32_t)(millis() - sPendingRelease) < 0) return;

    sPendingRelease = 0;
    sPendingIdx     = 0xFF;
    sHolding        = false;
    sReleaseNeeded  = true;                  // 到点了，该把键放掉
}

uint8_t SystemPeekPendingKey() {
    return sHolding ? sPendingIdx : 0xFF;
}

void SystemCompletePendingKey() {
    sPendingIdx     = 0xFF;
    sHolding        = false;
    sPendingRelease = millis() + KEY_HOLD_MS;
}

// main 取走"需要发全键释放"的请求
bool SystemTakeReleaseRequest() {
    bool r = sReleaseNeeded;
    sReleaseNeeded = false;
    return r;
}

// 供 system.cpp 内部的按键回调使用
void SystemHandleButtonEvent(ButtonId id, bool pressed, bool longPress, bool repeat) {
    onButton(id, pressed, longPress, repeat);
}

// ---------------------------------------------------------------- OLED 绘制
void SystemDrawDisplay() {
    char line[32];
    const KeyPreset &p = SettingsCurrentPreset();

    DisplayClear();

    switch (sMode) {
        // ================= 正常模式：状态行 + 按键弹出 =================
        // 第 0 行： 电池图标 + 电压（左）+ 蓝牙图标（右）
        // 其余全部留空。只有按下 K1~K5 时，才在屏幕上打出触发的键名，1.5 秒后消失。
        // 切换灯效（U4）不产生任何屏幕变化 —— 灯本身就是反馈，屏幕不用重复一遍。
        case MODE_NORMAL: {
            drawStatusRow();

            // ---- 按键弹出：只有这一种情况下屏幕才有文字 ----
            // 【关键】位置必须**固定**，不能跟着文字长度居中：
            //   之前每段文字各自居中，K1 和 K2 的说明长度不同 -> 起点 x 不同，
            //   连续按键时文字忽左忽右，看起来就像"内容一直往左跑"。
            //   现在固定左对齐在 POPUP_X，放在图标下面单独的一页（见文件头的布局说明）。
            //   （"整屏往左爬"真正的根因在 DisplayFlush 的 Wire 缓冲溢出，见 display.cpp。）
            if (sKeyPopupIdx >= 0) {
                snprintf(line, sizeof(line), "K%u: %s", (unsigned)(sKeyPopupIdx + 1),
                         p.keyDesc[sKeyPopupIdx]);
                DisplayText(POPUP_X, POPUP_PAGE, line);
            }
            drawClaudeLine();
            break;
        }

        // ================= 键位配置 =================
        case MODE_KEY_CONFIG: {
            snprintf(line, sizeof(line), "> Preset  Sound:%s", BuzzerLevelName(BuzzerLevel()));
            DisplayText(0, 0, line);
            snprintf(line, sizeof(line), "< %s >", p.name);
            DisplayText(0, 1, line);

            // 两个键的说明轮流显示
            uint8_t a = (uint8_t)(sScrollPreset % KEY_COUNT);
            uint8_t b = (uint8_t)((sScrollPreset + 1) % KEY_COUNT);
            snprintf(line, sizeof(line), "K%u:%s", (unsigned)(a + 1), p.keyDesc[a]);
            DisplayText(0, 2, line);
            snprintf(line, sizeof(line), "K%u:%s", (unsigned)(b + 1), p.keyDesc[b]);
            DisplayText(0, 3, line);
            break;
        }
    }
}

// ---------------------------------------------------------------- 脏标记
bool SystemDisplayDirty() { return sDispDirty && !sDispFrozen; }

// 内容变了才重画 + 刷屏。
// 【之前的 bug】不管 DisplayFlush() 有没有真的发出去都清脏标记：两次变化间隔
// 小于 40ms（限流窗口）时，后一次画面被限流挡掉、又被标成"已刷"，就再也不会显示。
// 现在只有真正刷出去了（或者屏不在 / 关屏，刷了也没意义）才清标记。
void SystemRefreshDisplay() {
    if (!SystemDisplayDirty()) return;
    SystemDrawDisplay();
    if (DisplayFlush() || !DisplayPresent() || !DisplayIsOn()) {
        sDispDirty = false;
    }
}

// ---------------------------------------------------------------- 串口诊断
// 单键命令，不需要回车。用来在没连蓝牙、没接电脑的时候也能把板子查清楚。
static void serialHelp() {
    usbLog::print(
        "\n=== ESP32-C3 BLE Keybrick 诊断命令 ===\n"
        " 1..5   切换到第 N 组键位预设（同时写回 flash）\n"
        " n      回正常键盘模式\n"
        " c      进键位预设切换模式\n"
        " v      切换“有没有装电池”（没装时电池图标空心，不显示假电量）\n"
        " s      打印状态（模式/预设/电量/连接）\n"
        " d      OLED 探活 + 冻结屏幕画图标和四角标记（观察是否位移）\n"
        " F      解除 / 恢复屏幕冻结\n"
        " P      自动模拟按键弹出 5 次（看文字左边缘是否固定）\n"
        " i      扫描整条 I2C 总线（排查 OLED 不亮）\n"
        " w      唤醒屏幕（重置息屏计时）\n"
        " p      打印当前预设的 5 个键的 HID 报文\n"
        " g      转储 GPIO 交换矩阵（查“这个脚到底被谁驱动”的利器）\n"
        " e      切到下一个灯效       k  查看当前灯效 + GPIO6 开关状态\n"
        " r      重新跑一次 LED 上电自检（红绿蓝白）\n"
        " x      逐颗逐通道检测（查某颗灯是不是某个颜色通道坏了）\n"
        " o / f  灯珠电源轨 开 / 关（量灯珠 VDD 用）\n"
        " @...   上位机改键协议（tools/keymap.html 使用，见 system.cpp）\n"
        " ?      本帮助\n"
        "=====================================\n");
}

static void serialDumpPreset() {
    const KeyPreset &p = SettingsCurrentPreset();
    usbLog::printf("\n[preset %u/%u] '%s'\n",
                   (unsigned)(SettingsPresetIndex() + 1), (unsigned)PRESET_COUNT, p.name);
    for (uint8_t i = 0; i < KEY_COUNT; i++) {
        const uint8_t *r = p.keymap[i];
        usbLog::printf("  K%u %-14s  %02X %02X | %02X %02X %02X %02X %02X %02X\n",
                       (unsigned)(i + 1), p.keyDesc[i],
                       r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
    }
}

// ---------------------------------------------------------------- 上位机改键协议
// 给 tools/keymap.html（浏览器 Web Serial）用的行协议。
// 以 '@' 开头、以换行结尾的一行是一条命令；不以 '@' 开头的字符仍按单键诊断命令处理。
// 回复也以 '@' 开头，网页据此把它和普通日志区分开。
//
//   @HELLO                      -> @HELLO KEYBRICK <协议版本> <预设数> <键数> <当前预设号>
//   @GET                        -> 每组一行 @P ...，最后 @END
//   @SET <i> <media> <keys> <name> <d1> <d2> <d3> <d4> <d5>
//                               -> @OK SET <i>   （只改内存，立即生效）
//   @SAVE                       -> @OK SAVE      （整张表写进 flash）
//   @RESET                      -> @OK RESET     （恢复出厂预设）
//   @USE <i>                    -> @OK USE <i>   （切到第 i 组并保存）
//   @CLAUDE <state> [tool]      -> @OK CLAUDE    （Claude Code 运行状态，tools/claude_status.py 发送）
//                                  state = off / idle / work / tool / wait / done
//   出错                        -> @ERR <原因>
//
//   @P <i> <media> <keys> <name> <d1> ... <d5>
//     i     : 预设号 0..PRESET_COUNT-1
//     media : 0 = 键盘报文，1 = 媒体键报文
//     keys  : 5 个键 x 8 字节 = 80 个十六进制字符
//     name / d1..d5 : 字符串的十六进制编码（避免空格、转义问题）；空字符串写 "-"
#define HOST_PROTO_VERSION  1
#define HOST_LINE_CAP       320

static char     sHostLine[HOST_LINE_CAP];
static uint16_t sHostLen   = 0;
static bool     sHostInLine = false;
static bool     sHostOverflow = false;

static const char HEXDIG[] = "0123456789ABCDEF";

static void hostPrintHex(const uint8_t *data, size_t n) {
    char buf[64];
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        buf[k++] = HEXDIG[data[i] >> 4];
        buf[k++] = HEXDIG[data[i] & 0x0F];
        if (k >= sizeof(buf) - 2) { usbLog::write(buf, k); k = 0; }
    }
    if (k) usbLog::write(buf, k);
}

static void hostPrintStr(const char *s) {
    usbLog::print(" ");
    if (s[0] == 0) { usbLog::print("-"); return; }
    hostPrintHex((const uint8_t *)s, strlen(s));
}

static void hostSendPreset(uint8_t i) {
    const KeyPreset &p = SettingsPreset(i);
    usbLog::printf("@P %u %u ", (unsigned)i, p.isMedia ? 1u : 0u);
    hostPrintHex(&p.keymap[0][0], sizeof(p.keymap));
    hostPrintStr(p.name);
    for (uint8_t k = 0; k < KEY_COUNT; k++) hostPrintStr(p.keyDesc[k]);
    usbLog::print("\n");
}

static int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// 十六进制 -> 字节。返回解出的字节数，格式错误返回 -1
static int hexDecode(const char *hex, uint8_t *out, size_t cap) {
    size_t n = strlen(hex);
    if (n % 2 != 0 || n / 2 > cap) return -1;
    for (size_t i = 0; i < n / 2; i++) {
        int hi = hexVal(hex[2 * i]), lo = hexVal(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return (int)(n / 2);
}

// 十六进制字符串 -> C 字符串（"-" = 空串）。cap 含结尾 0
static bool hexToText(const char *hex, char *out, size_t cap) {
    if (strcmp(hex, "-") == 0) { out[0] = 0; return true; }
    int n = hexDecode(hex, (uint8_t *)out, cap - 1);
    if (n < 0) return false;
    out[n] = 0;
    return true;
}

// 收到 "@CLAUDE <state> [tool]" 后更新状态
static bool claudeSet(const char *state, const char *arg) {
    uint32_t now  = millis();
    uint8_t  prev = sClaude;
    bool busy     = (prev == CL_WORK || prev == CL_TOOL || prev == CL_WAIT);

    // hooks 在电脑上是后台并发跑的，相邻很近的两个事件可能乱序到达。
    // 典型情况："要调用工具"（tool）后面紧跟着"请求权限"（wait），tool 晚到会把
    // "NEED YOU" 盖掉。所以刚进入 wait 的 1.5 秒内，迟到的 tool / work 一律忽略。
    if (prev == CL_WAIT && (now - sClaudeWaitAt) < CLAUDE_WAIT_GUARD_MS &&
        (strcmp(state, "tool") == 0 || strcmp(state, "work") == 0)) {
        return true;
    }

    if      (strcmp(state, "off")  == 0) sClaude = CL_OFF;
    else if (strcmp(state, "idle") == 0) sClaude = CL_IDLE;
    else if (strcmp(state, "work") == 0) { if (!busy) sClaudeStart = now; sClaude = CL_WORK; }
    else if (strcmp(state, "tool") == 0) {
        if (!busy) sClaudeStart = now;
        // 工具名只保留屏幕能显示的 ASCII
        size_t i = 0;
        for (; arg[i] && i < sizeof(sClaudeTool) - 1; i++) {
            char c = arg[i];
            sClaudeTool[i] = (c >= 0x20 && c <= 0x7E) ? c : '?';
        }
        sClaudeTool[i] = 0;
        sClaude = CL_TOOL;
    }
    else if (strcmp(state, "wait") == 0) {
        if (!busy) sClaudeStart = now;
        if (prev != CL_WAIT) sClaudeWaitAt = now;
        sClaude = CL_WAIT;
    }
    else if (strcmp(state, "done") == 0) {
        if (!busy) sClaudeStart = now;
        sClaudeEnd = now;
        sClaude    = CL_DONE;
    }
    else return false;

    sClaudeAt = now;
    if (sClaude != prev) {
        // 灯和提示音只在状态"变成"等你 / 做完的那一刻给一次
        if (sClaude == CL_WAIT)      { LED_EffectsSetAttention(LED_ATTN_WAIT); BuzzerAttention(); }
        else if (sClaude == CL_DONE) { LED_EffectsSetAttention(LED_ATTN_DONE); BuzzerDone(); }
        else if (prev == CL_WAIT)    { LED_EffectsSetAttention(LED_ATTN_NONE); }
        if (sClaude == CL_WAIT || sClaude == CL_DONE || !busy) SystemNoteActivity();   // 点亮屏幕
    }
    sDispDirty = true;
    return true;
}

static void hostHandleLine(char *line) {
    char *save = nullptr;
    char *tok[12];
    uint8_t nt = 0;
    for (char *t = strtok_r(line, " ", &save); t && nt < 12; t = strtok_r(nullptr, " ", &save)) {
        tok[nt++] = t;
    }
    if (nt == 0) return;
    const char *cmd = tok[0];

    if (strcmp(cmd, "HELLO") == 0) {
        usbLog::printf("@HELLO KEYBRICK %d %d %d %u\n", HOST_PROTO_VERSION, PRESET_COUNT, KEY_COUNT,
                       (unsigned)SettingsPresetIndex());
    } else if (strcmp(cmd, "GET") == 0) {
        for (uint8_t i = 0; i < PRESET_COUNT; i++) hostSendPreset(i);
        usbLog::print("@END\n");
    } else if (strcmp(cmd, "SET") == 0) {
        if (nt != 10) { usbLog::print("@ERR SET 参数个数不对\n"); return; }
        int idx = atoi(tok[1]);
        if (idx < 0 || idx >= PRESET_COUNT) { usbLog::print("@ERR SET 预设号越界\n"); return; }
        KeyPreset p;
        memset(&p, 0, sizeof(p));
        p.isMedia = (atoi(tok[2]) != 0);
        if (hexDecode(tok[3], &p.keymap[0][0], sizeof(p.keymap)) != (int)sizeof(p.keymap)) {
            usbLog::print("@ERR SET 键值格式不对\n"); return;
        }
        if (!hexToText(tok[4], p.name, sizeof(p.name))) { usbLog::print("@ERR SET 预设名太长或格式不对\n"); return; }
        for (uint8_t k = 0; k < KEY_COUNT; k++) {
            if (!hexToText(tok[5 + k], p.keyDesc[k], sizeof(p.keyDesc[k]))) {
                usbLog::printf("@ERR SET K%u 说明太长或格式不对\n", (unsigned)(k + 1)); return;
            }
        }
        SettingsUpdatePreset((uint8_t)idx, p);
        sDispDirty = true;
        usbLog::printf("@OK SET %d\n", idx);
    } else if (strcmp(cmd, "SAVE") == 0) {
        usbLog::print(SettingsSavePresets() ? "@OK SAVE\n" : "@ERR SAVE 写入 flash 失败\n");
    } else if (strcmp(cmd, "RESET") == 0) {
        SettingsResetPresets();
        sDispDirty = true;
        usbLog::print("@OK RESET\n");
    } else if (strcmp(cmd, "CLAUDE") == 0) {
        if (nt < 2 || !claudeSet(tok[1], nt >= 3 ? tok[2] : "")) {
            usbLog::print("@ERR CLAUDE 状态只能是 off/idle/work/tool/wait/done\n"); return;
        }
        usbLog::print("@OK CLAUDE\n");
    } else if (strcmp(cmd, "USE") == 0) {
        int idx = (nt >= 2) ? atoi(tok[1]) : -1;
        if (idx < 0 || idx >= PRESET_COUNT) { usbLog::print("@ERR USE 预设号越界\n"); return; }
        SettingsSetPresetIndex((uint8_t)idx, true);
        sDispDirty = true;
        usbLog::printf("@OK USE %d\n", idx);
    } else {
        usbLog::printf("@ERR 未知命令 %s\n", cmd);
    }
}

// 把一个字符喂给行协议。返回 true 表示这个字符被行协议吃掉了（不要再当单键命令）
static bool hostFeed(int c) {
    if (!sHostInLine) {
        if (c != '@') return false;
        sHostInLine   = true;
        sHostLen      = 0;
        sHostOverflow = false;
        return true;
    }
    if (c == '\r' || c == '\n') {
        sHostInLine = false;
        if (sHostOverflow) { usbLog::print("@ERR 命令太长\n"); return true; }
        sHostLine[sHostLen] = 0;
        hostHandleLine(sHostLine);
        return true;
    }
    if (sHostLen < HOST_LINE_CAP - 1) sHostLine[sHostLen++] = (char)c;
    else                              sHostOverflow = true;
    return true;
}

// 单键诊断命令
static void serialCommand(char c) {
        switch (c) {
            case '1': case '2': case '3': case '4': case '5': {
                uint8_t idx = (uint8_t)((char)c - '1');
                if (idx < PRESET_COUNT) {
                    SettingsSetPresetIndex(idx);
                    BuzzerConfirm();
                }
                break;
            }
            case 'n': enterMode(MODE_NORMAL);     break;
            case 'c': enterMode(MODE_KEY_CONFIG); break;

            // 切换"有没有装电池"。装上/拆下电池后不用重新烧录。
            case 'v':
                BatterySetPresent(!BatteryIsPresent());
                sDispDirty = true;
                break;

            case 's':
                usbLog::printf("\n[stat] mode=%s keys=%s preset=%u/%u '%s'\n",
                               modeName(sMode), sKeyEnabled ? "on" : "off",
                               (unsigned)(SettingsPresetIndex() + 1), (unsigned)PRESET_COUNT,
                               SettingsCurrentPreset().name);
                usbLog::printf("       bat=%.2fV %u%% low=%d battery_present=%d\n",
                               (double)BatteryVoltage(), (unsigned)BatteryPercent(),
                               BatteryIsLow() ? 1 : 0, BatteryIsPresent() ? 1 : 0);
                usbLog::printf("       screen=%s dim=%s sound=%s\n",
                               sScreenOn ? "on" : "off", sScreenDim ? "yes" : "no",
                               BuzzerLevelName(BuzzerLevel()));
                usbLog::printf("       led rail pin%d readback=%d (1=off)\n",
                               LED_POWER_PIN, LED_PowerRailReadback() ? 1 : 0);
                break;

            case 'p': serialDumpPreset(); break;

            case 'g':
                usbLog::print("\n[gpio] out_sel=128 才是普通 GPIO 输出；"
                              "6=UART0_TXD；51=RMT_OUT0\n");
                {
                    const uint8_t pins[] = {
                        LED_DATA_PIN, LED_POWER_PIN,
                        BTN1_PIN, BTN2_PIN, BTN3_PIN, BTN4_PIN, BTN5_PIN,
                        BUZZER_PIN, OLED_SDA_PIN, OLED_SCL_PIN, BATTERY_ADC_PIN
                    };
                    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
                        uint8_t p = pins[i];
                        usbLog::printf("  pin%-2u out_sel=%-4u inv=%u oen_sel=%-2u | in=%u out=%u\n",
                                       (unsigned)p,
                                       (unsigned)GPIO.func_out_sel_cfg[p].func_sel,
                                       (unsigned)GPIO.func_out_sel_cfg[p].inv_sel,
                                       (unsigned)GPIO.func_out_sel_cfg[p].oen_sel,
                                       (unsigned)((GPIO.in.val  >> p) & 1u),
                                       (unsigned)((GPIO.out.val >> p) & 1u));
                    }
#if LED_EFFECT_SWITCH_PRESENT
                    usbLog::printf("  pin%-2u (灯效开关 U4) in=%u -> %s\n",
                                   LED_EFFECT_SWITCH_PIN,
                                   (unsigned)((GPIO.in.val >> LED_EFFECT_SWITCH_PIN) & 1u),
                                   LED_EffectSwitchClosed() ? "closed" : "open");
#endif
                }
                break;

            // 灯效相关
            case 'e': LED_EffectsNext(); sDispDirty = true; break;
            case 'k':
                usbLog::printf("\n[led] effect=%u/%u %s\n",
                               (unsigned)(LED_EffectsGet() + 1),
                               (unsigned)LED_EffectsCount(),
                               LED_EffectsName(LED_EffectsGet()));
#if LED_EFFECT_SWITCH_PRESENT
                usbLog::printf("      switch U4 on GPIO%d: raw=%d closed=%d toggles=%u\n",
                               LED_EFFECT_SWITCH_PIN,
                               (int)digitalRead(LED_EFFECT_SWITCH_PIN),
                               LED_EffectSwitchClosed() ? 1 : 0,
                               (unsigned)LED_EffectSwitchToggleCount());
                usbLog::print("      （每按一下 U4，toggles 应该 +1；短按切灯效，长按 1 秒切模式）\n");
#endif
                break;

            case 'r':
                usbLog::print("[led] rerun selftest\n");
                LED_EffectsSelfTest();
                break;

            // 逐颗逐通道检测：判断"某颗灯颜色不对"是代码还是那颗灯坏了
            case 'x':
                usbLog::print("[diag] 逐颗逐通道扫描，看每颗灯能否单独出 红 / 绿 / 蓝 / 白\n");
                LED_DiagChannelSweep();
                sDispDirty = true;
                break;

            case 'd': {
                bool ack = DisplayPing();
                usbLog::printf("\n[oled] addr=0x%02X SDA=GPIO%d SCL=GPIO%d ACK=%d\n",
                               OLED_I2C_ADDR, OLED_SDA_PIN, OLED_SCL_PIN, ack ? 1 : 0);
                usbLog::printf("       present=%d power=%s dim=%s dirty=%d\n",
                               DisplayPresent() ? 1 : 0,
                               DisplayIsOn()  ? "on" : "off",
                               DisplayIsDim() ? "yes" : "no",
                               sDispDirty ? 1 : 0);
                if (ack) {
                    // 冻结屏幕后画：两个图标 + 四个角的标记点。
                    // 角落的点用来判断"画面有没有整体位移"——它们的位置是固定的。
                    sDispFrozen = true;
                    DisplaySetPower(true);
                    drawStatusRow();
                    // 四个角的定位标记（2x2 实心）
                    for (uint8_t dx = 0; dx < 2; dx++) {
                        for (uint8_t dy = 0; dy < 2; dy++) {
                            DisplayPixel((uint8_t)(0 + dx),   (uint8_t)(0 + dy));
                            DisplayPixel((uint8_t)(126 + dx), (uint8_t)(0 + dy));
                            DisplayPixel((uint8_t)(0 + dx),   (uint8_t)(30 + dy));
                            DisplayPixel((uint8_t)(126 + dx), (uint8_t)(30 + dy));
                        }
                    }
                    DisplayFlush(true);
                    usbLog::print("       screen FROZEN: 2 icons + 4 corner markers\n");
                    usbLog::print("       角落的点应固定不动；按 F 解除冻结\n");
                } else {
                    usbLog::print("       no ACK: 检查排针 3V3/GND/SDA/SCL 和地址\n");
                }
                break;
            }

            // 解除 / 恢复屏幕冻结
            case 'F':
                sDispFrozen = !sDispFrozen;
                sDispDirty  = true;
                usbLog::printf("[oled] frozen=%d\n", sDispFrozen ? 1 : 0);
                break;

            // 自动模拟按键弹出：每 1.5 秒换一个键，观察文字位置是否稳定。
            // 关键看：5 段文字的左边缘应该**在同一条竖线上**，不该忽左忽右。
            case 'P':
                sDispFrozen = false;
                usbLog::print("[oled] 自动模拟按键弹出 5 次（每 1.5s 一个），观察左边缘是否固定\n");
                for (uint8_t k = 0; k < KEY_COUNT; k++) {
                    sKeyPopupIdx = (int8_t)k;
                    sKeyPopupAt  = millis();
                    sDispDirty   = true;
                    // 等这个弹出显示满 1.5 秒
                    uint32_t t0 = millis();
                    while (millis() - t0 < KEY_POPUP_MS + 100) {
                        SystemRefreshDisplay();
                        delay(20);
                    }
                }
                sKeyPopupIdx = -1;
                sDispDirty   = true;
                usbLog::print("[oled] 模拟结束\n");
                break;

            case 'w':
                SystemNoteActivity();
                usbLog::print("[oled] wake\n");
                break;

            case 'i':
                DisplayScanBus();
                break;

            case 'o': LED_PowerRailOn();  usbLog::print("[led] rail ON\n");  break;
            case 'f': LED_PowerRailOff(); usbLog::print("[led] rail OFF\n"); break;

            case '?': case 'h': serialHelp(); break;

            default:
                // 忽略未知字符
                break;
        }
}

void SystemHandleSerial() {
    // 一次最多处理 64 个字符，避免主机狂发时卡在主循环里
    // （上位机一条改键命令约 300 字符，几轮主循环就收完）
    //
    // 单键诊断命令只在"这一批只收到这一个字符"时才执行：
    //   人在串口监视器里按键，字符是一个一个来的，而且不带换行；
    //   而上位机 / Claude 状态的消息如果开头的 '@' 丢了（比如串口被提前关掉、消息被截断），
    //   剩下的尾巴 "...idle\n" 会一整批到达 —— 以前里面的 'e' 会被当成"切换灯效"执行，
    //   'd' 会冻结屏幕，表现为灯效自己乱切。现在多字符的碎片、带换行的碎片一律丢弃。
    char    stray     = 0;
    uint8_t nStray    = 0;
    bool    sawNewline = false;
    for (uint8_t n = 0; n < 64; n++) {
        int c = usbLog::read();
        if (c < 0) break;
        if (hostFeed(c)) continue;          // '@' 开头的一行交给上位机协议
        if (c == '\r' || c == '\n') { sawNewline = true; continue; }
        if (nStray == 0) stray = (char)c;
        if (nStray < 255) nStray++;
    }
    if (nStray == 1 && !sawNewline) {
        serialCommand(stray);
    } else if (nStray > 1) {
        usbLog::printf("[serial] ignored %u stray chars\n", (unsigned)nStray);
    }
}
