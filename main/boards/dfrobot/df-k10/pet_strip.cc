#include "pet_strip.h"
#include "application.h"

#define TAG "PetStrip"

PetStrip::PetStrip(gpio_num_t gpio, uint16_t max_leds) : CircularStrip(gpio, max_leds) {
}

void PetStrip::SetMood(uint8_t red, uint8_t green, uint8_t blue, int level, bool breathe) {
    if (level < 1) {
        level = 1;
    }
    if (level > 10) {
        level = 10;
    }

    mood_color_.red = static_cast<uint8_t>(red * level / 10);
    mood_color_.green = static_cast<uint8_t>(green * level / 10);
    mood_color_.blue = static_cast<uint8_t>(blue * level / 10);
    mood_active_ = (mood_color_.red != 0 || mood_color_.green != 0 || mood_color_.blue != 0);
    mood_breathe_ = breathe;

    ApplyMood();
}

void PetStrip::ClearMood() {
    mood_active_ = false;
    mood_breathe_ = false;
    mood_color_ = {0, 0, 0};
    SetAllColor(mood_color_);
}

void PetStrip::ApplyMood() {
    if (!mood_active_) {
        SetAllColor({0, 0, 0});
        return;
    }

    if (mood_breathe_) {
        // 从 1/8 亮度缓慢升到目标色，再落回去。步长 1，所以 40ms 一步足够柔和
        StripColor low = {
            static_cast<uint8_t>(mood_color_.red / 8),
            static_cast<uint8_t>(mood_color_.green / 8),
            static_cast<uint8_t>(mood_color_.blue / 8),
        };
        Breathe(low, mood_color_, 40);
    } else {
        SetAllColor(mood_color_);
    }
}

void PetStrip::OnStateChanged() {
    if (mood_active_) {
        auto& app = Application::GetInstance();
        if (app.GetDeviceState() == kDeviceStateIdle) {
            // 关键改动：空闲时不要把心情色淡掉，重新点亮
            ApplyMood();
            return;
        }
    }
    CircularStrip::OnStateChanged();
}
