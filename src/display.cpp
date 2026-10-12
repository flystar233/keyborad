// ============================================================================
//  display.cpp —— SSD1306 128×32 OLED 驱动（ESP32-C3 硬件 I2C）
// ============================================================================
#include <Arduino.h>
#include <Wire.h>
#include <string.h>
#include "board_config.h"
#include "display.h"
#include "display_font.h"
#include "display_icons.h"
#include "usb_log.h"

#define PAGES            (OLED_HEIGHT / 8)          // 32/8 = 4
#define FB_SIZE          (OLED_WIDTH * PAGES)       // 128*4 = 512

#define OLED_CONTRAST_NORMAL  0xCF
#define OLED_CONTRAST_DIM     0x01

static uint8_t  sFb[FB_SIZE];                       // 帧缓冲：每页连续 128 字节
static bool     sPresent   = false;
static bool     sPowerOn   = true;
static bool     sDim       = false;
static uint32_t sLastFlush = 0;

// ---------------------------------------------------------------- I2C 低层
static bool writeCmd(uint8_t cmd) {
    Wire.beginTransmission(OLED_I2C_ADDR);
    Wire.write((uint8_t)0x00);                      // 控制字节：后面是命令
    Wire.write(cmd);
    return Wire.endTransmission() == 0;
}

static bool writeCmds(const uint8_t *cmds, size_t n) {
    Wire.beginTransmission(OLED_I2C_ADDR);
    Wire.write((uint8_t)0x00);
    for (size_t i = 0; i < n; i++) Wire.write(cmds[i]);
    return Wire.endTransmission() == 0;
}

// ---------------------------------------------------------------- 初始化
bool DisplayInit() {
    Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN, OLED_I2C_HZ);
    delay(50);

    // 探测一下：没应答就别再往下浪费时间
    Wire.beginTransmission(OLED_I2C_ADDR);
    sPresent = (Wire.endTransmission() == 0);
    if (!sPresent) {
        usbLog::printf("[oled] no ACK at 0x%02X (SDA=GPIO%d SCL=GPIO%d) - 面板未接?\n",
                       OLED_I2C_ADDR, OLED_SDA_PIN, OLED_SCL_PIN);
        return false;
    }

    // SSD1306 128x32 初始化序列（照原工程 oled.c 的序列，改成一次性批量下发）
    const uint8_t init[] = {
        0xAE,               // 关闭显示
        0xD5, 0x80,         // 时钟分频比 / 振荡器频率
        0xA8, 0x1F,         // 驱动路数 = 32 (0x1F + 1)
        0xD3, 0x00,         // 显示偏移
        0x40,               // 显示起始行 = 0
        0xA1,               // 段重映射（左右方向）
        0xC8,               // COM 扫描方向（上下方向）
        0xDA, 0x02,         // COM 硬件配置：128x32 用 0x02
        0x81, OLED_CONTRAST_NORMAL,   // 对比度
        0xD9, 0xF1,         // 预充电周期
        0xDB, 0x30,         // VCOMH 反压
        0xA4,               // 输出跟随 RAM 内容
        0xA6,               // 正常显示（非反色）
        0x8D, 0x14,         // 使能电荷泵
        0x20, 0x00,         // 内存寻址模式 = 水平寻址（配合整屏一次性写入）
        0x21, 0x00, 0x7F,   // 列地址 0..127
        0x22, 0x00, 0x03,   // 页地址 0..3
        0xAF,               // 开启显示
    };
    if (!writeCmds(init, sizeof(init))) {
        usbLog::print("[oled] init sequence failed\n");
        sPresent = false;
        return false;
    }

    memset(sFb, 0, sizeof(sFb));
    DisplayFlush();

    usbLog::printf("[oled] SSD1306 128x%d ready, addr 0x%02X, SDA=GPIO%d SCL=GPIO%d @%dkHz\n",
                   OLED_HEIGHT, OLED_I2C_ADDR, OLED_SDA_PIN, OLED_SCL_PIN, OLED_I2C_HZ / 1000);
    return true;
}

bool DisplayPresent() { return sPresent; }

// ---------------------------------------------------------------- 帧缓冲绘制
void DisplayClear() {
    memset(sFb, 0, sizeof(sFb));
}


