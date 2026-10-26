#include "pet_sensors.h"
#include "pet_config.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstring>
#include <cmath>

#define TAG "PetSensors"

#if PET_ENABLE_SENSORS

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

    uint8_t part = 0;
    if (ReadReg(PET_LTR303_ADDR, 0x0A, &part, 1) == ESP_OK) {
        ESP_LOGI(TAG, "LTR303 PART_ID=0x%02X", part);
    }

    uint8_t reset_cmd[2] = {0x00, 0x02};  // 软复位
    Write(PET_LTR303_ADDR, reset_cmd, 2);
    pet_delay_ms(20);

    uint8_t rate[2] = {0x01, 0x02};  // 100ms 测量周期
    Write(PET_LTR303_ADDR, rate, 2);

    uint8_t contr[2] = {0x00, 0x01};  // active + gain 1x
    if (Write(PET_LTR303_ADDR, contr, 2) != ESP_OK) {
        ESP_LOGW(TAG, "LTR303 启动失败");
        return;
    }
    pet_delay_ms(150);  // 等第一次转换完成
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

bool PetSensors::ReadLight(uint16_t& ch0, uint16_t& ch1, int& bright_pct) {
    if (!light_ok_) {
        return false;
    }
    uint8_t b[4] = {0};
    if (ReadReg(PET_LTR303_ADDR, 0x04, b, sizeof(b)) != ESP_OK) {
        return false;
    }
    ch1 = static_cast<uint16_t>((b[1] << 8) | b[0]);  // 红外
    ch0 = static_cast<uint16_t>((b[3] << 8) | b[2]);  // 可见光

    // 只是「房间里多亮」的粗略分档，不是标定过的 lux
    int pct = 0;
    if (ch0 >= 5) {
        float l = log10f(static_cast<float>(ch0));
        pct = static_cast<int>(l * 20.0f);
    }
    if (pct < 0) {
        pct = 0;
    }
    if (pct > 100) {
        pct = 100;
    }
    bright_pct = pct;
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
