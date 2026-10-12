// ============================================================================
//  ws2812.h —— WS2812B 驱动接口 + 灯珠电源高边开关控制
// ============================================================================
#pragma once

#include <Arduino.h>
#include "driver/rmt.h"

// ---------------------------------------------------------------- 灯珠电源轨
// 必须在 Ws2812::begin() 之前调用 LED_PowerRailOn()，否则灯珠没有供电。
void LED_PowerRailOn();
void LED_PowerRailOff();
// 返回栅极读回电平：true = 高 = P-MOS 关断（用于自检时确认脚真的被抢回来了）
bool LED_PowerRailReadback();

// ---------------------------------------------------------------- 灯带驱动
class Ws2812 {
public:
    Ws2812(uint8_t pin, uint16_t count, rmt_channel_t channel = RMT_CHANNEL_0);

    bool begin();          // 配置 RMT 并安装驱动、分配缓冲区

    bool  ready() const { return _begun && _items != nullptr; }
    uint16_t count() const { return _count; }

    void    setBrightness(uint8_t b) { _brightness = b; }

    void setPixel(uint16_t i, uint8_t r, uint8_t g, uint8_t b);
    void setAll(uint8_t r, uint8_t g, uint8_t b);
    void clear();

    // 把当前缓冲区刷到灯带。pixels = 0 表示发全部。
    // 返回 rmt_write_items 的结果，ESP_OK(0) = 成功
    esp_err_t show(uint16_t pixels = 0);

private:
    void freeBufs();
    static uint8_t scale(uint8_t v, uint8_t b) {
        return (uint8_t)(((uint16_t)v * (uint16_t)b) >> 8);
    }

    // 单位：20MHz tick = 50ns
    static const uint16_t T0H = 8;    // 0.40us
    static const uint16_t T0L = 17;   // 0.85us
    static const uint16_t T1H = 16;   // 0.80us
    static const uint16_t T1L = 9;    // 0.45us

    uint8_t       _pin;
    uint16_t      _count;
    rmt_channel_t _channel;
    uint8_t      *_buf;
    rmt_item32_t *_items;
    uint8_t       _brightness;
    bool          _begun;
};
