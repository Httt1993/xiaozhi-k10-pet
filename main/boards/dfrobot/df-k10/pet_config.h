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
 *
 * 2026-10-26 已实测确认（1x→2x→4x 严格倍增，8x 那档因为等待时间短于测量周期
 * 读到旧值），光感无异常，故关闭。需要再诊断时改回 1 即可。
 */
#define PET_LIGHT_GAIN_SWEEP 0

/* ==================================================================
 * 云端宠物逻辑（腾讯云 SCF）
 * ==================================================================
 *
 * 填了 PET_CLOUD_URL 就是「云端模式」：衰减、生病、金币、任务全部由云函数算，
 * 设备只负责把状态存进 NVS 并呈现。
 *
 * 留空就是「本地模式」：同一套数值规则在设备上跑精简实现。
 * 两种模式共用同一份状态格式，所以随时可以切换，进度不会丢。
 *
 * 怎么拿到 URL：
 *   见 cloud/README.md —— 上传 cloud/dist/pet-cloud-scf.zip 后创建「函数 URL」，
 *   形如 https://xxxx.ap-guangzhou.tencentscf.com/
 *
 * 注意：URL 要塞进双引号里，末尾的斜杠可有可无。
 */
#define PET_CLOUD_URL ""
#define PET_CLOUD_KEY ""

/*
 * 证书校验。默认开启（走 IDF 内置的根证书包）。
 * 只有当你的函数域名用了自签证书、且连接一直失败时才改成 1 —— 那等于关掉中间人防护。
 */
#define PET_CLOUD_TLS_INSECURE 0

/*
 * 状态自动联动：宠物饿了/生病/心情差时，不用等 AI 决定，自动换灯色和表情。
 *   生病   → 红灯 + 生病脸
 *   饿了   → 暖橙灯 + 饿脸
 *   心情差 → 蓝灯 + 难过脸
 *   状态好 → 绿灯 + 开心脸
 * 想让 AI 完全自己拿主意就改成 0。
 */
#define PET_CLOUD_AUTO_REACT 1

#endif  // _PET_CONFIG_H_
