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

/* ==================================================================
 * 活物感 UI（MotionEngine / 情绪 FSM / FX 层）
 * ==================================================================
 *
 * PET_MOTION_ENABLE = 1 : 开启 50Hz 体感引擎 + 情绪状态机 + FX 粒子层。
 *                         心情会跟着「拍一拍 / 摇一摇 / 摔落 / 静止」自己变。
 * PET_MOTION_ENABLE = 0 : 只保留原表情 + 眨眼（向后兼容，出问题先关这里）。
 */
#define PET_MOTION_ENABLE 1

/* 体感阈值（毫克，按 SC7A20H ±2g 量程），设计稿第 2.5 节实测值 */
#define PET_MOTION_TAP_MG          1300   /* 拍一拍：单样本加速度突变 */
#define PET_MOTION_FLIP_AZ_MG      (-550) /* 倒置：az 连续 300ms 低于此值 */
#define PET_MOTION_SHAKE_RMS_MG    600    /* 猛摇：500ms 窗口 jerk RMS 超此值 */
#define PET_MOTION_GENTLE_RMS_MG   200    /* 轻摇下限（200~600 = 轻摇） */
#define PET_MOTION_FREEFALL_MG     250    /* 自由落体：合加速度低于此值 */
#define PET_MOTION_IMPACT_MG       2000   /* 落地冲击：合加速度突变超过此值 */
#define PET_MOTION_STILL_SIGMA_MG  25     /* 静止判定：2 秒合加速度标准差 */
#define PET_MOTION_SIDE_LIE_AY_MG  700    /* 侧躺：|ay| 超此值 且 |az|<500 */
#define PET_MOTION_SIDE_LIE_AZ_MG  500

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
 * 心跳间隔（秒）。设备每隔这么久静默 sync 一次，用来取回家长在网页上点的指令。
 *
 *   0    = 不开心跳。家长点了喂饭，要等孩子下次跟小智说话才会送达。
 *   60   = 1 分钟（推荐）。家长点完最多 1 分钟生效，孩子不会觉得「点了没反应」。
 *   300  = 5 分钟（省电档）。家长点完最长等 5 分钟，实测体感偏慢。
 *
 * 额度不心疼：一次 sync 约 0.06 GB·s，
 *   60 秒一次 → 43200 次/月、2700 GB·s，腾讯云按量约 0.4 元/月；
 *   华为云 FunctionGraph 永久免费额度（100 万次 + 40 万 GB·s/月）只吃掉 4.3% / 0.7%。
 * 见 cloud/README.md 的「成本」一节。
 *
 * 断网时会自动退避到最多 15 分钟一次，不会在没网的时候按原频率硬撞。
 */
#define PET_HEARTBEAT_SEC 60

/*
 * 状态自动联动：宠物饿了/生病/心情差时，不用等 AI 决定，自动换灯色和表情。
 *   生病   → 红灯 + 生病脸
 *   饿了   → 暖橙灯 + 饿脸
 *   心情差 → 蓝灯 + 难过脸
 *   状态好 → 绿灯 + 开心脸
 * 想让 AI 完全自己拿主意就改成 0。
 */
#define PET_CLOUD_AUTO_REACT 1

/*
 * 属性衰减步长（秒）。数值规则：每过一步饱食-2、精力-1、心情-1。
 *
 *   60   = 1 分钟一步（默认）。测试验收要求「1 分钟内能看到缓慢衰减」，
 *          静置 2 分钟饱食/精力各降 2 点，肉眼可见又不至于瞬间饿扁。
 *   1800 = 0.5 小时一步（上线游玩档）。一晚上掉 32 点左右，节奏更像真宠物。
 *
 * 注意：pet_engine.h 的 decay_step_hours、pet_cloud.cc 的 kDecayStepSec
 * 都按本宏换算，改一处即可全生效；云端 pet-core.js 的 DEFAULTS 要同步。
 */
#define PET_DECAY_STEP_SEC 60

/*
 * 光感被动看护：每隔约 2 秒读一次照度，遇到骤亮（手电照额头）→ 好奇表情，
 * 骤暗（遮挡）→ 困倦表情。只在变化超过阈值时反应，不会一直乱动表情。
 *   1 = 开（默认，对应测试项 A-15「光感互动孔」）
 *   0 = 关（只保留 AI 主动读取 get_env 的路径）
 */
#define PET_LIGHT_WATCH 1

/*
 * TF 卡引脚探测（诊断开关）。
 *
 * K10 板上确有自弹式 microSD 卡座，但官方规格页和 wiki 都没给引脚，
 * 打开后开机遍历候选 GPIO 逐个尝试挂载 FAT32，命中就把真实引脚打进串口。
 *
 *   1 = 探测（约 1 分钟，开机慢，仅诊断用）
 *   0 = 跳过（拿到引脚后改 0，用固定引脚直接初始化）
 *
 * 注意：探测必须在屏幕/摄像头初始化之前跑，否则 SPI 总线抢不到。
 */
#define PET_SD_PROBE 0

#endif  // _PET_CONFIG_H_
