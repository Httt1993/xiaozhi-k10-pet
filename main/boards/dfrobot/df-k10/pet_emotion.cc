#include "pet_emotion.h"

#include <esp_log.h>

#define TAG "PetEmotion"

namespace {

struct MoodMeta {
    const char* name;
    uint8_t priority;
    int32_t hold_ms;
};

/* 数字越大优先级越高：
 *   P0 惊吓(6) > P1 生气/晕眩(5) > P2 游戏/对话(4) > P3 开心/难过/好奇(3) > P4 困(2) > P5 平静(0)
 * 同级后到的覆盖先到的（比如难过一阵后被拍拍 → 开心）。 */
const MoodMeta kMeta[PET_MOOD_COUNT] = {
    /* idle    */ {"idle", 0, 0},
    /* happy   */ {"happy", 3, 3000},
    /* angry   */ {"angry", 5, 1500},
    /* sad     */ {"sad", 3, 4000},
    /* scared  */ {"scared", 6, 400},   /* 惊吓 400ms 后自动接难过 */
    /* dizzy   */ {"dizzy", 5, 1500},
    /* curious */ {"curious", 3, 3000},
    /* sleepy  */ {"sleepy", 2, 12000},
    /* playing */ {"playing", 4, 10000},
    /* talking */ {"talking", 4, 8000},
};

PetEmotionFsm g_fsm;

}  // namespace

PetEmotionFsm& PetEmotion() { return g_fsm; }

const char* PetEmotionFsm::Name(PetMood m) {
    if (m < 0 || m >= PET_MOOD_COUNT) {
        return "idle";
    }
    return kMeta[m].name;
}

int PetEmotionFsm::DefaultPriority(PetMood m) {
    if (m < 0 || m >= PET_MOOD_COUNT) {
        return 0;
    }
    return kMeta[m].priority;
}

int PetEmotionFsm::DefaultHoldMs(PetMood m) {
    if (m < 0 || m >= PET_MOOD_COUNT) {
        return 0;
    }
    return kMeta[m].hold_ms;
}

bool PetEmotionFsm::Fire(PetMood mood, uint8_t priority, int32_t hold_ms) {
    if (mood < 0 || mood >= PET_MOOD_COUNT) {
        return false;
    }
    /* 同情绪续期，不换脸（避免重复闪） */
    if (mood == current_) {
        hold_left_ms_ = hold_ms;
        return true;
    }
    /* 低优先级请求被当前状态压住 */
    if (priority < priority_) {
        ESP_LOGD(TAG, "drop %s (prio %u < %u)", kMeta[mood].name, priority,
                 priority_);
        return false;
    }
    current_ = mood;
    priority_ = priority;
    hold_left_ms_ = hold_ms;
    ESP_LOGI(TAG, "PET_PATCH: mood -> %s (prio %u, hold %dms)",
             kMeta[mood].name, priority, hold_ms);
    return true;
}

void PetEmotionFsm::Tick(int32_t elapsed_ms) {
    if (current_ == PET_MOOD_IDLE || hold_left_ms_ <= 0) {
        return;
    }
    hold_left_ms_ -= elapsed_ms;
    if (hold_left_ms_ > 0) {
        return;
    }
    /* 惊吓是瞬时态：到期不回平静，降到难过（设计稿：惊吓→难过递进） */
    if (current_ == PET_MOOD_SCARED) {
        Fire(PET_MOOD_SAD, kMeta[PET_MOOD_SAD].priority,
             kMeta[PET_MOOD_SAD].hold_ms);
        return;
    }
    current_ = PET_MOOD_IDLE;
    priority_ = kMeta[PET_MOOD_IDLE].priority;
    hold_left_ms_ = 0;
    ESP_LOGI(TAG, "PET_PATCH: mood -> idle (hold expired)");
}
