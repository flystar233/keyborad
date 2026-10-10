// ============================================================================
//  led_effects.h —— 5 颗灯珠的灯效
//
//  两种层次的灯效：
//    1) 状态指示（始终有效，不会被灯效模式关掉）
//       - 连接状态：未连接 / 广播中 / 已连接
//       - 按下的那颗键闪白
//       - 当前模式配色（定时器 / 节拍器 / 配置）
//       - 低电提示
//    2) 灯效模式：由 GPIO6 上的开关 U4 切换，决定"打底"的动画
//
//  优先级（高 -> 低）：低电 > 按键反馈 > 模式指示 > 灯效动画 > 连接状态
// ============================================================================
#pragma once

#include <Arduino.h>

// 连接状态
enum LedLinkState {
    LED_LINK_OFF = 0,       // 还没开始广播
    LED_LINK_ADVERTISING,   // 正在广播，等配对
    LED_LINK_CONNECTED,     // 已连接
};

// 灯效模式（GPIO6 上的开关 U4 循环切换）
enum LedEffect {
    LED_FX_STATUS = 0,      // 状态指示：待机呼吸，最省心，默认
    LED_FX_SOLID,           // 常亮：一眼看出连没连上
    LED_FX_RAINBOW,         // 彩虹流动
    LED_FX_WAVE,            // 正弦波起伏，5 颗依次呼吸
    LED_FX_CHASE,           // 跑马灯
    LED_FX_TWINKLE,         // 随机闪烁
    LED_FX_COUNT            // 数量，必须放最后
};

void LED_EffectsInit();
void LED_EffectsSetLink(LedLinkState st);
void LED_EffectsSetMode(uint8_t mode);          // 与 SystemMode 对应，见 system.h
void LED_EffectsSetLowBattery(bool low);

void LED_EffectsKeyFeedback(uint8_t keyIndex);  // 0..4，按下时调用
void LED_EffectsBeat();                         // 节拍器每一拍调用

// 上电自检：红 -> 绿 -> 蓝 -> 白，每步 250ms。阻塞式，只在 setup() 里调
void LED_EffectsSelfTest();

// 逐颗逐通道检测：依次让 每颗灯 x 每个通道 单独全亮，用来判断
// "某颗灯颜色不对"到底是代码问题还是那颗灯的某个通道有问题。
// 例如第 1 颗灯发黄 = 红+绿亮、蓝不亮 -> 蓝通道故障。
// 阻塞式，只在串口诊断命令里调。
void LED_DiagChannelSweep();

// 只让第 idx 颗灯的单个通道亮（0=R,1=G,2=B），其余全灭
void LED_DiagSingle(uint8_t idx, uint8_t channel);

// 主循环里定期调用（内部自己限流，调用频率随意）。
// 内部也会轮询 GPIO6 开关，所以主循环不需要单独调 LED_EffectsPollSwitch()。
void LED_EffectsUpdate();

// ---------------------------------------------------------------- 灯效模式
void    LED_EffectsPollSwitch();                // 轮询 GPIO6 开关（Update 里已调）
void    LED_EffectsNext();                      // 手动切到下一个灯效
void    LED_EffectsSet(uint8_t fx);             // 直接指定
uint8_t LED_EffectsGet();
const char* LED_EffectsName(uint8_t fx);
uint8_t LED_EffectsCount();

// 开关 U4 的实时状态，用于诊断（确认接法是否符合预期）
bool     LED_EffectSwitchClosed();              // 当前是否"闭合/接通"
uint16_t LED_EffectSwitchToggleCount();         // 上电以来检测到多少次翻转
