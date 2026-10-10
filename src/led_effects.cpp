// ============================================================================
//  led_effects.cpp —— 5 颗灯珠的灯效实现
//
//  刷新策略：每 LED_REFRESH_MS 重算一帧并 rmt_write_items 发出去。
//  每次发送 120 bit 约 150us，RMT 硬件保证时序，BLE 中断打断也不影响。
//
//  灯效模式的切换开关在 GPIO6（板上 U4 SH-1806SA，另一端接 GND）：
//  每次检测到有效电平翻转就切到下一个灯效，所以点动按钮和拨动开关都能用，
//  而且接反了也照样工作（两个方向都算一次翻转）。
// ============================================================================
#include <Arduino.h>
#include "board_config.h"
#include "ws2812.h"
#include "led_effects.h"
#include "usb_log.h"

#define LED_REFRESH_MS      25      // 40Hz，人眼足够顺滑
#define SELFTEST_STEP_MS    250

// ---------------------------------------------------------------- 模块状态
static Ws2812     sStrip(LED_DATA_PIN, LED_COUNT, RMT_CHANNEL_0);
static bool       sStripOk      = false;

static LedLinkState sLink        = LED_LINK_OFF;
static uint8_t      sMode        = 0;
static bool         sLowBat      = false;
static uint8_t      sEffect      = LED_FX_STATUS;

static uint32_t     sLastRefresh = 0;
static uint32_t     sAnimPhase   = 0;      // 动画相位，每 5ms 累加
static uint32_t     sLastFade    = 0;

// 按键反馈：每颗键一个剩余亮度（255 -> 0 线性衰减）
static uint8_t      sKeyGlow[BUTTON_COUNT];

// 节拍器：拍到第几颗灯
static int8_t       sBeatFlash   = -1;

// GPIO6 开关
static uint8_t      sSwStable    = 1;      // 已消抖的闭合状态：1=闭合
static uint8_t      sSwRaw       = 1;
static uint32_t     sSwChangeMs  = 0;
static uint16_t     sSwToggles   = 0;

// 每颗灯的 RGB 工作缓冲
static uint8_t      sR[BUTTON_COUNT], sG[BUTTON_COUNT], sB[BUTTON_COUNT];

// ---------------------------------------------------------------- 工具
static inline uint8_t lerp8(uint8_t a, uint8_t b, uint8_t t) {
    return (uint8_t)((int16_t)a + (((int16_t)b - (int16_t)a) * (int16_t)t) / 255);
}

// 三角波：把相位映射成 0..255 的往返
static inline uint8_t tri(uint32_t phase, uint32_t periodMs) {
    uint32_t p    = phase % periodMs;
    uint32_t half = periodMs / 2;
    uint32_t v    = (p < half) ? (p * 255 / half) : ((periodMs - p) * 255 / half);
    return (uint8_t)(v > 255 ? 255 : v);
}

// 色轮：输入 0..255 输出 RGB（整数近似，不用浮点）
static void hueToRgb(uint8_t hue, uint8_t *r, uint8_t *g, uint8_t *b) {
    uint8_t region = (uint8_t)(hue / 43);                  // 0..5
    uint8_t rem    = (uint8_t)((hue - region * 43) * 6);   // 0..255
    uint8_t q      = (uint8_t)(255 - rem);
    switch (region) {
        case 0:  *r = 255; *g = rem; *b = 0;   break;
        case 1:  *r = q;   *g = 255; *b = 0;   break;
        case 2:  *r = 0;   *g = 255; *b = rem; break;
        case 3:  *r = 0;   *g = q;   *b = 255; break;
        case 4:  *r = rem; *g = 0;   *b = 255; break;
        default: *r = 255; *g = 0;   *b = q;   break;
    }
}

// ---------------------------------------------------------------- GPIO6 开关
bool LED_EffectSwitchClosed() { return sSwStable != 0; }
uint16_t LED_EffectSwitchToggleCount() { return sSwToggles; }

uint8_t LED_EffectsGet() { return sEffect; }
uint8_t LED_EffectsCount() { return (uint8_t)LED_FX_COUNT; }

