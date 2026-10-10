// ============================================================================
//  timer_metro.h —— 倒计时定时器 + 节拍器
//
//  两者都基于 millis() 的非阻塞状态机，不占用硬件定时器、不 delay。
//  （原工程用 hw_timer 中断 + 主循环 delay 混着来，容易和灯效、BLE 抢时间片。）
// ============================================================================
#pragma once

#include <Arduino.h>

// ---------------------------------------------------------------- 定时器
#define TIMER_DEFAULT_HOURS     0
#define TIMER_DEFAULT_MINUTES   1       // 默认 1 分钟，上手就能试出效果

struct TimerState {
    uint8_t  hours;
    uint8_t  minutes;
    uint32_t targetMs;      // 目标时刻（millis 基准）
    uint32_t durationMs;    // 设定的总时长
    bool     running;
    bool     fired;         // 到点标志，被读取后由上层清掉
};

// ---------------------------------------------------------------- 节拍器
#define METRO_DEFAULT_BPM       100
#define METRO_BPM_MIN           30
#define METRO_BPM_MAX           300

struct MetroState {
    uint16_t bpm;
    uint8_t  timeSig;       // 每小节拍数（4 或 3 等）
    bool     running;
    uint8_t  beat;          // 当前是第几拍（0 = 强拍）
    uint32_t nextBeatMs;
};

extern TimerState gTimer;
extern MetroState gMetro;

void TimerMetroInit();

// 主循环调用
void TimerUpdate();
void MetroUpdate();

// ---------------------------------------------------------------- 定时器操作
void TimerStart();
void TimerStop();
void TimerReset();
void TimerAddHours(int8_t d);       // 循环 0..23
void TimerAddMinutes(int8_t d);     // 循环 0..59，步进 5 分钟
uint32_t TimerRemainingMs();        // 剩余毫秒，未运行返回设定时长
void TimerFormatRemaining(char *buf, size_t n);     // "MM:SS" 或 "H:MM:SS"

// ---------------------------------------------------------------- 节拍器操作
void MetroToggle();
void MetroSetBpm(int16_t delta);    // 限幅在 BPM_MIN..BPM_MAX
void MetroCycleTimeSig();           // 4 -> 3 -> 2 -> 4
