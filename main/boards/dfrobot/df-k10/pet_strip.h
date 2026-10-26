#ifndef _PET_STRIP_H_
#define _PET_STRIP_H_

#include "led/circular_strip.h"

#include <esp_timer.h>
#include <mutex>

/*
 * PetStrip —— 让宠物心情灯「一直亮着」。
 *
 * 原版 CircularStrip 有几个坑，导致 AI 设的颜色总是自己消失：
 *
 *   1. OnStateChanged() 里 kDeviceStateIdle 直接 FadeOut(50)，空闲 0.45 秒后必黑。
 *   2. FadeOut() 和 Breathe() 共用同一个 strip_timer_，而且 FadeOut 是从 colors_[]
 *      开始淡出的 —— Breathe 从来不更新 colors_[]，所以「呼吸」一被 FadeOut 接管，
 *      淡出的起点是上一个状态的残留色，看起来就是「亮一下然后没了」。
 *   3. kDeviceStateListening / kDeviceStateSpeaking 会把灯改成红/绿，颜色名字跟
 *      心情色撞车，容易误判成「心情色被覆盖」。
 *
 * 这里的做法是彻底不依赖基类的特效定时器：
 *
 *   - PetStrip 自己持有一个 40ms 的 esp_timer；
 *   - 只要心情是激活的，就在 Idle/Connecting/Listening/Speaking/Notifying 这些
 *     状态里持续把心情色写回去（呼吸效果也由这个定时器自己算，不调 Breathe）；
 *   - 开机 / 配网 / 联网激活 / 升级 这些过程态不抢占，留给基类显示进度；
 *   - 目标色没变化就不重复刷新，静态心情灯实际只写一次。
 *
 * 另外把心情存进 NVS，重启后自动恢复 —— 既是宠物该有的行为，也方便验证
 * 「工具到底有没有被调用」。
 */
class PetStrip : public CircularStrip {
public:
    PetStrip(gpio_num_t gpio, uint16_t max_leds);
    ~PetStrip() override;

    /* r/g/b 为 0-255，level 1-10 用于整体压暗，breathe=true 走缓慢呼吸 */
    void SetMood(uint8_t red, uint8_t green, uint8_t blue, int level, bool breathe,
                 bool persist = true);
    void ClearMood();

    bool mood_active() const { return mood_active_; }
    const StripColor& mood_color() const { return mood_color_; }
    bool mood_breathe() const { return mood_breathe_; }
    int mood_level() const { return mood_level_; }

    void OnStateChanged() override;

private:
    void MoodTick(bool force);
    void ApplyMoodLocked();
    void LoadSavedMood();

    std::mutex mood_mutex_;

    bool mood_active_ = false;
    bool mood_breathe_ = false;
    int mood_level_ = 5;
    StripColor mood_raw_ = {0, 0, 0};    /* 未按 level 压暗的原始色 */
    StripColor mood_color_ = {0, 0, 0};  /* 实际点亮的颜色 */

    esp_timer_handle_t mood_timer_ = nullptr;
    int mood_phase_ = 0;
    StripColor mood_last_ = {0, 0, 0};
    bool mood_written_ = false;

    /* 开机阶段禁止触碰 Application 单例（Board 是在 Application 构造里建的） */
    bool app_ready_ = false;
};

#endif  // _PET_STRIP_H_