const char* LED_EffectsName(uint8_t fx) {
    switch (fx) {
        case LED_FX_STATUS:  return "STATUS";
        case LED_FX_SOLID:   return "SOLID";
        case LED_FX_RAINBOW: return "RAINBOW";
        case LED_FX_WAVE:    return "WAVE";
        case LED_FX_CHASE:   return "CHASE";
        case LED_FX_TWINKLE: return "TWINKLE";
        default:             return "?";
    }
}

void LED_EffectsSet(uint8_t fx) {
    if (fx >= LED_FX_COUNT) return;
    sEffect    = fx;
    sAnimPhase = 0;
    usbLog::printf("[led] effect -> %u/%u %s\n",
                   (unsigned)(fx + 1), (unsigned)LED_FX_COUNT, LED_EffectsName(fx));
}

void LED_EffectsNext() {
    LED_EffectsSet((uint8_t)((sEffect + 1) % (uint8_t)LED_FX_COUNT));
}

void LED_EffectsPollSwitch() {
#if LED_EFFECT_SWITCH_PRESENT
    uint8_t v = (uint8_t)digitalRead(LED_EFFECT_SWITCH_PIN);

    // 原始电平 -> 归一化成"闭合"语义（1 = 闭合/接通）
#if LED_EFFECT_SWITCH_ACTIVE_LOW
    uint8_t closed = (v == LOW)  ? 1 : 0;
#else
    uint8_t closed = (v == HIGH) ? 1 : 0;
#endif

    uint32_t now = millis();

    if (closed != sSwRaw) {
        sSwRaw      = closed;
        sSwChangeMs = now;
        return;
    }

    if (closed != sSwStable && (now - sSwChangeMs) >= LED_EFFECT_SWITCH_DEBOUNCE_MS) {
        sSwStable = closed;
        sSwToggles++;
        LED_EffectsNext();      // 一次有效翻转 = 切一个灯效
    }
#endif
}

// ---------------------------------------------------------------- 初始化
void LED_EffectsInit() {
    // ---- GPIO6 开关 ----
#if LED_EFFECT_SWITCH_PRESENT
    pinMode(LED_EFFECT_SWITCH_PIN, INPUT_PULLUP);
    {
        uint8_t raw = (uint8_t)digitalRead(LED_EFFECT_SWITCH_PIN);
#if LED_EFFECT_SWITCH_ACTIVE_LOW
        sSwStable = sSwRaw = (raw == LOW) ? 1 : 0;
#else
        sSwStable = sSwRaw = (raw == HIGH) ? 1 : 0;
#endif
    }
    sSwChangeMs = millis();
    usbLog::printf("[led] effect switch U4 on GPIO%d, initial=%s (raw=%d)\n",
                   LED_EFFECT_SWITCH_PIN,
                   sSwStable ? "closed" : "open",
                   (int)digitalRead(LED_EFFECT_SWITCH_PIN));
#endif

    // ---- 灯珠电源轨 ----
    LED_PowerRailOn();
    delay(5);                       // 等 3V3 轨和灯珠去耦电容稳定

    sStrip.setBrightness(LED_BRIGHTNESS);
    sStripOk = sStrip.begin();

    if (sStripOk) {
        sStrip.clear();
        sStrip.show();
    }

    usbLog::printf("[led] rail on, pin%d readback=%d (1=off), rmt=%s, effect=%s\n",
                   LED_POWER_PIN, LED_PowerRailReadback() ? 1 : 0,
                   sStripOk ? "ok" : "FAIL", LED_EffectsName(sEffect));
}

void LED_EffectsSetLink(LedLinkState st) { sLink = st; }
void LED_EffectsSetMode(uint8_t mode)    { sMode = mode; sBeatFlash = -1; }
void LED_EffectsSetLowBattery(bool low)  { sLowBat = low; }

void LED_EffectsKeyFeedback(uint8_t keyIndex) {
    if (keyIndex < BUTTON_COUNT) sKeyGlow[keyIndex] = 255;
}

void LED_EffectsBeat() {
    static int8_t beat = 0;
    beat = (int8_t)((beat + 1) % BUTTON_COUNT);
    sBeatFlash = beat;
}

