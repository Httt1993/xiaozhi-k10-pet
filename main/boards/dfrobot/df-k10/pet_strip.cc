#include "pet_strip.h"

#include "application.h"
#include "settings.h"

#include <esp_log.h>

#define TAG "PetStrip"

namespace {

/* 40ms 一步 —— 和基类 Breathe 的手感一样，足够柔和又不闪 */
constexpr int kMoodTickMs = 40;
/* 待机呼吸：单程 40 步 × 40ms = 1.6 秒，往返 3.2 秒 */
constexpr int kMoodPhaseMax = 40;
/* 待机呼吸最低亮度：目标色的 25%，不会彻底黑掉 */
constexpr int kMoodLowPct = 25;
/* 聆听脉动：单程 15 步 × 40ms = 0.6 秒，往返 1.2 秒，55%~100%
 * 用节奏而不是换色来表达「我在听」，这样不会把心情色顶掉 */
constexpr int kListenPhaseMax = 15;
constexpr int kListenLowPct = 55;

/* NVS 命名空间与键名 */
constexpr const char* kNvsNamespace = "pet_mood";
constexpr const char* kKeyR = "r";
constexpr const char* kKeyG = "g";
constexpr const char* kKeyB = "b";
constexpr const char* kKeyLevel = "level";
constexpr const char* kKeyBreathe = "breathe";

inline bool SameColor(const StripColor& a, const StripColor& b) {
    return a.red == b.red && a.green == b.green && a.blue == b.blue;
}

inline StripColor ScalePct(const StripColor& c, int pct) {
    StripColor out;
    out.red = static_cast<uint8_t>(c.red * pct / 100);
    out.green = static_cast<uint8_t>(c.green * pct / 100);
    out.blue = static_cast<uint8_t>(c.blue * pct / 100);
    return out;
}

/*
 * 哪些状态由心情灯说了算。
 *
 * 关键教训：这台设备说完话是回到 kDeviceStateListening（连续对话模式），
 * 不是回到 Idle。所以如果只在 Idle 接管，心情色几乎永远看不见 ——
 * 用户看到的就是「说的时候是绿的，说完变红」。
 *
 * 因此改成「除了过程态，其余全接管」：
 *   Starting / WifiConfiguring / Activating / Upgrading / FatalError
 *   这些蓝色滚动、闪烁是给人看进度的，一律不抢。
 */
bool MoodTakesOver(DeviceState state) {
    switch (state) {
        case kDeviceStateStarting:
        case kDeviceStateWifiConfiguring:
        case kDeviceStateActivating:
        case kDeviceStateUpgrading:
        case kDeviceStateFatalError:
            return false;
        default:
            return true;
    }
}

}  // namespace

PetStrip::PetStrip(gpio_num_t gpio, uint16_t max_leds) : CircularStrip(gpio, max_leds) {
    esp_timer_create_args_t args = {
        .callback = [](void* arg) { static_cast<PetStrip*>(arg)->MoodTick(false); },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK, /* 与基类 strip_timer_ 同任务，天然串行，不会打架 */
        .name = "pet_mood",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &mood_timer_));

    LoadSavedMood();
}

PetStrip::~PetStrip() {
    if (mood_timer_ != nullptr) {
        esp_timer_stop(mood_timer_);
        esp_timer_delete(mood_timer_);
        mood_timer_ = nullptr;
    }
}

