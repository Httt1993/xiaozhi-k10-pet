#ifndef _PET_ENGAGE_H_
#define _PET_ENGAGE_H_

/*
 * 注意力门控（AttentionGate）：静默 ⇄ 交互 两态机
 * 《AI-Buddy-宠物状态反馈系统完善设计》SECTION 04 的代码化。
 *
 *   SILENT 静默：交互反馈层全关（音效/字幕/庆祝/提醒链 L2+），
 *                状态展示层（脸+灯）照常 —— 慢呼吸/低亮度规格由 DndActive(calm) 处理。
 *   ENGAGED 交互：PET_ATTN_WINDOW_MS（60s）滑动窗口，反馈全开。
 *
 * 进门（任一即开门，续窗）：
 *   被拿起（PICKUP）/ 体感动作 / 人声（VAD）/ 唤醒词 / 按键 / 说出口的话（stt）
 * 提前收窗：
 *   加速度计静止 20s（PET_MOTION_STILL）且窗口内无后续活动。
 *
 * 线程模型（重要）：
 *   NoteActivity/NoteStill 任意线程可调（只写原子时间戳/待处理标记，不碰 UI）。
 *   Flush 只能在 LVGL 线程调（PetFaceView::Tick 每帧），负责触发进/出门 UI 钩子 ——
 *   「音频线程只报信、LVGL 线程才演戏」，和 MotionEngine 的 50Hz/30fps 解耦同一纪律。
 */

#include <cstdint>
#include <functional>

#include "pet_config.h"

namespace pet_engage {

/* 进门钩子：strong=1 拿起/唤醒词（见面礼：好奇脸+青闪+轻响）；strong=0 人声/说话（只青闪） */
using EnterFn = std::function<void(int strong)>;
using ExitFn = std::function<void()>;

void SetHooks(EnterFn on_enter, ExitFn on_exit);

/* 开机调一次：给 PET_ATTN_WINDOW_MS 宽限期，避免刚开机就被静默门锁死 */
void Init(int64_t now_ms);

/* 活动报信（任意线程）。strong: 1=拿起/唤醒/按键 0=人声/普通体感 */
void NoteActivity(int strong, int64_t now_ms);

/* 静止 20s 报信（任意线程）：把还开着的窗口压到现在（提前收窗） */
void NoteStill(int64_t now_ms);

/* 当前是否交互态（纯查询，任意线程） */
bool Engaged(int64_t now_ms);
bool Silent(int64_t now_ms);

/* LVGL Tick 每帧调用：处理进/出门状态迁移 + 触发 UI 钩子（一次性） */
void Flush(int64_t now_ms);

}  // namespace pet_engage

#endif  /* _PET_ENGAGE_H_ */