// 单个字符：6 列，低位在上（bit0 = 页内最上面的像素）
static void drawChar(uint8_t x, uint8_t page, uint8_t ch) {
    uint8_t idx;
    if (ch < FONT_FIRST || ch > FONT_LAST) ch = '?';
    idx = (uint8_t)(ch - FONT_FIRST);

    for (uint8_t col = 0; col < FONT_W; col++) {
        uint8_t px = (uint8_t)(x + col);
        if (px >= OLED_WIDTH) return;
        sFb[page * OLED_WIDTH + px] = FONT6X8[idx][col];
    }
}

void DisplayText(uint8_t x, uint8_t page, const char *str) {
    if (str == nullptr || page >= PAGES) return;
    while (*str) {
        if (x + FONT_W > OLED_WIDTH) return;        // 不换行，越界直接截断
        drawChar(x, page, (uint8_t)*str);
        x = (uint8_t)(x + FONT_W);
        str++;
    }
}


// ---------------------------------------------------------------- 图元 / 图标
void DisplayPixel(uint8_t x, uint8_t y) {
    if (x >= OLED_WIDTH || y >= (PAGES * 8)) return;
    sFb[(y / 8) * OLED_WIDTH + x] |= (uint8_t)(1u << (y & 7));
}

void DisplayFillRect(uint8_t x, uint8_t y, uint8_t w, uint8_t h) {
    for (uint8_t dy = 0; dy < h; dy++) {
        for (uint8_t dx = 0; dx < w; dx++) {
            DisplayPixel((uint8_t)(x + dx), (uint8_t)(y + dy));
        }
    }
}

// 画一幅"列优先页格式"位图：每列 1 字节，bit0 = 最上像素。
// 这与 SSD1306 帧缓冲的排布一致，所以可以直接按页写。
// 仅适用于高度 <= 8 的图标（本工程的两个图标都是 8x8）。
static void drawBitmap8(uint8_t x, uint8_t y, uint8_t w, uint8_t h, const uint8_t *data) {
    for (uint8_t col = 0; col < w; col++) {
        uint8_t bits = data[col];
        for (uint8_t row = 0; row < h; row++) {
            if ((bits >> row) & 1u) {
                DisplayPixel((uint8_t)(x + col), (uint8_t)(y + row));
            }
        }
    }
}

// 电池图标：u8g2 官方 8x8 字形查表（5 档，见 display_icons.h）。
//   percent < 0        -> 0% 档的空心外框（"没装电池"，不做任何猜测）
//   percent 0..100     -> 按 5 档取整
void DisplayIconBattery(uint8_t x, uint8_t y, int percent) {
    uint8_t lvl = 0;
    if (percent > 0) {
        if (percent > 100) percent = 100;
        // 0..100 映射到 0..(LEVELS-1)，四舍五入
        lvl = (uint8_t)((percent * (ICON_BAT_LEVELS - 1) + 50) / 100);
        if (lvl >= ICON_BAT_LEVELS) lvl = ICON_BAT_LEVELS - 1;
    }
    drawBitmap8(x, y, ICON_BAT_W, ICON_BAT_H, ICON_BAT[lvl]);
}

// 蓝牙图标：u8g2 官方 8x8 bluetooth.png 查表。
//   已连接 -> 画图标 + 右侧一个实心小方块（一眼区分"连上了"）
//   配对中 -> 只画图标
//   未启动 -> 不画（留空，表示没有蓝牙）
void DisplayIconBluetooth(uint8_t x, uint8_t y, BtIconState st) {
    if (st == BT_ICON_OFF) return;
    drawBitmap8(x, y, ICON_BT_W, ICON_BT_H, ICON_BT);
    if (st == BT_ICON_CONNECTED) {
        // 图标右侧 2x2 实心点：已连接
        for (uint8_t dx = 0; dx < 2; dx++) {
            for (uint8_t dy = 0; dy < 2; dy++) {
                DisplayPixel((uint8_t)(x + ICON_BT_W + dx),
                             (uint8_t)(y + 3 + dy));
            }
        }
    }
}

