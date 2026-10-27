#include "pet_config.h"
#include "sd_probe.h"

/* PET_SD_PROBE=0 时编译空 stub，跳过 210 轮引脚扫描 */
#if PET_SD_PROBE

#include <cstdio>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_err.h>
#include <esp_log.h>
#include <esp_vfs_fat.h>

#include <driver/gpio.h>
#include <driver/sdmmc_host.h>
#include <driver/sdspi_host.h>
#include <driver/spi_master.h>
#include <sdmmc_cmd.h>

static const char* TAG = "SD-PROBE";

namespace {

/*
 * 候选引脚：把 K10 上「确定没被占用」的 GPIO 列出来。
 *
 * 已经排掉的（config.h 里能查到）：
 *   0/3/4/5/6/7/8/9/10/11/12/13/14/15/16/17/18/19/20/21  屏幕 + 摄像头 + 音频
 *   39/45/46/47/48                                       音频 + RGB + I2C
 *   26~32                                                SPI Flash
 *   33~37                                                N16R8 的 Octal PSRAM
 *
 * 剩下能用的就是下面这 7 个。
 */
constexpr int kCands[] = {1, 2, 40, 41, 42, 43, 44};
constexpr int kN = static_cast<int>(sizeof(kCands) / sizeof(kCands[0]));

constexpr const char* kMount = "/sdprobe";
/* 共享屏幕那对 SPI 引脚（df_k10_board.cc 里 InitializeSpi 用的） */
constexpr int kLcdSclk = 12;
constexpr int kLcdMosi = 21;

bool ProbeSpi(int sclk, int mosi, int miso, int cs) {
    spi_bus_config_t bus = {};
    bus.mosi_io_num = mosi;
    bus.miso_io_num = miso;
    bus.sclk_io_num = sclk;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = 4000;

    if (spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        return false;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI3_HOST;

    sdspi_device_config_t slot = {};
    slot.host_id = SPI3_HOST;
    slot.gpio_cs = static_cast<gpio_num_t>(cs);
    slot.gpio_cd = SDSPI_SLOT_NO_CD;
    slot.gpio_wp = GPIO_NUM_NC;
    slot.gpio_int = GPIO_NUM_NC;

    esp_vfs_fat_mount_config_t mcfg = {};
    mcfg.format_if_mount_failed = false;
    mcfg.max_files = 4;
    mcfg.allocation_unit_size = 16 * 1024;

    sdmmc_card_t* card = nullptr;
    esp_err_t err = esp_vfs_fat_sdspi_mount(kMount, &host, &slot, &mcfg, &card);
    bool ok = (err == ESP_OK);
    if (ok) {
        ESP_LOGW(TAG, "★★★ 命中 SPI 模式：SCLK=%d MOSI=%d MISO=%d CS=%d", sclk, mosi, miso, cs);
        sdmmc_card_print_info(stdout, card);
        esp_vfs_fat_sdcard_unmount(kMount, card);
    }
    spi_bus_free(SPI3_HOST);
    return ok;
}

bool ProbeSdmmc1(int clk, int cmd, int d0) {
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT;
    host.max_freq_khz = 400;  /* 探测阶段用最低速，失败得快 */

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = static_cast<gpio_num_t>(clk);
    slot.cmd = static_cast<gpio_num_t>(cmd);
    slot.d0 = static_cast<gpio_num_t>(d0);
    slot.d1 = GPIO_NUM_NC;
    slot.d2 = GPIO_NUM_NC;
    slot.d3 = GPIO_NUM_NC;
    slot.width = 1;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_mount_config_t mcfg = {};
    mcfg.format_if_mount_failed = false;
    mcfg.max_files = 4;
    mcfg.allocation_unit_size = 16 * 1024;

    sdmmc_card_t* card = nullptr;
    esp_err_t err = esp_vfs_fat_sdmmc_mount(kMount, &host, &slot, &mcfg, &card);
    bool ok = (err == ESP_OK);
    if (ok) {
        ESP_LOGW(TAG, "★★★ 命中 SDMMC 1-bit：CLK=%d CMD=%d D0=%d", clk, cmd, d0);
        sdmmc_card_print_info(stdout, card);
        esp_vfs_fat_sdcard_unmount(kMount, card);
    }
    sdmmc_host_deinit();
    return ok;
}

}  // namespace

bool PetSdCardProbe() {
    /* 挂载失败会刷一堆 sdmmc 的错误日志，探测期先闭掉 */
    esp_log_level_set("sdmmc_common", ESP_LOG_NONE);
    esp_log_level_set("sdmmc_cmd", ESP_LOG_NONE);
    esp_log_level_set("sdspi_host", ESP_LOG_NONE);
    esp_log_level_set("sdspi_io", ESP_LOG_NONE);

    ESP_LOGW(TAG, "开始探测 TF 卡，候选引脚 %d 个：1,2,40,41,42,43,44", kN);

    /* A. 复用屏幕那条 SPI 总线 —— 最常见也最省引脚的做法 */
    int n_spi = 0;
    for (int i = 0; i < kN; i++) {
        for (int j = 0; j < kN; j++) {
            if (i == j) continue;
            n_spi++;
            if (ProbeSpi(kLcdSclk, kLcdMosi, kCands[i], kCands[j])) {
                esp_log_level_set("*", ESP_LOG_INFO);
                ESP_LOGW(TAG, ">>> 结果：SPI 共享屏幕总线，MISO=GPIO%d CS=GPIO%d", kCands[i], kCands[j]);
                return true;
            }
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    }
    ESP_LOGW(TAG, "SPI 共享总线没挂上（试了 %d 种）", n_spi);

    /* B. SDMMC 1-bit —— S3 上引脚可任意映射，三种有序组合 */
    int n_mmc = 0;
    for (int a = 0; a < kN; a++) {
        for (int b = 0; b < kN; b++) {
            if (a == b) continue;
            for (int c = 0; c < kN; c++) {
                if (c == a || c == b) continue;
                n_mmc++;
                if (ProbeSdmmc1(kCands[a], kCands[b], kCands[c])) {
                    esp_log_level_set("*", ESP_LOG_INFO);
                    ESP_LOGW(TAG, ">>> 结果：SDMMC 1-bit，CLK=GPIO%d CMD=GPIO%d D0=GPIO%d",
                             kCands[a], kCands[b], kCands[c]);
                    return true;
                }
                if ((n_mmc % 20) == 0) {
                    ESP_LOGW(TAG, "  SDMMC 进度 %d/%d", n_mmc, kN * (kN - 1) * (kN - 2));
                    vTaskDelay(pdMS_TO_TICKS(2));
                }
            }
        }
    }

    esp_log_level_set("*", ESP_LOG_INFO);
    ESP_LOGE(TAG, "探测结束：没找到 TF 卡（SPI %d 种 + SDMMC %d 种都失败）", n_spi, n_mmc);
    ESP_LOGE(TAG, "请确认：卡已插到底、格式化为 FAT32、不是 exFAT");
    return false;
}

#else  /* !PET_SD_PROBE */

bool PetSdCardProbe() {
    return false;
}

#endif