void PetStrip::SetMood(uint8_t red, uint8_t green, uint8_t blue, int level, bool breathe,
                       bool persist) {
    if (level < 1) {
        level = 1;
    }
    if (level > 10) {
        level = 10;
    }

    {
        std::lock_guard<std::mutex> lock(mood_mutex_);

        mood_raw_.red = red;
        mood_raw_.green = green;
        mood_raw_.blue = blue;
        mood_level_ = level;
        mood_breathe_ = breathe;
        mood_suppressed_ = false; /* 重新点亮，解除「明确关灯」 */

        /* level 1-10 → 10%..100% */
        mood_color_ = ScalePct(mood_raw_, level * 10);
        mood_active_ =
            (mood_color_.red != 0 || mood_color_.green != 0 || mood_color_.blue != 0);

        mood_phase_ = 0;
        ApplyMoodLocked();

        ESP_LOGI(TAG, "PET_PATCH: set_mood rgb(%u,%u,%u) level=%d breathe=%d -> on rgb(%u,%u,%u)",
                 red, green, blue, level, breathe ? 1 : 0, mood_color_.red, mood_color_.green,
                 mood_color_.blue);
    }

    if (persist) {
        Settings settings(kNvsNamespace, true);
        settings.SetInt(kKeyR, red);
        settings.SetInt(kKeyG, green);
        settings.SetInt(kKeyB, blue);
        settings.SetInt(kKeyLevel, level);
        settings.SetBool(kKeyBreathe, breathe);
    }
}

void PetStrip::ClearMood() {
    {
        std::lock_guard<std::mutex> lock(mood_mutex_);
        mood_active_ = false;
        mood_suppressed_ = true; /* 明确关灯：之后所有状态都保持黑 */
        mood_breathe_ = false;
        mood_raw_ = {0, 0, 0};
        mood_color_ = {0, 0, 0};
        mood_last_ = {0, 0, 0};
        mood_written_ = true;
        SetAllColor(mood_color_);
    }

    /* 黑色不需要动画，定时器可以停；状态切换时由 OnStateChanged 继续压住 */
    if (mood_timer_ != nullptr) {
        esp_timer_stop(mood_timer_);
    }

    ESP_LOGI(TAG, "PET_PATCH: clear_mood");

    /* 写 0 而不是删键：删键等于「从没设过」，下次开机会退回默认暖橙；
     * 写 0 才是「用户明确关灯」，重启也保持黑。 */
    Settings settings(kNvsNamespace, true);
    settings.SetInt(kKeyLevel, 0);
}

void PetStrip::LoadSavedMood() {
    int level = -1;
    uint8_t red = 0;
    uint8_t green = 0;
    uint8_t blue = 0;
    bool breathe = false;

    {
        Settings settings(kNvsNamespace, true);
        /* -1 = 这个键根本不存在（新板子/刷完 app 第一次启动）
         *  0 = 用户明确说过关灯，重启也要保持黑
         * 1-10 = 恢复正常 */
        level = settings.GetInt(kKeyLevel, -1);
        if (level > 0) {
            red = static_cast<uint8_t>(settings.GetInt(kKeyR, 0));
            green = static_cast<uint8_t>(settings.GetInt(kKeyG, 0));
            blue = static_cast<uint8_t>(settings.GetInt(kKeyB, 0));
            breathe = settings.GetBool(kKeyBreathe, false);
        }
    }

    if (level == 0 || (level > 0 && red == 0 && green == 0 && blue == 0)) {
        /* 上次是「明确关灯」，这次开灯也要保持黑 */
        mood_suppressed_ = true;
        ESP_LOGI(TAG, "PET_PATCH: mood saved as off, keep dark");
        return;
    }

    if (level < 0) {
        /* 从没设过：给一个默认的暖橙微呼吸，宠物一开机就是「活的」。
         * persist=true 是为了让「关灯」也能被记住，而不是重启后又亮起来。 */
        ESP_LOGI(TAG, "PET_PATCH: no saved mood, use default warm_orange");
        SetMood(255, 120, 20, 3, true, true);
        return;
    }

    /* persist=false：刚从 NVS 读出来，不要再写一遍 */
    SetMood(red, green, blue, level, breathe, false);

    /* 定时器已由 ApplyMoodLocked 启动；真正接管要等第一次 OnStateChanged
     * （那时 app_ready_ 才为 true，才允许读 Application 的设备状态） */
    ESP_LOGI(TAG, "PET_PATCH: restored mood rgb(%u,%u,%u) level=%d", red, green, blue, level);
}

