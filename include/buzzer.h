// ============================================================================
//  buzzer.h —— 蜂鸣器：提示音 + 简单旋律（GPIO10 -> R10 1K -> Q1 S9013 基极，NPN 低边）
//
//  GPIO10 输出高 -> Q1 导通 -> 蜂鸣器 BUZ1 负极接地 -> 响
//  蜂鸣器是 QMB-09B-03 无源电磁式（谐振约 2.7kHz），用 LEDC 硬件 PWM 出方波：
//  出音期间完全不占 CPU、也不会被 BLE 中断影响音准。
//
//  能放单音旋律：每个音符 = 频率 + 时长，由 BuzzerUpdate() 在主循环里非阻塞地推进。
//  它在 1~4kHz 最响，低于 500Hz 明显发闷，所以旋律都放在 C6 附近的音区。
//
//  三类声音，受音量档位控制：
//    按键音（Click） : 只在 SOUND_ON 档响；正在放旋律时不打断旋律
//    提示音（Cue）   : 开机 / 连接 / 断开 / 模式切换 / 确认，SOUND_QUIET 档也响
//    音乐（Song）    : 彩蛋曲子，SOUND_QUIET 档也响
//  SOUND_OFF 档全部静音。
// ============================================================================
#pragma once

#include <Arduino.h>

enum SoundLevel : uint8_t {
    SOUND_ON    = 0,    // 全部开
    SOUND_QUIET = 1,    // 关掉按键音，保留提示音和音乐
    SOUND_OFF   = 2,    // 全部静音
    SOUND_LEVEL_COUNT
};

void BuzzerInit();
void BuzzerUpdate();                    // 主循环调用：推进旋律、到时停音
bool BuzzerEnabled();

void        BuzzerSetLevel(uint8_t level);
uint8_t     BuzzerLevel();
const char* BuzzerLevelName(uint8_t level);   // "ON" / "QUIET" / "OFF"

void BuzzerStop();                      // 立刻停（包括正在放的旋律）

// ---------------------------------------------------------------- 按键音
void BuzzerClick();

// ---------------------------------------------------------------- 提示音（短旋律）
void BuzzerStartup();                   // 开机：do-mi-sol-do 上行
void BuzzerConnected();                 // 蓝牙连上：上扬两音
void BuzzerDisconnected();              // 蓝牙断开：下沉三音
void BuzzerModeEnter();                 // 进入预设切换
void BuzzerModeExit();                  // 不确认退出预设切换
void BuzzerConfirm();                   // 确认 / 保存成功："叮咚"
void BuzzerAttention();                 // Claude 在等你操作：嘀嘀嘀
void BuzzerDone();                      // Claude 做完了：三音上行

// ---------------------------------------------------------------- 音乐
uint8_t     BuzzerSongCount();
void        BuzzerPlaySong(uint8_t idx);
bool        BuzzerSongPlaying();        // 正在放彩蛋曲子（不含提示音）
