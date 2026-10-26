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

/*
 * 开机做一次「ALS 增益扫描」，把 1x/2x/4x/8x 四档的原始计数打进日志。
 *
 * 为什么需要：LTR-303 的增益是哪种位宽（bit[4:3] 两位 / bit[4:2] 三位）
 * 各家文档写法不一致，写错会得到非法编码被芯片静默丢弃。有了这四行日志，
 * 一眼就能看出读数是否按 1→2→4→8 倍增，也顺便判断「房间是真黑还是没读到」。
 *
 * 代价：开机多花约 2 秒。确认稳定后可以改成 0 关掉。
 */
#define PET_LIGHT_GAIN_SWEEP 1

#endif  // _PET_CONFIG_H_
