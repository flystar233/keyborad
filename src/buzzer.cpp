// ============================================================================
//  buzzer.cpp —— 蜂鸣器实现：LEDC 硬件 PWM + 非阻塞音符播放器
// ============================================================================
#include <Arduino.h>
#include "board_config.h"
#include "buzzer.h"
#include "usb_log.h"

// ---------------------------------------------------------------- 音符频率（Hz）
// 只列用得到的。蜂鸣器谐振约 2.7kHz，1~4kHz 最响；
// 第 5 组八度（587~988Hz）只给旋律里偏低的音用，再往下就明显发闷了。
#define REST   0
#define D5   587
#define E5   659
#define FS5  740
#define G5   784
#define A5   880
#define B5   988
#define C6  1047
#define CS6 1109
#define D6  1175
#define E6  1319
#define F6  1397
#define FS6 1480
#define G6  1568
#define A6  1760
#define B6  1976
#define C7  2093
#define E7  2637

struct Note { uint16_t hz; uint16_t ms; };

// ---------------------------------------------------------------- 提示音
static const Note kStartup[]      = { {C6, 90}, {E6, 90}, {G6, 90}, {C7, 200} };
static const Note kConnected[]    = { {G6, 90}, {C7, 170} };
static const Note kDisconnected[] = { {C7, 90}, {G6, 90}, {C6, 200} };
static const Note kModeEnter[]    = { {E6, 60}, {A6, 110} };
static const Note kModeExit[]     = { {A6, 60}, {E6, 110} };
static const Note kConfirm[]      = { {E7, 110}, {C7, 220} };     // 叮咚
static const Note kAttention[]    = { {A6, 100}, {REST, 80}, {A6, 100}, {REST, 80}, {A6, 100} };
static const Note kDone[]         = { {G6, 80}, {C7, 80}, {E7, 200} };

// ---------------------------------------------------------------- 音乐（公版曲目）
// 每首用自己的四分音符时长，节奏单独调：
//   改快慢只改 ODE_Q / CANON_Q 一个数；其余时值都按它换算。

// 《欢乐颂》（贝多芬第九交响曲）完整主题 A - A' - B - A'，约 21 秒
#define ODE_Q    330
#define OQ       ODE_Q              // 四分
#define OH       (ODE_Q * 2)        // 二分
#define ODQ      (ODE_Q * 3 / 2)    // 附点四分
#define OE       (ODE_Q / 2)        // 八分
static const Note kOdeToJoy[] = {
    // A
    {E6,OQ},{E6,OQ},{F6,OQ},{G6,OQ},   {G6,OQ},{F6,OQ},{E6,OQ},{D6,OQ},
    {C6,OQ},{C6,OQ},{D6,OQ},{E6,OQ},   {E6,ODQ},{D6,OE},{D6,OH},
    // A'
    {E6,OQ},{E6,OQ},{F6,OQ},{G6,OQ},   {G6,OQ},{F6,OQ},{E6,OQ},{D6,OQ},
    {C6,OQ},{C6,OQ},{D6,OQ},{E6,OQ},   {D6,ODQ},{C6,OE},{C6,OH},
    // B
    {D6,OQ},{D6,OQ},{E6,OQ},{C6,OQ},   {D6,OQ},{E6,OE},{F6,OE},{E6,OQ},{C6,OQ},
    {D6,OQ},{E6,OE},{F6,OE},{E6,OQ},{D6,OQ},   {C6,OQ},{D6,OQ},{G5,OH},
    // A'
    {E6,OQ},{E6,OQ},{F6,OQ},{G6,OQ},   {G6,OQ},{F6,OQ},{E6,OQ},{D6,OQ},
    {C6,OQ},{C6,OQ},{D6,OQ},{E6,OQ},   {D6,ODQ},{C6,OE},{C6,OH},
};

// 《卡农》（帕赫贝尔 D 大调卡农）第一小提琴声部里最有辨识度的十六分音符段落，约 22 秒。
// 原曲音高整体升高 1 个八度（原曲 F#5 -> 这里 F#6），音域 587~1760Hz，听感柔和不刺耳。
// 没有慢速前奏，直接从这一段开始；四小节完整重复两遍，再收在主音 D。
// 原曲这段偏快，这里放慢：十六分音符 150ms（CANON_Q = 600）。改快慢只改 CANON_Q。
#define CANON_Q  600
#define CQ       CANON_Q            // 四分
#define CH       (CANON_Q * 2)      // 二分
#define CE       (CANON_Q / 2)      // 八分
#define CS       (CANON_Q / 4)      // 十六分

