#ifndef _PET_STRIP_H_
#define _PET_STRIP_H_

#include "led/circular_strip.h"

/*
 * PetStrip —— 让灯语活过「空闲」的那一关。
 *
 * 原版 CircularStrip::OnStateChanged() 里，kDeviceStateIdle 直接 FadeOut(50)，
 * 所以 AI 设的颜色回到空闲后 0.45 秒就褪成黑的。这里只做一件事：
 * 空闲态下如果宠物有自己的心情色，就把它重新点亮（可呼吸），而不是淡出。
 *
 * 其它状态（聆听/说话/联网）完全走原逻辑，不改变任何既有行为。
 */
class PetStrip : public CircularStrip {
public:
    PetStrip(gpio_num_t gpio, uint16_t max_leds);

    /* r/g/b 为 0-255，level 1-10 用于整体压暗，breathe=true 走缓慢呼吸 */
    void SetMood(uint8_t red, uint8_t green, uint8_t blue, int level, bool breathe);
    void ClearMood();

    bool mood_active() const { return mood_active_; }
    const StripColor& mood_color() const { return mood_color_; }
    bool mood_breathe() const { return mood_breathe_; }

    void OnStateChanged() override;

private:
    void ApplyMood();

    bool mood_active_ = false;
    bool mood_breathe_ = false;
    StripColor mood_color_ = {0, 0, 0};
};

#endif  // _PET_STRIP_H_
