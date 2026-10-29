#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * BootDiag —— 开机"记账"诊断模块
 * ----------------------------------
 * 解决"手动重启 / 来不及记录就把崩溃现场冲掉"的痛点：
 *   1) 复位原因记账：每次开机把 esp_reset_reason 累计进 NVS，统计分布
 *      （崩溃 / 看门狗 / 欠压 / 手动 各占多少，一眼看出重启类型）
 *   2) shutdown handler：崩溃或重启前，把"死前阶段"固化到 RTC 内存（跨复位保留）
 *   3) 阶段打点：业务在关键节点调用 SetStage，重启后读回来就知道"死在哪一步"
 *
 * 只用稳定 API（esp_reset_reason / nvs / esp_register_shutdown_handler / RTC_NOINIT_ATTR）。
 */

/* 阶段打点值：在关键节点 BootDiag_SetStage(BOOT_STAGE_XXX) */
enum {
    BOOT_STAGE_UNKNOWN       = 0,
    BOOT_STAGE_NVS_READY     = 1,
    BOOT_STAGE_APP_INIT      = 2,
    BOOT_STAGE_DISPLAY_READY = 3,
    BOOT_STAGE_WIFI_CONNECT  = 4,
    BOOT_STAGE_NET_READY     = 5,
    BOOT_STAGE_HTTP_START    = 6,
    BOOT_STAGE_HTTP_READY    = 7,
    BOOT_STAGE_AUDIO_READY   = 8,
    BOOT_STAGE_SPEAKING      = 9,
    BOOT_STAGE_TASK_UPDATE   = 10,
    BOOT_STAGE_RUNNING       = 11,
};

/* 1) 复位原因记账：读 esp_reset_reason 并累计进 NVS，打印历史分布。
 *    必须在 nvs_flash_init() 之后、正常启动流程里调用（勿在 shutdown 里调）。 */
void BootDiag_LogResetReason(void);

/* 2) 注册崩溃/重启前钩子（shutdown handler）：把死前状态固化进 RTC。
 *    handler 内部只写 RTC 内存，不碰 flash/NVS（崩溃上下文安全）。 */
void BootDiag_RegisterShutdownHandler(void);

/* 3) 业务打点：记录当前阶段到 RTC（复位不丢、掉电才丢）。 */
void BootDiag_SetStage(uint32_t stage);

/* 4) 读取/打印上次死前阶段（开机时调用，看看上一次是死在哪儿）。 */
uint32_t BootDiag_GetLastStage(void);
void BootDiag_LogLastStage(void);

#ifdef __cplusplus
}
#endif