// 四小节，每行一小节
#define CANON_PASSAGE \
    {FS6,CE},{D6,CS},{E6,CS},{FS6,CE},{D6,CS},{E6,CS}, \
        {FS6,CS},{FS5,CS},{G5,CS},{A5,CS},{B5,CS},{CS6,CS},{D6,CS},{E6,CS}, \
    {FS6,CE},{D6,CS},{E6,CS},{FS6,CE}, \
        {FS5,CS},{G5,CS},{A5,CS},{B5,CS},{A5,CS},{G5,CS},{A5,CS},{FS5,CS},{G5,CS},{A5,CS}, \
    {G5,CE},{B5,CS},{A5,CS},{G5,CE},{FS5,CS},{E5,CS}, \
        {FS5,CS},{E5,CS},{D5,CS},{E5,CS},{FS5,CS},{G5,CS},{A5,CS},{B5,CS}, \
    {G5,CE},{B5,CS},{A5,CS},{B5,CE},{CS6,CS},{D6,CS}, \
        {A5,CS},{B5,CS},{CS6,CS},{D6,CS},{E6,CS},{FS6,CS},{G6,CS},{A6,CS}

static const Note kCanon[] = {
    CANON_PASSAGE,
    CANON_PASSAGE,
    // 收尾
    {FS6,CQ},{E6,CQ},{D6,CH + CQ},
};

struct Song { const char *name; const Note *notes; uint8_t count; };
static const Song kSongs[] = {
    { "Ode to Joy", kOdeToJoy, (uint8_t)(sizeof(kOdeToJoy) / sizeof(Note)) },
    { "Canon",      kCanon,    (uint8_t)(sizeof(kCanon)    / sizeof(Note)) },
};
#define SONG_COUNT  (sizeof(kSongs) / sizeof(kSongs[0]))

#define COUNT(a)  ((uint8_t)(sizeof(a) / sizeof((a)[0])))

// ---------------------------------------------------------------- 状态
#if BUZZER_ENABLED
static uint8_t  sChannel  = 0;
static bool     sReady    = false;
#endif
static uint8_t  sLevel    = SOUND_ON;

// 单音（按键音）：到 sStopAt 自动停
static bool     sBeeping  = false;
static uint32_t sStopAt   = 0;

// 旋律播放器
static const Note *sSeq     = nullptr;
static uint8_t     sSeqLen  = 0;
static uint8_t     sSeqPos  = 0;
static bool        sSeqGap  = false;    // 当前在音符之间的短暂停顿里
static uint32_t    sSeqNext = 0;        // 当前阶段结束的时刻
static bool        sSeqSong = false;    // 正在放的是彩蛋曲子（而不是提示音）

// ---------------------------------------------------------------- 底层出声
static void toneOn(uint32_t hz) {
#if BUZZER_ENABLED
    if (!sReady) return;
    ledcWriteTone(sChannel, hz);
    ledcWrite(sChannel, 128);               // 50% 占空比最响
#else
    (void)hz;
#endif
}

static void toneOff() {
#if BUZZER_ENABLED
    if (!sReady) return;
    ledcWrite(sChannel, 0);
#endif
}

bool BuzzerEnabled() {
#if BUZZER_ENABLED
    return sReady;
#else
    return false;
#endif
}

void BuzzerInit() {
#if BUZZER_ENABLED
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);          // 默认静音

    sChannel = 0;
    if (ledcSetup(sChannel, BUZZER_FREQ, 8)) {
        ledcAttachPin(BUZZER_PIN, sChannel);
        ledcWrite(sChannel, 0);             // 占空比 0 -> 不响
        sReady = true;
        usbLog::printf("[buz] GPIO%d ready (LEDC ch%d)\n", BUZZER_PIN, sChannel);
    } else {
        usbLog::print("[buz] LEDC setup FAILED\n");
    }
#endif
}

