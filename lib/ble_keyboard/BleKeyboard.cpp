// ============================================================================
//  BleKeyboard —— 基于 NimBLE 的 BLE HID 键盘，接口说明见 BleKeyboard.h
// ============================================================================
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>
#include "HIDTypes.h"
#include "BleKeyboard.h"
#include <stdarg.h>
#include <Preferences.h>
#include "nimble/nimble/host/services/gatt/include/services/gatt/ble_svc_gatt.h"

// 诊断日志：库本身不依赖工程的日志实现，由 setLogger() 接进来
static BleKeyboard::LogFn sLog = nullptr;
static void blog(const char *fmt, ...)
{
	if (!sLog) return;
	char buf[128];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	sLog(buf);
}

// Report IDs:
#define KEYBOARD_ID 0x01
#define MEDIA_KEYS_ID 0x02

static const uint8_t _hidReportDescriptor[] = {
	USAGE_PAGE(1), 0x01, // USAGE_PAGE (Generic Desktop Ctrls)
	USAGE(1), 0x06,		 // USAGE (Keyboard)
	COLLECTION(1), 0x01, // COLLECTION (Application)
	// ------------------------------------------------- Keyboard
	REPORT_ID(1), KEYBOARD_ID, //   REPORT_ID (1)
	USAGE_PAGE(1), 0x07,	   //   USAGE_PAGE (Kbrd/Keypad)
	USAGE_MINIMUM(1), 0xE0,	   //   USAGE_MINIMUM (0xE0)
	USAGE_MAXIMUM(1), 0xE7,	   //   USAGE_MAXIMUM (0xE7)
	LOGICAL_MINIMUM(1), 0x00,  //   LOGICAL_MINIMUM (0)
	LOGICAL_MAXIMUM(1), 0x01,  //   Logical Maximum (1)
	REPORT_SIZE(1), 0x01,	   //   REPORT_SIZE (1)
	REPORT_COUNT(1), 0x08,	   //   REPORT_COUNT (8)
	HIDINPUT(1), 0x02,		   //   INPUT (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
	REPORT_COUNT(1), 0x01,	   //   REPORT_COUNT (1) ; 1 byte (Reserved)
	REPORT_SIZE(1), 0x08,	   //   REPORT_SIZE (8)
	HIDINPUT(1), 0x01,		   //   INPUT (Const,Array,Abs,No Wrap,Linear,Preferred State,No Null Position)
	REPORT_COUNT(1), 0x05,	   //   REPORT_COUNT (5) ; 5 bits (Num lock, Caps lock, Scroll lock, Compose, Kana)
	REPORT_SIZE(1), 0x01,	   //   REPORT_SIZE (1)
	USAGE_PAGE(1), 0x08,	   //   USAGE_PAGE (LEDs)
	USAGE_MINIMUM(1), 0x01,	   //   USAGE_MINIMUM (0x01) ; Num Lock
	USAGE_MAXIMUM(1), 0x05,	   //   USAGE_MAXIMUM (0x05) ; Kana
	HIDOUTPUT(1), 0x02,		   //   OUTPUT (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
	REPORT_COUNT(1), 0x01,	   //   REPORT_COUNT (1) ; 3 bits (Padding)
	REPORT_SIZE(1), 0x03,	   //   REPORT_SIZE (3)
	HIDOUTPUT(1), 0x01,		   //   OUTPUT (Const,Array,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
	REPORT_COUNT(1), 0x06,	   //   REPORT_COUNT (6) ; 6 bytes (Keys)
	REPORT_SIZE(1), 0x08,	   //   REPORT_SIZE(8)
	LOGICAL_MINIMUM(1), 0x00,  //   LOGICAL_MINIMUM(0)
	LOGICAL_MAXIMUM(1), 0x65,  //   LOGICAL_MAXIMUM(0x65) ; 101 keys
	USAGE_PAGE(1), 0x07,	   //   USAGE_PAGE (Kbrd/Keypad)
	USAGE_MINIMUM(1), 0x00,	   //   USAGE_MINIMUM (0)
	USAGE_MAXIMUM(1), 0x65,	   //   USAGE_MAXIMUM (0x65)
	HIDINPUT(1), 0x00,		   //   INPUT (Data,Array,Abs,No Wrap,Linear,Preferred State,No Null Position)
	END_COLLECTION(0),		   // END_COLLECTION
	// ------------------------------------------------- Media Keys
	USAGE_PAGE(1), 0x0C,		 // USAGE_PAGE (Consumer)
	USAGE(1), 0x01,				 // USAGE (Consumer Control)
	COLLECTION(1), 0x01,		 // COLLECTION (Application)
	REPORT_ID(1), MEDIA_KEYS_ID, //   REPORT_ID (3)
	USAGE_PAGE(1), 0x0C,		 //   USAGE_PAGE (Consumer)
	LOGICAL_MINIMUM(1), 0x00,	 //   LOGICAL_MINIMUM (0)
	LOGICAL_MAXIMUM(1), 0x01,	 //   LOGICAL_MAXIMUM (1)
	REPORT_SIZE(1), 0x01,		 //   REPORT_SIZE (1)
	REPORT_COUNT(1), 0x10,		 //   REPORT_COUNT (16)
	USAGE(1), 0xB5,				 //   USAGE (Scan Next Track)     ; bit 0: 1
	USAGE(1), 0xB6,				 //   USAGE (Scan Previous Track) ; bit 1: 2
	USAGE(1), 0xB7,				 //   USAGE (Stop)                ; bit 2: 4
	USAGE(1), 0xCD,				 //   USAGE (Play/Pause)          ; bit 3: 8
	USAGE(1), 0xE2,				 //   USAGE (Mute)                ; bit 4: 16
	USAGE(1), 0xE9,				 //   USAGE (Volume Increment)    ; bit 5: 32
	USAGE(1), 0xEA,				 //   USAGE (Volume Decrement)    ; bit 6: 64
	USAGE(2), 0x23, 0x02,		 //   Usage (WWW Home)            ; bit 7: 128
	USAGE(2), 0x94, 0x01,		 //   Usage (My Computer) ; bit 0: 1
	USAGE(2), 0x92, 0x01,		 //   Usage (Calculator)  ; bit 1: 2
	USAGE(2), 0x2A, 0x02,		 //   Usage (WWW fav)     ; bit 2: 4
	USAGE(2), 0x21, 0x02,		 //   Usage (WWW search)  ; bit 3: 8
	USAGE(2), 0x26, 0x02,		 //   Usage (WWW stop)    ; bit 4: 16
	USAGE(2), 0x24, 0x02,		 //   Usage (WWW back)    ; bit 5: 32
	USAGE(2), 0x83, 0x01,		 //   Usage (Media sel)   ; bit 6: 64
	USAGE(2), 0x8A, 0x01,		 //   Usage (Mail)        ; bit 7: 128
	HIDINPUT(1), 0x02,			 //   INPUT (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
	END_COLLECTION(0),			 // END_COLLECTION


// Additional report map for Brightness control

	// USAGE_PAGE(1), 0x80,        // USAGE_PAGE (Monitor Control)
	// USAGE(1), 0x01,             // USAGE (Monitor Brightness)
	// COLLECTION(1), 0x01,        // COLLECTION (Application)
	// REPORT_ID(1), 0x03,         //   REPORT_ID (3)
	// LOGICAL_MINIMUM(1), 0x00,   //   LOGICAL_MINIMUM (0)
	// LOGICAL_MAXIMUM(1), 0xFF,   //   LOGICAL_MAXIMUM (255)
	// REPORT_SIZE(1), 0x08,       //   REPORT_SIZE (8)
	// REPORT_COUNT(1), 0x01,      //   REPORT_COUNT (1)
	// USAGE(1), 0x6F,             //   USAGE (Brightness Up)
	// USAGE(1), 0x70,             //   USAGE (Brightness Down)
	// HIDINPUT(1), 0x02,          //   INPUT (Data,Var,Abs)
	// END_COLLECTION(0)           // END_COLLECTION

};

