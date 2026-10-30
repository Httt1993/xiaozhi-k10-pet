/*
 * pet_engage.cc —— 注意力门控（AttentionGate）实现
 * 设计：《AI-Buddy-宠物状态反馈系统完善设计》SECTION 04/06。
 */

#include "pet_engage.h"

#include <atomic>

#include <esp_log.h>

namespace pet_engage {

namespace {

const char* TAG = "PetEngage";

/* 交互窗口截止时刻（ms）。任意线程写，CAS 拉长/压短 */
std::atomic<int64_t> g_engaged_until_ms{0};
/* 待处理进门强度：0=无 1=light（人声） 2=strong（拿起/唤醒/按键） */
std::atomic<int32_t> g_pending_enter{0};
/* 只在 Flush 线程（LVGL）读写的状态迁移记忆 */
bool g_was_engaged = false;

EnterFn g_on_enter;
ExitFn g_on_exit;

}  // namespace

void SetHooks(EnterFn on_enter, ExitFn on_exit) {
    g_on_enter = std::move(on_enter);
    g_on_exit = std::move(on_exit);
}

void Init(int64_t now_ms) {
    g_engaged_until_ms.store(now_ms + PET_ATTN_WINDOW_MS);
    g_pending_enter.store(0);
    g_was_engaged = true; /* 开机宽限期内算交互态，不出「进门」戏 */
    ESP_LOGI(TAG, "PET_PATCH: attn init, boot grace %dms", PET_ATTN_WINDOW_MS);
}

void NoteActivity(int strong, int64_t now_ms) {
    const int64_t until = now_ms + PET_ATTN_WINDOW_MS;
    int64_t cur = g_engaged_until_ms.load();
    while (until > cur && !g_engaged_until_ms.compare_exchange_weak(cur, until)) {
    }
    if (strong) {
        int32_t pe = g_pending_enter.load();
        while (pe < 2 && !g_pending_enter.compare_exchange_weak(pe, 2)) {
        }
    } else {
        int32_t pe = g_pending_enter.load();
        while (pe == 0 && !g_pending_enter.compare_exchange_weak(pe, 1)) {
        }
    }
}

void NoteStill(int64_t now_ms) {
    /* 静止 20s：提前收窗（把还开着的窗口压到现在）。后续新活动会重新开门。 */
    int64_t cur = g_engaged_until_ms.load();
    while (cur > now_ms && !g_engaged_until_ms.compare_exchange_weak(cur, now_ms)) {
    }
}

bool Engaged(int64_t now_ms) {
    return now_ms < g_engaged_until_ms.load();
}

bool Silent(int64_t now_ms) {
    return !Engaged(now_ms);
}

void Flush(int64_t now_ms) {
    const bool eng = Engaged(now_ms);
    if (eng != g_was_engaged) {
        const int32_t pe = g_pending_enter.exchange(0);
        g_was_engaged = eng;
        if (eng) {
            if (g_on_enter) {
                g_on_enter(pe >= 2 ? 1 : 0);
            }
        } else {
            if (g_on_exit) {
                g_on_exit();
            }
        }
    }
    /* 已在态内：活动只默默续窗，不重演见面礼（设计 6.2「已在交互态不重复啵」） */
}

}  // namespace pet_engage
