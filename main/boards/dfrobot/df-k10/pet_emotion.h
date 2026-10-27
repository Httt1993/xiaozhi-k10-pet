#ifndef _PET_EMOTION_H_
#define _PET_EMOTION_H_

#include <cstdint>

/*
 * 情绪状态机（EmotionFSM）：多来源事件的优先级仲裁。
 *
 * 为什么需要它：体感、按键、AI（小智 set_face）都在改表情，
 * 没有仲裁就会互相打断（孩子正高兴呢，一次轻抖把表情抢走）。
 *
 * 优先级（高 → 低，与设计稿 1.1 节一致）：
 *   P0 摔落惊吓 > P1 生气/晕眩 > P2 游戏/对话 > P3 开心/难过 > P4 困 > P5 平静
 *
 * 衰减：每种刺激都有保持时长，到期自动降级回 kIdle。
 * AI 主动 set_face 走 P2（Talking），保持 8 秒，但 P0/P1 仍可打断它。
 */

enum PetMood {
    PET_MOOD_IDLE = 0,     /* 平静 */
    PET_MOOD_HAPPY,        /* 开心 */
    PET_MOOD_ANGRY,        /* 生气（倒置） */
    PET_MOOD_SAD,          /* 难过 */
    PET_MOOD_SCARED,       /* 惊吓（摔落） */
    PET_MOOD_DIZZY,        /* 晕眩（猛摇） */
    PET_MOOD_CURIOUS,      /* 好奇（被拿起） */
    PET_MOOD_SLEEPY,       /* 困（静止/侧躺） */
    PET_MOOD_PLAYING,      /* 游戏中 */
    PET_MOOD_TALKING,      /* 对话中（AI set_face） */
    PET_MOOD_COUNT,
};

struct PetMoodRequest {
    PetMood mood;
    uint8_t priority;
    int32_t hold_ms;
};

class PetEmotionFsm {
public:
    /* 投递一个请求。优先级低于当前 → 直接拒绝。 */
    bool Fire(PetMood mood, uint8_t priority, int32_t hold_ms);
    /* 每帧调用（30ms），处理保持时长到期 → 回落 */
    void Tick(int32_t elapsed_ms);

    PetMood current() const { return current_; }
    uint8_t priority() const { return priority_; }

    static const char* Name(PetMood m);
    static int DefaultPriority(PetMood m);
    static int DefaultHoldMs(PetMood m);

private:
    PetMood current_ = PET_MOOD_IDLE;
    uint8_t priority_ = 0;
    int32_t hold_left_ms_ = 0;
};

/* 全局单例：体感任务只投事件，LVGL Tick 统一消费 */
PetEmotionFsm& PetEmotion();

#endif  /* _PET_EMOTION_H_ */