// ---------------------------------------------------------------- NUS 蓝牙串口
// Nordic UART Service：业界通用的"BLE 串口"。主机往 RX 写，键盘从 TX 通知回去。
// 读写都要求加密：只有配对过的主机能发命令（未配对的主机一访问就会触发配对）。
static const char *NUS_SERVICE = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
static const char *NUS_RX      = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";
static const char *NUS_TX      = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E";
static const size_t NUS_RX_BUF = 1024;

// 电脑会缓存已绑定设备的服务列表（GATT 缓存），固件加了服务它看不到；Windows 上浏览器
// （Web Bluetooth）强制重新查询这个 HID 设备时还可能失败，把缓存弄残（只剩部分服务）。所以：
//   - 每次已绑定的主机加密连上，都对 NUS 这一段发 Service Changed，让它重新发现 NUS。
//     只刷 NUS 这一段，不碰 HID 部分，不影响打字。
//   - 服务布局变了（GATT_LAYOUT_VERSION 和 NVS 里记的不一样）时，对整张表发一次。
// GATT 服务布局版本：增删服务 / 特征值时 +1。
//   1 = HID + 电池 + 设备信息   2 = 加了 NUS 蓝牙串口   3 = 强制所有主机整表刷新一次（修复残缺的缓存）
static const uint8_t GATT_LAYOUT_VERSION = 3;
static uint16_t sNusHandle = 0;   // NUS 服务的起始句柄，GATT 服务启动后才有

