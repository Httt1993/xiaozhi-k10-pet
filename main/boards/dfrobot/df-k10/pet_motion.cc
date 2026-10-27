#include "pet_motion.h"

#if PET_MOTION_ENABLE

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <cmath>
#include <cstring>

#define TAG "PetMotion"

namespace {

constexpr int kSampleHz = 50;
constexpr int kSampleMs = 1000 / kSampleHz;      /* 20ms */
constexpr int kJerkWin = 25;                     /* 500ms 窗口 */
constexpr int kDebounceTapMs = 300;
constexpr int kFlipMs = 300;
constexpr int kFreefallMinMs = 120;
constexpr int kFreefallMaxMs = 400;
constexpr int kStillMs = 20000;
constexpr int kWakeMs = 3000;                    /* 最近 3s 有体动 = 活跃 */
constexpr int kSideLieMs = 2000;
constexpr int kGentleMs = 2000;                  /* 轻摇持续 2s 判定 */

QueueHandle_t g_queue = nullptr;
PetSensors* g_sensors = nullptr;
volatile bool g_active = false;
volatile float g_gaze_x = 0.0f;
volatile float g_gaze_y = 0.0f;
volatile float g_tilt_deg = 0.0f;
char g_last_event[16] = "none";

void PushEvent(PetMotionEvent ev, const char* name) {
    if (g_queue != nullptr) {
        /* 队列满就丢最旧的 —— 表情更新宁可丢事件也不能堵采样任务 */
        if (xQueueSend(g_queue, &ev, 0) != pdTRUE) {
            PetMotionEvent dump;
            xQueueReceive(g_queue, &dump, 0);
            xQueueSend(g_queue, &ev, 0);
        }
    }
    strlcpy(g_last_event, name, sizeof(g_last_event));
}

struct Detector {
    /* 拍一拍 */
    int64_t last_tap_us = 0;
    /* 倒置 / 翻正 */
    int64_t flip_since_us = 0;
    bool flipped = false;
    int64_t unflip_since_us = 0;
    /* jerk RMS 窗口（环形缓冲） */
    float jerk_ring[kJerkWin] = {};
    int jerk_idx = 0;
    int jerk_filled = 0;
    float jerk_prev_norm = 0.0f;
    bool jerk_prev_valid = false;
    /* 轻摇持续计时 */
    int64_t gentle_since_us = 0;
    bool gentle_fired = false;
    /* 自由落体 → 冲击 */
    int64_t fall_since_us = 0;
    bool falling = false;
    /* 静止判定（2 秒滚动标准差） */
    float still_ring[100] = {};   /* 2s @50Hz */
    int still_idx = 0;
    int still_filled = 0;
    int64_t still_since_us = 0;
    bool asleep = false;
    /* 侧躺 */
    int64_t side_since_us = 0;
    bool side_lying = false;
    /* 拿起：倾角突变 */
    float tilt_ref_deg = 0.0f;
    int64_t tilt_ref_us = 0;
    /* 活跃时间戳 */
    int64_t last_active_us = 0;
};

Detector g_d;

float Norm(float x, float y, float z) {
    return sqrtf(x * x + y * y + z * z);
}

float JerkRms(const Detector& d) {
    if (d.jerk_filled < 3) {
        return 0.0f;
    }
    float acc = 0.0f;
    for (int i = 0; i < d.jerk_filled; i++) {
        acc += d.jerk_ring[i] * d.jerk_ring[i];
    }
    return sqrtf(acc / d.jerk_filled);
}

float StillSigma(const Detector& d) {
    if (d.still_filled < 10) {
        return 999.0f;
    }
    float mean = 0.0f;
    for (int i = 0; i < d.still_filled; i++) {
        mean += d.still_ring[i];
    }
    mean /= d.still_filled;
    float var = 0.0f;
    for (int i = 0; i < d.still_filled; i++) {
        float dv = d.still_ring[i] - mean;
        var += dv * dv;
    }
    return sqrtf(var / d.still_filled);
}

void Task(void*) {
    ESP_LOGI(TAG, "PET_PATCH: motion engine started (%dHz)", kSampleHz);
    TickType_t wake = xTaskGetTickCount();
    while (true) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(kSampleMs));
        if (g_sensors == nullptr) {
            continue;
        }
        int16_t ax = 0, ay = 0, az = 0;
        if (!g_sensors->ReadAccel(ax, ay, az)) {
            continue;
        }
        const int64_t now = esp_timer_get_time();
        const float fx = static_cast<float>(ax);
        const float fy = static_cast<float>(ay);
        const float fz = static_cast<float>(az);
        const float norm = Norm(fx, fy, fz);

