// ============================================================================
//  led_effects.h —— 5 颗灯珠的灯效
//
//  连接状态优先（盖住灯效模式）：
//    - 还没连上蓝牙（上电 / 广播中 / 断线）：5 颗灯黄色一起明暗呼吸
//    - 刚连上：5 颗灯绿色闪 2 下，然后交给当前灯效模式
//    （没连上时按 SW1 切灯效，会先预览新灯效 3 秒，再回到黄色呼吸）
//
//  灯效模式：GPIO6 上的 SW1 短按循环切换（长按是切系统模式），上电默认第 1 个（按键反馈）。
//
//  叠加在最上面的：
//    - 按下的那颗灯亮一下再渐暗（所有灯效都有）
//    - 预设切换模式配色（整条偏青）
//    - 低电提示
//
//  优先级（高 -> 低）：低电 > 按键反馈 > 模式指示 > 连接提示 > 灯效动画
// ============================================================================
#pragma once

#include <Arduino.h>

// 连接状态
enum LedLinkState {
    LED_LINK_OFF = 0,       // 还没开始广播
    LED_LINK_ADVERTISING,   // 正在广播，等配对
    LED_LINK_CONNECTED,     // 已连接
};

// 灯效模式（GPIO6 上的开关 SW1 循环切换）
enum LedEffect {
    LED_FX_KEYS = 0,        // 默认：全灭，只有按下的键亮一下再渐暗
    LED_FX_WHITE,           // 白色常亮
    LED_FX_BREATHE,         // 青蓝色整条呼吸
    LED_FX_RAINBOW_CHASE,   // 炫彩跑马灯：亮点来回跑，颜色沿色轮变化，带拖尾
    LED_FX_RAINBOW,         // 彩虹流动：整条色轮缓慢平移
    LED_FX_KEYS_COLOR,      // 缤纷按键：全灭，按键时亮一个随机颜色再渐暗
    LED_FX_COUNT            // 数量，必须放最后
};

void LED_EffectsInit();
void LED_EffectsSetLink(LedLinkState st);
void LED_EffectsSetMode(uint8_t mode);          // 与 SystemMode 对应，见 system.h
void LED_EffectsSetLowBattery(bool low);

void LED_EffectsKeyFeedback(uint8_t keyIndex);  // 0..4，按下时调用

// 最右边那颗灯（K5）的提醒，叠在灯效上：
//   LED_ATTN_WAIT = 橙色闪烁，直到改成别的；LED_ATTN_DONE = 绿色闪 3 下后自动熄灭
enum LedAttention { LED_ATTN_NONE = 0, LED_ATTN_WAIT, LED_ATTN_DONE };
void LED_EffectsSetAttention(uint8_t attn);

// 上电自检：红 -> 绿 -> 蓝 -> 白，每步 250ms。阻塞式，只在 setup() 里调
void LED_EffectsSelfTest();

// 逐颗逐通道检测：依次让 每颗灯 x 每个通道 单独全亮，用来判断
// "某颗灯颜色不对"到底是代码问题还是那颗灯的某个通道有问题。
// 例如第 1 颗灯发黄 = 红+绿亮、蓝不亮 -> 蓝通道故障。
// 阻塞式，只在串口诊断命令里调。
void LED_DiagChannelSweep();

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

// 开关 SW1 的实时状态，用于诊断（确认接法是否符合预期）
bool     LED_EffectSwitchClosed();              // 当前是否"闭合/接通"
uint16_t LED_EffectSwitchToggleCount();         // 上电以来 SW1 被按了多少次
