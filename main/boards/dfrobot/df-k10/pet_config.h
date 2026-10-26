#ifndef _PET_CONFIG_H_
#define _PET_CONFIG_H_

/*
 * K10 宠物养成板端补丁 · 总开关
 *
 * PET_ENABLE_SENSORS = 1 : 注册温湿度/光照/加速度三个传感器工具（需要额外 I2C 驱动）
 * PET_ENABLE_SENSORS = 0 : 只保留「灯语 + 表情 + 状态」，编译风险最低
 *
 * 如果你的构建在 pet_sensors.cc 上报错，先把它改成 0，能立刻得到一个可烧的固件，
 * 灯语和表情照常工作，传感器回头再单独调。
 */
#define PET_ENABLE_SENSORS 1

/* I2C 地址（K10 原理图上这三颗都在 i2c_port 1，SDA=47 / SCL=48） */
#define PET_AHT20_ADDR   0x38   /* 温湿度 AHT20 */
#define PET_LTR303_ADDR  0x29   /* 环境光 LTR-303ALS */
#define PET_SC7A20_ADDR  0x19   /* 三轴加速度 SC7A20H */

/* 加速度「拍一拍」判定阈值（毫克），越小越灵敏 */
#define PET_TAP_THRESHOLD_MG 1300

#endif  // _PET_CONFIG_H_