// ---------------------------------------------------------------- 音量档位
void BuzzerSetLevel(uint8_t level) {
    if (level >= SOUND_LEVEL_COUNT) level = SOUND_ON;
    sLevel = level;
    if (sLevel == SOUND_OFF) BuzzerStop();
}

uint8_t BuzzerLevel() { return sLevel; }

const char* BuzzerLevelName(uint8_t level) {
    switch (level) {
        case SOUND_ON:    return "ON";
        case SOUND_QUIET: return "QUIET";
        case SOUND_OFF:   return "OFF";
        default:          return "?";
    }
}

// ---------------------------------------------------------------- 播放
void BuzzerStop() {
    toneOff();
    sBeeping = false;
    sSeq     = nullptr;
    sSeqSong = false;
}

// 音符之间留一点停顿，连续的同音（比如欢乐颂开头两个 mi）才听得出是两下
static uint16_t gapOf(uint16_t ms) {
    uint16_t g = ms / 8;
    if (g < 15) g = 15;
    if (g > 45) g = 45;
    return g;
}

static void startNote() {
    const Note &n = sSeq[sSeqPos];
    uint16_t gap  = gapOf(n.ms);
    uint16_t on   = (n.ms > gap) ? (uint16_t)(n.ms - gap) : n.ms;
    if (n.hz == REST) toneOff(); else toneOn(n.hz);
    sSeqGap  = false;
    sSeqNext = millis() + on;
}

static void playSeq(const Note *seq, uint8_t len, bool isSong) {
    if (!BuzzerEnabled() || len == 0) return;
    sBeeping = false;                       // 旋律优先，掐掉还在响的按键音
    sSeq     = seq;
    sSeqLen  = len;
    sSeqPos  = 0;
    sSeqSong = isSong;
    startNote();
}

void BuzzerUpdate() {
    uint32_t now = millis();

    if (sBeeping && (int32_t)(now - sStopAt) >= 0) {
        toneOff();
        sBeeping = false;
    }

    if (sSeq == nullptr || (int32_t)(now - sSeqNext) < 0) return;

    if (!sSeqGap) {
        // 一个音符响完 -> 进入音符间的停顿
        toneOff();
        sSeqGap  = true;
        sSeqNext = now + gapOf(sSeq[sSeqPos].ms);
    } else {
        // 停顿结束 -> 下一个音符，或者整段结束
        sSeqPos++;
        if (sSeqPos >= sSeqLen) { sSeq = nullptr; sSeqSong = false; return; }
        startNote();
    }
}

// ---------------------------------------------------------------- 按键音
// 只在 SOUND_ON 档响；正在放旋律时不出声，免得把旋律打断
void BuzzerClick() {
    if (sLevel != SOUND_ON || sSeq != nullptr || !BuzzerEnabled()) return;
    toneOn(3200);
    sBeeping = true;
    sStopAt  = millis() + 12;
}

// ---------------------------------------------------------------- 提示音
static void cue(const Note *seq, uint8_t len) {
    if (sLevel == SOUND_OFF) return;
    playSeq(seq, len, false);
}

void BuzzerStartup()      { cue(kStartup,      COUNT(kStartup)); }
void BuzzerConnected()    { cue(kConnected,    COUNT(kConnected)); }
void BuzzerDisconnected() { cue(kDisconnected, COUNT(kDisconnected)); }
void BuzzerModeEnter()    { cue(kModeEnter,    COUNT(kModeEnter)); }
void BuzzerModeExit()     { cue(kModeExit,     COUNT(kModeExit)); }
void BuzzerConfirm()      { cue(kConfirm,      COUNT(kConfirm)); }
void BuzzerAttention()    { cue(kAttention,    COUNT(kAttention)); }
void BuzzerDone()         { cue(kDone,         COUNT(kDone)); }

// ---------------------------------------------------------------- 音乐
uint8_t BuzzerSongCount() { return (uint8_t)SONG_COUNT; }

void BuzzerPlaySong(uint8_t idx) {
    if (idx >= SONG_COUNT || sLevel == SOUND_OFF) return;
    playSeq(kSongs[idx].notes, kSongs[idx].count, true);
    usbLog::printf("[buz] play '%s'\n", kSongs[idx].name);
}

bool BuzzerSongPlaying() { return sSeq != nullptr && sSeqSong; }
