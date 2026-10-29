#ifndef _PET_REMIND_H_
#define _PET_REMIND_H_

#include <cstdint>
#include <string>

/*
 * pet_remind —— 儿童状态提醒升级状态机 + 免打扰策略
 * （设计文档《AI-Buddy-儿童状态提醒生动化设计》SECTION 03/06/07 的代码落地）
 *
 * 核心规则：
 *   1. 五级升级链：L1 软提醒(静默脸灯) → L2 出声 → L3 大字幕 → L4 互动求助 → L5 卖萌大招；
 *      孩子一响应立刻庆祝回落，5 分钟内连续响应 3 次本小时降频。
 *   2. 免打扰三道闸（任一命中 = 只留 L1 视觉，禁声禁弹幕）：
 *      时段闸（21:30–7:00 / 上课时段）+ 家长闸（看板 DND 开关）+ 静置闸（10 分钟无人碰）。
 *   3. 远程消息（看板下发）绝不立即演：进补播队列（≤3 条），孩子下次互动 10 分钟内补播，2h 过期。
 *   4. 同状态台词/音效变体轮换，72h 不重复；升级链每小时每状态只走一轮。
 */

namespace pet_remind {

/* 提醒级别 */
enum Level {
    LEVEL_OFF = 0,  /* 无提醒（状态健康） */
    LEVEL_L1,       /* 软提醒：脸+灯（静默） */
    LEVEL_L2,       /* + 音效 + 短台词 */
    LEVEL_L3,       /* + 大字幕 + 灯跑马 */
    LEVEL_L4,       /* + 互动求助（按我/摇我） */
    LEVEL_L5,       /* 卖萌大招（≤15s，之后回 L1） */
};

/* 对外回调（由 PetTools 注册，避免与 pet_tools 循环依赖） */
struct Hooks {
    /* 字幕/通知条（text UTF-8，ms 显示时长） */
    void (*show_notice)(const char* text, int ms);
    /* 音效：0=popup 1=success 2=exclamation 3=vibration */
    void (*play_sound)(int sound_idx);
    /* 按状态映射表应用脸+LED（calm=1 时灯效封顶慢呼吸、亮度封顶 4 级） */
    void (*apply_face_led)(const char* state, int calm);
    /* 庆祝演出（孩子响应/状态好转）：绿呼吸 + 开心脸 */
    void (*celebrate)(int ms);
};

void SetHooks(const Hooks& hooks);

/* 开机调用一次（恢复内部计时基线） */
void Init(int64_t now_ms);

/*
 * 状态变化入口（PetTools::ReactToEvents 调用）。
 * state 取值：sick / starving / hungry / tired / sad / bored / great / normal
 * normal/great → 停止提醒链（状态好转时若此前有活跃提醒 → 触发庆祝）。
 */
void OnState(const char* state, int64_t now_ms);

/* 2s 一拍（板级定时器）：驱动升级链、L5 定时退场、补播队列 */
void Tick(int64_t now_ms);

/* 体感事件（pet_face_view 事件环调用）——孩子动手 = 响应 */
void OnMotion(int motion_event, int64_t now_ms);

/* 显式响应（喂药长按 / 看板打卡 等），kind 仅用于日志 */
void OnChildResponse(const char* kind, int64_t now_ms);

/* 家长闸：看板 DND 开关 */
void SetParentDnd(bool on);
bool ParentDnd();

/* 当前是否处于免打扰（三道闸任一命中） */
bool DndActive(int64_t now_ms);

/* 远程消息入队（看板下发的通知/加油），由补播队列择机演 */
void QueueRemote(const char* text, int64_t now_ms);

/* 当前提醒状态 JSON（/api/remind 用） */
std::string StatusJson(int64_t now_ms);

/* 看板指令：cheer(加油) / dnd_on / dnd_off / checkin(打卡好转) / calm(立即安静) */
bool Command(const char* cmd, const char* arg, int64_t now_ms);

}  // namespace pet_remind

#endif  // _PET_REMIND_H_
