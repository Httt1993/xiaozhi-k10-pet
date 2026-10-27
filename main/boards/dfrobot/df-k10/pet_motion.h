#ifndef _PET_MOTION_H_
#define _PET_MOTION_H_

/*
 * 体感引擎（MotionEngine）：50Hz 读 SC7A20H，把「拍一拍 / 猛摇 / 摔落 / 侧躺」
 * 翻译成离散事件，供表情状态机消费。
 *
 * 线程模型（重要）：
 *   本类在独立 FreeRTOS 任务里跑 50Hz 采样，**绝不碰 LVGL**；
 *   只把事件丢进 FreeRTOS 队列 + 更新两个 float（视线值）。
 *   LVGL 侧（PetFaceView::Tick）调 PetMotionDrain() 取事件，
 *   所有屏幕绘制都在 LVGL 线程完成 —— 这是「传感器 50Hz 与 UI 30fps 解耦」的落地方式。
 *
 * 阈值全部在 pet_config.h（PET_MOTION_*），单位毫克，按 SC7A20H ±2g 量程。
 */

#include "pet_config.h"
#include "pet_sensors.h"

/* 事件表（按情绪状态机的优先级从高到低排列）。
 * 枚举放在开关外 —— PET_MOTION_ENABLE=0 的空壳签名也要用它。 */
enum PetMotionEvent {
    PET_MOTION_NONE = 0,
    PET_MOTION_FALL,       /* 摔落+落地冲击   → 惊吓(P0) → 难过 */
    PET_MOTION_SHAKE,      /* 猛摇            → 晕眩(P1) */
    PET_MOTION_FLIP,       /* 倒置            → 生气(P1) */
    PET_MOTION_GENTLE,     /* 轻摇安抚        → 平静/开心(P3) */
    PET_MOTION_TAP,        /* 拍一拍          → 开心(P3) + Q 弹 */
    PET_MOTION_FEED,       /* 按键喂食        → 开心(P3) + 咀嚼 */
    PET_MOTION_PET,        /* 摸摸头          → 开心(P3) */
    PET_MOTION_PICKUP,     /* 被拿起          → 好奇(P3) */
    PET_MOTION_SIDE_LIE,   /* 侧躺            → 困(P4) + 整脸倾斜 */
    PET_MOTION_SIDE_END,   /* 躺正            → 回平静 */
    PET_MOTION_STILL,      /* 静止 20s        → 困(P4) */
    PET_MOTION_UNFLIP,     /* 翻正            → 回平静 */
    PET_MOTION_WAKE,       /* 静止后被扰动    → 醒 */
};

#if PET_MOTION_ENABLE

/* 启动 50Hz 采样任务（无传感器时内部自检后自动停摆，不影响开机） */
void PetMotionInit(PetSensors* sensors);

/* 按键 / 云端等外部源注入事件（线程安全，进同一队列） */
void PetMotionInject(PetMotionEvent ev);

/* LVGL 线程调用：把队列里的事件一次取空，返回取到的条数 */
int PetMotionDrain(PetMotionEvent* out, int max_out);

/* 重力视线（像素偏移，±6px 平滑值），给 PetFaceView 做视线追踪 */
void PetMotionGetGaze(float* gx, float* gy);

/* 最近 3 秒内有明显体动（用于「静止睡着/被吵醒」判定） */
bool PetMotionActive(void);

/* 侧躺倾角（度，左右 ±15），脸跟着歪 */
float PetMotionTiltDeg(void);

/* 最近一次事件名（给 self.pet.get_status 的 motion_event 字段） */
const char* PetMotionLastEventName(void);

#else  /* PET_MOTION_ENABLE == 0 → 空壳 */

inline void PetMotionInit(PetSensors*) {}
inline void PetMotionInject(PetMotionEvent) {}
inline int PetMotionDrain(PetMotionEvent*, int) { return 0; }
inline void PetMotionGetGaze(float* gx, float* gy) {
    if (gx) *gx = 0;
    if (gy) *gy = 0;
}
inline bool PetMotionActive(void) { return false; }
inline float PetMotionTiltDeg(void) { return 0; }
inline const char* PetMotionLastEventName(void) { return "none"; }

#endif  /* PET_MOTION_ENABLE */

#endif  /* _PET_MOTION_H_ */
