// ============================================================================
//  usb_log.h —— 直连 USB Serial/JTAG 硬件 FIFO 的日志通道
//
//  为什么不直接用一个普通的 Serial.print：
//    arduino-esp32 2.0.x 的 HWCDC::write() 只有在"主机先主动发过数据"
//    （内部 connected 标志被置位）之后才真正往外写，否则一律
//    flushTXBuffer() 静默丢弃。表现就是：板子在正常跑，串口却一个字都没有。
//    这里改成直接操作 USB-Serial-JTAG 外设的 TX/RX FIFO（和 ROM 打印同一套），
//    不做任何连接状态判断，所以日志一定会送到主机；
//    主机不读时最多等 20ms 就丢弃这条，绝不阻塞灯效和按键。
//
//  用法：
//    usbLog::begin();                    // setup() 开头调一次
//    usbLog::printf("hello %d\n", 42);
//
//  注意：本文件不安装任何中断/驱动，因此不要再调用 Serial.begin()。
// ============================================================================
#pragma once

#include <Arduino.h>
#include <stdarg.h>
#include "hal/usb_serial_jtag_ll.h"

namespace usbLog {

// 主机不读时的放弃时间：绝不在这里卡住主循环
static const uint32_t WRITE_TIMEOUT_MS = 20;
static const size_t   LINE_CAP         = 192;

inline void begin() {
    // 和 HWCDC::begin() 一样把 USB 外设拉起来（ROM 其实已经枚举过，这里只是确保配置）
    USB_SERIAL_JTAG.conf0.phy_sel           = 0;
    USB_SERIAL_JTAG.conf0.pad_pull_override = 0;
    USB_SERIAL_JTAG.conf0.dp_pullup         = 1;
    USB_SERIAL_JTAG.conf0.usb_pad_enable    = 1;
    // 不使用中断：清掉并屏蔽全部 USB-Serial-JTAG 中断，
    // 避免出现"中断已触发但没有注册处理函数"的异常
    usb_serial_jtag_ll_disable_intr_mask(USB_SERIAL_JTAG_LL_INTR_MASK);
    usb_serial_jtag_ll_clr_intsts_mask(USB_SERIAL_JTAG_LL_INTR_MASK);
    usb_serial_jtag_ll_txfifo_flush();
}

inline void write(const char *s, size_t n) {
    size_t   off = 0;
    uint32_t t0  = millis();
    while (off < n) {
        if (!usb_serial_jtag_ll_txfifo_writable()) {
            if ((uint32_t)(millis() - t0) > WRITE_TIMEOUT_MS) return;   // 主机没在读，放弃
            delayMicroseconds(50);
            continue;
        }
        int w = usb_serial_jtag_ll_write_txfifo((const uint8_t *)s + off, (uint32_t)(n - off));
        if (w > 0) off += (size_t)w;
        usb_serial_jtag_ll_txfifo_flush();   // 不足 64 字节时也要推出去
    }
}

inline void print(const char *s) { write(s, strlen(s)); }

inline void printf(const char *fmt, ...) {
    char    buf[LINE_CAP];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
    write(buf, (size_t)n);
}

// 返回 -1 表示当前没有待读字节
inline int read() {
    if (!usb_serial_jtag_ll_rxfifo_data_available()) return -1;
    uint8_t b = 0;
    if (usb_serial_jtag_ll_read_rxfifo(&b, 1) != 1) return -1;
    return (int)b;
}

}  // namespace usbLog
