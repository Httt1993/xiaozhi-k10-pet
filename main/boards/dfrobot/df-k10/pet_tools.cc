#include "pet_tools.h"
#include "pet_config.h"
#include "pet_face_view.h"
#include "mcp_server.h"
#include "display/lvgl_display/lvgl_theme.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#define TAG "PetTools"

namespace {

struct PetColorEntry {
    const char* name;
    uint8_t red;
    uint8_t green;
    uint8_t blue;
};

// 用颜色名字而不是 RGB 三元组：模型说「暖橙」比说「255,120,20」可靠得多
const PetColorEntry kPetColors[] = {
    {"warm_orange", 255, 120, 20},   // 饿了
    {"amber", 255, 170, 0},          // 想要注意
    {"red", 255, 40, 40},            // 生病 / 生气
    {"pink", 255, 90, 160},          // 开心
    {"purple", 150, 60, 255},        // 好奇
    {"blue", 40, 90, 255},           // 低落
    {"sky_blue", 90, 190, 255},      // 平静
    {"cyan", 0, 200, 210},           // 听见你说话
    {"green", 60, 200, 90},          // 状态很好
    {"white", 255, 240, 220},        // 中性
    {"off", 0, 0, 0},                // 熄灭
};
const int kPetColorCount = sizeof(kPetColors) / sizeof(kPetColors[0]);

// 候选表情名。真正可用的由设备里的表情资源表决定，所以这只是「待探测清单」，
// 不是硬编码白名单——set_face 会拿它去资源表里逐个确认，确认不到的直接报错。
const char* const kPetFaceCandidates[] = {
    "neutral",  "happy",   "laughing",  "funny",    "sad",      "angry",
    "crying",   "loving",  "embarrassed", "surprised", "shocked", "thinking",
    "winking",  "cool",    "relaxed",   "delicious", "kissy",   "confident",
    "sleepy",   "silly",   "confused",  "bored",    "excited",  "proud",
    "shy",      "curious", "tired",     "scared",   "delighted", "worried",
};
const int kPetFaceCandidateCount = sizeof(kPetFaceCandidates) / sizeof(kPetFaceCandidates[0]);

const PetColorEntry* FindColor(const std::string& name) {
    for (int i = 0; i < kPetColorCount; i++) {
        if (name == kPetColors[i].name) {
            return &kPetColors[i];
        }
    }
    return nullptr;
}

std::string ColorNameList() {
    std::string out;
    for (int i = 0; i < kPetColorCount; i++) {
        if (!out.empty()) {
            out += " / ";
        }
        out += kPetColors[i].name;
    }
    return out;
}

/*
 * 把照度翻译成人话。
 *
 * 教训：以前只给模型一个 bright_pct（log10(ch0)*20），实测室内 CH0=148 会算出 43%，
 * 模型拿到这种没头没脑的百分数，嘴上就会说「黑黑的」。给词它就说得准。
 * 分档阈值按实测：普通室内灯约 100~200 lux，夜里关灯约 0~3 lux。
 */
const char* LightLevelText(float lux) {
    if (lux < 2.0f) {
        return "漆黑";
    }
    if (lux < 15.0f) {
        return "很暗";
    }
    if (lux < 60.0f) {
        return "偏暗";
    }
    if (lux < 300.0f) {
        return "室内正常";
    }
    if (lux < 1200.0f) {
        return "明亮";
    }
    return "很强";
}

}  // namespace

PetTools::PetTools(PetStrip* strip, Display* display, i2c_master_bus_handle_t i2c_bus)
    : strip_(strip), display_(display), i2c_bus_(i2c_bus), sensors_(i2c_bus) {
    sensors_.Init();

    RegisterMoodTools();
    RegisterFaceTools();
    RegisterSensorTools();

    ESP_LOGI(TAG, "pet tools registered (sensors: th=%d light=%d accel=%d)",
             sensors_.HasTempHumid() ? 1 : 0, sensors_.HasLight() ? 1 : 0,
             sensors_.HasAccel() ? 1 : 0);
}