static void notifyGattChanged(const NimBLEConnInfo &connInfo)
{
	const uint8_t *a = connInfo.getIdAddress().getVal();
	char key[13];
	snprintf(key, sizeof(key), "%02x%02x%02x%02x%02x%02x", a[5], a[4], a[3], a[2], a[1], a[0]);
	Preferences prefs;
	bool full = true;
	if (prefs.begin("ble_gatt", false)) {
		full = prefs.getUChar(key, 0) != GATT_LAYOUT_VERSION;
		if (full) prefs.putUChar(key, GATT_LAYOUT_VERSION);
		prefs.end();
	}
	if (full || sNusHandle == 0) {
		ble_svc_gatt_changed(0x0001, 0xFFFF);
		blog("[ble] service changed (all) -> %s\n", key);
	} else {
		ble_svc_gatt_changed(sNusHandle, 0xFFFF);   // NUS 是最后建的服务，后面没有别的
		blog("[ble] service changed (nus 0x%04X-) -> %s\n", sNusHandle, key);
	}
}

// 广播间隔（单位 0.625ms）：没人连时快一点，方便回连；已连接时慢一点，只为让网页 / 脚本还能搜到
static const uint16_t ADV_FAST_MIN = 160, ADV_FAST_MAX = 240;    // 100~150ms
static const uint16_t ADV_SLOW_MIN = 1600, ADV_SLOW_MAX = 2400;  // 1~1.5s

class BleKeyboard::NusRxCallbacks : public NimBLECharacteristicCallbacks
{
public:
	explicit NusRxCallbacks(BleKeyboard *owner) : owner(owner) {}

	// 在 NimBLE 的任务里调用：只把数据塞进流缓冲，由主循环 serialRead() 取走
	void onWrite(NimBLECharacteristic *c, NimBLEConnInfo &connInfo) override
	{
		const NimBLEAttValue &v = c->getValue();
		size_t n = xStreamBufferSend(owner->nusRx, v.data(), v.size(), 0);
		if (n < v.size()) blog("[ble] nus rx overflow, dropped %u bytes\n", (unsigned)(v.size() - n));
	}

private:
	BleKeyboard *owner;
};

class BleKeyboard::NusTxCallbacks : public NimBLECharacteristicCallbacks
{
public:
	explicit NusTxCallbacks(BleKeyboard *owner) : owner(owner) {}

	void onSubscribe(NimBLECharacteristic *c, NimBLEConnInfo &connInfo, uint16_t subValue) override
	{
		if (subValue & 0x0001) {
			owner->nusConn = connInfo.getConnHandle();
			blog("[ble] nus subscribed (mtu=%u)\n", (unsigned)connInfo.getMTU());
		} else if (owner->nusConn == connInfo.getConnHandle()) {
			owner->nusConn = BLE_HS_CONN_HANDLE_NONE;
			blog("[ble] nus unsubscribed\n");
		}
	}

private:
	BleKeyboard *owner;
};

// ---------------------------------------------------------------- 连接状态回调
// "已连接"以加密成功为准：只连上、没加密的链路发不了按键（HID 特征值要求加密），
// 这种情况不报连接，免得主机配对失败反复重连时灯和蜂鸣器跟着来回闪响。
// 允许同时连多个主机（比如电脑 HID + 网页走 NUS），所以按加密链路计数。
class BleKeyboard::ServerCallbacks : public NimBLEServerCallbacks
{
public:
	explicit ServerCallbacks(BleKeyboard *owner) : owner(owner) {}

