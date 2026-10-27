#include "pet_tools.h"
#include "pet_config.h"
#include "pet_face_view.h"
#include "pet_motion.h"
#include "mcp_server.h"
#include "display/lvgl_display/lvgl_theme.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <cJSON.h>
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
    cloud_.Init();

    RegisterMoodTools();
    RegisterFaceTools();
    RegisterSensorTools();
    RegisterCloudTools();

    /*
     * 家长看板 / HTTP / 心跳 这些非语音路径执行动作时，也要让设备「有反应」：
     * 灯色表情跟着状态事件走，宠物话术用通知条冒出来，动作成功给个表情反馈。
     * （测试项 A-11/A-12：看板发任务/指令后设备无任何响应 —— 就是缺了这个钩子）
     */
    cloud_.SetActionHook([this](const PetActionResult& r, const std::string& action) {
        ReactToEvents(r);
        /* 状态已变，30 秒缓存作废，下次问状态重新同步 */
        last_status_ms_ = 0;

        /* 静默动作不弹通知：sync/task_list/ping 会被看板 3 秒一次地轮询 */
        const bool quiet = action == "sync" || action == "task_list" ||
                           action == "ping" || action == "chat" || action.empty();
        if (quiet) {
            return;
        }
        if (display_ != nullptr && !r.say.empty()) {
            display_->ShowNotification(r.say.c_str(), 2500);
        }
        if (display_ != nullptr && r.ok) {
            if (action == "feed" || action == "play" || action == "pet" ||
                action == "task_publish" || action == "story" || action == "song") {
                display_->SetPetFace("happy");
            } else if (action == "sleep") {
                display_->SetPetFace("sleepy");
            } else if (action == "heal" || action == "medicine") {
                display_->SetPetFace("neutral");
            }
        }
    });

    ESP_LOGI(TAG, "pet tools registered (sensors: th=%d light=%d accel=%d, cloud=%s)",
             sensors_.HasTempHumid() ? 1 : 0, sensors_.HasLight() ? 1 : 0,
             sensors_.HasAccel() ? 1 : 0, cloud_.cloud_enabled() ? "on" : "local");

    /* 本地模式下开了也没用（cloud_enabled 会直接 return），索性按配置传，
       这样切换云端/本地不用改两处。 */
    cloud_.StartHeartbeat(PET_HEARTBEAT_SEC);
}

