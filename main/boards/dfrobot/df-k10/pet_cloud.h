#ifndef _PET_CLOUD_H_
#define _PET_CLOUD_H_

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

/*
 * 宠物云端客户端
 * =================================================================
 *
 * 分工：
 *   云端（腾讯云 SCF）负责「算」—— 按真实时间差算衰减、判生病、算金币、判任务；
 *   设备（本类）负责「记」和「呈现」—— 状态整份存 NVS，串口/灯/脸都由它驱动。
 *
 * 为什么云函数是无状态的：
 *   免费额度按 GB·s 计，无状态函数一次调用约 12 GB·s，一天几十次也才几百，
 *   而常驻式设计要烧掉四成额度。另外设备断电重连时把 lastDecay 带上去，
 *   云端照样能算出「这三天它饿了多少」，所以不需要云端数据库。
 *
 * 断网怎么办：
 *   降级成本地演化（同一套数值规则的精简实现），状态照样存 NVS。
 *   恢复网络后把本地 state 原样发给云端，云端 normalizeDoc 会兼容，
 *   不会出现「离线期间白养一场」。
 */

/** 宠物状态快照。给灯/脸联动和 AI 工具用，不含原始 JSON。 */
struct PetStatus {
    bool valid = false;
    std::string name = "乔利";
    int energy = 0;
    int hunger = 0;
    int mood = 0;
    int coins = 0;
    int exp = 0;
    int level = 1;
    bool sick = false;
    std::string mood_word;                /* 宠物此刻的心声 */
    std::vector<std::string> events;      /* sick / starving / hungry / tired / sad / bored / great */
};

/** 一次动作的结果 */
struct PetActionResult {
    bool ok = false;
    bool from_cloud = false;   /* false = 本地兜底算出来的 */
    std::string code;          /* 云端返回的错误码，如 FULL / NO_COIN / NOT_SICK */
    std::string say;           /* 宠物要说的话（模型可直接转述） */
    std::string error;         /* 网络/解析错误，仅日志用 */
    std::vector<std::string> events;
    int coins = 0;
};

class PetCloud {
public:
    PetCloud();
    ~PetCloud();

    /** 从 NVS 读配置和状态。Board 构造时调用一次。 */
    void Init();

    /** 是否配置了云端地址 */
    bool cloud_enabled() const { return !url_.empty(); }

    /** 当前状态快照（纯内存，不联网，随时可调） */
    PetStatus GetStatus();

    /**
     * 执行一个动作。
     * action 与云端 pet-core.js 的 run() 完全一致：
     *   sync / feed / medicine / play / heal / sleep / story / song / teach /
     *   pet / chat / task_list / task_submit / task_publish / task_approve /
     *   task_reject / coins / reset
     * args_json 形如 {"food":"苹果"}，可为 nullptr。
     */
    PetActionResult Act(const std::string& action, const std::string& args_json);

    /** 任务清单的人话版，直接给模型 */
    std::string TaskListText(bool include_done = false);

    /** 云端最近一次成功同步的耗时（毫秒），0 = 还没成功过 */
    int last_latency_ms() const { return last_latency_ms_; }

    /** 连续失败次数，工具层据此决定要不要提示「现在没联网」 */
    int fail_streak() const { return fail_streak_; }

private:
    /* Init 内部版本：调用方已持锁时用，避免 std::mutex 不可重入导致死锁 */
    void InitLocked();

    /* ---- 网络 ---- */
    bool HttpPost(const std::string& body, std::string& out, std::string& err);

    /* ---- 响应处理 ---- */
    bool ApplyResponse(const char* resp, PetActionResult& out);
    void RefreshStatusFromState();

    /* ---- 本地降级 ---- */
    PetActionResult ActLocal(const std::string& action, const std::string& args_json);
    void DecayLocal(int64_t now);

    /* ---- 状态存取 ---- */
    void LoadFromNvs();
    void PersistState();
    void PersistStatus();

    mutable std::mutex mutex_;
    std::string state_json_;   /* 整份状态 JSON，云端和本地共用同一格式 */
    PetStatus status_;
    std::string url_;
    std::string key_;

    int64_t last_sync_ms_ = 0;
    int last_latency_ms_ = 0;
    int fail_streak_ = 0;
    bool inited_ = false;
};

#endif  // _PET_CLOUD_H_