	void onConnect(NimBLEServer *pServer, NimBLEConnInfo &connInfo) override
	{
		blog("[ble] link up: %s, bonded=%d, encrypted=%d\n",
			 connInfo.getAddress().toString().c_str(), connInfo.isBonded(), connInfo.isEncrypted());
		// 加密由主机发起（Windows / macOS / 手机读 HID 特征值时会自动配对或用已绑定的密钥）
		// 连上后广播会自动停；继续慢速广播，网页 / Python 脚本才能搜到键盘来连 NUS
		owner->startAdvertising();
	}

	void onDisconnect(NimBLEServer *pServer, NimBLEConnInfo &connInfo, int reason) override
	{
		if (connInfo.isEncrypted() && owner->encryptedLinks > 0) owner->encryptedLinks--;
		owner->connected = owner->encryptedLinks > 0;
		if (owner->nusConn == connInfo.getConnHandle()) owner->nusConn = BLE_HS_CONN_HANDLE_NONE;
		// reason = 0x200 + HCI 错误码：0x213 主机主动断开，0x208 超时，0x23D MIC 错误（密钥不对）
		blog("[ble] link down: reason=0x%03X\n", reason);
		owner->startAdvertising();
	}

	void onAuthenticationComplete(NimBLEConnInfo &connInfo) override
	{
		blog("[ble] auth: encrypted=%d, bonded=%d, authenticated=%d\n",
			 connInfo.isEncrypted(), connInfo.isBonded(), connInfo.isAuthenticated());
		if (connInfo.isEncrypted()) {
			owner->encryptedLinks++;
			owner->connected = true;
			if (connInfo.isBonded()) notifyGattChanged(connInfo);
		}
	}

private:
	BleKeyboard *owner;
};

// ---------------------------------------------------------------- BleKeyboard
BleKeyboard::BleKeyboard(std::string deviceName, std::string deviceManufacturer, uint8_t batteryLevel)
	: batteryLevel(batteryLevel), deviceManufacturer(deviceManufacturer), deviceName(deviceName)
{
}

void BleKeyboard::begin(void)
{
	if (this->ready) return; // 只能初始化一次；断线重连用 reAdvertise()

	NimBLEDevice::init(this->deviceName);
	// 绑定 + 安全连接，无显示无键盘（Just Works）；配对信息存在 NVS，重启后自动回连
	NimBLEDevice::setSecurityAuth(true, false, true);
	NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

	this->server = NimBLEDevice::createServer();
	this->server->setCallbacks(new ServerCallbacks(this));
	this->server->advertiseOnDisconnect(false); // 断开后由 onDisconnect 按连接数选快 / 慢广播

	this->hid = new NimBLEHIDDevice(this->server);
	this->inputKeyboard = this->hid->getInputReport(KEYBOARD_ID);
	this->hid->getOutputReport(KEYBOARD_ID); // 建出 LED 输出报告（CapsLock 等灯），报告描述符里有它
	this->inputMediaKeys = this->hid->getInputReport(MEDIA_KEYS_ID);

	this->hid->setManufacturer(this->deviceManufacturer);
	this->hid->setPnp(0x02, 0xe502, 0xa111, 0x0210);
	this->hid->setHidInfo(0x00, 0x01);
	this->hid->setReportMap((uint8_t *)_hidReportDescriptor, sizeof(_hidReportDescriptor));
	this->hid->setBatteryLevel(this->batteryLevel);

	// NUS 蓝牙串口
	this->nusRx = xStreamBufferCreate(NUS_RX_BUF, 1);
	NimBLEService *nus = this->server->createService(NUS_SERVICE);
	NimBLECharacteristic *rx = nus->createCharacteristic(
		NUS_RX, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::WRITE_ENC);
	rx->setCallbacks(new NusRxCallbacks(this));
	this->nusTx = nus->createCharacteristic(
		NUS_TX, NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC);
	this->nusTx->setCallbacks(new NusTxCallbacks(this));

	NimBLEAdvertising *adv = this->server->getAdvertising();
	adv->setAppearance(HID_KEYBOARD);
	adv->addServiceUUID(this->hid->getHidService()->getUUID());
	adv->setName(this->deviceName);
	this->ready = true;
	this->startAdvertising(); // 会顺带启动 GATT 服务
	sNusHandle = nus->getHandle();
}