        /* ---------- 重力视线（±6px，低通平滑） ---------- */
        if (norm > 200.0f) {
            float ngx = (fx / norm) * 6.0f;
            float ngy = (fy / norm) * 6.0f;
            g_gaze_x += (ngx - g_gaze_x) * 0.08f;
            g_gaze_y += (ngy - g_gaze_y) * 0.08f;
        }

        /* ---------- jerk（单样本合加速度突变） ---------- */
        float jerk = 0.0f;
        if (g_d.jerk_prev_valid) {
            jerk = fabsf(norm - g_d.jerk_prev_norm);
        }
        g_d.jerk_prev_norm = norm;
        g_d.jerk_prev_valid = true;
        g_d.jerk_ring[g_d.jerk_idx] = jerk;
        g_d.jerk_idx = (g_d.jerk_idx + 1) % kJerkWin;
        if (g_d.jerk_filled < kJerkWin) {
            g_d.jerk_filled++;
        }
        const float jerk_rms = JerkRms(g_d);

        /* 活跃判定：任一明显抖动都算「醒着」 */
        if (jerk > 120.0f) {
            g_d.last_active_us = now;
            g_active = true;
            if (g_d.asleep) {
                g_d.asleep = false;
                PushEvent(PET_MOTION_WAKE, "wake");
            }
        } else if (now - g_d.last_active_us > kWakeMs * 1000LL) {
            g_active = false;
        }

        /* ---------- 拍一拍：合加速度突变 > 1300mg，300ms 去抖 ---------- */
        if (jerk > PET_MOTION_TAP_MG &&
            now - g_d.last_tap_us > kDebounceTapMs * 1000LL) {
            g_d.last_tap_us = now;
            PushEvent(PET_MOTION_TAP, "tap");
        }

        /* ---------- 倒置：az < -550mg 持续 300ms ---------- */
        if (fz < PET_MOTION_FLIP_AZ_MG) {
            if (g_d.flip_since_us == 0) {
                g_d.flip_since_us = now;
            } else if (!g_d.flipped &&
                       now - g_d.flip_since_us > kFlipMs * 1000LL) {
                g_d.flipped = true;
                PushEvent(PET_MOTION_FLIP, "flip");
            }
            g_d.unflip_since_us = 0;
        } else if (fz > 300.0f) {
            g_d.flip_since_us = 0;
            if (g_d.flipped) {
                if (g_d.unflip_since_us == 0) {
                    g_d.unflip_since_us = now;
                } else if (now - g_d.unflip_since_us > kFlipMs * 1000LL) {
                    g_d.flipped = false;
                    PushEvent(PET_MOTION_UNFLIP, "unflip");
                }
            }
        } else {
            g_d.flip_since_us = 0;
            g_d.unflip_since_us = 0;
        }

        /* ---------- 猛摇 / 轻摇：jerk RMS 分档 ---------- */
        if (jerk_rms > PET_MOTION_SHAKE_RMS_MG) {
            g_d.gentle_since_us = 0;
            g_d.gentle_fired = false;
            if (now - g_d.last_tap_us > 400 * 1000LL) {
                /* 与拍一拍去重：刚拍过就不再报摇晃 */
                g_d.last_tap_us = now;  /* 借用去抖窗口 */
                PushEvent(PET_MOTION_SHAKE, "shake");
            }
        } else if (jerk_rms > PET_MOTION_GENTLE_RMS_MG && !g_d.gentle_fired) {
            if (g_d.gentle_since_us == 0) {
                g_d.gentle_since_us = now;
            } else if (now - g_d.gentle_since_us > kGentleMs * 1000LL) {
                g_d.gentle_fired = true;
                PushEvent(PET_MOTION_GENTLE, "gentle");
            }
        } else if (jerk_rms < PET_MOTION_GENTLE_RMS_MG * 0.5f) {
            g_d.gentle_since_us = 0;
            g_d.gentle_fired = false;
        }

        /* ---------- 自由落体 → 落地冲击 ---------- */
        if (norm < PET_MOTION_FREEFALL_MG) {
            if (!g_d.falling) {
                g_d.falling = true;
                g_d.fall_since_us = now;
            }
        } else if (g_d.falling) {
            const int64_t fall_ms = (now - g_d.fall_since_us) / 1000LL;
            if (jerk > PET_MOTION_IMPACT_MG && fall_ms >= kFreefallMinMs &&
                fall_ms <= kFreefallMaxMs * 3) {
                PushEvent(PET_MOTION_FALL, "fall");
            }
            g_d.falling = false;
        }

