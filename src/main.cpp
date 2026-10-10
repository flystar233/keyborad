// ============================================================================
//  main.cpp —— ESP32-C3 智能桌面小键盘 (BLE Keybrick)
//
//  这块板（原理图 Schematic1 / Board1 V1.0）的引脚分配：
//      K1..K5     : GPIO0 / GPIO2 / GPIO20 / GPIO7 / GPIO8   (按下拉 GND)
//      WS2812 x5  : GPIO1  -> R13 100R -> LED1.DI，级联到 LED5
//      灯珠电源   : GPIO21 -> AO3401 栅极（低导通高边开关）
//      OLED       : GPIO4 = SDA, GPIO5 = SCL（SSD1306 128x32，硬件 I2C）
//      蜂鸣器     : GPIO10 -> R10 1K -> S9013 基极（高电平响）
//      电池检测   : GPIO3  <- R9 100K/R15 100K 分压（0.5）
//      USB        : GPIO18 = D-, GPIO19 = D+（日志也走这里，见 usb_log.h）
//
//  为什么日志必须走 USB 而不是 UART0：
//      GPIO20(K3) 和 GPIO21(灯珠电源) 正好是 UART0 的默认 RXD/TXD。
//      日志走 UART0 会抢走这两个脚 -> K3 读不到、灯珠永远没电。
// ============================================================================
#include <Arduino.h>

#include "Hid2Ble.h"

#include "board_config.h"
#include "usb_log.h"
#include "buttons.h"
#include "display.h"
#include "led_effects.h"
#include "buzzer.h"
#include "battery.h"
#include "settings.h"
#include "system.h"

// BLE HID 设备
static Hid2Ble sKeyboard(BLE_DEVICE_NAME, BLE_DEVICE_MANUFACTURER, 100);

// 上一轮的连接状态，用于检测边沿
static bool sWasConnected = false;

// 最近发出的是不是媒体键（Consumer 报文）：是的话释放时也要发一条 Consumer 释放
static bool sMediaHeld = false;

// 自动重连：断开后等一段时间重新开始广播
static uint32_t sDisconnectedAt = 0;
#define RECONNECT_DELAY_MS   3000UL

// ---------------------------------------------------------------------------
//  按键事件 -> 交给系统层
// ---------------------------------------------------------------------------
static void onButtonEvent(ButtonId id, bool pressed, bool longPress, bool repeat) {
    SystemHandleButtonEvent(id, pressed, longPress, repeat);
}

// ---------------------------------------------------------------------------
void setup() {
    // 日志通道必须在最前面：后面每一步都要打日志
    usbLog::begin();
    usbLog::print("\n\n=== ESP32-C3 BLE Keybrick booting ===\n");
    usbLog::printf("[sys] chip=%s cores=%d flash=%luKB heap=%lu\n",
                   ESP.getChipModel(), ESP.getChipCores(),
                   (unsigned long)(ESP.getFlashChipSize() / 1024),
                   (unsigned long)ESP.getFreeHeap());

    // ---- 配置持久化（NVS）----
    SettingsInit();

    // ---- 电池 ADC ----
    BatteryInit();
    BatteryUpdate();

    // ---- 蜂鸣器 ----
    BuzzerInit();

    // ---- 灯珠：先给电源轨上电，再跑 RMT 自检 ----
    LED_EffectsInit();
    LED_EffectsSetLink(LED_LINK_OFF);

    // ---- OLED ----
    bool oledOk = DisplayInit();
    usbLog::printf("[oled] %s\n", oledOk ? "OK" : "NOT FOUND (继续跑，不影响键盘)");

    // ---- 按键 ----
    ButtonsInit(onButtonEvent);

    // ---- 系统层 ----
    SystemInit();

    // ---- BLE 键盘 ----
    // 放在灯效自检之前：begin() 只是起一个任务异步搭 BLE 栈，自检那 1 秒
    // 正好让它在后台跑，开机后能更早被连上。
    // 【重要】begin() 只是创建一个 FreeRTOS 任务去异步搭 BLE 栈，
    // hid / inputKeyboard 要等那个任务跑完才有值。所以：
    //   - 库内部用 ready 标志保护了 setBatteryLevel / send2Ble / isConnected，
    //     在 begin() 之前或之后立刻调用都不会再空指针崩溃。
    //   - 灯效的"正在广播"状态要等 isReady() 才切，否则会误导（看着在闪其实还没广播）。
    sKeyboard.begin();
    sKeyboard.setBatteryLevel(BatteryPercent());
    LED_EffectsSetLink(LED_LINK_OFF);
    usbLog::printf("[ble] advertising as \"%s\"\n", BLE_DEVICE_NAME);

    // ---- 灯效自检：红 -> 绿 -> 蓝 -> 白，确认 5 颗灯、线序、颜色都对 ----
    LED_EffectsSelfTest();

    SystemNoteActivity();
    BuzzerStartup();            // 开机音：自检之后再响，BuzzerUpdate() 在主循环里推进
    usbLog::print("[sys] ready. 串口敲 ? 看诊断命令\n");
}