void PetTools::WatchLightTick() {
#if PET_LIGHT_WATCH
    if (!sensors_.HasLight()) {
        return;
    }
    uint16_t ch0 = 0, ch1 = 0;
    float lux = 0.0f;
    if (!sensors_.ReadLight(ch0, ch1, lux)) {
        return;
    }
    int64_t now_ms = esp_timer_get_time() / 1000;
    /* 反应冷却 3 秒：手电晃动不要连续换脸 */
    if (now_ms - light_react_ms_ < 3000) {
        last_lux_ = lux;
        light_valid_ = true;
        return;
    }
    if (light_valid_ && display_ != nullptr) {
        /* 骤亮：手电照额头（比上一次亮 2.5 倍以上且绝对值够大） */
        bool sudden_bright = lux > 120.0f && lux > last_lux_ * 2.5f + 40.0f;
        /* 骤暗：遮挡光感孔 */
        bool sudden_dark = last_lux_ > 120.0f && lux * 2.5f + 40.0f < last_lux_;
        if (sudden_bright) {
            display_->SetPetFace("curious");
            ESP_LOGI(TAG, "PET_PATCH: light jump %.0f -> %.0f lux -> curious", last_lux_,
                     static_cast<double>(lux));
            light_react_ms_ = now_ms;
        } else if (sudden_dark) {
            display_->SetPetFace("sleepy");
            ESP_LOGI(TAG, "PET_PATCH: light drop %.0f -> %.0f lux -> sleepy", last_lux_,
                     static_cast<double>(lux));
            light_react_ms_ = now_ms;
        }
    }
    last_lux_ = lux;
    light_valid_ = true;
#endif
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
        "读取宠物的完整状态：饿不饿、有没有力气、心情好不好、有没有生病、有多少游戏币、几级，"
        "以及屏幕表情和灯色。孩子问『你饿不饿』『你几级了』『你有多少钱』『你哪里不舒服』时调用。"
        "返回里的 say 是宠物此刻的心声，可以顺着它的话聊。",
        PropertyList(),
        [this](const PropertyList&) -> ToolResult {
            /* 30 秒内问过就不再打云端：一句话里模型可能连着问好几次状态，
             * 每次都走 TLS 握手会把响应拖到 3 秒以上。 */
            int64_t now_ms = esp_timer_get_time() / 1000;
            bool need_sync = (last_status_ms_ == 0) || (now_ms - last_status_ms_ > 30000);

            if (need_sync) {
                PetActionResult r = cloud_.Act("sync", "");
                last_status_ms_ = now_ms;
                if (r.from_cloud) {
                    ReactToEvents(r);
                }
            }

            PetStatus st = cloud_.GetStatus();
            return StatusJson(st, cloud_.fail_streak() > 0 ? "local" : "cloud");
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

// ==================================================================== 养成动作
// 这里是「宠物真正活着」的部分：饿、累、生病、金币、任务都不是写死的动画，
// 而是由云端按真实时间推演出来的状态，设备只负责呈现。

std::string PetTools::StatusJson(const PetStatus& st, const char* mode) {
    char buf[640];
    snprintf(buf, sizeof(buf),
             "{\"hunger\":%d,\"energy\":%d,\"mood\":%d,\"sick\":%s,\"coins\":%d,\"level\":%d,"
             "\"exp\":%d,\"face\":\"%s\",\"light\":\"%s\",\"mode\":\"%s\",\"say\":\"%s\","
             "\"motion\":\"%s\"}",
             st.hunger, st.energy, st.mood, st.sick ? "true" : "false", st.coins, st.level, st.exp,
             face_.c_str(), mood_name_.c_str(), mode, st.mood_word.c_str(),
             PetMotionLastEventName());
    return std::string(buf);
}

/*
 * 状态 → 灯 + 脸 的自动联动。
 *
 * 只在「最高优先级事件发生变化」时动手。原因：孩子可能刚说过「把灯设成紫色」，
 * 如果每轮动作都按状态覆盖一遍，用户的手动设置就永远留不住。
 * 事件变了才代表宠物真的换了状态（比如刚饿到不行、刚生病、刚治好），这时候变灯才合理。
 */
void PetTools::ReactToEvents(const PetActionResult& result) {
#if PET_CLOUD_AUTO_REACT
    struct Reaction {
        const char* event;
        const char* color;
        int level;
        const char* face;
    };
    /* 顺序无关紧要：events 已经是按严重程度排好序的，取第一个即可 */
    static const Reaction kReactions[] = {
        {"sick", "red", 6, "sick"},
        {"starving", "warm_orange", 8, "hungry"},
        {"hungry", "warm_orange", 5, "hungry"},
        {"tired", "blue", 4, "sad"},
        {"sad", "blue", 4, "sad"},
        {"bored", "sky_blue", 4, "neutral"},
        {"great", "green", 5, "happy"},
    };

    if (result.events.empty()) {
        /* 恢复正常：清掉记录，下次再出状况还能触发 */
        last_react_event_.clear();
        return;
    }

    const std::string& top = result.events.front();
    if (top == last_react_event_) {
        return; /* 状态没本质变化，不打扰用户手动设的灯 */
    }
    last_react_event_ = top;

    for (const auto& r : kReactions) {
        if (top != r.event) {
            continue;
        }
        const PetColorEntry* entry = FindColor(r.color);
        if (entry != nullptr && strip_ != nullptr) {
            level_ = r.level;
            mood_name_ = entry->name;
            strip_->SetMood(entry->red, entry->green, entry->blue, r.level, true);
        }
        if (display_ != nullptr && FaceExists(r.face)) {
            if (display_->SetPetFace(r.face)) {
                face_ = r.face;
            }
        }
        ESP_LOGI(TAG, "PET_PATCH: auto react event=%s -> light=%s level=%d face=%s", r.event,
                 r.color, r.level, r.face);
        return;
    }
#else
    (void)result;
#endif
}

void PetTools::RegisterCloudTools() {
    auto& mcp_server = McpServer::GetInstance();

    /*
     * 统一动作入口。刻意做成「一个工具 + action 枚举」，而不是给每个动作单独一个工具：
     * 工具列表越长，模型选错的概率越高；而这十几个动作对孩子来说本来就是「对它做点什么」
     * 这一件事的不同说法。
     */
    mcp_server.AddTool(
        "self.pet.action",
        "对宠物做一件事。孩子说「喂它吃苹果」「陪它玩」「让它睡觉」「给我讲故事」「摸摸它」时用本工具。\n"
        "action 取值：\n"
        "  feed     —— 喂食，必须同时给 food（小饼干 / 苹果 / 牛奶 / 胡萝卜 / 热汤面 / 小蛋糕）\n"
        "  medicine —— 喂药，必须同时给 medicine（退烧药 / 感冒冲剂 / 维生素）\n"
        "  play     —— 陪它玩（花 3 币，心情大涨）\n"
        "  heal     —— 带它看病（花 10 币，治好病）\n"
        "  sleep    —— 让它睡觉（回精力）\n"
        "  story    —— 讲故事，可选给 topic（恐龙 / 太空 / 小兔子…）\n"
        "  song     —— 唱歌\n"
        "  teach    —— 学新东西，可选给 what\n"
        "  pet      —— 摸摸头（免费，只加一点点心情）\n"
        "  chat     —— 只是想聊两句、看看它状态\n"
        "返回 ok=false 时，code 说明原因（FULL 吃饱了 / NO_COIN 币不够 / NOT_SICK 没生病 /\n"
        "WRONG_MED 吃错药 / DAILY_CAP 今天次数用完），把 say 顺其自然地告诉孩子，不要念 code。\n"
        "返回的 say 是宠物自己的话，用它来回答孩子最自然。",
        PropertyList({
            Property("action", kPropertyTypeString, std::string("chat")),
            Property("food", kPropertyTypeString, std::string("")),
            Property("medicine", kPropertyTypeString, std::string("")),
            Property("topic", kPropertyTypeString, std::string("")),
            Property("what", kPropertyTypeString, std::string("")),
        }),
        [this](const PropertyList& properties) -> ToolResult {
            std::string action = properties["action"].value<std::string>();
            std::string food = properties["food"].value<std::string>();
            std::string medicine = properties["medicine"].value<std::string>();
            std::string topic = properties["topic"].value<std::string>();
            std::string what = properties["what"].value<std::string>();

            if (action.empty()) {
                action = "chat";
            }

            cJSON* args = cJSON_CreateObject();
            if (!food.empty()) {
                cJSON_AddStringToObject(args, "food", food.c_str());
            }
            if (!medicine.empty()) {
                cJSON_AddStringToObject(args, "medicine", medicine.c_str());
            }
            if (!topic.empty()) {
                cJSON_AddStringToObject(args, "topic", topic.c_str());
            }
            if (!what.empty()) {
                cJSON_AddStringToObject(args, "what", what.c_str());
            }
            char* text = cJSON_PrintUnformatted(args);
            std::string args_json = text != nullptr ? text : "{}";
            if (text != nullptr) {
                cJSON_free(text);
            }
            cJSON_Delete(args);

            PetActionResult r = cloud_.Act(action, args_json);
            if (r.from_cloud) {
                ReactToEvents(r);
                /* 动作之后状态变了，缓存标记作废，下次问状态会重新同步 */
                last_status_ms_ = esp_timer_get_time() / 1000;
            }

            PetStatus st = cloud_.GetStatus();
            std::string out;
            out.reserve(320);
            out += "{\"ok\":";
            out += r.ok ? "true" : "false";
            out += ",\"code\":\"";
            out += r.code;
            out += "\",\"say\":\"";
            out += r.say;
            out += "\",\"mode\":\"";
            out += r.from_cloud ? "cloud" : "local";
            out += "\",\"hunger\":";
            out += std::to_string(st.hunger);
            out += ",\"energy\":";
            out += std::to_string(st.energy);
            out += ",\"mood\":";
            out += std::to_string(st.mood);
            out += ",\"sick\":";
            out += st.sick ? "true" : "false";
            out += ",\"coins\":";
            out += std::to_string(st.coins);
            out += "}";
            return out;
        });

    mcp_server.AddTool(
        "self.pet.task",
        "孩子的任务清单。家长布置任务、孩子报备完成、家长确认发币都走这里。\n"
        "op 取值：\n"
        "  list    —— 列出当前任务（孩子问「我今天要做什么」时用）\n"
        "  submit  —— 孩子说「我做完了整理房间」，要给 title\n"
        "  publish —— 家长说「给她加个任务：整理房间，奖励 10 个金币」，要给 title 和 reward\n"
        "  approve —— 家长确认孩子报备的任务，给 title\n"
        "  reject  —— 家长说这个任务没做好，打回去，给 title\n"
        "报备后默认要家长确认才发币，这是刻意的：让孩子学会「做完还要被检查」。",
        PropertyList({
            Property("op", kPropertyTypeString, std::string("list")),
            Property("title", kPropertyTypeString, std::string("")),
            Property("reward", kPropertyTypeInteger, 10, 1, 999),
        }),
        [this](const PropertyList& properties) -> ToolResult {
            std::string op = properties["op"].value<std::string>();
            std::string title = properties["title"].value<std::string>();
            int reward = properties["reward"].value<int>();

            if (op.empty() || op == "list") {
                return std::string("{\"ok\":true,\"mode\":\"") +
                       (cloud_.fail_streak() > 0 ? "local" : "cloud") + "\",\"tasks\":\"" +
                       cloud_.TaskListText(false) + "\"}";
            }

            if (title.empty()) {
                return std::unexpected("要告诉我任务名字，孩子才知道做哪一件。");
            }

            cJSON* args = cJSON_CreateObject();
            cJSON_AddStringToObject(args, "title", title.c_str());
            if (op == "publish") {
                cJSON_AddNumberToObject(args, "reward", reward);
            }
            char* text = cJSON_PrintUnformatted(args);
            std::string args_json = text != nullptr ? text : "{}";
            if (text != nullptr) {
                cJSON_free(text);
            }
            cJSON_Delete(args);

            PetActionResult r = cloud_.Act("task_" + op, args_json);

            std::string out;
            out.reserve(256);
            out += "{\"ok\":";
            out += r.ok ? "true" : "false";
            out += ",\"code\":\"";
            out += r.code;
            out += "\",\"say\":\"";
            out += r.say;
            out += "\",\"mode\":\"";
            out += r.from_cloud ? "cloud" : "local";
            out += "\",\"coins\":";
            out += std::to_string(cloud_.GetStatus().coins);
            out += "}";
            return out;
        });
}
