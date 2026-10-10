// ============================================================================
//  battery.cpp —— 电池电压检测实现
//
//  关于"没接电池时 BAT+ 也是 4.2V"：
//    TP4056 没接电池时处于恒压(CV)环路空载状态，BAT 脚会顶在 4.2V 调节点。
//    它判定"充满"看的是充电电流是否跌到设定值的 1/10（R20 2.4K -> 500mA，
//    即 50mA），而本板 100K/100K 分压只抽 21uA，所以它永远不判满，
//    CHRG 红灯常亮、BAT+ 恒为 4.2V。
//
//    结论：软件无法靠 BAT+ 电压区分"没装电池"和"已充满"，所以用
//    board_config.h 里的 BATTERY_PRESENT 明确开关，而不是猜。
// ============================================================================
#include <Arduino.h>
#include "board_config.h"
#include "battery.h"
#include "usb_log.h"

#if BATTERY_ENABLED

static bool     sPresent    = (BATTERY_PRESENT != 0);
static float    sVoltage    = 0.0f;
static uint8_t  sPercent    = 0;
static bool     sLow        = false;
static uint32_t sLastRead   = 0;

// 锂电池"电压 -> 电量"对照表（小电流负载下的典型放电曲线），从高到低。
// 【之前的问题】在 2.64V~4.20V 之间按直线换算：3.7V 显示约 68%，按曲线实际只剩约 15%；
// 而且锂电放电曲线中段很平、两头很陡，直线换算中段严重偏高。
// 两个点之间线性插值；低于最后一点算 0%。
struct BatPoint { float v; uint8_t pct; };
static const BatPoint kBatCurve[] = {
    { 4.20f, 100 }, { 4.10f, 90 }, { 4.00f, 80 }, { 3.93f, 70 },
    { 3.87f,  60 }, { 3.82f, 50 }, { 3.79f, 40 }, { 3.76f, 30 },
    { 3.73f,  20 }, { 3.68f, 10 }, { 3.60f,  5 }, { 3.40f,  0 },
};

static uint8_t voltsToPercent(float v) {
    const size_t n = sizeof(kBatCurve) / sizeof(kBatCurve[0]);
    if (v >= kBatCurve[0].v)     return 100;
    if (v <= kBatCurve[n - 1].v) return 0;
    for (size_t i = 1; i < n; i++) {
        if (v >= kBatCurve[i].v) {
            const BatPoint &hi = kBatCurve[i - 1], &lo = kBatCurve[i];
            float p = lo.pct + (v - lo.v) * (hi.pct - lo.pct) / (hi.v - lo.v);
            return (uint8_t)(p + 0.5f);
        }
    }
    return 0;
}

// 单次采样：取多次平均，抑制 ADC 噪声
static uint32_t adcMilliVoltsAvg() {
    uint32_t sum = 0;
    for (uint8_t i = 0; i < BATTERY_SAMPLES; i++) {
        sum += analogReadMilliVolts(BATTERY_ADC_PIN);
    }
    return sum / BATTERY_SAMPLES;
}

#endif  // BATTERY_ENABLED

void BatteryInit() {
#if BATTERY_ENABLED
    pinMode(BATTERY_ADC_PIN, INPUT);                    // 只能做输入！
    analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_11db); // 量程约 0..2.5V
    sLastRead = 0;                                      // 0 = 下一次 BatteryUpdate() 立刻采样

    if (!sPresent) {
        usbLog::printf("[bat] 未启用（BATTERY_PRESENT=0）。GPIO%d 已配成模拟输入但不采样。\n",
                       BATTERY_ADC_PIN);
        usbLog::print("     注意：没接电池时 TP4056 会让 BAT+ 一直停在 4.2V，");
        usbLog::print("这不是故障，但它不能代表电量。\n");
        usbLog::print("     装上电池后把 board_config.h 的 BATTERY_PRESENT 改成 1 重新烧录，");
        usbLog::print("或运行时串口发 v。\n");
        return;
    }

    usbLog::printf("[bat] ADC on GPIO%d, divider=%.3f, 11dB\n",
                   BATTERY_ADC_PIN, (double)BATTERY_DIVIDER);
#else
    usbLog::print("[bat] disabled by board_config.h\n");
#endif
}

void BatteryUpdate() {
#if BATTERY_ENABLED
    if (!sPresent) return;      // 没装电池就不采样，免得显示一个误导人的 100%

    uint32_t now = millis();
    if (sLastRead != 0 && (now - sLastRead) < BATTERY_READ_INTERVAL_MS) return;
    sLastRead = now;

    uint32_t mv = adcMilliVoltsAvg();
    float vadc  = (float)mv / 1000.0f;
    // 反推电池电压：Vbat = Vadc / divider * 校准系数
    float vbat  = (vadc / BATTERY_DIVIDER) * BATTERY_CAL;

    // 读数过低说明分压点基本没电压（电池被拔掉 / 断路）
    if (vbat < 2.0f) vbat = 0.0f;

    sVoltage = vbat;

    sPercent = (vbat <= 0.0f) ? 0 : voltsToPercent(vbat);

    // 低电带回差：低于 BAT_LOW_V 才进入，回升到 BAT_LOW_RELEASE_V 以上才解除。
    // 【之前的问题】只有一个阈值，电压在 3.30V 附近时 ADC 噪声会让低电状态
    // 每 10 秒来回切换，灯一会儿红闪一会儿正常，屏幕也跟着暗了又亮。
    if (vbat <= 0.0f)                 sLow = false;
    else if (!sLow && vbat < BAT_LOW_V)        sLow = true;
    else if (sLow && vbat > BAT_LOW_RELEASE_V) sLow = false;

    static bool firstLog = true;
    if (firstLog) {
        firstLog = false;
        usbLog::printf("[bat] first read: adc=%lumV -> %.3fV, %u%%, low=%d\n",
                       (unsigned long)mv, (double)vbat, (unsigned)sPercent, sLow ? 1 : 0);
    }
#endif
}

// 运行时开关：装上 / 拆下电池后不用重新烧录
void BatterySetPresent(bool present) {
#if BATTERY_ENABLED
    sPresent = present;
    sLastRead = 0;                      // 强制下一次立刻采样
    usbLog::printf("[bat] battery present -> %s\n", present ? "YES（开始采样）" : "NO（停止采样）");
#else
    (void)present;
#endif
}

bool BatteryIsPresent() {
#if BATTERY_ENABLED
    return sPresent;
#else
    return false;
#endif
}

float BatteryVoltage() {
#if BATTERY_ENABLED
    return sPresent ? sVoltage : 0.0f;
#else
    return 0.0f;
#endif
}

uint8_t BatteryPercent() {
#if BATTERY_ENABLED
    return sPresent ? sPercent : 0;
#else
    return 0;
#endif
}

bool BatteryIsLow() {
#if BATTERY_ENABLED
    return sPresent ? sLow : false;
#else
    return false;
#endif
}
