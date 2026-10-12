#ifndef ESP32_BLE_KEYBOARD_H
#define ESP32_BLE_KEYBOARD_H
// ============================================================================
//  BleKeyboard —— BLE HID 键盘（键盘报告 + 媒体键报告 + 电池电量）
//
//  协议栈：NimBLE（h2zero/NimBLE-Arduino 2.x），不再用 Arduino 自带的 Bluedroid。
//    - 更省内存和 Flash
//    - 配对 / 绑定信息存在 NVS，键盘重启、电脑睡眠唤醒后自动回连，不用删配对
//    - 断开后协议栈自动重新广播
//
//  "已连接"以加密成功为准：主机配对失败反复重连时不会报连接。
//  诊断日志（连接 / 断开原因 / 配对结果）通过 setLogger() 输出。
// ============================================================================
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>
#include <string>
#include <freertos/FreeRTOS.h>
#include <freertos/stream_buffer.h>

class BleKeyboard
{
public:
  BleKeyboard(std::string deviceName = "ESP32 BLE Keyboard", std::string deviceManufacturer = "Espressif", uint8_t batteryLevel = 100);

  void begin(void);
  /** 发 8 字节键盘报告：[修饰键, 保留, 键1..键6] */
  void sendKeyReport(char *keys);
  /** 发 2 字节媒体键报告（16 位位图，顺序见 BleKeyboard.cpp 的报告描述符） */
  void sendMediaReport(char *keys);
  bool isConnected(void);
  /** BLE 栈是否已经建好（可以安全调用 sendKeyReport / setBatteryLevel） */
  bool isReady(void);
  /** 重启广播（断线重连用）。只重发广播包，不会重建服务；正在广播时什么也不做。 */
  void reAdvertise(void);
  /** 设置电池电量（0~100），已连接时会通知主机 */
  void setBatteryLevel(uint8_t level);
  /** 诊断日志输出（连接、断开原因、配对结果），不设就不输出 */
  typedef void (*LogFn)(const char *line);
  static void setLogger(LogFn fn);

  // ---- NUS 蓝牙串口（Nordic UART Service）：和 USB 串口一样收发文本行 ----
  /** 取一个收到的字节，没有返回 -1。只在主循环里调用 */
  int serialRead(void);
  /** 发数据给订阅了 NUS 的主机（自动按 MTU 分包），返回实际发出的字节数 */
  size_t serialWrite(const uint8_t *data, size_t len);
  /** 把蓝牙状态（连接、加密、广播、NUS 订阅）打到诊断日志里 */
  void dumpStatus(void);

  uint8_t batteryLevel;
  std::string deviceManufacturer;
  std::string deviceName;


private:
  class ServerCallbacks;
  class NusRxCallbacks;
  class NusTxCallbacks;
  void startAdvertising(void);

  NimBLEServer *server = nullptr;
  NimBLEHIDDevice *hid = nullptr;
  NimBLECharacteristic *inputKeyboard = nullptr;
  NimBLECharacteristic *inputMediaKeys = nullptr;
  // 所有对外接口都先查它：begin() 之前调用 setBatteryLevel() 等函数不能碰空指针
  volatile bool ready = false;
  volatile bool connected = false;
  volatile uint8_t encryptedLinks = 0;   // 已加密的连接数（可能同时连着电脑和网页）
  NimBLECharacteristic *nusTx = nullptr;
  StreamBufferHandle_t nusRx = nullptr;
  volatile uint16_t nusConn = BLE_HS_CONN_HANDLE_NONE;   // 订阅了 NUS 的连接
};

#endif
