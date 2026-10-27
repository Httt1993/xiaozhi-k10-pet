#ifndef _PET_TOOLS_H_
#define _PET_TOOLS_H_

#include "pet_cloud.h"
#include "pet_sensors.h"
#include "pet_strip.h"
#include "display.h"

#include <driver/i2c_master.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class EmojiCollection;

/*
 * 把宠物需要的能力注册成「AI 能看见的板载工具」。
 *
 * 设计原则（都是为了 6 岁孩子能用嘴控制）：
 *   - 不给模型 RGB 三元组，给颜色名字（warm_orange / sky_blue ...），模型不会调错
 *   - 灯语默认带呼吸，因为「一直亮着」比「闪一下」更像活的
 *   - 一次 get_env 就把温湿度/光照/加速度全带回来，少一次往返就少一秒等待
 *   - 养成逻辑不给模型做选择题：喂食/玩耍/生病/金币全部收敛到 self.pet.action 一个入口，
 *     模型只要听孩子说「喂它吃苹果」就能映射成 action=feed, food=苹果
 */
class PetTools {
public:
    PetTools(PetStrip* strip, Display* display, i2c_master_bus_handle_t i2c_bus);

private:
    void RegisterMoodTools();
    void RegisterFaceTools();
    void RegisterSensorTools();
    void RegisterCloudTools();

    /* 表情是否真的存在于本机资源里（不猜名字，问资源表） */
    std::shared_ptr<EmojiCollection> EmojiTable();
    bool FaceExists(const std::string& face);
    std::string FaceNameList();

    /* 把云端返回的状态事件翻译成灯色 + 表情。
     * 只在「最高优先级事件变化」时才动手，避免把用户手动设的灯盖掉。 */
    void ReactToEvents(const PetActionResult& result);

    /* 组一个紧凑的 JSON 给模型。字段少但够判断，避免上下文被撑爆。 */
    std::string StatusJson(const PetStatus& st, const char* mode);

    PetStrip* strip_ = nullptr;
    Display* display_ = nullptr;
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    PetSensors sensors_;
    PetCloud cloud_;

public:
    /* 体感引擎要复用同一颗传感器（同一个 I2C 总线句柄 + 已探测好的设备） */
    PetSensors& sensors() { return sensors_; }

    /* HTTP 看板需要访问 PetCloud 来执行动作 */
    PetCloud& cloud() { return cloud_; }

private:

    int level_ = 5;
    std::string mood_name_ = "off";
    std::string face_ = "neutral";
    std::vector<std::string> faces_;  // 首次用到时才向资源表探测

    // 拍一拍 / 晃动检测用的历史状态
    int16_t last_x_ = 0;
    int16_t last_y_ = 0;
    int16_t last_z_ = 0;
    int64_t last_move_us_ = 0;
    bool has_last_accel_ = false;

    // 状态缓存：30 秒内重复问状态就不再打云端，避免一句话触发多次网络往返
    int64_t last_status_ms_ = 0;
    std::string last_react_event_;  // 上一次联动用的最高优先级事件
};

#endif  // _PET_TOOLS_H_
