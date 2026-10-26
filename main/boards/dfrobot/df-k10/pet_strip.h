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
 *   3. Listening 会被改成红、Speaking 会被改成绿。而这台设备说完话是回到 Listening
 *      （连续对话模式）而不是 Idle，所以心情色几乎永远看不见 —— 这是最坑的一条。
 *
 * 这里的做法是彻底不依赖基类的特效定时器：
 *
 *   - PetStrip 自己持有一个 40ms 的 esp_timer；
 *   - 除「开机 / 配网 / 联网激活 / 升级 / 致命错误」这些过程态外，一律照着心情色写，
 *     谁把它改掉都能在 40ms 内自愈；
 *   - 设备状态改用「节奏」表达，不再换颜色：
 *       待机   → 按心情设定呼吸（3.2 秒一个来回）或恒定
 *       聆听   → 快速脉动（1.2 秒一个来回，55%~100%），像在专注听你说话
 *       说话   → 恒定最亮
 *   - clear_mood 之后进入「明确关灯」状态，所有状态下都保持黑，重启也保持黑。
 *
 * 另外把心情存进 NVS，重启后自动恢复。
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
    bool mood_suppressed() const { return mood_suppressed_; }
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
    /* clear_mood 之后为 true：所有状态都保持黑，重启也保持黑 */
    bool mood_suppressed_ = false;
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