// ---------------------------------------------------------------- 刷屏
bool DisplayFlush(bool force) {
    if (!sPresent || !sPowerOn) return false;

    uint32_t now = millis();
    if (!force && now - sLastFlush < OLED_FLUSH_INTERVAL_MS) return false;
    sLastFlush = now;

    // 【画面每刷一次就往左挪的根因】
    //   以前每个事务是 1 个控制字节 + 128 个数据字节 = 129 字节，但 Arduino Wire 的
    //   发送缓冲只有 I2C_BUFFER_LENGTH = 128 字节，满了 Wire.write() 直接返回 0、
    //   不报错 —— 每个事务的最后一个数据字节被悄悄丢掉，一次刷屏只发出 508 字节。
    //   SSD1306 的写指针因此每刷一次少走 4 列，下一帧整体错位 4 列，
    //   按一次键刷几次屏，画面就往左"爬"一截；每页最右一列也从来没被写到。
    //
    // 两道防线：
    //   1) 每次刷屏前重新设一次列/页地址窗口，把写指针拉回 (0,0)。
    //      以后就算哪次 I2C 出错只发了半帧，下一帧也会从原点重新对齐，不会累积错位。
    //   2) 每个事务只装 FLUSH_CHUNK 个数据字节，远小于 Wire 缓冲；
    //      并检查 Wire.write() 的返回值，写不进去就当失败处理（调用方会重试）。
    static const uint8_t setWindow[] = {
        0x21, 0x00, OLED_WIDTH - 1,                 // 列地址 0..127
        0x22, 0x00, PAGES - 1,                      // 页地址 0..3
    };
    if (!writeCmds(setWindow, sizeof(setWindow))) return false;

    const uint16_t FLUSH_CHUNK = 32;                // 1 + 32 = 33 字节/事务
    for (uint16_t off = 0; off < FB_SIZE; off += FLUSH_CHUNK) {
        Wire.beginTransmission(OLED_I2C_ADDR);
        Wire.write((uint8_t)0x40);                  // 控制字节：后面是 GDDRAM 数据
        if (Wire.write(&sFb[off], FLUSH_CHUNK) != FLUSH_CHUNK) {
            Wire.endTransmission();
            return false;
        }
        if (Wire.endTransmission() != 0) return false;
    }
    return true;
}

// ---------------------------------------------------------------- 亮度 / 开关
void DisplaySetDim(bool dim) {
    if (!sPresent || dim == sDim) return;
    sDim = dim;
    writeCmd(0x81);
    writeCmd(dim ? OLED_CONTRAST_DIM : OLED_CONTRAST_NORMAL);
}

void DisplaySetPower(bool on) {
    if (!sPresent) { sPowerOn = on; return; }
    if (on == sPowerOn) return;
    sPowerOn = on;
    writeCmd(on ? 0xAF : 0xAE);
    if (on) sLastFlush = 0;                         // 重新点亮时立刻刷一次
}

bool DisplayIsOn() { return sPowerOn; }

bool DisplayIsDim() { return sDim; }

// 重新在总线上 ping 一次：I2C 上到底有没有器件响应。
bool DisplayPing() {
    Wire.beginTransmission(OLED_I2C_ADDR);
    bool ack = (Wire.endTransmission() == 0);
    sPresent = ack;
    return ack;
}

// 扫描整条 I2C 总线，把有应答的地址打印出来。
// 用来区分"面板没插好"和"引脚/地址不对"：如果 0x78 没应答但别的地址有，
// 说明总线是通的，只是器件在别的地址；如果一个都没有，基本就是接线/供电问题。
void DisplayScanBus() {
    usbLog::printf("[i2c] scanning SDA=GPIO%d SCL=GPIO%d ...\n", OLED_SDA_PIN, OLED_SCL_PIN);
    uint8_t found = 0;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
            usbLog::printf("       found device at 0x%02X\n", addr);
            found++;
        }
        delay(2);
    }
    usbLog::printf("[i2c] scan done, %u device(s) found\n", (unsigned)found);
    if (found == 0) {
        usbLog::print("       总线上一片死寂 -> 检查排针 3V3 / GND / SDA / SCL 是否插反或虚焊\n");
    }
}