// ---------------------------------------------------------------- 上电自检
void LED_EffectsSelfTest() {
    if (!sStripOk) {
        usbLog::print("[led] selftest skipped: RMT not ready\n");
        return;
    }
    // 顺序点亮红/绿/蓝/白，一眼就能看出：能不能亮、颜色对不对、是不是 5 颗
    const uint8_t seq[4][3] = { {255,0,0}, {0,255,0}, {0,0,255}, {200,200,200} };
    const char  *name[4]    = { "RED", "GREEN", "BLUE", "WHITE" };

    for (uint8_t s = 0; s < 4; s++) {
        sStrip.setAll(seq[s][0], seq[s][1], seq[s][2]);
        esp_err_t e = sStrip.show();
        usbLog::printf("[led] selftest %-5s show=%d\n", name[s], (int)e);
        delay(SELFTEST_STEP_MS);
    }
    sStrip.clear();
    sStrip.show();
    delay(80);
}

// 逐颗逐通道检测：判断"某颗灯颜色不对"是代码问题还是那颗灯的通道坏了。
// 每颗灯依次只点亮 R、G、B 一个通道 —— 正常的话每颗灯都会看到 红/绿/蓝 三次纯色。
// 如果某颗灯点"蓝"时是暗的，那它蓝通道就是坏的（典型表现：本该白色的地方发黄）。
void LED_DiagChannelSweep() {
    if (!sStripOk) {
        usbLog::print("[diag] RMT not ready\n");
        return;
    }
    const char *cname[3] = { "R", "G", "B" };
    const uint8_t ch[3][3] = { {255,0,0}, {0,255,0}, {0,0,255} };

    for (uint16_t i = 0; i < LED_COUNT; i++) {
        for (uint8_t c = 0; c < 3; c++) {
            sStrip.clear();
            sStrip.setPixel(i, ch[c][0], ch[c][1], ch[c][2]);
            sStrip.show();
            usbLog::printf("[diag] LED%u %s\n", (unsigned)(i + 1), cname[c]);
            delay(400);
        }
        // 三通道全开 = 白，用来对照"是不是真的缺一个通道"
        sStrip.clear();
        sStrip.setPixel(i, 255, 255, 255);
        sStrip.show();
        usbLog::printf("[diag] LED%u WHITE (缺通道的话这里会偏色)\n", (unsigned)(i + 1));
        delay(500);
    }
    sStrip.clear();
    sStrip.show();
    usbLog::print("[diag] sweep done\n");
}

void LED_DiagSingle(uint8_t idx, uint8_t channel) {
    if (!sStripOk || idx >= LED_COUNT) return;
    sStrip.clear();
    switch (channel) {
        case 0: sStrip.setPixel(idx, 255, 0, 0);   break;
        case 1: sStrip.setPixel(idx, 0, 255, 0);   break;
        default: sStrip.setPixel(idx, 0, 0, 255);  break;
    }
    sStrip.show();
}

