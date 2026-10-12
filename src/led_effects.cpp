// ============================================================================
//  led_effects.cpp —— 5 颗灯珠的灯效实现
//
//  刷新策略：每 LED_REFRESH_MS 重算一帧并 rmt_write_items 发出去。
//  每次发送 120 bit 约 150us，RMT 硬件保证时序，BLE 中断打断也不影响。
//
//  灯效模式的切换开关在 GPIO6（板上 SW1 TS35CA，另一端接 GND）：
//  SW1 是点按开关：这里只做消抖，短按切灯效 / 长按切模式由 SystemUpdate() 判定。
// ============================================================================
#include <Arduino.h>
#include "board_config.h"
#include "ws2812.h"
#include "led_effects.h"
#include "system.h"          // SystemMode：模式配色
#include "usb_log.h"

#define LED_REFRESH_MS      25      // 40Hz，人眼足够顺滑
#define SELFTEST_STEP_MS    250

// ---------------------------------------------------------------- 模块状态
static Ws2812     sStrip(LED_DATA_PIN, LED_COUNT, RMT_CHANNEL_0);
static bool       sStripOk      = false;

static LedLinkState sLink        = LED_LINK_OFF;
static uint8_t      sMode        = 0;
static bool         sLowBat      = false;
static uint8_t      sEffect      = LED_FX_KEYS;     // 上电默认：全灭 + 按键反馈

static uint32_t     sLastRefresh = 0;
static uint32_t     sAnimPhase   = 0;      // 动画相位，每 5ms 累加
static uint32_t     sLastFade    = 0;

// 按键反馈：每颗键一个剩余亮度（255 -> 0 衰减），以及"缤纷按键"灯效用的随机色相
static uint8_t      sKeyGlow[BUTTON_COUNT];
static uint8_t      sKeyHue[BUTTON_COUNT];

// K5 那颗灯的提醒（Claude 状态用），见 LedAttention
static uint8_t      sAttn        = LED_ATTN_NONE;
static uint32_t     sAttnAt      = 0;

// 刚连上蓝牙的绿闪动画开始时刻，0 = 没在播
static uint32_t     sConnectAnimAt = 0;
// 没连上时切灯效：在这个时刻之前先预览新灯效，0 = 不预览
#define EFFECT_PREVIEW_MS   3000
static uint32_t     sPreviewUntil  = 0;


// GPIO6 开关
static uint8_t      sSwStable    = 1;      // 已消抖的闭合状态：1=闭合
static uint8_t      sSwRaw       = 1;
static uint32_t     sSwChangeMs  = 0;
static uint16_t     sSwToggles   = 0;

// 每颗灯的 RGB 工作缓冲
static uint8_t      sR[BUTTON_COUNT], sG[BUTTON_COUNT], sB[BUTTON_COUNT];

static void railOnBlocking();           // 灯珠电源自动管理，定义在主循环前面

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
        case LED_FX_KEYS:          return "KEYS";
        case LED_FX_WHITE:         return "WHITE";
        case LED_FX_BREATHE:       return "BREATHE";
        case LED_FX_RAINBOW_CHASE: return "RAINBOW_CHASE";
        case LED_FX_RAINBOW:       return "RAINBOW";
        case LED_FX_KEYS_COLOR:    return "KEYS_COLOR";
        default:             return "?";
    }
}