/*
 * 调用方必须已持有 mood_mutex_。
 * 只负责「立即把心情色写出去」，不做状态判断。
 */
void PetStrip::ApplyMoodLocked() {
    if (!mood_active_) {
        SetAllColor({0, 0, 0});
        mood_last_ = {0, 0, 0};
        mood_written_ = false;
        return;
    }

    /* 呼吸的第一帧从最低亮度起步，免得一亮就从最亮开始 */
    StripColor first = mood_breathe_ ? ScalePct(mood_color_, kMoodLowPct) : mood_color_;
    SetAllColor(first);
    mood_last_ = first;
    mood_written_ = true;

    if (mood_timer_ != nullptr && !esp_timer_is_active(mood_timer_)) {
        esp_timer_start_periodic(mood_timer_, kMoodTickMs * 1000);
    }
}

/*
 * 每 40ms 一次。
 *
 * 颜色永远是心情色，设备状态改用「节奏」表达，这样既不丢反馈，
 * 又不会把用户设的颜色顶掉：
 *
 *   Listening / AudioTesting → 快速脉动（单程 15 步 = 0.6s，往返 1.2s），55%~100%
 *   Speaking / Notifying     → 恒定最亮
 *   其它（Idle / Connecting）→ 按心情设定：breathe 则 3.2s 一个来回（25%~100%），否则恒定
 *
 * mood_suppressed_（用户说过关灯）时统一写黑。
 */
void PetStrip::MoodTick(bool force) {
    if (!app_ready_) {
        return; /* 开机早期，Application 单例还没构造完，别碰 */
    }

    std::lock_guard<std::mutex> lock(mood_mutex_);
    if (!mood_active_ && !mood_suppressed_) {
        return;
    }

    auto& app = Application::GetInstance();
    DeviceState state = app.GetDeviceState();
    if (!MoodTakesOver(state)) {
        mood_written_ = false; /* 交回基类；回到可接管状态时会重新写 */
        return;
    }

    StripColor target;

    if (!mood_active_) {
        /* 明确关灯：所有状态都保持黑 */
        target = {0, 0, 0};
    } else {
        int period = 0;  /* 0 = 恒定，不呼吸 */
        int low_pct = 100;

        switch (state) {
            case kDeviceStateListening:
            case kDeviceStateAudioTesting:
                period = kListenPhaseMax;
                low_pct = kListenLowPct;
                break;
            case kDeviceStateSpeaking:
            case kDeviceStateNotifying:
                period = 0;
                break;
            default:
                if (mood_breathe_) {
                    period = kMoodPhaseMax;
                    low_pct = kMoodLowPct;
                }
                break;
        }

        if (period == 0) {
            target = mood_color_;
        } else {
            mood_phase_++;
            if (mood_phase_ >= period * 2) {
                mood_phase_ = 0;
            }
            int step = mood_phase_ < period ? mood_phase_ : (period * 2 - mood_phase_);
            int pct = low_pct + (100 - low_pct) * step / period;
            target = ScalePct(mood_color_, pct);
        }
    }

    if (!force && mood_written_ && SameColor(target, mood_last_)) {
        return;
    }

    SetAllColor(target);
    mood_last_ = target;
    mood_written_ = true;
}

void PetStrip::OnStateChanged() {
    app_ready_ = true; /* 能走到这里说明 Application 已经起来了 */

    DeviceState state = Application::GetInstance().GetDeviceState();

    if ((mood_active_ || mood_suppressed_) && MoodTakesOver(state)) {
        /* 心情优先：不调 CircularStrip::OnStateChanged()，
         * 否则 Idle 会被 FadeOut 淡掉、Listening/Speaking 会被改成红/绿。 */
        MoodTick(true);
        ESP_LOGI(TAG, "PET_PATCH: mood holds rgb(%u,%u,%u) in state %d%s",
                 mood_last_.red, mood_last_.green, mood_last_.blue,
                 static_cast<int>(state), mood_active_ ? "" : " (off)");
        return;
    }

    CircularStrip::OnStateChanged();
}