// ---------------------------------------------------------------- 灯效动画
// 每个动画只负责往 sR/sG/sB 里填色，后面的叠加逻辑统一在 composeFrame() 做
static void renderEffect() {
    for (uint8_t i = 0; i < BUTTON_COUNT; i++) { sR[i] = sG[i] = sB[i] = 0; }

    switch (sEffect) {
        // ---- 1) 状态指示：整条灯做暗绿呼吸（不看连接状态，自己就会动）----
        //   第一版只在"已连接"时才呼吸，结果没配对时整条灯是纯黑，
        //   看起来像"第一档没效果"。现在无条件呼吸，一眼就知道它在工作。
        case LED_FX_STATUS: {
            uint8_t v = (uint8_t)(25 + (uint16_t)tri(sAnimPhase, 3000) * 125 / 255);
            for (uint8_t i = 0; i < BUTTON_COUNT; i++) {
                sR[i] = 0;
                sG[i] = v;
                sB[i] = (uint8_t)((uint16_t)v / 4);      // 带一点蓝 -> 偏青绿
            }
            break;
        }

        // ---- 2) 常亮：稳定青色，不闪，和 1) 的呼吸一眼分得开 ----
        case LED_FX_SOLID:
            for (uint8_t i = 0; i < BUTTON_COUNT; i++) {
                sR[i] = 0; sG[i] = 140; sB[i] = 80;
            }
            break;

        // ---- 3) 彩虹流动：整条色轮随时间平移 ----
        case LED_FX_RAINBOW: {
            uint8_t base = (uint8_t)(sAnimPhase / 16);      // 约 4 秒转一圈
            for (uint8_t i = 0; i < BUTTON_COUNT; i++) {
                uint8_t h = (uint8_t)(base + i * 255 / BUTTON_COUNT);
                uint8_t r, g, b;
                hueToRgb(h, &r, &g, &b);
                sR[i] = (uint8_t)((uint16_t)r * 4 / 5);     // 压到 80% 免得刺眼
                sG[i] = (uint8_t)((uint16_t)g * 4 / 5);
                sB[i] = (uint8_t)((uint16_t)b * 4 / 5);
            }
            break;
        }

        // ---- 4) 波浪起伏：相邻灯相位错开 ----
        case LED_FX_WAVE: {
            for (uint8_t i = 0; i < BUTTON_COUNT; i++) {
                uint8_t v = tri(sAnimPhase + (uint32_t)i * 350, 2400);
                sR[i] = 0;
                sG[i] = (uint8_t)((uint16_t)v * 160 / 255);
                sB[i] = (uint8_t)((uint16_t)v * 220 / 255);
            }
            break;
        }

        // ---- 5) 跑马灯：一颗亮珠来回跑 ----
        case LED_FX_CHASE: {
            uint8_t period = (uint8_t)(BUTTON_COUNT * 2 - 2);         // 8
            uint8_t pos    = (uint8_t)((sAnimPhase / 120) % period);   // 每 120ms 挪一格
            uint8_t idx    = (pos < BUTTON_COUNT) ? pos : (uint8_t)(period - pos);
            sR[idx] = 255; sG[idx] = 60; sB[idx] = 0;
            int8_t trail = (int8_t)idx - 1;                            // 拖一点余晖
            if (trail >= 0) { sR[trail] = 70; sG[trail] = 16; }
            break;
        }

        // ---- 6) 随机闪烁：每 90ms 换一批暖白点 ----
        case LED_FX_TWINKLE: {
            uint32_t seed = sAnimPhase / 90;
            for (uint8_t i = 0; i < BUTTON_COUNT; i++) {
                uint32_t h = (seed + 1) * 2654435761UL + (uint32_t)i * 40503UL;
                uint8_t  v = (uint8_t)((h >> 7) & 0xFF);
                if (v > 150) {                      // 约 40% 概率点亮
                    uint8_t b = (uint8_t)(70 + (h & 0x7F));
                    sR[i] = b;
                    sG[i] = (uint8_t)((uint16_t)b * 3 / 4);
                    sB[i] = (uint8_t)((uint16_t)b / 2);
                }
            }
            break;
        }

        default:
            break;
    }
}

// ---------------------------------------------------------------- 帧合成
// 重构说明（实机反馈后改的）：
//   第一版在"广播中"时把整条灯往橙色偏 90/255，结果 RAINBOW/WAVE/CHASE/TWINKLE
//   四档在没配对时全被冲成同一种"橙色乱闪"，用户实际只能看出 3 种灯效。
//   第二版改成只动第 1 颗灯，但那时它是"亮 200ms / 灭 1300ms"的普通闪烁，
//   叠在青绿呼吸上看起来就是"一颗发黄、忽明忽暗的灯"，仍然误导。
//   现在改成**双击闪**：一眼就能认出这是"未连接"的提示音式信号，
//   不会被误当成灯效本身的颜色。
//
//     未连接 -> LED1 双击橙闪（闪-灭-闪，然后长灭），周期 2.4s
//     已连接 -> LED1 单次绿心跳，周期 3s
#define LINK_DBL_PERIOD_MS     2400     // 双击周期
#define LINK_DBL_ON_MS         130      // 每下亮多久
#define LINK_DBL_GAP_MS        140      // 两下之间
#define LINK_HEARTBEAT_PERIOD  3000
#define LINK_HEARTBEAT_ON_MS   300