void PetTools::RegisterMoodTools() {
    auto& mcp_server = McpServer::GetInstance();

    mcp_server.AddTool(
        "self.pet.set_mood",
        "设置宠物心情灯的颜色，这盏灯会一直亮着直到再次改变，用来表达宠物的状态。"
        "可用颜色：warm_orange(饿了) / amber(想要注意) / red(生病) / pink(开心) / "
        "purple(好奇) / blue(低落) / sky_blue(平静) / cyan(听见你说话) / green(状态很好) / "
        "white(中性) / off(熄灭)。level 是亮度 1-10，breathe 为 true 时缓慢呼吸。",
        PropertyList({
            Property("color", kPropertyTypeString, std::string("warm_orange")),
            Property("level", kPropertyTypeInteger, 5, 1, 10),
            Property("breathe", kPropertyTypeBoolean, false),
        }),
        [this](const PropertyList& properties) -> ToolResult {
            std::string color = properties["color"].value<std::string>();
            int level = properties["level"].value<int>();
            bool breathe = properties["breathe"].value<bool>();

            const PetColorEntry* entry = FindColor(color);
            if (entry == nullptr) {
                return std::unexpected("没有这个颜色：" + color + "。可用：" + ColorNameList());
            }

            level_ = level;
            mood_name_ = entry->name;
            if (strip_ != nullptr) {
                strip_->SetMood(entry->red, entry->green, entry->blue, level, breathe);
            }

            char buf[96];
            snprintf(buf, sizeof(buf), "灯已设为 %s，亮度 %d%s", entry->name, level,
                     breathe ? "，缓慢呼吸" : "");
            return std::string(buf);
        });

    mcp_server.AddTool(
        "self.pet.clear_mood",
        "熄灭宠物的心情灯（宠物睡觉或不需要表达时使用）。",
        PropertyList(),
        [this](const PropertyList&) -> ToolResult {
            mood_name_ = "off";
            if (strip_ != nullptr) {
                strip_->ClearMood();
            }
            return std::string("灯已熄灭");
        });
}

std::shared_ptr<EmojiCollection> PetTools::EmojiTable() {
    if (display_ == nullptr) {
        return nullptr;
    }
    // 固件构建时关掉了 RTTI（CONFIG_COMPILER_CXX_RTTI=n），所以这里只能 static_cast，
    // 与 lcd_display.cc 自身取表情表的写法保持一致。
    auto* theme = static_cast<LvglTheme*>(display_->GetTheme());
    if (theme == nullptr) {
        return nullptr;
    }
    return theme->emoji_collection();
}

bool PetTools::FaceExists(const std::string& face) {
    /* 猫脸模式优先：这台板子用的是矢量猫脸，表情名由 PetFaceView 定义，
     * 跟官方 emoji 资源表无关。 */
    if (display_ != nullptr) {
        const char* pet_list = display_->PetFaceList();
        if (pet_list != nullptr && pet_list[0] != '\0') {
            return PetFaceView::Known(face.c_str());
        }
    }

    auto table = EmojiTable();
    if (table == nullptr) {
        // 表情资源还没加载（开机早期），不做判断，交给 SetEmotion 自己报
        return true;
    }
    if (faces_.empty()) {
        for (int i = 0; i < kPetFaceCandidateCount; i++) {
            if (table->GetEmojiImage(kPetFaceCandidates[i]) != nullptr) {
                faces_.push_back(kPetFaceCandidates[i]);
            }
        }
    }
    for (const auto& name : faces_) {
        if (name == face) {
            return true;
        }
    }
    return false;
}

std::string PetTools::FaceNameList() {
    if (display_ != nullptr) {
        const char* pet_list = display_->PetFaceList();
        if (pet_list != nullptr && pet_list[0] != '\0') {
            return std::string(pet_list);
        }
    }

    auto table = EmojiTable();
    if (table == nullptr) {
        return "(表情资源未就绪)";
    }
    if (faces_.empty()) {
        for (int i = 0; i < kPetFaceCandidateCount; i++) {
            if (table->GetEmojiImage(kPetFaceCandidates[i]) != nullptr) {
                faces_.push_back(kPetFaceCandidates[i]);
            }
        }
    }
    std::string out;
    for (const auto& name : faces_) {
        if (!out.empty()) {
            out += " / ";
        }
        out += name;
    }
    if (out.empty()) {
        out = "neutral / happy / sad";
    }
    return out;
}

void PetTools::RegisterFaceTools() {
    auto& mcp_server = McpServer::GetInstance();

    mcp_server.AddTool(
        "self.pet.set_face",
        "切换宠物脸上的表情。表情会一直保持（眼睛会自己眨），直到下次修改。"
        "主动用表情表达情绪：孩子讲开心的事就设 happy，你自己饿了就设 hungry。"
        "先用 self.pet.list_faces 查可用名字。",
        PropertyList({
            Property("face", kPropertyTypeString, std::string("neutral")),
        }),
        [this](const PropertyList& properties) -> ToolResult {
            std::string face = properties["face"].value<std::string>();
            if (!FaceExists(face)) {
                return std::unexpected("这台设备没有这个表情：" + face + "。可用：" + FaceNameList());
            }
            if (display_ == nullptr || !display_->SetPetFace(face.c_str())) {
                return std::unexpected(std::string("切换表情失败，屏幕可能还没就绪：") + face);
            }
            face_ = face;
            return std::string("表情已切换为 ") + face;
        });

    mcp_server.AddTool(
        "self.pet.list_faces",
        "列出这台设备真正支持的表情名。别猜表情名，先查一次再用。",
        PropertyList(),
        [this](const PropertyList&) -> ToolResult {
            return std::string("可用表情：") + FaceNameList();
        });
}

