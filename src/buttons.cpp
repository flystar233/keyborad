// ============================================================================
//  buttons.cpp —— 5 个按键的消抖、长按与连发实现
// ============================================================================
#include <Arduino.h>
#include "board_config.h"
#include "buttons.h"
#include "usb_log.h"

static const uint8_t sPins[BUTTON_COUNT] = {
    BTN1_PIN, BTN2_PIN, BTN3_PIN, BTN4_PIN, BTN5_PIN
};

typedef struct {
    uint8_t  stable;        // 已消抖的稳定电平：0=按下 1=抬起
    uint8_t  raw;           // 上次采样电平
    uint32_t rawChangeMs;   // raw 最后一次变化的时刻
    uint32_t downMs;        // 稳定按下开始的时刻
    uint32_t nextRepeatMs;  // 下一次连发的时刻
    bool     longFired;     // 这次按住期间是否已经报过长按
} BtnState;

static BtnState       sBtn[BUTTON_COUNT];
static ButtonCallback sCb = nullptr;

void ButtonsInit(ButtonCallback cb) {
    sCb = cb;

    for (uint8_t i = 0; i < BUTTON_COUNT; i++) {
        pinMode(sPins[i], INPUT_PULLUP);
        sBtn[i].stable       = 1;
        sBtn[i].raw          = 1;
        sBtn[i].rawChangeMs  = millis();
        sBtn[i].downMs       = 0;
        sBtn[i].nextRepeatMs = 0;
        sBtn[i].longFired    = false;
    }

    usbLog::printf("[btn] K1..K5 = GPIO%d,%d,%d,%d,%d (INPUT_PULLUP)\n",
                   BTN1_PIN, BTN2_PIN, BTN3_PIN, BTN4_PIN, BTN5_PIN);
}

void ButtonsPoll() {
    uint32_t now = millis();

    for (uint8_t i = 0; i < BUTTON_COUNT; i++) {
        uint8_t v = (uint8_t)digitalRead(sPins[i]);

        // ---- 电平抖动中：重新计时 ----
        if (v != sBtn[i].raw) {
            sBtn[i].raw         = v;
            sBtn[i].rawChangeMs = now;
            continue;
        }

        // ---- 稳定超过消抖时间 -> 状态翻转，产生事件 ----
        if (v != sBtn[i].stable && (now - sBtn[i].rawChangeMs) >= BTN_DEBOUNCE_MS) {
            sBtn[i].stable = v;

            if (v == 0) {                               // 按下
                sBtn[i].downMs       = now;
                sBtn[i].longFired    = false;
                sBtn[i].nextRepeatMs = now + BTN_REPEAT_DELAY_MS;
                if (sCb) sCb((ButtonId)i, true, false, false);
            } else {                                    // 抬起
                if (sCb) sCb((ButtonId)i, false, sBtn[i].longFired, false);
                sBtn[i].downMs       = 0;
                sBtn[i].nextRepeatMs = 0;
                sBtn[i].longFired    = false;
            }
            continue;
        }

        if (sBtn[i].stable != 0) continue;              // 没按下，后面都不用管

        // ---- 按住不放：连发 ----
        if (sBtn[i].nextRepeatMs != 0 && (int32_t)(now - sBtn[i].nextRepeatMs) >= 0) {
            sBtn[i].nextRepeatMs = now + BTN_REPEAT_MS;
            if (sCb) sCb((ButtonId)i, true, false, true);
        }

        // ---- 按住满长按时间：只报一次 ----
        if (!sBtn[i].longFired && sBtn[i].downMs != 0 &&
            (now - sBtn[i].downMs) >= BTN_LONGPRESS_MS) {
            sBtn[i].longFired    = true;               // 长按事件只报一次，连发照常继续
            if (sCb) sCb((ButtonId)i, true, true, false);
        }
    }
}

bool ButtonsIsDownRaw(uint8_t idx) {
    if (idx >= BUTTON_COUNT) return false;
    return sBtn[idx].stable == 0;
}

