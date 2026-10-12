// ============================================================================
//  ws2812.cpp —— WS2812B / WS2812B-2020 驱动（ESP32-C3 内置 RMT 外设）
//
//  为什么不用 Adafruit_NeoPixel / FastLED：
//    用芯片自带的 RMT 硬件产生波形，零外部库依赖、离线也能编译；而且能一次把
//    5 颗灯（120 bit）整帧发完，时序由硬件保证，不怕被 BLE 中断打断。
//
//  时序（WS2812B 家族通用，与 arduino-esp32 core 自带的 neopixelWrite 一致）：
//    RMT 时钟 = APB 80MHz / clk_div 4 = 20MHz -> 1 tick = 50ns
//    逻辑 0：高 0.40us + 低 0.85us
//    逻辑 1：高 0.80us + 低 0.45us
//    每 bit 周期 1.25us；帧尾保持 >50us 低电平即完成锁存
// ============================================================================
#include <Arduino.h>
#include <string.h>
#include "driver/rmt.h"
#include "board_config.h"
#include "ws2812.h"

// ---------------------------------------------------------------- 灯珠电源轨
// P-MOS 高边开关 U6 (AO3401)：源极 3V3、漏极灯珠 VDD、栅极 GPIO21（R17 10K 上拉）
//   -> 栅极拉低才导通
//
// 这里有一个"这块板一定会踩"的坑：
//   GPIO21 同时是 UART0 TXD 的默认引脚。上电时 bootloader 会把该脚的输出通路
//   （GPIO 交换矩阵 FUNC21_OUT_SEL_CFG）接到 UART0 TXD 上，空闲置高。
//   这种状态下只调 pinMode() / gpio_set_direction() 是抢不回来的：
//   pad 仍然由 UART 驱动，固件写 LOW 也拉不下去（实测寄存器读回电平恒为 1），
//   于是 P-MOS 栅极为高 = 关断 -> 灯珠 VDD 一直是 0V -> 5 颗灯全灭。
//   修法：驱动前先 gpio_reset_pin()，把输出信号接回 SIG_GPIO_OUT_IDX。
static inline void railDrive(bool on) {
#if LED_POWER_SWITCH_PRESENT
    gpio_reset_pin((gpio_num_t)LED_POWER_PIN);      // 关键：把 GPIO21 从 UART0 TXD 手里抢回来
    pinMode(LED_POWER_PIN, OUTPUT);
#if LED_POWER_ACTIVE_LOW
    digitalWrite(LED_POWER_PIN, on ? LOW : HIGH);
#else
    digitalWrite(LED_POWER_PIN, on ? HIGH : LOW);
#endif
#else
    (void)on;
#endif
}

void LED_PowerRailOn()  { railDrive(true);  }
void LED_PowerRailOff() { railDrive(false); }

bool LED_PowerRailReadback() {
#if LED_POWER_SWITCH_PRESENT
    return digitalRead(LED_POWER_PIN) == HIGH;      // true = 栅极高 = 关断
#else
    return false;
#endif
}

// ============================================================================
//  Ws2812 类
// ============================================================================
Ws2812::Ws2812(uint8_t pin, uint16_t count, rmt_channel_t channel)
    : _pin(pin), _count(count), _channel(channel),
      _buf(nullptr), _items(nullptr), _brightness(LED_BRIGHTNESS), _begun(false) {}

void Ws2812::freeBufs() {
    if (_buf)   { free(_buf);   _buf   = nullptr; }
    if (_items) { free(_items); _items = nullptr; }
}

bool Ws2812::begin() {
    if (_begun) return true;
    if (_count == 0) return false;

    _buf   = (uint8_t *)calloc((size_t)_count * 3, 1);
    _items = (rmt_item32_t *)calloc((size_t)_count * 24, sizeof(rmt_item32_t));
    if (_buf == nullptr || _items == nullptr) { freeBufs(); return false; }

    rmt_config_t cfg = RMT_DEFAULT_CONFIG_TX((gpio_num_t)_pin, _channel);
    cfg.clk_div             = 4;                        // 80MHz / 4 = 20MHz
    cfg.mem_block_num       = 1;
    cfg.tx_config.carrier_en      = false;
    cfg.tx_config.loop_en         = false;
    cfg.tx_config.idle_output_en  = true;
    cfg.tx_config.idle_level      = RMT_IDLE_LEVEL_LOW; // 空闲保持低，避免灯珠误触发

    if (rmt_config(&cfg) != ESP_OK)                   { freeBufs(); return false; }
    if (rmt_driver_install(_channel, 0, 0) != ESP_OK) { freeBufs(); return false; }

    _begun = true;
    clear();
    return true;
}


void Ws2812::setPixel(uint16_t i, uint8_t r, uint8_t g, uint8_t b) {
    if (_buf == nullptr || i >= _count) return;
    _buf[i * 3 + 0] = r;
    _buf[i * 3 + 1] = g;
    _buf[i * 3 + 2] = b;
}

void Ws2812::setAll(uint8_t r, uint8_t g, uint8_t b) {
    for (uint16_t i = 0; i < _count; i++) setPixel(i, r, g, b);
}

void Ws2812::clear() {
    if (_buf) memset(_buf, 0, (size_t)_count * 3);
}

esp_err_t Ws2812::show(uint16_t pixels) {
    if (!ready()) return ESP_FAIL;

    uint16_t n = (pixels == 0 || pixels > _count) ? _count : pixels;
    uint16_t idx = 0;
    for (uint16_t i = 0; i < n; i++) {
        const uint8_t *px = &_buf[i * 3];           // 缓冲区里的顺序是 R,G,B
        const uint8_t order[3] = { 1, 0, 2 };       // 灯珠要 G,R,B 且高位在前
        for (uint8_t k = 0; k < 3; k++) {
            uint8_t v = scale(px[order[k]], _brightness);
            for (int8_t bit = 7; bit >= 0; bit--) {
                bool one = ((v >> bit) & 0x01) != 0;
                rmt_item32_t *it = &_items[idx++];
                it->level0    = 1;
                it->duration0 = one ? T1H : T0H;
                it->level1    = 0;
                it->duration1 = one ? T1L : T0L;
            }
        }
    }

    esp_err_t err = rmt_write_items(_channel, _items, (int)(n * 24), true);
    delayMicroseconds(80);                          // 帧尾低电平 > 50us -> 锁存
    return err;
}
