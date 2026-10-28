#ifndef _PET_CLOUD_H_
#define _PET_CLOUD_H_

#include <cstdint>
#include <functional>
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
 *   一次调用约 0.06 GB·s（128MB × 0.5s），加上 60 秒一次的心跳，
 *   一个月 43200 次、2700 GB·s —— 腾讯云前三个月免费额度内；
 *   三个月后转按量计费约 0.4 元/月，迁到华为云 FunctionGraph 则永久免费
 *   （免费额度 100 万次 + 40 万 GB·s/月，见 cloud/README.md「成本」）。
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
     *   task_reject / task_delete / coins / reset
     * args_json 形如 {"food":"苹果"}，可为 nullptr。
     */
    PetActionResult Act(const std::string& action, const std::string& args_json);

    /**
     * 动作完成钩子：每次 Act() 结束（无论云端还是本地算）都会回调。
     * 用途：家长看板/HTTP 路径执行完动作后，同步触发设备端「灯 + 脸 + 提示」联动，
     * 否则看板点按钮设备没任何反应（测试项 A-11/A-12）。
     * 线程：在调用 Act 的线程里同步执行，钩子里不要做耗时操作。
     */
    using ActionHook = std::function<void(const PetActionResult& result, const std::string& action)>;
    void SetActionHook(ActionHook hook);

    /** 任务清单的人话版，直接给模型 */
    std::string TaskListText(bool include_done = false);

    /** 任务清单的 JSON 数组字符串（家长看板用），如 [{"id":"t1","title":"写作业","reward":10,"status":"pending"},...] */
    std::string GetTasksJson(bool include_done = false);

    /** 未完成任务数（pending+submitted），设备屏角标用；只读本地 state，不发网络请求 */
    int CountOpenTasks();

    /**
     * 启动后台心跳：每 interval_sec 秒静默 sync 一次，把家长看板排队的指令取回来。
     *
     * 为什么要它：家长在网页上点了「喂饭」，设备不主动来问就永远不知道。
     * 心跳就是「设备每隔一会儿来问一句：有我的事吗」。
     *
     * 为什么不是每 5 分钟就无脑写一次 NVS：
     * 衰减是每 0.5 小时才动一步，5 分钟里状态大概率没变。
     * 如果每次都落盘，一年就是十万次擦写，NVS 分区撑不住。
     * 所以心跳走 keep_memory=true 的路径：内存态照更新，只有"值得记"的变化才落盘。
     *
     * interval_sec <= 0 表示不启动（PET_CLOUD_URL 为空时启动也没意义）。
     */
    void StartHeartbeat(int interval_sec);

    /** 云端最近一次成功同步的耗时（毫秒），0 = 还没成功过 */
    int last_latency_ms() const { return last_latency_ms_; }

    /** 连续失败次数，工具层据此决定要不要提示「现在没联网」 */
    int fail_streak() const { return fail_streak_; }

private:
    /* Init 内部版本：调用方已持锁时用，避免 std::mutex 不可重入导致死锁 */
    void InitLocked();

    /* ---- 网络 ---- */
    bool HttpPost(const std::string& body, std::string& out, std::string& err);

    /* ---- 心跳 ---- */
    static void HeartbeatTask(void* arg);
    bool HeartbeatEligible();
    /* 状态指纹：只挑"重启后不能丢"的字段。和上次一样就不写 NVS。 */
    std::string StateFingerprint() const;

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
    ActionHook action_hook_;   /* Act 完成后的设备联动钩子 */

    int64_t last_sync_ms_ = 0;
    int last_latency_ms_ = 0;
    int fail_streak_ = 0;
    bool inited_ = false;

    /* ---- 心跳 ---- */
    void* heartbeat_task_ = nullptr;   /* TaskHandle_t，头文件不引 FreeRTOS，用 void* 挡一下 */
    int heartbeat_sec_ = 0;
    int heartbeat_ok_ = 0;
    int heartbeat_fail_ = 0;
    std::string persisted_fingerprint_;  /* 上次真正写进 NVS 的状态指纹 */
};

#endif  // _PET_CLOUD_H_
