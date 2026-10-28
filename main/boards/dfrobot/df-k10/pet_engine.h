#ifndef _PET_ENGINE_H_
#define _PET_ENGINE_H_

#include <cstdint>
#include <string>

/*
 * 宠物养成核心引擎 · 本地 C++ 移植
 * =================================================================
 *
 * 本文件是 `cloud/scf/pet-core.js` 的 C++ 移植（「全本地化方案」的核心）。
 *
 * 为什么要它：原方案把「算」放在腾讯云 SCF（跑 pet-core.js），设备只「记 + 呈现」。
 * 全本地化 = 把这一层搬到 K10 本机：数值/衰减/生病/金币/任务全部本地计算，
 * 不再依赖云端 SCF/CLS，云端只留「小智大脑 + 语音」。
 *
 * 移植契约（保证数值逻辑 0 漂移）：
 *   1. 纯函数：不碰网络、不碰存储、不碰时钟——时间一律由调用方传入 `ts`；
 *   2. 状态是同一份 JSON（NVS 循环存取），结构与 pet-core.js 的 doc 完全一致；
 *   3. 入口 `Run()` 与 JS 的 `run(req, env)` 等价，返回 SCF 风格响应 JSON，
 *      因此可以作为 PetCloud 里 HTTP 调用的「本地替身」，响应处理一行不用改；
 *   4. 衰减靠时间戳差（不是定时器），断电三天再开机也能正确补算并封顶。
 *
 * 数值/规则的唯一权威来源是 pet-core.js（含详细中文注释），本移植逐函数对应，
 * 函数命名与 JS 保持一致（feed / applyDecay / submitTask ...），便于对照回归。
 */

namespace pet_engine {

/** 运行时配置。字段与 pet-core.js 的 DEFAULTS 一一对应。 */
struct Config {
    std::string pet_name = "乔利";
    double decay_step_hours = 1.0 / 120.0;  // 默认 30 秒衰减一步（PET_DECAY_STEP_SEC=30）
    int max_decay_steps = 2880;       // 单次最多补算 2880 步（=24h），防久不开机瞬间清零
    int64_t sick_after_starving = 43200;  // 饿到 0 持续 12h → 生病
    int daily_task_reward_cap = 5;
    int exp_per_action = 2;
    int exp_per_level = 50;
    int task_needs_parent_approve = 1;
    int cost_feed = 2;
    int cost_play = 3;
    int cost_heal = 10;
    int cost_medicine = 3;
    int init_coins = 20;
    int daily_fun_cap = 3;           // 讲故事/唱歌/学东西 每日上限
    int full_hunger = 99;
    int action_cooldown_sec = 2;     // 简易节流，挡语音误识别连击刷币
    int coin_log_keep = 8;           // 体积预算：整份 state 目标 < 3KB
    int task_keep = 12;
};

/** 默认配置（= pet-core.js 的 DEFAULTS）。 */
Config DefaultConfig();

/** 覆盖/读取全局配置。设备可在 Init 时从 NVS 载入自定义项。 */
void SetConfig(const Config& cfg);
Config GetConfig();

/**
 * 唯一入口：与 pet-core.js 的 `run(req, env)` 等价。
 *
 * @param state_json  设备带来的完整状态 JSON；nullptr 或空 = 首次，自动初始化。
 * @param action      sync / feed / medicine / play / heal / sleep / story / song /
 *                    teach / pet / chat / task_list / task_submit / task_publish /
 *                    task_approve / task_reject / coins / reset / ping
 * @param args_json   动作参数，如 {"food":"苹果"}；可为 nullptr。
 * @param client_ts   设备时间（秒）；<=0 则用本地时间。偏差>5 分钟以本地时间为准。
 * @param out_response_json  输出：SCF 风格完整响应（含 status/state/events/say/...）。
 * @param out_state_json     输出：应写回 NVS 的新状态 JSON（ping 时为规范化态）。
 * @param out_err    失败原因（仅内部/解析错误时填）。
 * @return 成功 true；无法解析/内部错误 false。
 */
bool Run(const char* state_json,
         const char* action,
         const char* args_json,
         int64_t client_ts,
         std::string& out_response_json,
         std::string& out_state_json,
         std::string& out_err);

/**
 * 仅取状态快照（灯/脸联动、AI 工具用），不修改、不落盘。
 * 输出 getStatus() 对象的 JSON（name/energy/hunger/mood/sick/coins/exp/level/fun_left）。
 */
bool GetStatusJson(const char* state_json, int64_t ts, std::string& out_status_json);

/**
 * 便捷：返回宠物此刻心声（moodWord），给模型当口吻参考。
 */
std::string MoodWordOf(const char* state_json, int64_t ts);

}  // namespace pet_engine

#endif  // _PET_ENGINE_H_