void PetTools::RegisterSensorTools() {
    auto& mcp_server = McpServer::GetInstance();

    mcp_server.AddTool(
        "self.pet.get_env",
        "读取设备周围环境：温度、湿度、房间亮度、设备是否被拿起/晃过。"
        "适合在孩子说『你摸摸我』『你冷不冷』『房间里黑不黑』时调用。"
        "返回的 light_level 已经是「漆黑/很暗/偏暗/室内正常/明亮/很强」这样的结论，"
        "请直接采信它来描述房间亮度，不要自己拿 light_lux 或别的字段做二次推断；"
        "只有 light_level 说「漆黑」「很暗」时，才可以形容房间很暗。",
        PropertyList(),
        [this](const PropertyList&) -> ToolResult {
            float temp_c = 0.0f;
            float humidity = 0.0f;
            uint16_t ch0 = 0;
            uint16_t ch1 = 0;
            float lux = 0.0f;
            int16_t x = 0;
            int16_t y = 0;
            int16_t z = 0;

            bool ok_th = sensors_.ReadTempHumid(temp_c, humidity);
            bool ok_light = sensors_.ReadLight(ch0, ch1, lux);
            bool ok_accel = sensors_.ReadAccel(x, y, z);

            bool moved = false;
            if (ok_accel) {
                int64_t now = esp_timer_get_time();
                if (has_last_accel_) {
                    int dx = x - last_x_;
                    int dy = y - last_y_;
                    int dz = z - last_z_;
                    int delta = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy) + (dz < 0 ? -dz : dz);
                    if (delta > PET_TAP_THRESHOLD_MG) {
                        last_move_us_ = now;
                    }
                }
                last_x_ = x;
                last_y_ = y;
                last_z_ = z;
                has_last_accel_ = true;
                moved = last_move_us_ != 0 && (now - last_move_us_) < 10 * 1000 * 1000;
            }

            std::string out = "{";
            char buf[128];
            if (ok_th) {
                snprintf(buf, sizeof(buf), "\"temp_c\":%.1f,\"humidity\":%.0f,", temp_c, humidity);
                out += buf;
            }
            if (ok_light) {
                snprintf(buf, sizeof(buf),
                         "\"light_level\":\"%s\",\"light_lux\":%.0f,\"light_raw\":%u,",
                         LightLevelText(lux), static_cast<double>(lux),
                         static_cast<unsigned>(ch0));
                out += buf;
            }
            if (ok_accel) {
                snprintf(buf, sizeof(buf), "\"accel_mg\":[%d,%d,%d],\"moved\":%s,", x, y, z,
                         moved ? "true" : "false");
                out += buf;
            }
            if (out.size() > 1 && out.back() == ',') {
                out.pop_back();
            }
            out += "}";

            if (!ok_th && !ok_light && !ok_accel) {
                return std::unexpected("这台设备的传感器都没读到数据，先跑 self.pet.i2c_scan 看看总线上有哪些设备");
            }
            return out;
        });

    mcp_server.AddTool(
        "self.pet.get_status",
        "读取宠物当前的表情、灯色和可用传感器，用于自我检查。",
        PropertyList(),
        [this](const PropertyList&) -> ToolResult {
            char buf[192];
            snprintf(buf, sizeof(buf),
                     "{\"face\":\"%s\",\"mood\":\"%s\",\"level\":%d,\"sensors\":{\"temp_humid\":%s,"
                     "\"light\":%s,\"accel\":%s}}",
                     face_.c_str(), mood_name_.c_str(), level_,
                     sensors_.HasTempHumid() ? "true" : "false",
                     sensors_.HasLight() ? "true" : "false",
                     sensors_.HasAccel() ? "true" : "false");
            return std::string(buf);
        });

    mcp_server.AddTool(
        "self.pet.i2c_scan",
        "诊断工具：扫描 I2C 总线并列出有应答的设备地址。传感器读数不对时先跑这个。",
        PropertyList(),
        [this](const PropertyList&) -> ToolResult {
            uint8_t found[16] = {0};
            int count = sensors_.ScanBus(found, sizeof(found));
            std::string out = "发现 ";
            out += std::to_string(count);
            out += " 个设备：";
            char buf[8];
            for (int i = 0; i < count; i++) {
                snprintf(buf, sizeof(buf), "0x%02X ", found[i]);
                out += buf;
            }
            return out;
        });
}