        /* ---------- 静止 20s → 困 ---------- */
        g_d.still_ring[g_d.still_idx] = norm;
        g_d.still_idx = (g_d.still_idx + 1) % 100;
        if (g_d.still_filled < 100) {
            g_d.still_filled++;
        }
        const float sigma = StillSigma(g_d);
        if (sigma > PET_MOTION_STILL_SIGMA_MG) {
            g_d.still_since_us = now;
        } else if (!g_d.asleep && g_d.still_since_us != 0 &&
                   now - g_d.still_since_us > kStillMs * 1000LL) {
            g_d.asleep = true;
            PushEvent(PET_MOTION_STILL, "still");
        }

        /* ---------- 侧躺：|ay|>700 且 |az|<500 持续 2s ---------- */
        const bool side_now = (fabsf(fy) > PET_MOTION_SIDE_LIE_AY_MG &&
                               fabsf(fz) < PET_MOTION_SIDE_LIE_AZ_MG);
        if (side_now) {
            if (g_d.side_since_us == 0) {
                g_d.side_since_us = now;
            } else if (!g_d.side_lying &&
                       now - g_d.side_since_us > kSideLieMs * 1000LL) {
                g_d.side_lying = true;
                PushEvent(PET_MOTION_SIDE_LIE, "side_lie");
            }
        } else {
            if (g_d.side_lying &&
                now - g_d.side_since_us > kSideLieMs * 1000LL) {
                g_d.side_lying = false;
                PushEvent(PET_MOTION_SIDE_END, "side_end");
            }
            g_d.side_since_us = 0;
        }

        /* 侧躺倾角（度）：左右歪 ±15 */
        if (side_now) {
            float deg = atan2f(fy, sqrtf(fx * fx + fz * fz)) *
                        (180.0f / 3.1415926f);
            if (deg > 15.0f) deg = 15.0f;
            if (deg < -15.0f) deg = -15.0f;
            g_tilt_deg += (deg - g_tilt_deg) * 0.1f;
        } else {
            g_tilt_deg += (0.0f - g_tilt_deg) * 0.1f;
        }

        /* ---------- 被拿起：1 秒内倾角变化 > 25° ---------- */
        const float tilt_now =
            atan2f(sqrtf(fx * fx + fy * fy), fz) * (180.0f / 3.1415926f);
        if (g_d.tilt_ref_us == 0 ||
            now - g_d.tilt_ref_us > 1000 * 1000LL) {
            g_d.tilt_ref_deg = tilt_now;
            g_d.tilt_ref_us = now;
        } else if (fabsf(tilt_now - g_d.tilt_ref_deg) > 25.0f &&
                   jerk < PET_MOTION_TAP_MG) {
            /* 只有「倾斜」没「敲击」才算拿起，避免拍桌子误触发 */
            g_d.tilt_ref_deg = tilt_now;
            g_d.tilt_ref_us = now;
            PushEvent(PET_MOTION_PICKUP, "pickup");
        }
    }
}

}  // namespace

void PetMotionInit(PetSensors* sensors) {
    g_sensors = sensors;
    if (g_queue == nullptr) {
        g_queue = xQueueCreate(16, sizeof(PetMotionEvent));
    }
    static bool started = false;
    if (!started && sensors != nullptr && sensors->HasAccel()) {
        started = true;
        xTaskCreate(Task, "pet_motion", 4096, nullptr, 5, nullptr);
    } else if (sensors == nullptr || !sensors->HasAccel()) {
        ESP_LOGW(TAG, "PET_PATCH: no accel, motion engine stays off");
    }
}

void PetMotionInject(PetMotionEvent ev) {
    static const char* const kNames[] = {
        "none", "fall", "shake", "flip", "gentle", "tap", "feed",
        "pet", "pickup", "side_lie", "side_end", "still", "unflip", "wake",
    };
    const int idx = static_cast<int>(ev);
    PushEvent(ev, (idx >= 0 && idx < 14) ? kNames[idx] : "none");
}

int PetMotionDrain(PetMotionEvent* out, int max_out) {
    if (g_queue == nullptr || out == nullptr || max_out <= 0) {
        return 0;
    }
    int n = 0;
    while (n < max_out &&
           xQueueReceive(g_queue, &out[n], 0) == pdTRUE) {
        n++;
    }
    return n;
}

void PetMotionGetGaze(float* gx, float* gy) {
    if (gx) *gx = g_gaze_x;
    if (gy) *gy = g_gaze_y;
}

bool PetMotionActive(void) { return g_active; }

float PetMotionTiltDeg(void) { return g_tilt_deg; }

const char* PetMotionLastEventName(void) { return g_last_event; }

#endif  /* PET_MOTION_ENABLE */
