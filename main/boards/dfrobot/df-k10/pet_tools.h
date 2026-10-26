#ifndef _PET_TOOLS_H_
#define _PET_TOOLS_H_

#include "pet_strip.h"
#include "pet_sensors.h"
#include "display.h"

#include <driver/i2c_master.h>
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
 */
class PetTools {
public:
    PetTools(PetStrip* strip, Display* display, i2c_master_bus_handle_t i2c_bus);

private:
    void RegisterMoodTools();
    void RegisterFaceTools();
    void RegisterSensorTools();

    /* 表情是否真的存在于本机资源里（不猜名字，问资源表） */
    std::shared_ptr<EmojiCollection> EmojiTable();
    bool FaceExists(const std::string& face);
    std::string FaceNameList();

    PetStrip* strip_ = nullptr;
    Display* display_ = nullptr;
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    PetSensors sensors_;

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
};

#endif  // _PET_TOOLS_H_
