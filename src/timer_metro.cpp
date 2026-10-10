// ============================================================================
//  timer_metro.cpp —— 倒计时定时器 + 节拍器实现
// ============================================================================
#include <Arduino.h>
#include <stdio.h>
#include "board_config.h"
#include "timer_metro.h"
#include "buzzer.h"
#include "led_effects.h"
#include "usb_log.h"

TimerState gTimer = { TIMER_DEFAULT_HOURS, TIMER_DEFAULT_MINUTES, 0, 0, false, false };
MetroState gMetro = { METRO_DEFAULT_BPM, 4, false, 0, 0 };

void TimerMetroInit() {
    gTimer.hours   = TIMER_DEFAULT_HOURS;
    gTimer.minutes = TIMER_DEFAULT_MINUTES;
    gTimer.running = false;
    gTimer.fired   = false;
    gTimer.durationMs = ((uint32_t)gTimer.hours * 3600UL + (uint32_t)gTimer.minutes * 60UL) * 1000UL;

    gMetro.bpm       = METRO_DEFAULT_BPM;
    gMetro.timeSig   = 4;
    gMetro.running   = false;
    gMetro.beat      = 0;
    gMetro.nextBeatMs = 0;

    usbLog::printf("[tmr] default %02u:%02u, metro %ubpm %u/4\n",
                   (unsigned)gTimer.hours, (unsigned)gTimer.minutes,
                   (unsigned)gMetro.bpm, (unsigned)gMetro.timeSig);
}

// ---------------------------------------------------------------- 定时器
static void timerSyncDuration() {
    gTimer.durationMs = ((uint32_t)gTimer.hours * 3600UL + (uint32_t)gTimer.minutes * 60UL) * 1000UL;
    if (gTimer.durationMs == 0) gTimer.durationMs = 60000UL;    // 不允许 0 分钟
}

void TimerStart() {
    timerSyncDuration();
    gTimer.targetMs = millis() + gTimer.durationMs;
    gTimer.running  = true;
    gTimer.fired    = false;
    usbLog::printf("[tmr] start, %lu s\n", (unsigned long)(gTimer.durationMs / 1000));
}

void TimerStop() {
    if (!gTimer.running) return;
    // 停止时把剩余时间折回 hours/minutes，方便再次启动
    uint32_t remain = TimerRemainingMs();
    gTimer.hours    = (uint8_t)(remain / 3600000UL);
    gTimer.minutes  = (uint8_t)((remain % 3600000UL) / 60000UL);
    gTimer.running  = false;
    usbLog::print("[tmr] stop\n");
}

void TimerReset() {
    gTimer.running = false;
    gTimer.fired   = false;
    gTimer.hours   = TIMER_DEFAULT_HOURS;
    gTimer.minutes = TIMER_DEFAULT_MINUTES;
    timerSyncDuration();
    usbLog::print("[tmr] reset\n");
}

void TimerAddHours(int8_t d) {
    if (gTimer.running) return;
    int16_t v = (int16_t)gTimer.hours + d;
    while (v < 0)  v += 24;
    while (v > 23) v -= 24;
    gTimer.hours = (uint8_t)v;
    timerSyncDuration();
}

void TimerAddMinutes(int8_t d) {
    if (gTimer.running) return;
    int16_t v = (int16_t)gTimer.minutes + d * 5;
    while (v < 0)  v += 60;
    while (v > 55) v -= 60;
    gTimer.minutes = (uint8_t)v;
    timerSyncDuration();
}

uint32_t TimerRemainingMs() {
    if (!gTimer.running) return gTimer.durationMs;
    uint32_t now = millis();
    if ((int32_t)(now - gTimer.targetMs) >= 0) return 0;
    return gTimer.targetMs - now;
}

void TimerFormatRemaining(char *buf, size_t n) {
    uint32_t sec = TimerRemainingMs() / 1000UL;
    uint32_t h = sec / 3600UL;
    uint32_t m = (sec % 3600UL) / 60UL;
    uint32_t s = sec % 60UL;
    if (h > 0) snprintf(buf, n, "%lu:%02lu:%02lu", (unsigned long)h, (unsigned long)m, (unsigned long)s);
    else       snprintf(buf, n, "%02lu:%02lu",     (unsigned long)m, (unsigned long)s);
}

void TimerUpdate() {
    if (!gTimer.running) return;
    if ((int32_t)(millis() - gTimer.targetMs) >= 0) {
        gTimer.running = false;
        gTimer.fired   = true;
        usbLog::print("[tmr] TIME UP\n");
        // 到点提示：三声高音。BuzzerBeep 是非阻塞的，这里只触发最后一声，
        // 前面的两声由 System 层在显示到点画面时补。
        BuzzerBeep(2200, 400);
    }
}

// ---------------------------------------------------------------- 节拍器
static inline uint32_t beatIntervalMs() {
    // 60000 / bpm
    return 60000UL / (uint32_t)(gMetro.bpm == 0 ? 1 : gMetro.bpm);
}

void MetroToggle() {
    gMetro.running = !gMetro.running;
    gMetro.beat    = 0;
    if (gMetro.running) {
        gMetro.nextBeatMs = millis();
        usbLog::printf("[met] run %ubpm %u/4\n", (unsigned)gMetro.bpm, (unsigned)gMetro.timeSig);
    } else {
        usbLog::print("[met] stop\n");
    }
}

void MetroSetBpm(int16_t delta) {
    int16_t v = (int16_t)gMetro.bpm + delta;
    if (v < METRO_BPM_MIN) v = METRO_BPM_MIN;
    if (v > METRO_BPM_MAX) v = METRO_BPM_MAX;
    gMetro.bpm = (uint16_t)v;
}

void MetroCycleTimeSig() {
    switch (gMetro.timeSig) {
        case 4:  gMetro.timeSig = 3; break;
        case 3:  gMetro.timeSig = 2; break;
        default: gMetro.timeSig = 4; break;
    }
    gMetro.beat = 0;
}

void MetroUpdate() {
    if (!gMetro.running) return;

    uint32_t now = millis();
    if ((int32_t)(now - gMetro.nextBeatMs) < 0) return;

    // 出拍：强拍高音响一点，弱拍轻一点
    if (gMetro.beat == 0) BuzzerBeatAccent();
    else                  BuzzerBeatNormal();

    LED_EffectsBeat();          // 通知灯效往前挪一颗

    gMetro.beat = (uint8_t)((gMetro.beat + 1) % (gMetro.timeSig == 0 ? 1 : gMetro.timeSig));
    gMetro.nextBeatMs = now + beatIntervalMs();
}