void BleKeyboard::startAdvertising(void)
{
	if (!this->server) return;
	uint8_t links = this->server->getConnectedCount();
	if (links >= CONFIG_BT_NIMBLE_MAX_CONNECTIONS) return; // 连满了，没法再接新连接
	NimBLEAdvertising *adv = this->server->getAdvertising();
	if (adv->isAdvertising()) adv->stop();
	bool slow = links > 0;
	adv->setMinInterval(slow ? ADV_SLOW_MIN : ADV_FAST_MIN);
	adv->setMaxInterval(slow ? ADV_SLOW_MAX : ADV_FAST_MAX);
	// 在断开回调里调用时，协议栈可能还在收尾上一个连接，这时会失败；主循环的 reAdvertise() 会每秒重试
	if (!adv->start()) blog("[ble] advertising start failed (links=%u), will retry\n", (unsigned)links);
}


void BleKeyboard::setLogger(LogFn fn)
{
	sLog = fn;
}

bool BleKeyboard::isConnected(void)
{
	return this->ready && this->connected;
}

bool BleKeyboard::isReady(void)
{
	return this->ready;
}

void BleKeyboard::reAdvertise(void)
{
	if (!this->ready) return;
	if (!this->server->getAdvertising()->isAdvertising()) this->startAdvertising();
}

void BleKeyboard::setBatteryLevel(uint8_t level)
{
	this->batteryLevel = level; // 总是记下，begin() 时一起设置
	if (!this->ready) return;
	this->hid->setBatteryLevel(level, this->connected);
}


void BleKeyboard::sendKeyReport(char *keys)
{
	if (!this->isConnected()) return;
	// notify() 只会发给已经订阅了这个报告的主机
	this->inputKeyboard->setValue((uint8_t *)keys, 8);
	this->inputKeyboard->notify();
}

void BleKeyboard::sendMediaReport(char *keys)
{
	if (!this->isConnected()) return;
	this->inputMediaKeys->setValue((uint8_t *)keys, 2);
	this->inputMediaKeys->notify();
}

// ---------------------------------------------------------------- 诊断
void BleKeyboard::dumpStatus(void)
{
	if (!this->ready) { blog("[ble] not ready\n"); return; }
	NimBLEAdvertising *adv = this->server->getAdvertising();
	blog("[ble] links=%u encrypted=%u connected=%d advertising=%d nus_conn=%d nus_handle=0x%04X\n",
		 (unsigned)this->server->getConnectedCount(), (unsigned)this->encryptedLinks, (int)this->connected,
		 (int)adv->isAdvertising(), this->nusConn == BLE_HS_CONN_HANDLE_NONE ? -1 : (int)this->nusConn, sNusHandle);
	for (uint16_t h : this->server->getPeerDevices()) {
		NimBLEConnInfo ci = this->server->getPeerInfoByHandle(h);
		blog("[ble]   conn=%u %s id=%s enc=%d bond=%d mtu=%u\n", (unsigned)h,
			 ci.getAddress().toString().c_str(), ci.getIdAddress().toString().c_str(),
			 ci.isEncrypted(), ci.isBonded(), (unsigned)ci.getMTU());
	}
}

// ---------------------------------------------------------------- NUS 读写（主循环里调用）
int BleKeyboard::serialRead(void)
{
	if (!this->nusRx) return -1;
	uint8_t c;
	return xStreamBufferReceive(this->nusRx, &c, 1, 0) == 1 ? c : -1;
}


size_t BleKeyboard::serialWrite(const uint8_t *data, size_t len)
{
	uint16_t conn = this->nusConn;
	if (!this->ready || conn == BLE_HS_CONN_HANDLE_NONE) return 0;
	// 每包最多 MTU-3 字节（默认 MTU 23 -> 20 字节，协商后通常能到 200 多）
	uint16_t mtu = this->server->getPeerMTU(conn);
	size_t chunk = (mtu > 3) ? mtu - 3 : 20;
	size_t sent = 0;
	while (sent < len) {
		size_t n = (len - sent < chunk) ? len - sent : chunk;
		// 协议栈的发送缓冲满了 notify 会失败：稍等再试，最多等约 200ms
		int tries = 0;
		while (!this->nusTx->notify(data + sent, n, conn)) {
			if (++tries > 40 || this->nusConn != conn) { blog("[ble] nus notify failed\n"); return sent; }
			vTaskDelay(pdMS_TO_TICKS(5));
		}
		sent += n;
	}
	return sent;
}
