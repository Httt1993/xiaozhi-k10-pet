#include "pet_remind.h"

#include <esp_log.h>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "pet_config.h"
#include "pet_motion.h"

#define TAG "PetRemind"

#if PET_REMIND_ENABLE

namespace pet_remind {

namespace {

Hooks g_hooks;

/* ---- 当前状态 ---- */
/* 必须是 string 拷贝：OnState 的入参常指向临时 events vector，存裸指针必悬空 */
std::string g_state = "normal";
Level g_level = LEVEL_OFF;
int64_t g_level_since_ms = 0;   /* 进入当前级别的时间 */
int64_t g_l5_until_ms = 0;      /* L5 演完自动退场时间 */
int64_t g_last_touch_ms = 0;    /* 任何体感/按键（含拿起） */
int64_t g_last_response_ms = 0;
int g_response_streak = 0;      /* 5 分钟窗口内的响应次数 */
int64_t g_streak_start_ms = 0;
bool g_parent_dnd = false;
int g_chain_done_hour = -1;     /* 本轮升级链已走完的小时（每小时每状态一轮） */
bool g_responded_this_hour = false;
int g_last_say_hour = -1;       /* 台词去重：同一句 72h 不重复（简化为轮换） */

/* 变体轮换下标（每状态独立，轮完一圈从头来，绝不连续重复） */
int g_variant_idx[8] = {0, 0, 0, 0, 0, 0, 0, 0};

/* 远程消息补播队列 */
struct RemoteItem {
    char text[96];
    int64_t ts;
    bool used;
};
RemoteItem g_queue[3];

constexpr int64_t kEscalateL2Ms = 30 * 1000;
constexpr int64_t kEscalateL3Ms = 60 * 1000;
constexpr int64_t kEscalateL4Ms = 120 * 1000;
constexpr int64_t kEscalateL5Ms = 300 * 1000;
constexpr int64_t kL5ShowMs = 15 * 1000;
constexpr int64_t kStillGateMs = 10 * 60 * 1000;   /* 静置闸：10 分钟无人碰 */
constexpr int64_t kRemoteTtlMs = 2 * 60 * 60 * 1000; /* 补播 2h 过期 */
constexpr int64_t kStreakWindowMs = 5 * 60 * 1000;
constexpr int kStreakSnoozeCount = 3;

/* 状态下标（与变体池对齐） */
int StateIdx(const char* s) {
    if (strcmp(s, "sick") == 0) return 0;
    if (strcmp(s, "starving") == 0) return 1;
    if (strcmp(s, "hungry") == 0) return 2;
    if (strcmp(s, "tired") == 0) return 3;
    if (strcmp(s, "sad") == 0) return 4;
    if (strcmp(s, "bored") == 0) return 5;
    if (strcmp(s, "great") == 0) return 6;
    return 7; /* normal */
}

/* 台词池（≥4 变体，第一人称求助式、≤12 字，出自设计文档 SECTION 04） */
const char* const kSay[8][4] = {
    /* sick */
    {"宝宝……我头好晕呀", "我好像发烧了……抱抱我", "好难受，照顾我一下", "陪我休息一会儿～"},
    /* starving */
    {"咕噜噜……肚子在叫啦", "好饿好饿……饭呢？", "咕～肚子咕咕叫了", "我们去吃饭饭！"},
    /* hungry */
    {"肚子有点饿啦～", "想吃点东西啦", "咕～开饭了吗？", "闻到饭香啦！"},
    /* tired */
    {"好累呀，歇一会儿", "陪我躺一会儿吧", "眼皮好重……", "我们休息一下～"},
    /* sad */
    {"你好像不开心……", "抱抱我好不好？", "我陪你坐一会儿", "不想说也没关系"},
    /* bored */
    {"有点无聊……陪陪我", "我们玩点什么吧？", "讲个故事好不好？", "动一动嘛～"},
    /* great */
    {"今天状态超好！", "嘿嘿，真开心～", "一起玩呀！", "感觉棒棒的！"},
    /* normal */
    {"感觉还不错～", "安心啦", "就这样待着挺好", "嗯嗯，平静"},
};

bool IsNight(int64_t now_ms) {
    time_t now = static_cast<time_t>(now_ms / 1000);
    struct tm lt;
    if (now < 1577836800 /* 2020-01-01 时钟未同步 */ || localtime_r(&now, &lt) == nullptr) {
        return false;
    }
    int minutes = lt.tm_hour * 60 + lt.tm_min;
    return minutes >= 21 * 60 + 30 || minutes < 7 * 60;
}

int HourOf(int64_t now_ms) {
    time_t now = static_cast<time_t>(now_ms / 1000);
    struct tm lt;
    if (localtime_r(&now, &lt) == nullptr) {
        return -1;
    }
    return lt.tm_hour;
}

const char* NextVariant() {
    int idx = StateIdx(g_state.c_str());
    const char* text = kSay[idx][g_variant_idx[idx] % 4];
    g_variant_idx[idx]++;
    return text;
}

void PlayLevelShow(Level lv, int64_t now_ms) {
    if (g_hooks.play_sound == nullptr || g_hooks.show_notice == nullptr) {
        return;
    }
    /* 免打扰只到 L1，不会走到这里；双保险再拦一次 */
    if (DndActive(now_ms) && lv >= LEVEL_L2) {
        return;
    }
    switch (lv) {
        case LEVEL_L2:
            g_hooks.play_sound(0); /* popup 轻响 */
            g_hooks.show_notice(NextVariant(), 2500);
            break;
        case LEVEL_L3:
            g_hooks.play_sound(2); /* exclamation */
            g_hooks.show_notice(NextVariant(), 3000);
            break;
        case LEVEL_L4: {
            char buf[112];
            snprintf(buf, sizeof(buf), "%s（按我一下帮帮我）", NextVariant());
            g_hooks.play_sound(3); /* vibration 提醒音 */
            g_hooks.show_notice(buf, 3500);
            break;
        }
        case LEVEL_L5:
            g_hooks.play_sound(2);
            g_hooks.show_notice("呜呜……陪陪我嘛……", 4000);
            break;
        default:
            g_hooks.show_notice(NextVariant(), 2000);
            break;
    }
}

void StartChain(const char* state, int64_t now_ms) {
    g_state = state;
    g_level = LEVEL_L1;
    g_level_since_ms = now_ms;
    g_l5_until_ms = 0;
    g_responded_this_hour = false;
    g_chain_done_hour = -1;
    /* L1 视觉：脸+灯立即对齐（免打扰时 calm=1） */
    if (g_hooks.apply_face_led != nullptr) {
        g_hooks.apply_face_led(state, DndActive(now_ms) ? 1 : 0);
    }
    ESP_LOGI(TAG, "PET_PATCH: remind chain start state=%s dnd=%d", state,
             DndActive(now_ms) ? 1 : 0);
}

void StopChain(int64_t now_ms, bool celebrate) {
    if (celebrate && g_level >= LEVEL_L2 && g_hooks.celebrate != nullptr) {
        g_hooks.celebrate(2500);
    }
    g_level = LEVEL_OFF;
    g_level_since_ms = now_ms;
    g_l5_until_ms = 0;
}

}  // namespace

void SetHooks(const Hooks& hooks) {
    g_hooks = hooks;
}

void Init(int64_t now_ms) {
    g_last_touch_ms = now_ms; /* 开机算一次「有人在」，避免刚开机就被静置闸锁死 */
    g_level = LEVEL_OFF;
}

void OnState(const char* state, int64_t now_ms) {
    if (state == nullptr) {
        return;
    }
    if (state == g_state && g_level != LEVEL_OFF) {
        return; /* 状态没变，链继续 */
    }

    bool was_active = g_level != LEVEL_OFF;
    bool healthy = (strcmp(state, "normal") == 0 || strcmp(state, "great") == 0);

    if (healthy) {
        if (was_active) {
            /* 状态好转：庆祝收尾（病愈/吃饱的因果反馈） */
            StopChain(now_ms, true);
        }
        g_state = state;
        if (g_hooks.apply_face_led != nullptr && !was_active) {
            g_hooks.apply_face_led(state, DndActive(now_ms) ? 1 : 0);
        }
        return;
    }

    /* 不健康状态：重启提醒链（恶化穿透——立刻换脸换灯，但声音仍受免打扰约束） */
    StartChain(state, now_ms);
}

void Tick(int64_t now_ms) {
    /* ---- 补播队列：孩子近 10 分钟内动过设备才播，一次一条，间隔 20s ---- */
    {
        static int64_t last_replay_ms = 0;
        bool child_near = (now_ms - g_last_touch_ms) < kStillGateMs;
        if (child_near && !DndActive(now_ms) && now_ms - last_replay_ms > 20000) {
            for (auto& item : g_queue) {
                if (!item.used && item.text[0] != '\0') {
                    if (now_ms - item.ts > kRemoteTtlMs) {
                        item.used = true; /* 过期丢弃 */
                        continue;
                    }
                    if (g_hooks.show_notice != nullptr) {
                        g_hooks.show_notice(item.text, 3000);
                    }
                    if (g_hooks.play_sound != nullptr) {
                        g_hooks.play_sound(1); /* success 小甜音 */
                    }
                    item.used = true;
                    last_replay_ms = now_ms;
                    ESP_LOGI(TAG, "PET_PATCH: replay remote msg: %s", item.text);
                    break;
                }
            }
        }
    }

    if (g_level == LEVEL_OFF) {
        return;
    }

    /* ---- L5 演完自动退场 ---- */
    if (g_level == LEVEL_L5 && g_l5_until_ms != 0 && now_ms >= g_l5_until_ms) {
        g_level = LEVEL_L1;
        g_level_since_ms = now_ms;
        g_chain_done_hour = HourOf(now_ms); /* 这轮链走完，本小时不再升 */
        ESP_LOGI(TAG, "PET_PATCH: L5 done -> back to L1 quiet");
        return;
    }

    /* ---- 免打扰：级别封顶 L1 ---- */
    if (DndActive(now_ms)) {
        if (g_level >= LEVEL_L2) {
            g_level = LEVEL_L1;
            g_level_since_ms = now_ms;
            ESP_LOGI(TAG, "PET_PATCH: dnd active, drop to L1 quiet");
        }
        return;
    }

    /* 本小时已走过一轮 / 已响应过 / 连击降频 → 停在 L1 观察 */
    int hour = HourOf(now_ms);
    if (g_responded_this_hour || g_chain_done_hour == hour) {
        return;
    }

    int64_t elapsed = now_ms - g_level_since_ms;
    switch (g_level) {
        case LEVEL_L1:
            if (elapsed >= kEscalateL2Ms) {
                g_level = LEVEL_L2;
                g_level_since_ms = now_ms;
                PlayLevelShow(LEVEL_L2, now_ms);
            }
            break;
        case LEVEL_L2:
            if (elapsed >= kEscalateL3Ms) {
                g_level = LEVEL_L3;
                g_level_since_ms = now_ms;
                PlayLevelShow(LEVEL_L3, now_ms);
            }
            break;
        case LEVEL_L3:
            if (elapsed >= kEscalateL4Ms) {
                g_level = LEVEL_L4;
                g_level_since_ms = now_ms;
                PlayLevelShow(LEVEL_L4, now_ms);
            }
            break;
        case LEVEL_L4:
            if (elapsed >= kEscalateL5Ms) {
                g_level = LEVEL_L5;
                g_level_since_ms = now_ms;
                g_l5_until_ms = now_ms + kL5ShowMs;
                PlayLevelShow(LEVEL_L5, now_ms);
            }
            break;
        default:
            break;
    }
}

void OnMotion(int motion_event, int64_t now_ms) {
    g_last_touch_ms = now_ms;
    /* 真正算「响应」的：拍一拍 / 按键喂食 / 摸摸头 / 轻摇安抚 */
    if (motion_event == PET_MOTION_TAP || motion_event == PET_MOTION_FEED ||
        motion_event == PET_MOTION_PET || motion_event == PET_MOTION_GENTLE) {
        OnChildResponse("motion", now_ms);
    }
}

void OnChildResponse(const char* kind, int64_t now_ms) {
    g_last_touch_ms = now_ms;
    g_last_response_ms = now_ms;
    (void)kind;

    /* 5 分钟连击：3 次响应 → 本小时降频（不再升级） */
    if (g_streak_start_ms == 0 || now_ms - g_streak_start_ms > kStreakWindowMs) {
        g_streak_start_ms = now_ms;
        g_response_streak = 1;
    } else {
        g_response_streak++;
    }
    if (g_response_streak >= kStreakSnoozeCount) {
        g_responded_this_hour = true;
    }

    if (g_level != LEVEL_OFF) {
        /* 一响应立刻庆祝回落——绝不「叫到 L5 必须把话说完」 */
        if (g_level >= LEVEL_L2 && g_hooks.celebrate != nullptr) {
            g_hooks.celebrate(2200);
        }
        g_level = LEVEL_L1;
        g_level_since_ms = now_ms;
        g_l5_until_ms = 0;
        g_responded_this_hour = true;
        ESP_LOGI(TAG, "PET_PATCH: child responded (%s), celebrate + quiet L1", kind);
    }
}

void SetParentDnd(bool on) {
    g_parent_dnd = on;
    ESP_LOGI(TAG, "PET_PATCH: parent dnd = %d", on ? 1 : 0);
}

bool ParentDnd() {
    return g_parent_dnd;
}

bool DndActive(int64_t now_ms) {
    /* 三道闸：时段 / 家长开关 / 静置 10 分钟 */
    if (IsNight(now_ms)) {
        return true;
    }
    if (g_parent_dnd) {
        return true;
    }
    if (now_ms - g_last_touch_ms > kStillGateMs) {
        return true;
    }
    return false;
}

void QueueRemote(const char* text, int64_t now_ms) {
    if (text == nullptr || text[0] == '\0') {
        return;
    }
    /* 找空位；满了丢最旧 */
    RemoteItem* slot = nullptr;
    for (auto& item : g_queue) {
        if (item.used || item.text[0] == '\0') {
            slot = &item;
            break;
        }
    }
    if (slot == nullptr) {
        slot = &g_queue[0];
        for (auto& item : g_queue) {
            if (item.ts < slot->ts) {
                slot = &item;
            }
        }
    }
    snprintf(slot->text, sizeof(slot->text), "%s", text);
    slot->ts = now_ms;
    slot->used = false;
    ESP_LOGI(TAG, "PET_PATCH: queued remote msg: %s", text);
}

std::string StatusJson(int64_t now_ms) {
    char buf[320];
    const char* dnd_reason = "none";
    if (IsNight(now_ms)) {
        dnd_reason = "night";
    } else if (g_parent_dnd) {
        dnd_reason = "parent";
    } else if (now_ms - g_last_touch_ms > kStillGateMs) {
        dnd_reason = "still";
    }
    int queued = 0;
    for (const auto& item : g_queue) {
        if (!item.used && item.text[0] != '\0') {
            queued++;
        }
    }
    snprintf(buf, sizeof(buf),
             "{\"state\":\"%s\",\"level\":%d,\"dnd\":%s,\"dnd_reason\":\"%s\","
             "\"queued\":%d,\"streak\":%d,\"responded\":%s}",
             g_state.c_str(), static_cast<int>(g_level), DndActive(now_ms) ? "true" : "false",
             dnd_reason, queued, g_response_streak,
             g_responded_this_hour ? "true" : "false");
    return std::string(buf);
}

bool Command(const char* cmd, const char* arg, int64_t now_ms) {
    if (cmd == nullptr) {
        return false;
    }
    if (strcmp(cmd, "dnd_on") == 0) {
        SetParentDnd(true);
        return true;
    }
    if (strcmp(cmd, "dnd_off") == 0) {
        SetParentDnd(false);
        return true;
    }
    if (strcmp(cmd, "cheer") == 0) {
        /* 家长「给它加油」：入补播队列，不立即演（SECTION 07 规则④） */
        QueueRemote(arg && arg[0] ? arg : "爸爸/妈妈给你点赞啦！", now_ms);
        return true;
    }
    if (strcmp(cmd, "checkin") == 0) {
        /* 打卡「已吃药/已吃饭/已睡觉」→ 视为状态好转确认，庆祝收尾 */
        StopChain(now_ms, true);
        g_state = "normal";
        return true;
    }
    if (strcmp(cmd, "calm") == 0) {
        StopChain(now_ms, false);
        return true;
    }
    return false;
}

}  // namespace pet_remind

#else /* PET_REMIND_ENABLE == 0 → 空壳 */

namespace pet_remind {
void SetHooks(const Hooks&) {}
void Init(int64_t) {}
void OnState(const char*, int64_t) {}
void Tick(int64_t) {}
void OnMotion(int, int64_t) {}
void OnChildResponse(const char*, int64_t) {}
void SetParentDnd(bool) {}
bool ParentDnd() { return false; }
bool DndActive(int64_t) { return true; }
void QueueRemote(const char*, int64_t) {}
std::string StatusJson(int64_t) { return "{\"level\":0,\"dnd\":true}"; }
bool Command(const char*, const char*, int64_t) { return false; }
}  // namespace pet_remind

#endif  // PET_REMIND_ENABLE
