// ============================================================================
//  system.cpp —— 模式状态机、OLED 界面、按键事件分发
//
//  模式与操作方式（沿用原工程，只把引脚和对齐改了）：
//    正常模式    ：K1~K5 发送当前预设的键值
//    K4 长按     ：进入 / 退出节拍器
//    K5 长按     ：进入定时器设置；再长按退出
//    K4+K5 同时长按：进入键位预设切换
//    节拍器/定时器/配置模式里：K1~K4 是功能键，不发送键值
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
#include "timer_metro.h"
#include "ws2812.h"
#include "usb_log.h"

#define SCREEN_DIM_AFTER_MS     20000UL
#define SCREEN_OFF_AFTER_MS     30000UL
#define COMBO_WINDOW_MS         150     // K4+K5 组合键的判定窗口

static SystemMode sMode          = MODE_NORMAL;
static bool       sKeyEnabled    = true;

// 当前 BLE 连接状态，由 main 通过 SystemSetLink() 同步过来，用于 OLED 显示
static LedLinkState sLinkState   = LED_LINK_OFF;

// 屏幕状态
static bool       sScreenOn      = true;
static bool       sScreenDim     = false;
static uint32_t   sLastActivity  = 0;

// 显示脏标记：只有可见内容变化时才重画 + 刷屏
static bool       sDispDirty     = true;

// 屏幕冻结：为 true 时不重画、不刷屏，让串口诊断画的测试图案能留在屏上观察。
// 用串口命令 g 反套路... 实际上用 'F' 开关（见 SystemHandleSerial）。
static bool       sDispFrozen    = false;

// 只显示图标的"纯净"画面：用来单独观察两个图标（按 K 弹出时不显示文字）
void SystemDrawIconsOnly() {
    DisplayClear();
    DisplayIconBattery(1, 12, BatteryIsPresent() ? (int)BatteryPercent() : -1);
    BtIconState bt = BT_ICON_OFF;
    if (sLinkState == LED_LINK_CONNECTED)        bt = BT_ICON_CONNECTED;
    else if (sLinkState == LED_LINK_ADVERTISING) bt = BT_ICON_PAIRING;
    DisplayIconBluetooth((uint8_t)(OLED_WIDTH - 13), 12, bt);
}

// K4 / K5 长按组合判定
static uint32_t   sK4LongAt      = 0;
static uint32_t   sK5LongAt      = 0;
static bool       sK4LongPending = false;
static bool       sK5LongPending = false;
static bool       sK3ShortCut    = false;   // 配置模式里 K3 短按确认

// 滚动显示的相位
static uint8_t    sScrollKey     = 0;
static uint8_t    sScrollPreset  = 0;
static uint32_t   sLastScroll    = 0;

// 正常模式下的"按键弹出提示"：按 K1~K5 时临时显示是哪个键、发什么，1.5 秒后自动消失。
// 这样平时屏幕只留连接状态和电量，不会一直被滚动文字占着。
#define KEY_POPUP_MS   1500
static int8_t     sKeyPopupIdx   = -1;      // -1 = 不显示
static uint32_t   sKeyPopupAt    = 0;

// 定时器到点提示的闪烁
static uint32_t   sTimerFiredAt  = 0;

// ---------------------------------------------------------------- 工具
static const char* modeName(SystemMode m) {
    switch (m) {
        case MODE_NORMAL:     return "NORMAL";
        case MODE_TIMER_SET:  return "TIMER";
        case MODE_METRONOME:  return "METRO";
        case MODE_KEY_CONFIG: return "CONFIG";
        default:              return "?";
    }
}

static void notifyModeToLed() {
    LED_EffectsSetMode((uint8_t)sMode);
}

