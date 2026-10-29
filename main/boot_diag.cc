#include "boot_diag.h"

#include <esp_log.h>
#include <esp_system.h>
#include <esp_attr.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <string.h>

#define TAG "BootDiag"

/* 复位原因名称：与 esp_reset_reason_t 顺序一致（esp_system.h） */
static const char* kResetNames[] = {
    "UNKNOWN",     /* 0  ESP_RST_UNKNOWN    */
    "POWERON",     /* 1  ESP_RST_POWERON    */
    "EXT",         /* 2  ESP_RST_EXT        */
    "SW",          /* 3  ESP_RST_SW         */
    "PANIC",       /* 4  ESP_RST_PANIC      */
    "INT_WDT",     /* 5  ESP_RST_INT_WDT    */
    "TASK_WDT",    /* 6  ESP_RST_TASK_WDT   */
    "WDT",         /* 7  ESP_RST_WDT        */
    "DEEPSLEEP",   /* 8  ESP_RST_DEEPSLEEP  */
    "BROWNOUT",    /* 9  ESP_RST_BROWNOUT   */
    "SDIO",        /* 10 ESP_RST_SDIO       */
    "USB",         /* 11 ESP_RST_USB        */
};
#define RESET_NAME_COUNT (sizeof(kResetNames) / sizeof(kResetNames[0]))

/* 累计统计，存 NVS blob "reset_hist"（namespace "boot"） */
struct ResetStats {
    uint32_t count[RESET_NAME_COUNT];
    uint32_t total;
    uint32_t last_reason;
};

/* ---- RTC 内存：跨复位保留、掉电才丢，用来记"死前状态" ---- */
RTC_NOINIT_ATTR static uint32_t s_last_stage;      /* 死前阶段 */
RTC_NOINIT_ATTR static uint32_t s_rtc_magic;       /* RTC 有效性魔数（掉电后内容不可信） */
RTC_NOINIT_ATTR static uint32_t s_shutdown_count;  /* 经历 shutdown handler 的次数 */

static constexpr uint32_t kRtcMagic = 0xB007D1A6u;

static const char* ResetName(uint32_t r) {
    return (r < RESET_NAME_COUNT) ? kResetNames[r] : "?";
}

/* shutdown handler：崩溃 / esp_restart 前被调用。
 * 只写 RTC 内存——崩溃上下文里绝不能碰 flash / NVS / 打日志。 */
static void OnShutdown(void) {
    s_shutdown_count++;
    s_rtc_magic = kRtcMagic;   /* 固化"经历了 shutdown"这一事实 */
}

extern "C" void BootDiag_RegisterShutdownHandler(void) {
    esp_err_t err = esp_register_shutdown_handler(OnShutdown);
    ESP_LOGI(TAG, "shutdown handler registered (%s)", esp_err_to_name(err));
}

extern "C" void BootDiag_SetStage(uint32_t stage) {
    s_last_stage = stage;
    s_rtc_magic = kRtcMagic;
}

extern "C" uint32_t BootDiag_GetLastStage(void) {
    return (s_rtc_magic == kRtcMagic) ? s_last_stage : 0;
}

extern "C" void BootDiag_LogLastStage(void) {
    if (s_rtc_magic == kRtcMagic) {
        ESP_LOGI(TAG, "上次死前阶段=%u  shutdown次数=%u",
                 (unsigned)s_last_stage, (unsigned)s_shutdown_count);
    } else {
        ESP_LOGW(TAG, "RTC 状态无效（冷启动/掉电后首次上电），死前阶段不可信");
    }
}

extern "C" void BootDiag_LogResetReason(void) {
    esp_reset_reason_t r = esp_reset_reason();
    ESP_LOGI(TAG, "本次复位原因=%d (%s)", (int)r, ResetName((uint32_t)r));

    /* 累计进 NVS（仅正常启动时调用，shutdown handler 里不可调） */
    ResetStats stats;
    memset(&stats, 0, sizeof(stats));

    nvs_handle_t h;
    esp_err_t err = nvs_open("boot", NVS_READWRITE, &h);
    if (err == ESP_OK) {
        size_t len = sizeof(stats);
        if (nvs_get_blob(h, "reset_hist", &stats, &len) != ESP_OK) {
            memset(&stats, 0, sizeof(stats));  /* 首次，从零开始 */
        }
        if ((uint32_t)r < RESET_NAME_COUNT) {
            stats.count[r]++;
        }
        stats.total++;
        stats.last_reason = (uint32_t)r;
        nvs_set_blob(h, "reset_hist", &stats, sizeof(stats));
        nvs_commit(h);
        nvs_close(h);
    } else {
        ESP_LOGW(TAG, "nvs_open(\"boot\") 失败: %s", esp_err_to_name(err));
    }

    /* 打印历史分布，方便一眼判断重启类型 */
    ESP_LOGI(TAG, "=== 重启原因统计 (共 %u 次) ===", (unsigned)stats.total);
    for (uint32_t i = 0; i < RESET_NAME_COUNT; i++) {
        if (stats.count[i]) {
            ESP_LOGI(TAG, "  %-9s : %u 次", ResetName(i), (unsigned)stats.count[i]);
        }
    }
}
