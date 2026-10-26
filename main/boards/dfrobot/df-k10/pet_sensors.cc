#include "pet_sensors.h"
#include "pet_config.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstring>
#include <cmath>

#define TAG "PetSensors"

#if PET_ENABLE_SENSORS

/*
 * LTR-303 当前配置：增益 4×、积分 400ms。
 * 相对「1× / 100ms」的基准灵敏度是 4 × 4 = 16 倍，
 * 所以把原始计数除以 16 就近似回到「1 计数 ≈ 1 lux」的基准，
 * 这也是下面 lux 换算的唯一依据。改初始化里的增益/积分时间，必须同步改这里。
 */
static constexpr float kLightScale = 16.0f;

static inline void pet_delay_ms(int ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}

PetSensors::PetSensors(i2c_master_bus_handle_t bus) : bus_(bus) {
}

esp_err_t PetSensors::EnsureDevice(uint8_t addr) {
    if (bus_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    for (int i = 0; i < 3; i++) {
        if (dev_[i] != nullptr && dev_addr_[i] == addr) {
            return ESP_OK;
        }
    }
    // 先探测，没人应答就别往总线上加设备
    esp_err_t err = i2c_master_probe(bus_, addr, 50);
    if (err != ESP_OK) {
        return err;
    }
    for (int i = 0; i < 3; i++) {
        if (dev_[i] == nullptr) {
            i2c_device_config_t cfg = {
                .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                .device_address = addr,
                .scl_speed_hz = 100 * 1000,
                .scl_wait_us = 0,
                .flags =
                    {
                        .disable_ack_check = 0,
                    },
            };
            err = i2c_master_bus_add_device(bus_, &cfg, &dev_[i]);
            if (err != ESP_OK) {
                dev_[i] = nullptr;
                return err;
            }
            dev_addr_[i] = addr;
            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM;
}

i2c_master_dev_handle_t PetSensors::DevOf(uint8_t addr) const {
    for (int i = 0; i < 3; i++) {
        if (dev_[i] != nullptr && dev_addr_[i] == addr) {
            return dev_[i];
        }
    }
    return nullptr;
}

esp_err_t PetSensors::Write(uint8_t addr, const uint8_t* buf, size_t len) {
    esp_err_t err = EnsureDevice(addr);
    if (err != ESP_OK) {
        return err;
    }
    i2c_master_dev_handle_t dev = DevOf(addr);
    if (dev == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_transmit(dev, buf, len, 100);
}

esp_err_t PetSensors::Receive(uint8_t addr, uint8_t* buf, size_t len) {
    esp_err_t err = EnsureDevice(addr);
    if (err != ESP_OK) {
        return err;
    }
    i2c_master_dev_handle_t dev = DevOf(addr);
    if (dev == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_receive(dev, buf, len, 100);
}

esp_err_t PetSensors::ReadReg(uint8_t addr, uint8_t reg, uint8_t* buf, size_t len) {
    esp_err_t err = EnsureDevice(addr);
    if (err != ESP_OK) {
        return err;
    }
    i2c_master_dev_handle_t dev = DevOf(addr);
    if (dev == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_transmit_receive(dev, &reg, 1, buf, len, 100);
}

void PetSensors::Init() {
    th_ok_ = false;
    light_ok_ = false;
    accel_ok_ = false;

    InitTempHumid();
    InitLight();
    InitAccel();

    ESP_LOGI(TAG, "sensor ready: temp_humid=%d light=%d accel=%d",
             th_ok_ ? 1 : 0, light_ok_ ? 1 : 0, accel_ok_ ? 1 : 0);
}

void PetSensors::InitTempHumid() {
    if (EnsureDevice(PET_AHT20_ADDR) != ESP_OK) {
        ESP_LOGW(TAG, "AHT20 不在 0x%02X，温湿度不可用", PET_AHT20_ADDR);
        return;
    }
    pet_delay_ms(50);

    uint8_t status = 0;
    if (ReadReg(PET_AHT20_ADDR, 0x71, &status, 1) != ESP_OK) {
        ESP_LOGW(TAG, "AHT20 读状态失败");
        return;
    }
    if ((status & 0x08) == 0) {
        // 未校准，发初始化命令
        uint8_t cmd[3] = {0xBE, 0x08, 0x00};
        if (Write(PET_AHT20_ADDR, cmd, 3) != ESP_OK) {
            ESP_LOGW(TAG, "AHT20 初始化命令失败");
            return;
        }
        pet_delay_ms(10);
    }
    th_ok_ = true;
}

void PetSensors::InitLight() {
    if (EnsureDevice(PET_LTR303_ADDR) != ESP_OK) {
        ESP_LOGW(TAG, "LTR303 不在 0x%02X，光照不可用", PET_LTR303_ADDR);
        return;
    }

    // ★ LTR-303 寄存器地址全在 0x80~0x8F 区间，不存在 0x00/0x01/0x04 这类低地址。
    // 早期版本误用了低地址，I2C 写入照样 ACK，所以"传感器存在"判断是假阳性、
    // PART_ID 恒读 0x00、光照数据全是垃圾。以 0x86 PART_ID 为准。
    uint8_t part = 0;
    uint8_t manu = 0;
    if (ReadReg(PET_LTR303_ADDR, 0x86, &part, 1) != ESP_OK) {
        ESP_LOGW(TAG, "LTR303 PART_ID 读失败");
    }
    if (ReadReg(PET_LTR303_ADDR, 0x87, &manu, 1) != ESP_OK) {
        ESP_LOGW(TAG, "LTR303 MANUFAC_ID 读失败");
    }
    // LTR-303ALS-01: PART_ID=0xA3 MANUFAC_ID=0x05
    // LTR-329ALS-01: PART_ID=0x0A MANUFAC_ID=0x05
    // LTR-553ALS-01: PART_ID=0x50 MANUFAC_ID=0x05
    ESP_LOGI(TAG, "LTR303 PART_ID=0x%02X MANUFAC_ID=0x%02X (expect 0xA3/0x0A/0x50 + 0x05)",
             part, manu);
    if (manu != 0x05) {
        ESP_LOGW(TAG, "LTR303 厂商 ID 不是 0x05，光照读数可能不可信（继续尝试）");
    }

    uint8_t reset_cmd[2] = {0x80, 0x02};  // 0x80 ALS_CONTR: bit1 = 软复位
    Write(PET_LTR303_ADDR, reset_cmd, 2);
    pet_delay_ms(20);

    // 0x85 ALS_MEAS_RATE：
    //   bit[5:3] 积分时间 = 011 → 400ms（分辨率最高，弱光下计数更大）
    //   bit[2:0] 测量周期 = 011 → 500ms
    uint8_t rate[2] = {0x85, 0x1B};
    Write(PET_LTR303_ADDR, rate, 2);

    // 0x80 ALS_CONTR：bit0 = active，bit[4:3] 增益 = 10 → 4x
    // 相对原来的 1x/100ms，灵敏度提高 16 倍；饱和点仍在 6 万 lux 以上，室内不会过曝
    uint8_t contr[2] = {0x80, 0x11};
    if (Write(PET_LTR303_ADDR, contr, 2) != ESP_OK) {
        ESP_LOGW(TAG, "LTR303 启动失败");
        return;
    }
    pet_delay_ms(600);  // 400ms 积分时间，留足一次完整转换（周期 500ms）

    // 光靠"写入成功"不算数，必须真读到非零数据才算可用
    uint8_t probe[4] = {0};
    uint16_t ch0_first = 0;
    if (ReadReg(PET_LTR303_ADDR, 0x88, probe, sizeof(probe)) == ESP_OK) {
        ch0_first = static_cast<uint16_t>((probe[3] << 8) | probe[2]);
    }
    if (ch0_first == 0xFFFF) {
        ESP_LOGW(TAG, "LTR303 数据全 1（0xFFFF），光照不可用");
        return;
    }
    ESP_LOGI(TAG, "LTR303 首次采样 CH0=%u CH1=%u → 约 %.1f lux（增益4x/积分400ms）",
             ch0_first, static_cast<unsigned>((probe[1] << 8) | probe[0]),
             static_cast<double>(ch0_first / kLightScale));
    light_ok_ = true;
}

void PetSensors::InitAccel() {
    if (EnsureDevice(PET_SC7A20_ADDR) != ESP_OK) {
        ESP_LOGW(TAG, "SC7A20H 不在 0x%02X，加速度不可用", PET_SC7A20_ADDR);
        return;
    }

    uint8_t who = 0;
    if (ReadReg(PET_SC7A20_ADDR, 0x0F, &who, 1) == ESP_OK) {
        ESP_LOGI(TAG, "SC7A20H WHO_AM_I=0x%02X（若为 0x00/0xFF 说明型号或地址不对）", who);
    }

    uint8_t ctrl1[2] = {0x20, 0x57};  // 100Hz / XYZ 使能
    if (Write(PET_SC7A20_ADDR, ctrl1, 2) != ESP_OK) {
        ESP_LOGW(TAG, "SC7A20H CTRL_REG1 写失败");
        return;
    }
    uint8_t ctrl4[2] = {0x23, 0x80};  // BDU，±2g
    Write(PET_SC7A20_ADDR, ctrl4, 2);
    pet_delay_ms(20);
    accel_ok_ = true;
}

bool PetSensors::ReadTempHumid(float& temp_c, float& humidity) {
    if (!th_ok_) {
        return false;
    }
    uint8_t trigger[3] = {0xAC, 0x33, 0x00};
    if (Write(PET_AHT20_ADDR, trigger, 3) != ESP_OK) {
        return false;
    }
    pet_delay_ms(90);

    uint8_t b[7] = {0};
    if (Receive(PET_AHT20_ADDR, b, sizeof(b)) != ESP_OK) {
        return false;
    }
    if ((b[0] & 0x80) != 0) {  // busy，本次没测完
        return false;
    }

    uint32_t hum_raw = ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
    hum_raw >>= 4;

    uint32_t temp_raw = (((uint32_t)b[3] & 0x0F) << 16) | ((uint32_t)b[4] << 8) | b[5];

    humidity = static_cast<float>(hum_raw) * 100.0f / 1048576.0f;
    temp_c = static_cast<float>(temp_raw) * 200.0f / 1048576.0f - 50.0f;
    return true;
}

bool PetSensors::ReadLight(uint16_t& ch0, uint16_t& ch1, float& lux) {
    if (!light_ok_) {
        return false;
    }
    uint8_t b[4] = {0};
    // 0x88 = ALS_DATA_CH1_0：顺序为 CH1_L CH1_H CH0_L CH0_H
    if (ReadReg(PET_LTR303_ADDR, 0x88, b, sizeof(b)) != ESP_OK) {
        return false;
    }
    ch1 = static_cast<uint16_t>((b[1] << 8) | b[0]);  // 红外
    ch0 = static_cast<uint16_t>((b[3] << 8) | b[2]);  // 可见光 + 红外

    /*
     * CH0 本身已经包含红外，但 LTR-303 的 CH0 对可见光的响应远高于 CH1，
     * 直接用 CH0 归一化就足够判断「房间亮不亮」。刻意不减 CH1：
     * 白炽灯/暖光下 CH1 占比很高，相减会把读数压到 0，反而误判成「黑的」。
     */
    lux = static_cast<float>(ch0) / kLightScale;
    if (lux < 0.0f) {
        lux = 0.0f;
    }
    return true;
}

bool PetSensors::ReadAccel(int16_t& x_mg, int16_t& y_mg, int16_t& z_mg) {
    if (!accel_ok_) {
        return false;
    }
    uint8_t b[6] = {0};
    if (ReadReg(PET_SC7A20_ADDR, 0x28 | 0x80, b, sizeof(b)) != ESP_OK) {
        return false;
    }
    int32_t rx = static_cast<int16_t>((b[1] << 8) | b[0]);
    int32_t ry = static_cast<int16_t>((b[3] << 8) | b[2]);
    int32_t rz = static_cast<int16_t>((b[5] << 8) | b[4]);

    // 12bit 左对齐，±2g 时 1mg/digit
    x_mg = static_cast<int16_t>(rx >> 4);
    y_mg = static_cast<int16_t>(ry >> 4);
    z_mg = static_cast<int16_t>(rz >> 4);
    return true;
}

int PetSensors::ScanBus(uint8_t* out, int max_out) {
    if (bus_ == nullptr || out == nullptr) {
        return 0;
    }
    int found = 0;
    for (uint8_t addr = 0x08; addr < 0x78 && found < max_out; addr++) {
        if (i2c_master_probe(bus_, addr, 30) == ESP_OK) {
            out[found++] = addr;
        }
    }
    return found;
}

#endif  // PET_ENABLE_SENSORS
