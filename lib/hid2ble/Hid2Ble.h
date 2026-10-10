#ifndef ESP32_BLE_KEYBOARD_H
#define ESP32_BLE_KEYBOARD_H
#include "sdkconfig.h"
#if defined(CONFIG_BT_ENABLED)

#include "BleConnectionStatus.h"
#include "BLEHIDDevice.h"
#include "BLECharacteristic.h"

class Hid2Ble
{
private:
  BleConnectionStatus *connectionStatus;
  BLEHIDDevice *hid;
  BLECharacteristic *inputKeyboard;
  BLECharacteristic *outputKeyboard;
  BLECharacteristic *inputMediaKeys;
  BLECharacteristicCallbacks *callBack;
  static void taskServer(void *pvParameter);
  // BLE 栈是在 taskServer() 里异步创建的（begin() 只负责 xTaskCreate）。
  // 在它跑完之前 hid / inputKeyboard 都还是空指针，任何调用都会崩，
  // 所以这里加一个就绪标志，所有对外接口都先查它。
  // 【本板踩过的坑】在 begin() 之前调 setBatteryLevel() 会在
  // BLEHIDDevice::setBatteryLevel() 里对空指针写值 -> Load access fault (MTVAL=0x28)。
  volatile bool ready;
  // 保存服务器指针，供断线后重启广播使用。
  // 注意：断线重连绝不能重复调用 begin() —— begin() 会再建一个任务，
  // 再跑一遍 BLEDevice::init() + createServer()，造成重复初始化和内存耗尽。
  BLEServer *server;

public:
  /**
    * 构造函数
    */
  Hid2Ble(std::string deviceName = "ESP32 BLE Keyboard", std::string deviceManufacturer = "Espressif", uint8_t batteryLevel = 100);
  void begin(void);
  void end(void);
  void send2Ble(char *keys);
  void sendMedia2Ble(char *keys);
  bool isConnected(void);
  /**
   * BLE 栈是否已经建好（可以安全调用 send2Ble / setBatteryLevel）
   */
  bool isReady(void);
  /**
   * 重启广播（断线重连用）。只重发广播包，不会重建服务。
   */
  void reAdvertise(void);
  /**
   * 设置电池电量
  */
  void setBatteryLevel(uint8_t level);

  void setCallBack(BLECharacteristicCallbacks *callBack);
  /**
   * 电池电量
   */ 
  uint8_t batteryLevel;
  /**
   * 设备制造商名称
   */ 
  std::string deviceManufacturer;
  /**
   * 设备名称
   */ 
  std::string deviceName;

protected:
  virtual void onStarted(BLEServer *pServer){};
};

#endif
#endif