static void composeFrame() {
    // 1) 灯效动画打底（满色，不做任何冲淡）
    renderEffect();

    // 2) 连接状态指示：只动第一颗灯
    if (sLink != LED_LINK_OFF) {
        bool show = false;
        uint8_t ir = 0, ig = 0, ib = 0;

        if (sLink == LED_LINK_ADVERTISING) {
            // 还没配对：双击橙闪。两下短闪 vs 灯效的连续渐变极好区分
            uint32_t p = sAnimPhase % LINK_DBL_PERIOD_MS;
            show = (p < LINK_DBL_ON_MS) ||
                   (p >= (LINK_DBL_ON_MS + LINK_DBL_GAP_MS) &&
                    p <  (LINK_DBL_ON_MS * 2 + LINK_DBL_GAP_MS));
            ir = 255; ig = 90; ib = 0;
        } else {
            // 已连接：单次绿心跳，节奏慢，不抢戏
            show = ((sAnimPhase % LINK_HEARTBEAT_PERIOD) < LINK_HEARTBEAT_ON_MS);
            ir = 0; ig = 200; ib = 0;
        }

        if (show) {
            sR[0] = ir;
            sG[0] = ig;
            sB[0] = ib;
        }
    }

    // 3) 模式指示（定时器=蓝 / 节拍器=紫 / 配置=青）
    switch (sMode) {
        case 1:  // MODE_TIMER_SET
            for (uint8_t i = 0; i < BUTTON_COUNT; i++) {
                sR[i] = lerp8(sR[i], 0,   180);
                sG[i] = lerp8(sG[i], 60,  180);
                sB[i] = lerp8(sB[i], 200, 180);
            }
            break;
        case 2:  // MODE_METRONOME
            for (uint8_t i = 0; i < BUTTON_COUNT; i++) {
                sR[i] = lerp8(sR[i], 120, 180);
                sG[i] = lerp8(sG[i], 0,   180);
                sB[i] = lerp8(sB[i], 180, 180);
            }
            if (sBeatFlash >= 0) {
                uint8_t k = (uint8_t)sBeatFlash;
                sR[k] = 255; sG[k] = 255; sB[k] = 255;
            }
            break;
        case 3:  // MODE_KEY_CONFIG
            for (uint8_t i = 0; i < BUTTON_COUNT; i++) {
                sR[i] = lerp8(sR[i], 0,   180);
                sG[i] = lerp8(sG[i], 200, 180);
                sB[i] = lerp8(sB[i], 200, 180);
            }
            break;
        default:
            break;
    }

    // 4) 按键反馈：按下的那颗闪白（优先级高于灯效）
    for (uint8_t i = 0; i < BUTTON_COUNT; i++) {
        if (sKeyGlow[i] == 0) continue;
        uint8_t t = sKeyGlow[i];
        sR[i] = lerp8(sR[i], 255, t);
        sG[i] = lerp8(sG[i], 255, t);
        sB[i] = lerp8(sB[i], 255, t);
    }

    // 5) 低电提示：整体压成暗红闪烁，优先级最高
    if (sLowBat) {
        bool on = ((sAnimPhase / 800) & 1) == 0;
        for (uint8_t i = 0; i < BUTTON_COUNT; i++) {
            sR[i] = on ? 220 : 40;
            sG[i] = 0;
            sB[i] = 0;
        }
    }

    for (uint8_t i = 0; i < BUTTON_COUNT; i++) sStrip.setPixel(i, sR[i], sG[i], sB[i]);
}

// ---------------------------------------------------------------- 主循环
void LED_EffectsUpdate() {
    // 开关轮询不依赖 RMT，即使灯带初始化失败也要能读
    LED_EffectsPollSwitch();

    if (!sStripOk) return;

    uint32_t now = millis();

    // 每 5ms 推进一次：动画相位 + 按键余晖衰减
    if (now - sLastFade >= 5) {
        uint32_t steps = (now - sLastFade) / 5;
        sLastFade = now;

        sAnimPhase += steps * 5;

        for (uint8_t i = 0; i < BUTTON_COUNT; i++) {
            uint32_t drop = steps * 4;
            sKeyGlow[i] = (sKeyGlow[i] > drop) ? (uint8_t)(sKeyGlow[i] - drop) : 0;
        }
    }

    if (now - sLastRefresh < LED_REFRESH_MS) return;
    sLastRefresh = now;

    composeFrame();
    sStrip.show();
}