// ---------------------------------------------------------------------------
void loop() {
    // 1) 按键采样（非阻塞轮询）
    ButtonsPoll();

    // 2) 串口诊断命令
    SystemHandleSerial();

    // 3) BLE 连接状态
    //    isReady() 之前 BLE 栈还在另一个任务里搭建，此时 isConnected() 恒为 false。
    static bool sLinkShown = false;
    bool connected = sKeyboard.isConnected();

    if (!connected && !sLinkShown && sKeyboard.isReady()) {
        sLinkShown = true;
        LED_EffectsSetLink(LED_LINK_ADVERTISING);
        SystemSetLink(LED_LINK_ADVERTISING);
        usbLog::print("[ble] stack ready, advertising\n");
    }

    if (connected != sWasConnected) {
        sWasConnected = connected;
        if (connected) {
            LED_EffectsSetLink(LED_LINK_CONNECTED);
            SystemSetLink(LED_LINK_CONNECTED);
            sKeyboard.setBatteryLevel(BatteryPercent());
            BuzzerConnected();
            usbLog::print("[ble] HOST CONNECTED\n");
        } else {
            LED_EffectsSetLink(LED_LINK_ADVERTISING);
            SystemSetLink(LED_LINK_ADVERTISING);
            BuzzerDisconnected();
            // 断开后不要立刻重启广播：有的主机会在断开瞬间马上回连，
            // 这里留 3 秒 + 最多 1.5 秒随机抖动，避开刚断开那一小段。
            sDisconnectedAt = millis() + RECONNECT_DELAY_MS + (esp_random() % 1500);
            usbLog::print("[ble] disconnected\n");
        }
        SystemNoteActivity();
    }

    // 断开后重启广播。
    // 【关键】这里调 reAdvertise() 而不是 begin()：begin() 会再建一个任务并
    // 重新执行 BLEDevice::init() + createServer()，导致重复初始化、内存耗尽。
    if (!connected && sDisconnectedAt != 0 && (int32_t)(millis() - sDisconnectedAt) >= 0) {
        sDisconnectedAt = 0;
        sKeyboard.reAdvertise();
        LED_EffectsSetLink(LED_LINK_ADVERTISING);
        SystemSetLink(LED_LINK_ADVERTISING);
        usbLog::print("[ble] re-advertising\n");
    }

    // 4) 周期任务
    BatteryUpdate();
    BuzzerUpdate();
    SystemUpdate();

    // 5) 键盘报文发送：按下 -> 隔 60ms -> 释放（时序由系统层记账）
    //    先发释放、再发按下：连续快按时，上一个键的释放请求和新键的按下会在同一轮里
    //    出现，顺序反了的话新键刚按下就被这条释放取消掉。
    SystemTick();
    if (connected) {
        if (SystemTakeReleaseRequest()) {
            sKeyboard.send2Ble((char *)HID_RELEASE);
            if (sMediaHeld) {
                sKeyboard.sendMedia2Ble((char *)HID_RELEASE);
                sMediaHeld = false;
            }
        }
        uint8_t k = SystemPeekPendingKey();
        if (k != 0xFF) {
            if (SettingsCurrentPreset().isMedia) {
                sKeyboard.sendMedia2Ble((char *)SettingsKeyReport(k));
                sMediaHeld = true;
            } else {
                sKeyboard.send2Ble((char *)SettingsKeyReport(k));
            }
            SystemCompletePendingKey();
        }
    }

    // 6) 电池电量同步给 BLE HID 的电池服务
    static uint32_t lastBatPush = 0;
    if (connected && (millis() - lastBatPush > 30000UL)) {
        lastBatPush = millis();
        sKeyboard.setBatteryLevel(BatteryPercent());
    }

    // 7) 灯效
    LED_EffectsUpdate();

    // 8) OLED：内容变了才重画 + 刷屏（被限流挡回来会在下一轮重试）
    SystemRefreshDisplay();

    // 让出 CPU 1ms。
    // 【之前的问题】loop() 没有任何等待，CPU 一直 100% 空转在 160MHz，电池供电时白白耗电。
    // 1ms 对按键（20ms 消抖）、灯效（25ms 一帧）都毫无影响，
    // 但 CPU 绝大部分时间可以进 idle，续航明显变长。
    delay(1);
}
