// ============================================================================
//  buzzer.cpp —— 蜂鸣器实现（LEDC 硬件 PWM）
// ============================================================================
#include <Arduino.h>
#include "board_config.h"
#include "buzzer.h"
#include "usb_log.h"

#if BUZZER_ENABLED
static uint8_t  sChannel  = 0;
static bool     sReady    = false;
static uint32_t sStopAt   = 0;      // 0 = 不自动停
static bool     sBeeping  = false;
#endif

bool BuzzerEnabled() {
#if BUZZER_ENABLED
    return sReady;
#else
    return false;
#endif
}

void BuzzerInit() {
#if BUZZER_ENABLED
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);          // 默认静音

    // LEDC: 2.7kHz 方波，8 位分辨率足够（占空比固定 50% 最响）
    sChannel = 0;
    if (ledcSetup(sChannel, BUZZER_FREQ, 8)) {
        ledcAttachPin(BUZZER_PIN, sChannel);
        ledcWrite(sChannel, 0);             // 占空比 0 -> 不响
        sReady = true;
        usbLog::printf("[buz] GPIO%d ready @%dHz (LEDC ch%d)\n", BUZZER_PIN, BUZZER_FREQ, sChannel);
    } else {
        usbLog::print("[buz] LEDC setup FAILED\n");
    }
#endif
}

void BuzzerTone(uint32_t freqHz) {
#if BUZZER_ENABLED
    if (!sReady) return;
    if (freqHz == 0) { BuzzerStop(); return; }
    ledcWriteTone(sChannel, freqHz);
    ledcWrite(sChannel, 128);               // 50% 占空比
    sBeeping = true;
    sStopAt  = 0;
#else
    (void)freqHz;
#endif
}

void BuzzerStop() {
#if BUZZER_ENABLED
    if (!sReady) return;
    ledcWrite(sChannel, 0);
    sBeeping = false;
    sStopAt  = 0;
#endif
}

void BuzzerBeep(uint32_t freqHz, uint32_t ms) {
#if BUZZER_ENABLED
    if (!sReady || ms == 0) return;
    ledcWriteTone(sChannel, freqHz);
    ledcWrite(sChannel, 128);
    sBeeping = true;
    sStopAt  = millis() + ms;
    if (sStopAt == 0) sStopAt = 1;          // 避免正好是 0 被当成"不自动停"
#else
    (void)freqHz; (void)ms;
#endif
}

void BuzzerUpdate() {
#if BUZZER_ENABLED
    if (sBeeping && sStopAt != 0 && (int32_t)(millis() - sStopAt) >= 0) {
        BuzzerStop();
    }
#endif
}

// ---------------------------------------------------------------- 常用提示音
// 这些"音效"都用 Beep 串起来，但不在内部 delay —— delay 会卡住灯效和按键。
// 需要连响的地方交给 System 层用状态机排，这里只提供单音。
void BuzzerClick()      { BuzzerBeep(3200, 12); }
void BuzzerOk()         { BuzzerBeep(1800, 70);  }
void BuzzerError()      { BuzzerBeep(300,  180); }
void BuzzerModeChange() { BuzzerBeep(1200, 60);  }

void BuzzerBeatAccent() { BuzzerBeep(2000, 35);  }
void BuzzerBeatNormal() { BuzzerBeep(1400, 25);  }
