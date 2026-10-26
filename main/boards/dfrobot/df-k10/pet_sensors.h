#ifndef _PET_SENSORS_H_
#define _PET_SENSORS_H_

#include "pet_config.h"

#include <driver/i2c_master.h>
#include <esp_err.h>
#include <cstdint>

/*
 * K10 上三颗「AI 看不见」的传感器：AHT20 温湿度 / LTR-303ALS 光照 / SC7A20H 加速度。
 *
 * 两点决定了这个类为什么自己写、而不复用 boards/common/i2c_device.h：
 *   1) I2cDevice 内部全部用 ESP_ERROR_CHECK，传感器不在总线上会直接 abort → 设备反复重启。
 *      这里每一步都返回 esp_err_t，失败只是「这个能力暂时没有」。
 *   2) 上电先 i2c_master_probe 探测，探测不到就不初始化，绝不阻塞开机。
 *
 * PET_ENABLE_SENSORS = 0 时整个类退化成空壳，pet_sensors.cc 也编不进任何代码，
 * 这样即使传感器驱动在你的 IDF 版本上报错，也能一键退回「只有灯语和表情」的固件。
 */

#if PET_ENABLE_SENSORS

class PetSensors {
public:
    explicit PetSensors(i2c_master_bus_handle_t bus);

    /* 开机调一次。缺哪个传感器就只关哪个能力，不影响其它 */
    void Init();

    bool HasTempHumid() const { return th_ok_; }
    bool HasLight() const { return light_ok_; }
    bool HasAccel() const { return accel_ok_; }

    /* 温度 ℃ / 湿度 %RH */
    bool ReadTempHumid(float& temp_c, float& humidity);

    /* 环境光：可见光通道计数 ch0、红外通道 ch1，以及估算照度 lux（非标定值，用于分档） */
    bool ReadLight(uint16_t& ch0, uint16_t& ch1, float& lux);

    /* 加速度，单位毫克（mg），重力约 1000mg */
    bool ReadAccel(int16_t& x_mg, int16_t& y_mg, int16_t& z_mg);

    /* 诊断：扫描 0x08-0x77，把有应答的地址写进 out，返回个数 */
    int ScanBus(uint8_t* out, int max_out);

private:
    esp_err_t EnsureDevice(uint8_t addr);
    i2c_master_dev_handle_t DevOf(uint8_t addr) const;
    esp_err_t Write(uint8_t addr, const uint8_t* buf, size_t len);
    esp_err_t Receive(uint8_t addr, uint8_t* buf, size_t len);
    esp_err_t ReadReg(uint8_t addr, uint8_t reg, uint8_t* buf, size_t len);

    void InitTempHumid();
    void InitLight();
    void InitAccel();

    i2c_master_bus_handle_t bus_ = nullptr;
    i2c_master_dev_handle_t dev_[3] = {nullptr, nullptr, nullptr};
    uint8_t dev_addr_[3] = {0, 0, 0};

    bool th_ok_ = false;
    bool light_ok_ = false;
    bool accel_ok_ = false;
};

#else  // PET_ENABLE_SENSORS == 0 → 空壳，编译进来但什么都不做

class PetSensors {
public:
    explicit PetSensors(i2c_master_bus_handle_t) {}
    void Init() {}
    bool HasTempHumid() const { return false; }
    bool HasLight() const { return false; }
    bool HasAccel() const { return false; }
    bool ReadTempHumid(float&, float&) { return false; }
    bool ReadLight(uint16_t&, uint16_t&, float& lux) {
        lux = 0.0f;
        return false;
    }
    bool ReadAccel(int16_t&, int16_t&, int16_t&) { return false; }
    int ScanBus(uint8_t*, int) { return 0; }
};

#endif  // PET_ENABLE_SENSORS

#endif  // _PET_SENSORS_H_
