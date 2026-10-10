// ============================================================================
//  buttons.h —— 5 个按键的消抖、长按与连发
//
//  硬件：K1..K5 一端接 GPIO、另一端接 GND，按下为低电平，用芯片内部上拉。
//        GPIO0 / GPIO2 / GPIO8 是 strapping 脚 —— 上电瞬间不要按住。
//
//  事件模型：不再用原工程那套 10ms 定时器中断 + 主循环里 delay(5) 重复读的办法
//  （那种写法每轮最多阻塞 25ms，还会漏掉快速点按）。这里改成：
//    主循环里非阻塞轮询 -> 每个键独立的稳定状态 + 按压起始时间
//    -> 变化时回调
//
//  回调参数：
//    pressed  : true = 按下（含长按触发、连发）；false = 抬起
//    longPress: true = 这条事件是"按住超过 BTN_LONGPRESS_MS"
//    repeat   : true = 这条是"按住不放的连发"（用于预设翻页）
//
//  一次完整按下会产生的事件序列（按住超过 2 秒）：
//    (pressed=1, long=0, rep=0)          <- 刚按下
//    (pressed=1, long=0, rep=1) ...      <- 每 BTN_REPEAT_MS 一条，一直持续到松开
//    (pressed=1, long=1, rep=0)          <- 满 2 秒，只报一次（目前上层不使用）
//    (pressed=0, long=1, rep=0)          <- 抬起（long 表示这次是长按）
// ============================================================================
#pragma once

#include <Arduino.h>

#define BTN_DEBOUNCE_MS     20      // 机械轴抖动量级 <5ms，20ms 足够且不钝
#define BTN_LONGPRESS_MS    2000    // 与原工程 LONG_PRESS_TIME 一致
#define BTN_REPEAT_DELAY_MS 450     // 按住多久后开始连发
#define BTN_REPEAT_MS       140     // 连发间隔

enum ButtonId {
    BTN_K1 = 0,
    BTN_K2 = 1,
    BTN_K3 = 2,
    BTN_K4 = 3,
    BTN_K5 = 4,
};

typedef void (*ButtonCallback)(ButtonId id, bool pressed, bool longPress, bool repeat);

// 初始化引脚并注册回调（回调可以为 nullptr）
void ButtonsInit(ButtonCallback cb);

// 主循环里每次调用，内部非阻塞。调用频率越高响应越准（建议 >=100Hz）
void ButtonsPoll();

bool ButtonsIsDownRaw(uint8_t idx);
inline bool ButtonIsDown(ButtonId id) { return ButtonsIsDownRaw((uint8_t)id); }

// 是否有任何键处于按下状态（用于"有操作就唤醒屏幕"）
bool ButtonsAnyDown();