// ---------------------------------------------------------------- 初始化
void SystemInit() {
    sLastActivity = millis();
    sScreenOn     = true;
    sScreenDim    = false;
    sMode         = MODE_NORMAL;
    sKeyEnabled   = true;
    notifyModeToLed();

    usbLog::printf("[sys] boot: mode=%s, preset=%u, battery=%u%%\n",
                   modeName(sMode), (unsigned)(SettingsPresetIndex() + 1),
                   (unsigned)BatteryPercent());
}

void SystemSetLink(LedLinkState st) {
    if (st == sLinkState) return;
    sLinkState = st;
    sDispDirty = true;
}

uint8_t SystemModeGet()     { return (uint8_t)sMode; }
bool    SystemKeyEnabled()  { return sKeyEnabled; }
void    SystemNextLinkState() { /* 占位：连接状态由 main 直接设给灯效层 */ }

void SystemNoteActivity() {
    sLastActivity = millis();
    if (!sScreenOn) {
        DisplaySetPower(true);
        sScreenOn = true;
    }
    if (sScreenDim) {
        DisplaySetDim(false);
        sScreenDim = false;
    }
}

// ---------------------------------------------------------------- 按键事件
// 这个函数只负责"记下事件"，真正的模式动作在 SystemUpdate() 里做，
// 避免在按键回调里嵌套调用一堆东西。
static void onButton(ButtonId id, bool pressed, bool longPress, bool repeat) {
    (void)repeat;

    if (pressed) SystemNoteActivity();

    switch (id) {
        // ---------------- K1 ----------------
        case BTN_K1:
            if (!pressed) break;
            switch (sMode) {
                case MODE_NORMAL:
                    if (!sKeyEnabled) break;
                    if (longPress) break;
                    SystemSendKeyPress(0);
                    break;
                case MODE_TIMER_SET:
                    if (!longPress) TimerAddHours(1);
                    break;
                case MODE_METRONOME:
                    if (!longPress) MetroSetBpm(-5);
                    break;
                case MODE_KEY_CONFIG:
                    if (!longPress) {
                        uint8_t p = SettingsPresetIndex();
                        p = (p == 0) ? (PRESET_COUNT - 1) : (uint8_t)(p - 1);
                        SettingsSetPresetIndex(p);
                        sScrollPreset = 0;
                        BuzzerClick();
                    }
                    break;
            }
            break;

        // ---------------- K2 ----------------
        case BTN_K2:
            if (!pressed) break;
            switch (sMode) {
                case MODE_NORMAL:
                    if (!sKeyEnabled) break;
                    if (longPress) break;
                    SystemSendKeyPress(1);
                    break;
                case MODE_TIMER_SET:
                    if (!longPress) TimerAddMinutes(1);
                    break;
                case MODE_METRONOME:
                    if (!longPress) MetroSetBpm(5);
                    break;
                case MODE_KEY_CONFIG:
                    if (!longPress) {
                        uint8_t p = (uint8_t)((SettingsPresetIndex() + 1) % PRESET_COUNT);
                        SettingsSetPresetIndex(p);
                        sScrollPreset = 0;
                        BuzzerClick();
                    }
                    break;
            }
            break;

        // ---------------- K3 ----------------
        case BTN_K3:
            if (!pressed) break;
            switch (sMode) {
                case MODE_NORMAL:
                    if (!sKeyEnabled) break;
                    if (longPress) break;
                    SystemSendKeyPress(2);
                    break;
                case MODE_TIMER_SET:
                    if (!longPress) {
                        if (gTimer.running) TimerStop(); else TimerStart();
                        BuzzerModeChange();
                    }
                    break;
                case MODE_METRONOME:
                    if (!longPress) {
                        MetroToggle();
                        BuzzerModeChange();
                    }
                    break;
                case MODE_KEY_CONFIG:
                    if (!longPress) sK3ShortCut = true;     // 确认，在 SystemUpdate 里落地
                    break;
            }
            break;

        // ---------------- K4 ----------------
        case BTN_K4:
            if (longPress && pressed) { sK4LongAt = millis(); sK4LongPending = true; break; }
            if (!pressed) break;
            switch (sMode) {
                case MODE_NORMAL:
                    if (!sKeyEnabled) break;
                    SystemSendKeyPress(3);
                    break;
                case MODE_TIMER_SET:
                    if (!longPress) {
                        TimerReset();
                        BuzzerClick();
                    }
                    break;
                case MODE_METRONOME:
                    if (!longPress) {
                        MetroCycleTimeSig();
                        BuzzerClick();
                    }
                    break;
                case MODE_KEY_CONFIG:
                    break;      // 配置模式里 K4 不做事
            }
            break;

        // ---------------- K5 ----------------
        case BTN_K5:
            if (longPress && pressed) { sK5LongAt = millis(); sK5LongPending = true; break; }
            if (!pressed) break;
            if (sMode == MODE_NORMAL) {
                if (!sKeyEnabled) break;
                SystemSendKeyPress(4);
            }
            break;

        default:
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

    // ---- 离开某个模式时，必须把它自己的活动停掉 ----
    // 【实机踩到的 bug】原来这里只改了 sMode，没管 gMetro.running / gTimer.running。
    // 而 MetroUpdate() / TimerUpdate() 只看那个 running 标志、根本不看当前模式，
    // 于是 K4 长按退出节拍器后，节拍器还在后台一直"哒哒哒"响。
    //
    // 现在两个模式的处理是**一致的：退出就完全结束**。
    //   节拍器：停止出声。
    //   定时器：停止倒数，并且把 fired 标志和时长一起复位 ——
    //           "退出"就是这次计时作废，不留残影，下次进来是干净的状态。
    //           （曾经试过让定时器退出后继续后台倒数，但那样到点了你也不知道，
    //             反而像是出了 bug，所以改成完全结束。）
    static bool sMetroWasRunning = false;

    if (prev == MODE_METRONOME && m != MODE_METRONOME) {
        sMetroWasRunning = gMetro.running;
        if (gMetro.running) {
            gMetro.running = false;
            BuzzerStop();                       // 立刻掐掉可能还在响的那一声
            usbLog::print("[met] 退出节拍器模式 -> 已停止\n");
        }
    } else if (prev != MODE_METRONOME && m == MODE_METRONOME) {
        if (sMetroWasRunning) {
            gMetro.running    = true;
            gMetro.beat       = 0;
            gMetro.nextBeatMs = millis();
            usbLog::print("[met] 回到节拍器模式 -> 已恢复运行\n");
        }
    }

    if (prev == MODE_TIMER_SET && m != MODE_TIMER_SET) {
        // "退出就完全结束"：停止 + 清 fired + 时长回到默认
        if (gTimer.running || gTimer.fired) {
            TimerReset();
            usbLog::print("[tmr] 退出定时器模式 -> 已结束并复位\n");
        }
        BuzzerStop();
    }

    sScrollPreset = 0;
    sScrollKey    = 0;

    notifyModeToLed();
    BuzzerModeChange();

    usbLog::printf("[sys] mode -> %s (keys %s)\n", modeName(sMode), (m == MODE_NORMAL) ? "enabled" : "disabled");
}

void SystemUpdate() {
    uint32_t now = millis();

    // ---- K4+K5 组合键判定：两个长按在 150ms 窗口内到达 -> 键位配置 ----
    if (sK4LongPending && sK5LongPending) {
        sK4LongPending = sK5LongPending = false;
        enterMode((sMode == MODE_KEY_CONFIG) ? MODE_NORMAL : MODE_KEY_CONFIG);
    } else if (sK4LongPending && (now - sK4LongAt) > COMBO_WINDOW_MS) {
        sK4LongPending = false;
        // 单独 K4 长按：正常模式进节拍器，其他模式回正常
        if (sMode == MODE_NORMAL)       enterMode(MODE_METRONOME);
        else if (sMode == MODE_METRONOME) enterMode(MODE_NORMAL);
    } else if (sK5LongPending && (now - sK5LongAt) > COMBO_WINDOW_MS) {
        sK5LongPending = false;
        // 单独 K5 长按：正常模式进定时器，其他模式回正常
        if (sMode == MODE_NORMAL)        enterMode(MODE_TIMER_SET);
        else if (sMode == MODE_TIMER_SET) enterMode(MODE_NORMAL);
    }

    // ---- 配置模式里 K3 短按 = 确认 ----
    if (sK3ShortCut) {
        sK3ShortCut = false;
        if (sMode == MODE_KEY_CONFIG) {
            BuzzerOk();
            usbLog::printf("[cfg] confirmed preset '%s'\n", SettingsCurrentPreset().name);
            enterMode(MODE_NORMAL);
        }
    }

    // ---- 按键是否发往 BLE ----
    sKeyEnabled = (sMode == MODE_NORMAL);

    // ---- 定时器到点提示 ----
    // 定时器只在定时器模式下运行（退出即结束），所以这里的条件实际上总是成立；
    // 保留这个判断只是防御性的，避免以后改动时在别的模式里误响。
    if (gTimer.fired && sMode == MODE_TIMER_SET) {
        gTimer.fired  = false;
        sTimerFiredAt = now;
        BuzzerBeep(2200, 500);
    }

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
    //   · 电量百分比数字变了（或 没装电池<->装了电池 切换了）
    //   · 定时器界面需要按秒刷新
    //   · 节拍器界面要跟着拍子动
    //   · 按键弹出提示的到期由上面的弹窗逻辑负责标脏
    static int      lastPercent = -2;
    static uint8_t  lastLink    = 255;

    int     nowPercent = BatteryIsPresent() ? (int)BatteryPercent() : -1;
    uint8_t nowLink    = (uint8_t)sLinkState;

    if (sMode == MODE_NORMAL) {
        if (nowPercent != lastPercent || nowLink != lastLink) {
            lastPercent = nowPercent;
            lastLink    = nowLink;
            sDispDirty  = true;
        }
    } else if (sMode == MODE_TIMER_SET || sMode == MODE_METRONOME) {
        static uint32_t lastTick = 0;
        if (now - lastTick > 250) {      // 秒级刷新就够
            lastTick   = now;
            sDispDirty = true;
        }
    }

    // ---- 节拍器 / 定时器推进 ----
    // 两个都只在各自的模式里走：双保险，即使有什么路径漏改 running 标志，
    // 退出模式后也绝不会再出声或继续计数（这就是"长按 K4 退出后还在响"的根源）。
    if (sMode == MODE_METRONOME) {
        MetroUpdate();
    }
    if (sMode == MODE_TIMER_SET) {
        TimerUpdate();
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
        // ================= 正常模式：只有两个图标 =================
        // 第 0 行： 电池图标（左）+ 蓝牙图标（右）
        // 其余全部留空。只有按下 K1~K5 时，才在屏幕上打出触发的键名，1.5 秒后消失。
        // 切换灯效（U4）不产生任何屏幕变化 —— 灯本身就是反馈，屏幕不用重复一遍。
        case MODE_NORMAL: {
            // ---- 电池图标：左上角，垂直居中 ----
            DisplayIconBattery(1, 12, BatteryIsPresent() ? (int)BatteryPercent() : -1);

            // ---- 蓝牙图标：右上角，垂直居中（8 宽 + 右侧 2px 连接点，共留 13px）----
            BtIconState bt = BT_ICON_OFF;
            if (sLinkState == LED_LINK_CONNECTED)        bt = BT_ICON_CONNECTED;
            else if (sLinkState == LED_LINK_ADVERTISING) bt = BT_ICON_PAIRING;
            DisplayIconBluetooth((uint8_t)(OLED_WIDTH - 13), 12, bt);

            // ---- 按键弹出：只有这一种情况下屏幕才有文字 ----
            // 【关键】位置必须**固定**，不能跟着文字长度居中：
            //   之前每段文字各自居中，K1 和 K2 的说明长度不同 -> 起点 x 不同，
            //   连续按键时文字忽左忽右，看起来就像"内容一直往左跑"。
            //   现在固定左对齐在 POPUP_X，并先清掉这条带子，避免残留上一段文字。
            if (sKeyPopupIdx >= 0) {
                const uint8_t POPUP_X = 26;      // 固定起点：两个图标内侧
                const uint8_t POPUP_W = 96;      // 可用的中间区域宽度
                DisplayClearRect(POPUP_X, 2, POPUP_W, 1);
                snprintf(line, sizeof(line), "K%u: %s", (unsigned)(sKeyPopupIdx + 1),
                         p.keyDesc[sKeyPopupIdx]);
                DisplayText(POPUP_X, 2, line);
            }
            break;
        }

        // ================= 定时器设置 =================
        case MODE_TIMER_SET: {
            if (gTimer.running) {
                // 剩余时间放在第一行最显眼的位置
                TimerFormatRemaining(line, sizeof(line));
                DisplayText(0, 0, "> Timer RUN");
                DisplayText(72, 0, line);
                DisplayText(0, 1, "Countdown running");
            } else {
                snprintf(line, sizeof(line), "> Timer %02u:%02u",
                         (unsigned)gTimer.hours, (unsigned)gTimer.minutes);
                DisplayText(0, 0, line);
                DisplayText(0, 1, "Ready - K3 to start");
            }
            DisplayText(0, 2, "K1|HH+  K2|MM+");
            DisplayText(0, 3, "K3|Run/Stop K4|Rst");
            break;
        }

        // ================= 节拍器 =================
        case MODE_METRONOME: {
            snprintf(line, sizeof(line), "> Metronome %s", gMetro.running ? "[RUN]" : "[OFF]");
            DisplayText(0, 0, line);

            snprintf(line, sizeof(line), "BPM:%3u  %u/4  beat:%u",
                     (unsigned)gMetro.bpm, (unsigned)gMetro.timeSig, (unsigned)(gMetro.beat + 1));
            DisplayText(0, 1, line);

            // 拍点指示条：当前拍用一个方块标出来
            for (uint8_t i = 0; i < gMetro.timeSig && i < 16; i++) {
                DisplayText((uint8_t)(0 + i * 6), 2, (i == gMetro.beat) ? "#" : ".");
            }

            DisplayText(0, 3, "K1|- K2|+ K3|On K4|Sig");
            break;
        }

        // ================= 键位配置 =================
        case MODE_KEY_CONFIG: {
            snprintf(line, sizeof(line), "> Config Preset");
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

    // 定时器到点的闪烁提示，压在最上层
    if (sTimerFiredAt != 0) {
        if (millis() - sTimerFiredAt > 5000) {
            sTimerFiredAt = 0;
        } else if (((millis() - sTimerFiredAt) / 250) & 1) {
            DisplayTextCentered(1, "** TIME UP **");
        }
    }
}

// ---------------------------------------------------------------- 脏标记
bool SystemDisplayDirty() { return sDispDirty && !sDispFrozen; }
void SystemDisplayMarkClean() { sDispDirty = false; }
bool SystemDisplayFrozen() { return sDispFrozen; }

// ---------------------------------------------------------------- 串口诊断
// 单键命令，不需要回车。用来在没连蓝牙、没接电脑的时候也能把板子查清楚。
static void serialHelp() {
    usbLog::print(
        "\n=== ESP32-C3 BLE Keybrick 诊断命令 ===\n"
        " 1..6   切换到第 N 组键位预设（同时写回 flash）\n"
        " n      回正常键盘模式\n"
        " t      进定时器设置模式    m  进节拍器模式    c  进键位配置模式\n"
        " a      切换节拍器开/关     b  节拍器 BPM +10\n"
        " u      定时器 启动/停止     q  定时器 复位\n"
        " v      切换“有没有装电池”（没装时显示 USB，不显示假电量）\n"
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

void SystemHandleSerial() {
    // 一次最多处理 8 个字符，避免主机狂发时卡在主循环里
    for (uint8_t n = 0; n < 8; n++) {
        int c = usbLog::read();
        if (c < 0) return;

        switch ((char)c) {
            case '1': case '2': case '3': case '4': case '5': case '6': {
                uint8_t idx = (uint8_t)((char)c - '1');
                if (idx < PRESET_COUNT) {
                    SettingsSetPresetIndex(idx);
                    BuzzerOk();
                }
                break;
            }
            case 'n': enterMode(MODE_NORMAL);     break;
            case 't': enterMode(MODE_TIMER_SET);  break;
            case 'm': enterMode(MODE_METRONOME);  break;
            case 'c': enterMode(MODE_KEY_CONFIG); break;

            case 'a': MetroToggle();   break;
            case 'b': MetroSetBpm(10); break;

            // 定时器：物理上是 K3 启动/停止、K4 复位，串口给一套等价命令方便调试
            case 'u':
                if (gTimer.running) TimerStop(); else TimerStart();
                usbLog::printf("[tmr] %s at %02u:%02u\n",
                               gTimer.running ? "START" : "STOP",
                               (unsigned)gTimer.hours, (unsigned)gTimer.minutes);
                sDispDirty = true;
                break;
            case 'q':
                TimerReset();
                usbLog::print("[tmr] reset\n");
                sDispDirty = true;
                break;

            // 切换"有没有装电池"。装上/拆下电池后不用重新烧录。
            // （用 v 而不是 t：t 已经是"进定时器模式"了）
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
                usbLog::printf("       screen=%s dim=%s\n",
                               sScreenOn ? "on" : "off", sScreenDim ? "yes" : "no");
                usbLog::printf("       timer %02u:%02u %s | remain=%lus | fired=%d\n",
                               (unsigned)gTimer.hours, (unsigned)gTimer.minutes,
                               gTimer.running ? "RUN" : "off",
                               (unsigned long)(TimerRemainingMs() / 1000UL),
                               gTimer.fired ? 1 : 0);
                usbLog::printf("       metro %ubpm %u/4 %s\n",
                               (unsigned)gMetro.bpm, (unsigned)gMetro.timeSig,
                               gMetro.running ? "RUN" : "off");
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
                usbLog::print("      （拨动/按下 U4 时 toggles 应该 +1，灯效同时切换）\n");
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
                    DisplayIconBattery(1, 12, BatteryIsPresent() ? (int)BatteryPercent() : -1);
                    BtIconState bt = BT_ICON_OFF;
                    if (sLinkState == LED_LINK_CONNECTED)        bt = BT_ICON_CONNECTED;
                    else if (sLinkState == LED_LINK_ADVERTISING) bt = BT_ICON_PAIRING;
                    DisplayIconBluetooth((uint8_t)(OLED_WIDTH - 13), 12, bt);
                    // 四个角的定位标记（2x2 实心）
                    for (uint8_t dx = 0; dx < 2; dx++) {
                        for (uint8_t dy = 0; dy < 2; dy++) {
                            DisplayPixel((uint8_t)(0 + dx),   (uint8_t)(0 + dy));
                            DisplayPixel((uint8_t)(126 + dx), (uint8_t)(0 + dy));
                            DisplayPixel((uint8_t)(0 + dx),   (uint8_t)(30 + dy));
                            DisplayPixel((uint8_t)(126 + dx), (uint8_t)(30 + dy));
                        }
                    }
                    DisplayFlush();
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
                        if (SystemDisplayDirty()) {
                            SystemDrawDisplay();
                            DisplayFlush();
                            SystemDisplayMarkClean();
                        }
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
                // 忽略换行和未知字符
                break;
        }
    }
}