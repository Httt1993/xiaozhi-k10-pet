#include <esp_log.h>
#include <esp_err.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_event.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "application.h"
#include "boot_diag.h"

#define TAG "main"

extern "C" void app_main(void)
{
    // Initialize NVS flash for WiFi configuration
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS flash to fix corruption");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // ---- 诊断记账：让设备自己记录"上一次怎么复位的 / 死在哪一步" ----
    BootDiag_RegisterShutdownHandler();   // 崩溃/重启前固化死前状态到 RTC
    BootDiag_LogResetReason();            // 复位原因累计进 NVS + 打印分布
    BootDiag_LogLastStage();              // 看看上一次死在哪个阶段
    BootDiag_SetStage(BOOT_STAGE_NVS_READY);

    // Initialize and run the application
    auto& app = Application::GetInstance();
    BootDiag_SetStage(BOOT_STAGE_APP_INIT);
    app.Initialize();
    BootDiag_SetStage(BOOT_STAGE_RUNNING);
    app.Run();  // This function runs the main event loop and never returns
}