void LED_EffectsSet(uint8_t fx) {
    if (fx >= LED_FX_COUNT) return;
    sEffect    = fx;
    sAnimPhase = 0;
    // 没连上时整条灯是黄色呼吸，切了灯效看不出来 -> 先预览新灯效几秒
    sPreviewUntil = millis() + EFFECT_PREVIEW_MS;
    if (sPreviewUntil == 0) sPreviewUntil = 1;
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
        // 这里只做消抖和计数。短按切灯效 / 长按切模式的判定在 SystemUpdate() 里，
        // 因为长按要切换的是系统模式，不归灯效模块管。
        if (closed) sSwToggles++;
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
    usbLog::printf("[led] effect switch SW1 on GPIO%d, initial=%s (raw=%d)\n",
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

void LED_EffectsSetLink(LedLinkState st) {
    // 从"没连上"变成"已连接"的那一刻，播一次绿闪
    if (st == LED_LINK_CONNECTED && sLink != LED_LINK_CONNECTED) {
        sConnectAnimAt = millis();
        if (sConnectAnimAt == 0) sConnectAnimAt = 1;
    } else if (st != LED_LINK_CONNECTED) {
        sConnectAnimAt = 0;                 // 断开了就别再闪绿
    }
    sLink = st;
}
void LED_EffectsSetMode(uint8_t mode)    { sMode = mode; }
void LED_EffectsSetLowBattery(bool low)  { sLowBat = low; }

void LED_EffectsSetAttention(uint8_t attn) {
    sAttn   = attn;
    sAttnAt = millis();
}

void LED_EffectsKeyFeedback(uint8_t keyIndex) {
    if (keyIndex >= BUTTON_COUNT) return;
    sKeyGlow[keyIndex] = 255;
    sKeyHue[keyIndex]  = (uint8_t)esp_random();   // "缤纷按键"灯效每次换一个颜色
}

// ---------------------------------------------------------------- 上电自检
void LED_EffectsSelfTest() {
    if (!sStripOk) {
        usbLog::print("[led] selftest skipped: RMT not ready\n");
        return;
    }
    railOnBlocking();
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
    railOnBlocking();
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

// ---------------------------------------------------------------- 灯效动画
// 每个动画只负责往 sR/sG/sB 里填色，后面的叠加逻辑统一在 composeFrame() 做
#define CHASE_STEP_MS       110     // 炫彩跑马灯每格停留时间
#define CHASE_HUE_STEP      23      // 每挪一格色相前进多少（255 一圈，约 11 格转一圈）
#define BREATHE_PERIOD_MS   3200    // 呼吸一个来回

static void renderEffect() {
    for (uint8_t i = 0; i < BUTTON_COUNT; i++) { sR[i] = sG[i] = sB[i] = 0; }

    switch (sEffect) {
        // ---- 1) 默认：全灭，只靠按键反馈点亮（反馈在 composeFrame 里叠加）----
        case LED_FX_KEYS:
        case LED_FX_KEYS_COLOR:
            break;

        // ---- 2) 白色常亮 ----
        case LED_FX_WHITE:
            for (uint8_t i = 0; i < BUTTON_COUNT; i++) {
                sR[i] = 255; sG[i] = 255; sB[i] = 255;
            }
            break;

        // ---- 3) 呼吸：青蓝色，和"没连上"的黄色、"刚连上"的绿色都分得开 ----
        //   亮度按三角波平方走（近似人眼的感知曲线），暗处停留更久、过渡更柔
        case LED_FX_BREATHE: {
            uint16_t t = tri(sAnimPhase, BREATHE_PERIOD_MS);
            uint8_t  v = (uint8_t)(8 + (t * t / 255) * 247 / 255);
            for (uint8_t i = 0; i < BUTTON_COUNT; i++) {
                sR[i] = 0;
                sG[i] = (uint8_t)((uint16_t)v * 150 / 255);
                sB[i] = v;
            }
            break;
        }

        // ---- 4) 炫彩跑马灯：一颗亮点来回跑，每挪一格换一个颜色，后面拖两格余晖 ----
        case LED_FX_RAINBOW_CHASE: {
            const uint8_t period = (uint8_t)(BUTTON_COUNT * 2 - 2);      // 来回一趟 8 步
            uint32_t step = sAnimPhase / CHASE_STEP_MS;
            // 从最老的余晖画到亮点，后画的覆盖先画的（亮点永远在最上面）
            for (int8_t age = 2; age >= 0; age--) {
                if ((int32_t)step - age < 0) continue;
                uint32_t s   = step - (uint32_t)age;
                uint8_t  pos = (uint8_t)(s % period);
                uint8_t  idx = (pos < BUTTON_COUNT) ? pos : (uint8_t)(period - pos);
                uint8_t  r, g, b;
                hueToRgb((uint8_t)(s * CHASE_HUE_STEP), &r, &g, &b);
                uint8_t  k   = (age == 0) ? 255 : (age == 1 ? 70 : 18);   // 亮点 / 余晖
                sR[idx] = (uint8_t)((uint16_t)r * k / 255);
                sG[idx] = (uint8_t)((uint16_t)g * k / 255);
                sB[idx] = (uint8_t)((uint16_t)b * k / 255);
            }
            break;
        }

        // ---- 5) 彩虹流动：整条色轮随时间平移 ----
        case LED_FX_RAINBOW: {
            uint8_t base = (uint8_t)(sAnimPhase / 16);      // 约 4 秒转一圈
            for (uint8_t i = 0; i < BUTTON_COUNT; i++) {
                uint8_t h = (uint8_t)(base + i * 255 / BUTTON_COUNT);
                hueToRgb(h, &sR[i], &sG[i], &sB[i]);
            }
            break;
        }

        default:
            break;
    }
}

// ---------------------------------------------------------------- 帧合成
#define LINK_WAIT_PERIOD_MS    2000     // 没连上：黄色呼吸一个来回
#define CONNECT_BLINKS         2        // 刚连上：绿色闪几下
#define CONNECT_ON_MS          220
#define CONNECT_OFF_MS         200

// 刚连上的绿闪是否还在进行；在的话顺便告诉调用方这一刻是亮还是灭
static bool connectAnimActive(uint32_t now, bool *on) {
    if (sConnectAnimAt == 0) return false;
    uint32_t t     = now - sConnectAnimAt;
    uint32_t cycle = CONNECT_ON_MS + CONNECT_OFF_MS;
    if (t >= cycle * CONNECT_BLINKS) {
        sConnectAnimAt = 0;                 // 播完了
        return false;
    }
    *on = (t % cycle) < CONNECT_ON_MS;
    return true;
}

static void composeFrame() {
    uint32_t now = millis();

    // 1) 打底：连接状态优先，其次是灯效模式
    bool blinkOn = false;
    bool previewing = (sPreviewUntil != 0 && (int32_t)(now - sPreviewUntil) < 0);
    if (connectAnimActive(now, &blinkOn)) {
        // 刚连上：5 颗一起绿闪 2 下
        for (uint8_t i = 0; i < BUTTON_COUNT; i++) {
            sR[i] = 0; sG[i] = blinkOn ? 255 : 0; sB[i] = 0;
        }
    } else if (sLink != LED_LINK_CONNECTED && !previewing) {
        // 没连上：5 颗一起黄色明暗呼吸（最暗不全灭，一眼看得出"在等配对"）
        uint16_t t = tri(sAnimPhase, LINK_WAIT_PERIOD_MS);
        uint8_t  v = (uint8_t)(20 + (t * t / 255) * 235 / 255);
        for (uint8_t i = 0; i < BUTTON_COUNT; i++) {
            sR[i] = v;
            sG[i] = (uint8_t)((uint16_t)v * 150 / 255);    // 黄 = 红满 + 绿六成，不偏绿
            sB[i] = 0;
        }
    } else {
        renderEffect();
    }

    // 2) 模式指示（预设切换=整条偏青）
    switch (sMode) {
        case MODE_KEY_CONFIG:
            for (uint8_t i = 0; i < BUTTON_COUNT; i++) {
                sR[i] = lerp8(sR[i], 0,   180);
                sG[i] = lerp8(sG[i], 200, 180);
                sB[i] = lerp8(sB[i], 200, 180);
            }
            break;
        default:
            break;
    }

    // 3) 按键反馈：按下的那颗亮一下再渐暗（所有灯效都有）
    //    · 平时是白色；白色常亮灯效下白色看不出来，改成青色
    //    · "缤纷按键"灯效下每次按都是随机颜色
    //    渐暗按平方走，结尾收得更柔，不会"啪"一下灭掉
    for (uint8_t i = 0; i < BUTTON_COUNT; i++) {
        if (sKeyGlow[i] == 0) continue;
        uint8_t t = (uint8_t)((uint16_t)sKeyGlow[i] * sKeyGlow[i] / 255);
        uint8_t kr = 255, kg = 255, kb = 255;
        if (sEffect == LED_FX_KEYS_COLOR)  hueToRgb(sKeyHue[i], &kr, &kg, &kb);
        else if (sEffect == LED_FX_WHITE)  { kr = 0; kg = 200; kb = 255; }
        sR[i] = lerp8(sR[i], kr, t);
        sG[i] = lerp8(sG[i], kg, t);
        sB[i] = lerp8(sB[i], kb, t);
    }

    // 3.5) K5 那颗灯的提醒：等你操作 = 橙色闪；做完 = 绿色闪 3 下
    {
        const uint8_t k = BUTTON_COUNT - 1;
        if (sAttn == LED_ATTN_WAIT) {
            if (((now - sAttnAt) / 400) % 2 == 0) { sR[k] = 255; sG[k] = 110; sB[k] = 0; }
        } else if (sAttn == LED_ATTN_DONE) {
            uint32_t t = now - sAttnAt;
            if (t >= 1500)                { sAttn = LED_ATTN_NONE; }
            else if ((t / 250) % 2 == 0)  { sR[k] = 0; sG[k] = 220; sB[k] = 40; }
        }
    }

    // 4) 低电提示：整体压成暗红闪烁，优先级最高
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

// ---------------------------------------------------------------- 灯珠电源自动管理
#define RAIL_IDLE_OFF_MS   60000UL  // 全黑持续这么久就断电
#define RAIL_WARMUP_MS     5        // 上电后等这么久再发数据（灯珠 VDD 和去耦电容稳定）

static bool     sRailOn      = true;    // LED_EffectsInit() 里已经上电
static uint32_t sDarkSince   = 0;       // 从什么时候开始一直全黑，0 = 当前不是全黑
static uint32_t sRailReadyAt = 0;

static bool frameIsDark() {
    for (uint8_t i = 0; i < BUTTON_COUNT; i++) {
        if (sR[i] | sG[i] | sB[i]) return false;
    }
    return true;
}

// 自检 / 诊断这些阻塞式的直接点灯：确保电源是开的
static void railOnBlocking() {
    sDarkSince = 0;
    if (sRailOn) return;
    LED_PowerRailOn();
    sRailOn = true;
    delay(RAIL_WARMUP_MS);
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
            uint32_t drop = steps * 2;      // 每 5ms 掉 2 -> 约 0.65 秒从最亮渐暗到灭
            sKeyGlow[i] = (sKeyGlow[i] > drop) ? (uint8_t)(sKeyGlow[i] - drop) : 0;
        }
    }

    if (now - sLastRefresh < LED_REFRESH_MS) return;
    sLastRefresh = now;

    composeFrame();

    // ---- 灯珠电源自动管理（GPIO21 高边开关）----
    //   WS2812 全灭时每颗仍有 ~0.5~1mA 静态电流，5 颗共 3~5mA，用电池时白白耗电。
    //   全黑持续 RAIL_IDLE_OFF_MS 就断电；一旦有要亮的帧，先上电等 RAIL_WARMUP_MS 再发数据。
    if (frameIsDark()) {
        if (sDarkSince == 0) sDarkSince = now ? now : 1;
        if (sRailOn && (now - sDarkSince) >= RAIL_IDLE_OFF_MS) {
            LED_PowerRailOff();
            sRailOn = false;
            usbLog::print("[led] all dark for 60s -> LED power OFF\n");
        }
        if (!sRailOn) return;               // 已断电，不用再发数据
    } else {
        sDarkSince = 0;
        if (!sRailOn) {
            LED_PowerRailOn();
            sRailOn      = true;
            sRailReadyAt = now + RAIL_WARMUP_MS;
            // 让下一帧在 RAIL_WARMUP_MS 后就刷新，而不是再等一整个刷新周期
            sLastRefresh = now - LED_REFRESH_MS + RAIL_WARMUP_MS;
            usbLog::print("[led] LED power ON\n");
            return;
        }
        if ((int32_t)(now - sRailReadyAt) < 0) return;   // 还在等电源稳定
    }

    sStrip.show();
}
