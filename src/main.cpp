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
#include <esp_sleep.h>
#include "soc/usb_serial_jtag_struct.h"

#include "BleKeyboard.h"

#include "board_config.h"
#include "usb_log.h"
#include "buttons.h"
#include "display.h"
#include "led_effects.h"
#include "ws2812.h"
#include "buzzer.h"
#include "battery.h"
#include "settings.h"
#include "system.h"

// BLE HID 设备
static BleKeyboard sKeyboard(BLE_DEVICE_NAME, BLE_DEVICE_MANUFACTURER, 100);

// 上一轮的连接状态，用于检测边沿
static bool sWasConnected = false;

// 最近发出的是不是媒体键（Consumer 报文）：是的话释放时也要发一条 Consumer 释放
static bool sMediaHeld = false;

// 自动重连：断开后等一段时间重新开始广播
static uint32_t sDisconnectedAt = 0;
#define RECONNECT_DELAY_MS   3000UL

// ---------------------------------------------------------------------------
//  自动休眠
// ---------------------------------------------------------------------------
// USB 是不是插在电脑上：电脑每 1ms 发一个 SOF 帧，帧号一直在变；只插充电器、电脑睡眠时帧号不动
static bool usbHostPresent() {
    static uint16_t lastFrame  = 0;
    static uint32_t lastChange = 0;
    uint16_t f = USB_SERIAL_JTAG.fram_num.sof_frame_index;
    if (f != lastFrame) { lastFrame = f; lastChange = millis(); }
    return lastChange != 0 && millis() - lastChange < 500;
}

static void enterDeepSleep() {
    usbLog::print("[pwr] 蓝牙 1 分钟没连上、也没人按键 -> 深度睡眠（按 K1 / K2 唤醒）\n");
    BuzzerStop();
    // 屏幕提示一下再关
    DisplaySetPower(true);
    DisplayClear();
    DisplayText(0, 0, "Sleeping...");
    DisplayText(0, 2, "Press K1/K2 to wake");
    DisplayFlush(true);
    delay(1500);
    DisplaySetPower(false);         // SSD1306 进睡眠，约 10uA
    LED_PowerRailOff();             // 灯珠断电；睡眠时 GPIO21 释放，靠板上 10K 上拉保持关断
    // 唤醒脚还按着就先等松开，否则一睡下去马上又被唤醒
    while (digitalRead(BTN1_PIN) == LOW || digitalRead(BTN2_PIN) == LOW) delay(10);
    // 低电平唤醒；K1 用芯片内部上拉（睡眠时由唤醒配置自动保持），K2 板上有 10K 上拉
    esp_deep_sleep_enable_gpio_wakeup((1ULL << BTN1_PIN) | (1ULL << BTN2_PIN), ESP_GPIO_WAKEUP_GPIO_LOW);
    esp_deep_sleep_start();         // 不会返回：醒来是从头重启
}

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
    const bool wokeFromSleep = (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO);
    if (wokeFromSleep) usbLog::print("[pwr] 按键唤醒（从深度睡眠）\n");
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
    // 放在灯效自检之前：begin() 建好 NimBLE 栈就开始广播，自检那 1 秒里主机就能连上。
    // 库内部用 ready 标志保护了 setBatteryLevel / sendKeyReport / isConnected，
    // 在 begin() 之前调用也不会碰到空指针。
    BleKeyboard::setLogger([](const char *line) { usbLog::print(line); });
    sKeyboard.begin();
    // 蓝牙 NUS 串口接到上位机行协议（改键网页 / Claude 状态都能走蓝牙）
    SystemSetBleDump([]() { sKeyboard.dumpStatus(); });
    SystemSetBleSerial([]() { return sKeyboard.serialRead(); },
                       [](const uint8_t *d, size_t n) { return sKeyboard.serialWrite(d, n); });
    sKeyboard.setBatteryLevel(BatteryPercent());
    LED_EffectsSetLink(LED_LINK_OFF);
    usbLog::printf("[ble] advertising as \"%s\"\n", BLE_DEVICE_NAME);

    // ---- 灯效自检：红 -> 绿 -> 蓝 -> 白，确认 5 颗灯、线序、颜色都对 ----
    // 从休眠唤醒时跳过自检和开机音，尽快回连
    if (!wokeFromSleep) LED_EffectsSelfTest();

    SystemNoteActivity();
    if (!wokeFromSleep) BuzzerStartup();   // 开机音：自检之后再响，BuzzerUpdate() 在主循环里推进
    usbLog::print("[sys] ready. 串口敲 ? 看诊断命令\n");
}

// ---------------------------------------------------------------------------
void loop() {
    // 1) 按键采样（非阻塞轮询）
    ButtonsPoll();

    // 2) 串口诊断命令
    SystemHandleSerial();

    // 3) BLE 连接状态
    //    "已连接"以加密成功为准（见 BleKeyboard.cpp），只连上没加密的链路不算。
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
    // 断开回调里会直接重启广播，但那一刻协议栈可能还在收尾上一个连接，偶尔会失败；
    // 失败了就再也搜不到键盘（电脑删掉配对后想重新添加时最容易碰到）。
    // 所以每秒检查一次：没在广播、连接又没满，就重新广播。reAdvertise() 正在广播时什么也不做。
    // 不要再调 begin()，BLE 栈只能初始化一次。
    static uint32_t sLastAdvCheck = 0;
    if (millis() - sLastAdvCheck >= 1000) {
        sLastAdvCheck = millis();
        sKeyboard.reAdvertise();
    }
    if (!connected && sDisconnectedAt != 0 && (int32_t)(millis() - sDisconnectedAt) >= 0) {
        sDisconnectedAt = 0;
        LED_EffectsSetLink(LED_LINK_ADVERTISING);
        SystemSetLink(LED_LINK_ADVERTISING);
    }

#if AUTO_SLEEP_ENABLED
    // 自动休眠：没连蓝牙、USB 没插电脑、也没人按键，满 AUTO_SLEEP_MS 就睡
    static uint32_t sLinkedAt = 0;               // 最近一次"不该睡"的时刻
    if (connected || usbHostPresent()) sLinkedAt = millis();
    uint32_t since = SystemLastActivityMs();
    if ((int32_t)(sLinkedAt - since) > 0) since = sLinkedAt;
    if (millis() - since >= AUTO_SLEEP_MS) enterDeepSleep();
#endif

    // 4) 周期任务
    BatteryUpdate();
    BuzzerUpdate();
    SystemUpdate();

    // 5) 键盘报文发送：按下发按下报文，松开才发释放报文（按住时主机自己连发）
    //    先发释放、再发按下：按住一个键时又按了另一个键，旧键的释放请求和新键的按下会在
    //    同一轮里出现，顺序反了的话新键刚按下就被这条释放取消掉。
    if (connected) {
        if (SystemTakeReleaseRequest()) {
            sKeyboard.sendKeyReport((char *)HID_RELEASE);
            if (sMediaHeld) {
                sKeyboard.sendMediaReport((char *)HID_RELEASE);
                sMediaHeld = false;
            }
        }
        uint8_t k = SystemPeekPendingKey();
        if (k != 0xFF) {
            if (SettingsCurrentPreset().isMedia) {
                sKeyboard.sendMediaReport((char *)SettingsKeyReport(k));
                sMediaHeld = true;
            } else {
                sKeyboard.sendKeyReport((char *)SettingsKeyReport(k));
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
