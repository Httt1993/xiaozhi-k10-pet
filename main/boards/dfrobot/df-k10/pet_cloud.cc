#include "pet_cloud.h"
#include "pet_config.h"
#include "application.h"
#include "device_state.h"

#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <nvs.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#define TAG "PetCloud"

namespace {

/* NVS 命名空间。刻意不用 Settings 类：它在写入失败时走 ESP_ERROR_CHECK 会直接 panic，
 * 而 NVS 分区只有 16KB，多写几次就有撑满的可能，这里必须能容忍失败。 */
constexpr const char* kNvsNs = "pet_cloud";
constexpr const char* kKeyState = "state";
constexpr const char* kKeyUrl = "url";
constexpr const char* kKeyKey = "key";

constexpr int kMaxRequest = 4096;      /* 请求体上限：state 1.5KB + args，够了 */
constexpr int kMaxResponse = 8192;     /* 响应体上限 */
constexpr int kHttpTimeoutMs = 7000;

/* 数值规则与云端 pet-core.js 保持一一对应。改这里必须同步改云端，否则离线/在线切换会分叉。 */
constexpr int kDecayStepSec = 1800;      /* 每 0.5 小时一步 */
constexpr int kMaxDecaySteps = 48;       /* 单次最多补算 24 小时 */
constexpr int kSickAfterStarving = 43200;/* 饿到 0 持续 12 小时 → 生病 */
constexpr int kExpPerAction = 2;
constexpr int kExpPerLevel = 50;
constexpr int kCostPlay = 3;
constexpr int kCostHeal = 10;
constexpr int kFullHunger = 99;
constexpr int kDailyFunCap = 3;
constexpr int kInitCoins = 20;
constexpr int kActionCooldownSec = 2;

struct FoodDef {
    const char* id;
    const char* name;
    const char* aliases;  /* 逗号分隔 */
    int hunger;
    int mood;
    int energy;
    int cost;
    const char* line;
};

const FoodDef kFoods[] = {
    {"cookie",  "小饼干", "饼干,曲奇,cookie", 25,  5, 3, 2, "咔嚓咔嚓……小饼干最好吃啦！"},
    {"apple",   "苹果",   "苹果片,果子",      20,  8, 2, 2, "苹果甜甜的，脆脆的，我还要！"},
    {"milk",    "牛奶",   "奶,热牛奶,酸奶",   15,  5, 8, 3, "咕嘟咕嘟……喝完牛奶身上暖暖的～"},
    {"carrot",  "胡萝卜", "萝卜,红萝卜",      22,  2, 2, 2, "胡萝卜……好吧，为了眼睛亮亮的，我吃！"},
    {"noodles", "热汤面", "汤面,面条,面,汤",  30,  6, 6, 4, "呼——好烫！不过热汤面最暖肚子了～"},
    {"cake",    "小蛋糕", "蛋糕,布丁,甜点",   18, 14, 4, 5, "哇！是蛋糕！今天是什么好日子呀～"},
};
constexpr int kFoodCount = sizeof(kFoods) / sizeof(kFoods[0]);

struct MedDef {
    const char* id;
    const char* name;
    const char* aliases;
    int cure;
    int tonic;
    int mood;
    int energy;
    int cost;
    const char* line;
};

const MedDef kMeds[] = {
    {"fever",   "退烧药",   "退烧,退热药",      1, 0, 6, 4, 4, "药有点苦……但是头不晕了，谢谢你！"},
    {"cold",    "感冒冲剂", "感冒药,感冒灵,冲剂", 1, 0, 4, 2, 3, "甜甜的冲剂，喝完鼻子通啦～"},
    {"vitamin", "维生素",   "维C,维他命,vc",     0, 1, 4, 6, 2, "维C酸酸甜甜的，吃完感觉有力气啦！"},
};
constexpr int kMedCount = sizeof(kMeds) / sizeof(kMeds[0]);

int Clamp100(int v) { return v < 0 ? 0 : (v > 100 ? 100 : v); }
int Max0(int v) { return v < 0 ? 0 : v; }

/** 上海时区当天 00:00。SCF 是 UTC，设备是本地时间，这里统一按 +8 算自然日。 */
int64_t DayStart(int64_t ts) {
    const int64_t off = 8 * 3600;
    return ((ts + off) / 86400) * 86400 - off;
}

int64_t NowSec() {
    time_t t = time(nullptr);
    /* 小于 2020-09-13 说明 SNTP/OTA 还没同步时间，返回 0 表示「时间未知」 */
    if (t < 1600000000) {
        return 0;
    }
    return static_cast<int64_t>(t);
}

int64_t NowMs() {
    return esp_timer_get_time() / 1000;
}

/** 在 "a,b,c" 里找 needle，返回命中长度；不命中返回 0 */
int AliasHit(const char* aliases, const char* needle) {
    if (aliases == nullptr || needle == nullptr) {
        return 0;
    }
    std::string s(aliases);
    size_t start = 0;
    while (start <= s.size()) {
        size_t end = s.find(',', start);
        if (end == std::string::npos) {
            end = s.size();
        }
        std::string one = s.substr(start, end - start);
        if (!one.empty() && strstr(needle, one.c_str()) != nullptr) {
            return static_cast<int>(one.size());
        }
        start = end + 1;
    }
    return 0;
}

/** 最长别名优先：避免「红萝卜」被「萝卜」抢先命中 */
int FindFood(const char* ref) {
    if (ref == nullptr || ref[0] == '\0') {
        return 0; /* 默认小饼干 */
    }
    int best = -1;
    int bestLen = 0;
    for (int i = 0; i < kFoodCount; i++) {
        int hit = static_cast<int>(strlen(kFoods[i].name));
        if (strstr(ref, kFoods[i].name) == nullptr) {
            hit = 0;
        }
        hit = std::max(hit, AliasHit(kFoods[i].aliases, ref));
        if (hit > bestLen) {
            bestLen = hit;
            best = i;
        }
    }
    return best < 0 ? 0 : best;
}

int FindMed(const char* ref) {
    if (ref == nullptr || ref[0] == '\0') {
        return -1;
    }
    int best = -1;
    int bestLen = 0;
    for (int i = 0; i < kMedCount; i++) {
        int hit = static_cast<int>(strlen(kMeds[i].name));
        if (strstr(ref, kMeds[i].name) == nullptr) {
            hit = 0;
        }
        hit = std::max(hit, AliasHit(kMeds[i].aliases, ref));
        if (hit > bestLen) {
            bestLen = hit;
            best = i;
        }
    }
    return best;
}

std::string FoodNameList() {
    std::string s;
    for (int i = 0; i < kFoodCount; i++) {
        if (!s.empty()) s += "、";
        s += kFoods[i].name;
    }
    return s;
}

std::string MedNameList() {
    std::string s;
    for (int i = 0; i < kMedCount; i++) {
        if (!s.empty()) s += "、";
        s += kMeds[i].name;
    }
    return s;
}

/* ---- cJSON 小工具 ---- */

int GetInt(cJSON* obj, const char* key, int def) {
    if (obj == nullptr) return def;
    cJSON* it = cJSON_GetObjectItem(obj, key);
    return cJSON_IsNumber(it) ? it->valueint : def;
}

int64_t GetI64(cJSON* obj, const char* key, int64_t def) {
    if (obj == nullptr) return def;
    cJSON* it = cJSON_GetObjectItem(obj, key);
    if (cJSON_IsNumber(it)) {
        return static_cast<int64_t>(it->valuedouble);
    }
    return def;
}

void SetInt(cJSON* obj, const char* key, int v) {
    cJSON_DeleteItemFromObject(obj, key);
    cJSON_AddNumberToObject(obj, key, v);
}

void SetI64(cJSON* obj, const char* key, int64_t v) {
    cJSON_DeleteItemFromObject(obj, key);
    cJSON_AddNumberToObject(obj, key, static_cast<double>(v));
}

const char* GetStr(cJSON* obj, const char* key, const char* def) {
    if (obj == nullptr) return def;
    cJSON* it = cJSON_GetObjectItem(obj, key);
    return cJSON_IsString(it) ? it->valuestring : def;
}

}  // namespace

// ==================================================================== 生命周期

PetCloud::PetCloud() = default;
PetCloud::~PetCloud() = default;

void PetCloud::Init() {
    std::lock_guard<std::mutex> lock(mutex_);
    InitLocked();
}

void PetCloud::InitLocked() {
    if (inited_) {
        return;
    }
    inited_ = true;

    /* 配置优先级：NVS > 编译期宏。这样以后换地址不用重新烧固件。 */
    url_ = PET_CLOUD_URL;
    key_ = PET_CLOUD_KEY;

    nvs_handle_t h = 0;
    if (nvs_open(kNvsNs, NVS_READONLY, &h) == ESP_OK) {
        char buf[256];
        size_t len = sizeof(buf);
        if (nvs_get_str(h, kKeyUrl, buf, &len) == ESP_OK && buf[0] != '\0') {
            url_ = buf;
        }
        len = sizeof(buf);
        if (nvs_get_str(h, kKeyKey, buf, &len) == ESP_OK) {
            key_ = buf;
        }
        nvs_close(h);
    }

    LoadFromNvs();

    ESP_LOGI(TAG, "PET_PATCH: cloud %s, state %u B, hunger=%d mood=%d coins=%d",
             url_.empty() ? "disabled (local mode)" : url_.c_str(),
             static_cast<unsigned>(state_json_.size()),
             status_.hunger, status_.mood, status_.coins);
}

void PetCloud::LoadFromNvs() {
    nvs_handle_t h = 0;
    if (nvs_open(kNvsNs, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t len = 0;
    if (nvs_get_str(h, kKeyState, nullptr, &len) == ESP_OK && len > 2 && len < 16384) {
        std::vector<char> buf(len);
        if (nvs_get_str(h, kKeyState, buf.data(), &len) == ESP_OK) {
            state_json_.assign(buf.data());
        }
    }
    nvs_close(h);

    if (!state_json_.empty()) {
        RefreshStatusFromState();
    }
}

/**
 * 状态指纹：只挑「重启后不能丢」的字段。
 *
 * 刻意不含 lastDecay —— 它每次 sync 都会变，把它算进去等于每次都写 NVS。
 * 而 lastDecay 丢了也不要紧：云端/本地都会按「上次记录的时间」重算衰减，
 * 结果一样，只是多算一次。
 *
 * 含 tasks 的 id+status：家长新布置的任务、孩子报备的状态都必须落盘，
 * 否则一断电「今天该做什么」就没了。
 */
std::string PetCloud::StateFingerprint() const {
    if (state_json_.empty()) {
        return "";
    }
    cJSON* root = cJSON_Parse(state_json_.c_str());
    if (root == nullptr) {
        return "";
    }
    cJSON* p = cJSON_GetObjectItem(root, "pet");
    if (!cJSON_IsObject(p)) {
        cJSON_Delete(root);
        return "";
    }

    char head[160];
    snprintf(head, sizeof(head), "%d|%d|%d|%d|%d|%d|%lld|",
             GetInt(p, "hunger", 0), GetInt(p, "energy", 0), GetInt(p, "mood", 0),
             GetInt(p, "coins", 0), GetInt(p, "exp", 0), GetInt(p, "sick", 0),
             static_cast<long long>(GetI64(p, "starvingSince", 0)));

    std::string fp(head);
    cJSON* tasks = cJSON_GetObjectItem(root, "tasks");
    if (cJSON_IsArray(tasks)) {
        cJSON* t = nullptr;
        cJSON_ArrayForEach(t, tasks) {
            cJSON* id = cJSON_GetObjectItem(t, "id");
            cJSON* st = cJSON_GetObjectItem(t, "status");
            if (cJSON_IsString(id)) {
                fp += id->valuestring;
            }
            if (cJSON_IsString(st)) {
                fp += ':';
                fp += st->valuestring;
            }
            fp += ',';
        }
    }
    cJSON_Delete(root);
    return fp;
}

void PetCloud::PersistState() {
    if (state_json_.empty()) {
        return;
    }

    /* 内容没实质变化就不落盘。
       心跳每几分钟来一次，如果每次都写，NVS 分区一年就是十万次擦写。
       这里挡掉的是「只有 lastDecay 变了」的那种空写。 */
    std::string fp = StateFingerprint();
    if (!fp.empty() && fp == persisted_fingerprint_) {
        return;
    }

    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(kNvsNs, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS 打开失败，状态只在内存里（%s）", esp_err_to_name(err));
        return;
    }
    err = nvs_set_str(h, kKeyState, state_json_.c_str());
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    if (err != ESP_OK) {
        /* 这里绝不能 panic：NVS 满了也只是丢一次持久化，内存态继续跑 */
        ESP_LOGW(TAG, "状态写入 NVS 失败（%s），%u B", esp_err_to_name(err),
                 static_cast<unsigned>(state_json_.size()));
    } else {
        persisted_fingerprint_ = fp;
    }
    nvs_close(h);
}

// ==================================================================== 心跳

void PetCloud::StartHeartbeat(int interval_sec) {
    if (interval_sec <= 0) {
        ESP_LOGI(TAG, "心跳未启动（PET_HEARTBEAT_SEC=%d）", interval_sec);
        return;
    }
    if (heartbeat_task_ != nullptr) {
        return;
    }
    heartbeat_sec_ = interval_sec;

    /* 栈给 8KB：这条路径要跑 esp_http_client + mbedtls 握手，
       给少了会栈溢出重启 —— 这种重启日志里只看到 Guru Meditation，很难查。 */
    BaseType_t ok = xTaskCreate(&PetCloud::HeartbeatTask, "pet_beat", 8192, this, 3,
                                reinterpret_cast<TaskHandle_t*>(&heartbeat_task_));
    if (ok != pdPASS) {
        ESP_LOGW(TAG, "心跳任务创建失败（内存不足）：家长指令只能等孩子说话时才送达");
        heartbeat_task_ = nullptr;
        return;
    }
    ESP_LOGI(TAG, "心跳已启动：每 %d 秒静默 sync 一次，用来取回家长看板的指令", interval_sec);
}

bool PetCloud::HeartbeatEligible() {
    /* 只在待命/聆听时同步。说话中、升级中、配网中抢网络只会让体验变差。 */
    DeviceState state = Application::GetInstance().GetDeviceState();
    return state == kDeviceStateIdle || state == kDeviceStateListening;
}

void PetCloud::HeartbeatTask(void* arg) {
    auto* self = static_cast<PetCloud*>(arg);

    /* 开机头 30 秒让路：激活、OTA 校时、AFE 初始化都在这个窗口里。
       30 秒一过先同步一次 —— 家长在网页上点的指令，孩子一开机就该到，
       没必要让第一条指令等满一个心跳周期。 */
    vTaskDelay(pdMS_TO_TICKS(30000));

    while (true) {
        if (!self->cloud_enabled()) {
            vTaskDelay(pdMS_TO_TICKS(60000));
            continue;
        }
        if (!self->HeartbeatEligible()) {
            /* 正在说话/升级/配网。这里绝不按整周期等 —— 否则一次撞上说话，
               指令要再等 5 分钟，家长会以为按钮坏了。15 秒后再看。 */
            vTaskDelay(pdMS_TO_TICKS(15000));
            continue;
        }

        PetActionResult r = self->Act("sync", "{}");
        if (r.ok) {
            self->heartbeat_ok_++;
            self->fail_streak_ = 0;
            ESP_LOGI(TAG, "PET_PATCH: 心跳 sync ok %dms hunger=%d coins=%d fail=%d",
                     self->last_latency_ms_, self->status_.hunger, self->status_.coins,
                     self->fail_streak_);
        } else {
            self->heartbeat_fail_++;
            self->fail_streak_++;
            ESP_LOGW(TAG, "PET_PATCH: 心跳 sync 失败 %s（累计 %d 次）",
                     r.error.empty() ? "未知原因" : r.error.c_str(), self->heartbeat_fail_);
        }

        int wait_sec = self->heartbeat_sec_;
        if (self->fail_streak_ >= 3) {
            /* 断网时别按原频率硬撞，退避到最多 15 分钟 */
            wait_sec *= 3;
            if (wait_sec > 900) {
                wait_sec = 900;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(wait_sec * 1000));
    }
}


void PetCloud::PersistStatus() {
    /* 状态已经跟着 state 一起存了，这里不用单独存。保留空实现是为了以后
     * 想把「最近一次同步时间」单独落盘时不必改头文件。 */
}

// ==================================================================== 状态解析

void PetCloud::RefreshStatusFromState() {
    status_ = PetStatus();
    if (state_json_.empty()) {
        return;
    }
    cJSON* root = cJSON_Parse(state_json_.c_str());
    if (root == nullptr) {
        ESP_LOGW(TAG, "state JSON 解析失败，将重新初始化");
        state_json_.clear();
        return;
    }
    cJSON* p = cJSON_GetObjectItem(root, "pet");
    if (cJSON_IsObject(p)) {
        status_.valid = true;
        status_.name = GetStr(p, "name", "乔利");
        status_.energy = GetInt(p, "energy", 0);
        status_.hunger = GetInt(p, "hunger", 0);
        status_.mood = GetInt(p, "mood", 0);
        status_.coins = GetInt(p, "coins", 0);
        status_.exp = GetInt(p, "exp", 0);
        status_.level = status_.exp / kExpPerLevel + 1;
        status_.sick = GetInt(p, "sick", 0) != 0;

        if (status_.sick) {
            status_.mood_word = "我有点难受……头昏昏的，能不能给我看看医生呀？";
        } else if (status_.hunger <= 15) {
            status_.mood_word = "我肚子饿得咕咕叫了，快给我点吃的吧～";
        } else if (status_.mood <= 20) {
            status_.mood_word = "有点无聊呢，陪我玩一会儿好不好？";
        } else if (status_.mood >= 80 && status_.energy >= 70) {
            status_.mood_word = "我今天状态超好！";
        } else {
            status_.mood_word = "感觉还不错～";
        }

        status_.events.clear();
        if (status_.sick) status_.events.push_back("sick");
        if (status_.hunger <= 15) status_.events.push_back("starving");
        else if (status_.hunger <= 35) status_.events.push_back("hungry");
        if (status_.energy <= 20) status_.events.push_back("tired");
        if (status_.mood <= 20) status_.events.push_back("sad");
        else if (status_.mood <= 40) status_.events.push_back("bored");
        if (status_.events.empty() && status_.mood >= 80 && status_.energy >= 70) {
            status_.events.push_back("great");
        }
    }
    cJSON_Delete(root);
}

PetStatus PetCloud::GetStatus() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!inited_) {
        /* 没初始化就按默认值给，避免工具层拿到全 0 的假状态 */
        PetStatus s;
        s.valid = false;
        s.name = "乔利";
        s.energy = 80;
        s.hunger = 60;
        s.mood = 80;
        s.coins = kInitCoins;
        s.level = 1;
        return s;
    }
    return status_;
}

// ==================================================================== 网络

/*
 * 手动走 open → write → fetch_headers → read → close，而不是用 esp_http_client_perform。
 *
 * 原因：perform 在没有设置 on_data 回调时会把响应体读进内部 buffer 就丢掉，
 * 之后再调 esp_http_client_read 只能拿到空字符串 —— 表现是「HTTP 200 但 body 为空」，
 * 排查起来非常费劲。手动分步没有这个歧义。
 */
bool PetCloud::HttpPost(const std::string& body, std::string& out, std::string& err) {
    if (url_.empty()) {
        err = "未配置云端地址";
        return false;
    }

    esp_http_client_config_t cfg = {};
    cfg.url = url_.c_str();
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = kHttpTimeoutMs;
    cfg.buffer_size = 2048;      /* 收缓冲。state 约 1.5KB，响应 2-3KB，2KB 够翻页 */
    cfg.buffer_size_tx = 2048;
#if !PET_CLOUD_TLS_INSECURE
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
#endif

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        err = "http client 初始化失败";
        return false;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    if (!key_.empty()) {
        esp_http_client_set_header(client, "X-Pet-Key", key_.c_str());
    }

    int64_t t0 = NowMs();

    esp_err_t ret = esp_http_client_open(client, static_cast<int>(body.size()));
    if (ret != ESP_OK) {
        err = std::string("连接失败: ") + esp_err_to_name(ret);
        esp_http_client_cleanup(client);
        return false;
    }

    int written = esp_http_client_write(client, body.c_str(), static_cast<int>(body.size()));
    if (written != static_cast<int>(body.size())) {
        err = "请求体发送不完整";
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    int content_len = esp_http_client_fetch_headers(client);
    int code = esp_http_client_get_status_code(client);

    /* 一次把响应读完。content_len < 0 表示 chunked，靠 read 返回 0 判断结束。 */
    std::vector<char> buf(static_cast<size_t>(kMaxResponse));
    int total = 0;
    while (total < kMaxResponse - 1) {
        int r = esp_http_client_read(client, buf.data() + total, kMaxResponse - 1 - total);
        if (r <= 0) {
            break;
        }
        total += r;
    }
    buf[total] = '\0';

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    int64_t cost = NowMs() - t0;
    last_latency_ms_ = static_cast<int>(cost);

    if (code != 200) {
        err = std::string("HTTP ") + std::to_string(code);
        if (total > 0) {
            err += std::string(": ") + std::string(buf.data(), std::min(total, 120));
        }
        return false;
    }

    if (total == 0) {
        err = "响应为空（content_length=" + std::to_string(content_len) + "）";
        return false;
    }

    out.assign(buf.data(), total);
    ESP_LOGD(TAG, "HTTP %d, %d B in %lld ms", code, total, static_cast<long long>(cost));
    return true;
}

bool PetCloud::ApplyResponse(const char* resp, PetActionResult& out) {
    cJSON* root = cJSON_Parse(resp);
    if (root == nullptr) {
        out.error = "响应不是合法 JSON";
        return false;
    }

    cJSON* okItem = cJSON_GetObjectItem(root, "ok");
    out.ok = cJSON_IsTrue(okItem);
    out.code = GetStr(root, "code", "");
    out.say = GetStr(root, "say", "");
    out.from_cloud = true;

    cJSON* st = cJSON_GetObjectItem(root, "status");
    if (cJSON_IsObject(st)) {
        out.coins = GetInt(st, "coins", 0);
        out.events.clear();
        cJSON* ev = cJSON_GetObjectItem(root, "events");
        if (cJSON_IsArray(ev)) {
            cJSON* it = nullptr;
            cJSON_ArrayForEach(it, ev) {
                if (cJSON_IsString(it)) {
                    out.events.push_back(it->valuestring);
                }
            }
        }
    }

    /* 云端回传的 state 是权威版本，原样存下来（包含任务、金币流水等本地不维护的东西） */
    cJSON* state = cJSON_GetObjectItem(root, "state");
    if (cJSON_IsObject(state)) {
        char* text = cJSON_PrintUnformatted(state);
        if (text != nullptr) {
            state_json_.assign(text);
            cJSON_free(text);
            RefreshStatusFromState();
        }
    }

    /* 设备时钟偏差过大时提醒一次，方便排查「一开机就饿扁了」 */
    int skew = GetInt(root, "skew_sec", 0);
    if (skew > 300 || skew < -300) {
        ESP_LOGW(TAG, "设备时钟与云端相差 %d 秒，衰减已按云端时间计算", skew);
    }

    cJSON_Delete(root);
    return true;
}

// ==================================================================== 动作入口

PetActionResult PetCloud::Act(const std::string& action, const std::string& args_json) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!inited_) {
        InitLocked();
    }

    PetActionResult result;

    /* ---------- 有云端就走云端 ---------- */
    if (!url_.empty()) {
        cJSON* body = cJSON_CreateObject();
        cJSON_AddStringToObject(body, "action", action.c_str());

        if (!state_json_.empty()) {
            cJSON* state = cJSON_Parse(state_json_.c_str());
            if (state != nullptr) {
                cJSON_AddItemToObject(body, "state", state);
            }
        } else {
            cJSON_AddNullToObject(body, "state");
        }

        cJSON* args = nullptr;
        if (!args_json.empty()) {
            args = cJSON_Parse(args_json.c_str());
        }
        if (args == nullptr) {
            args = cJSON_CreateObject();
        }
        cJSON_AddItemToObject(body, "args", args);

        int64_t now = NowSec();
        if (now > 0) {
            cJSON_AddNumberToObject(body, "ts", static_cast<double>(now));
        }

        char* text = cJSON_PrintUnformatted(body);
        std::string payload = text != nullptr ? text : "";
        if (text != nullptr) {
            cJSON_free(text);
        }
        cJSON_Delete(body);

        if (!payload.empty() && static_cast<int>(payload.size()) < kMaxRequest) {
            std::string resp;
            std::string err;
            int64_t t0 = NowMs();
            bool http_ok = HttpPost(payload, resp, err);
            if (http_ok && ApplyResponse(resp.c_str(), result)) {
                fail_streak_ = 0;
                last_sync_ms_ = NowMs();
                PersistState();
                ESP_LOGI(TAG, "PET_PATCH: %s via cloud ok=%d code=%s %dms (hunger=%d mood=%d coins=%d)",
                         action.c_str(), result.ok ? 1 : 0, result.code.c_str(),
                         static_cast<int>(NowMs() - t0), status_.hunger, status_.mood,
                         status_.coins);
                return result;
            }
            result.error = http_ok ? result.error : err;
            fail_streak_++;
            ESP_LOGW(TAG, "PET_PATCH: %s cloud failed (%s), 降级本地 · 连续失败 %d 次",
                     action.c_str(), err.empty() ? "解析失败" : err.c_str(), fail_streak_);
        } else {
            result.error = "请求体过大";
        }
    }

    /* ---------- 云端不可用：本地演化 ---------- */
    PetActionResult local = ActLocal(action, args_json);
    local.error = result.error;
    /* 本地算出来的状态同样要落盘，否则断网期间喂的饭重启就没了 */
    PersistState();
    return local;
}

// ==================================================================== 本地兜底

void PetCloud::DecayLocal(int64_t now) {
    if (now <= 0 || state_json_.empty()) {
        return;
    }
    cJSON* root = cJSON_Parse(state_json_.c_str());
    if (root == nullptr) {
        return;
    }
    cJSON* p = cJSON_GetObjectItem(root, "pet");
    if (!cJSON_IsObject(p)) {
        cJSON_Delete(root);
        return;
    }

    int64_t last = GetI64(p, "lastDecay", now);
    int64_t steps = (now - last) / kDecayStepSec;
    if (steps <= 0) {
        cJSON_Delete(root);
        return;
    }
    steps = std::min<int64_t>(steps, kMaxDecaySteps);

    int hunger = GetInt(p, "hunger", 0);
    int energy = GetInt(p, "energy", 0);
    int mood = GetInt(p, "mood", 0);
    int sick = GetInt(p, "sick", 0);
    int64_t starvingSince = GetI64(p, "starvingSince", 0);

    for (int64_t i = 0; i < steps; i++) {
        int64_t stepTs = last + (i + 1) * kDecayStepSec;
        hunger = Max0(hunger - 2);
        energy = Max0(energy - 1);
        mood = Max0(mood - (hunger <= 10 ? 4 : 1));
        if (sick) {
            mood = Max0(mood - 3);
        }
        if (hunger == 0) {
            if (starvingSince == 0) {
                starvingSince = stepTs;
            } else if (stepTs - starvingSince > kSickAfterStarving) {
                sick = 1;
            }
        } else {
            starvingSince = 0;
        }
    }

    SetInt(p, "hunger", hunger);
    SetInt(p, "energy", energy);
    SetInt(p, "mood", mood);
    SetInt(p, "sick", sick);
    SetI64(p, "starvingSince", starvingSince);
    SetI64(p, "lastDecay", last + steps * kDecayStepSec);

    char* text = cJSON_PrintUnformatted(root);
    if (text != nullptr) {
        state_json_.assign(text);
        cJSON_free(text);
    }
    cJSON_Delete(root);
    RefreshStatusFromState();

    ESP_LOGI(TAG, "PET_PATCH: local decay %lld steps -> hunger=%d energy=%d mood=%d sick=%d",
             static_cast<long long>(steps), status_.hunger, status_.energy, status_.mood,
             status_.sick ? 1 : 0);
}

PetActionResult PetCloud::ActLocal(const std::string& action, const std::string& args_json) {
    PetActionResult out;
    out.from_cloud = false;

    int64_t now = NowSec();

    /* 首次使用：按云端同样的初值建一份 */
    if (state_json_.empty()) {
        cJSON* root = cJSON_CreateObject();
        cJSON_AddNumberToObject(root, "v", 0);
        cJSON* p = cJSON_AddObjectToObject(root, "pet");
        cJSON_AddStringToObject(p, "name", "乔利");
        cJSON_AddNumberToObject(p, "energy", 80);
        cJSON_AddNumberToObject(p, "hunger", 60);
        cJSON_AddNumberToObject(p, "mood", 80);
        cJSON_AddNumberToObject(p, "sick", 0);
        cJSON_AddNumberToObject(p, "coins", kInitCoins);
        cJSON_AddNumberToObject(p, "exp", 0);
        cJSON_AddNumberToObject(p, "lastDecay", static_cast<double>(now > 0 ? now : 0));
        cJSON_AddNumberToObject(p, "starvingSince", 0);
        cJSON_AddNumberToObject(p, "lastFeed", 0);
        cJSON_AddNumberToObject(p, "lastActionAt", 0);
        cJSON_AddNumberToObject(p, "createdAt", static_cast<double>(now > 0 ? now : 0));
        cJSON* counters = cJSON_AddObjectToObject(p, "counters");
        cJSON_AddNumberToObject(counters, "day", static_cast<double>(DayStart(now > 0 ? now : 0)));
        cJSON_AddArrayToObject(root, "tasks");
        cJSON_AddArrayToObject(root, "coinLog");
        cJSON_AddObjectToObject(root, "meta");

        char* text = cJSON_PrintUnformatted(root);
        if (text != nullptr) {
            state_json_.assign(text);
            cJSON_free(text);
        }
        cJSON_Delete(root);
        RefreshStatusFromState();
        ESP_LOGI(TAG, "PET_PATCH: local state initialized (%u B)",
                 static_cast<unsigned>(state_json_.size()));
    }

    DecayLocal(now);

    cJSON* root = cJSON_Parse(state_json_.c_str());
    if (root == nullptr) {
        out.error = "本地 state 解析失败";
        return out;
    }
    cJSON* p = cJSON_GetObjectItem(root, "pet");
    cJSON* args = nullptr;
    if (!args_json.empty()) {
        args = cJSON_Parse(args_json.c_str());
    }

    int hunger = GetInt(p, "hunger", 60);
    int energy = GetInt(p, "energy", 80);
    int mood = GetInt(p, "mood", 80);
    int sick = GetInt(p, "sick", 0);
    int coins = GetInt(p, "coins", kInitCoins);
    int exp = GetInt(p, "exp", 0);
    int64_t lastActionAt = GetI64(p, "lastActionAt", 0);
    int64_t starvingSince = GetI64(p, "starvingSince", 0);

    /* 节流：2 秒内连击直接拒，挡语音误识别刷币 */
    bool throttled = (now > 0 && lastActionAt > 0 && now - lastActionAt < kActionCooldownSec);

    auto touch = [&]() {
        if (now > 0) {
            SetI64(p, "lastActionAt", now);
        }
    };
    auto commit = [&]() {
        SetInt(p, "hunger", hunger);
        SetInt(p, "energy", energy);
        SetInt(p, "mood", mood);
        SetInt(p, "sick", sick);
        SetInt(p, "coins", coins);
        SetInt(p, "exp", exp);
        SetI64(p, "starvingSince", starvingSince);
        char* text = cJSON_PrintUnformatted(root);
        if (text != nullptr) {
            state_json_.assign(text);
            cJSON_free(text);
        }
        RefreshStatusFromState();
        out.coins = coins;
        out.events = status_.events;
        if (out.say.empty()) {
            out.say = status_.mood_word;
        }
    };

    /* 节流只针对「宠物动作」：家长连说两个任务、孩子连续报备都不该被拦 */
    bool is_task_action = action.compare(0, 5, "task_") == 0;
    if (throttled && action != "sync" && action != "coins" && !is_task_action) {
        out.ok = false;
        out.code = "TOO_FAST";
        out.say = "慢一点点啦，我还没反应过来～";
        commit();
        cJSON_Delete(root);
        if (args) cJSON_Delete(args);
        return out;
    }

    if (action == "sync") {
        out.ok = true;
        out.code = "SYNC";
        out.say = status_.mood_word;
    } else if (action == "feed") {
        const char* ref = args ? GetStr(args, "food", nullptr) : nullptr;
        int fi = FindFood(ref);
        const FoodDef& f = kFoods[fi];
        if (hunger >= kFullHunger) {
            out.ok = false;
            out.code = "FULL";
            out.say = "我肚子已经圆滚滚啦，实在吃不下了……等会儿再喂我好不好？";
        } else if (coins < f.cost) {
            out.ok = false;
            out.code = "NO_COIN";
            out.say = std::string("我的游戏币不够啦，") + f.name + "要 " + std::to_string(f.cost) +
                      " 枚呢。现在只有 " + std::to_string(coins) + " 枚。";
        } else {
            coins -= f.cost;
            hunger = Clamp100(hunger + f.hunger);
            mood = Clamp100(mood + f.mood);
            energy = Clamp100(energy + f.energy);
            exp += kExpPerAction;
            touch();
            out.ok = true;
            out.code = "FED";
            out.say = std::string(f.line) + "（饱食 +" + std::to_string(f.hunger) + "，心情 +" +
                      std::to_string(f.mood) + "）";
        }
    } else if (action == "medicine") {
        const char* ref = args ? GetStr(args, "medicine", nullptr) : nullptr;
        if (ref == nullptr && args) {
            ref = GetStr(args, "med", nullptr);
        }
        int mi = FindMed(ref);
        if (mi < 0) {
            out.ok = false;
            out.code = "NO_MED";
            out.say = "我的药箱里没有这个药呀。现在有：" + MedNameList() + "。";
        } else {
            const MedDef& m = kMeds[mi];
            if (!sick) {
                if (m.tonic) {
                    if (coins < m.cost) {
                        out.ok = false;
                        out.code = "NO_COIN";
                        out.say = std::string("维生素要 ") + std::to_string(m.cost) + " 枚游戏币，我的币不够啦。";
                    } else {
                        coins -= m.cost;
                        mood = Clamp100(mood + m.mood);
                        energy = Clamp100(energy + m.energy);
                        exp += kExpPerAction;
                        touch();
                        out.ok = true;
                        out.code = "TONIC";
                        out.say = std::string(m.line) + "（我没生病，这个只是补身体～）";
                    }
                } else {
                    out.ok = false;
                    out.code = "NOT_SICK";
                    out.say = "我现在没生病呀，乱吃药肚子会疼的。要是不舒服，先问问我哪里难受～";
                }
            } else if (!m.cure) {
                out.ok = false;
                out.code = "WRONG_MED";
                out.say = std::string(m.name) + "只能保健，治不了病哦。我这次是真的不舒服，得吃退烧药或者感冒冲剂。";
            } else if (coins < m.cost) {
                out.ok = false;
                out.code = "NO_COIN";
                out.say = std::string(m.name) + "要 " + std::to_string(m.cost) + " 枚游戏币，我的币不够啦……";
            } else {
                coins -= m.cost;
                sick = 0;
                starvingSince = 0;
                mood = Clamp100(mood + m.mood);
                energy = Clamp100(energy + m.energy);
                exp += kExpPerAction * 3;
                touch();
                out.ok = true;
                out.code = "CURED";
                out.say = std::string(m.line) + " 我的病好啦！";
            }
        }
    } else if (action == "play") {
        if (sick) {
            out.ok = false;
            out.code = "SICK";
            out.say = "我现在病着呢，玩不动啦……先给我治治病好不好？";
        } else if (coins < kCostPlay) {
            out.ok = false;
            out.code = "NO_COIN";
            out.say = "游戏币不够啦，玩一次要 " + std::to_string(kCostPlay) + " 枚。现在只有 " +
                      std::to_string(coins) + " 枚。";
        } else if (energy < 10) {
            out.ok = false;
            out.code = "TIRED";
            out.say = "我有点累了，想睡一会儿，明天再玩好不好？";
        } else {
            coins -= kCostPlay;
            mood = Clamp100(mood + 18);
            energy = Max0(energy - 8);
            hunger = Max0(hunger - 3);
            exp += kExpPerAction;
            touch();
            out.ok = true;
            out.code = "PLAY";
            out.say = "太好玩啦！我最喜欢和你一起玩～";
        }
    } else if (action == "heal") {
        if (!sick) {
            out.ok = false;
            out.code = "HEALTHY";
            out.say = "我现在很健康哦，不需要看病～";
        } else if (coins < kCostHeal) {
            out.ok = false;
            out.code = "NO_COIN";
            out.say = "治病要 " + std::to_string(kCostHeal) + " 枚游戏币，我的币不够啦……";
        } else {
            coins -= kCostHeal;
            sick = 0;
            starvingSince = 0;
            mood = Clamp100(mood + 20);
            energy = Clamp100(energy + 10);
            exp += kExpPerAction * 3;
            touch();
            out.ok = true;
            out.code = "HEALED";
            out.say = "谢谢医生！我感觉好多啦，又活蹦乱跳了～";
        }
    } else if (action == "sleep") {
        energy = Clamp100(energy + 20);
        hunger = Max0(hunger - 5);
        touch();
        out.ok = true;
        out.code = "SLEEP";
        out.say = "那我先睡一会儿啦，呼呼～";
    } else if (action == "pet") {
        mood = Clamp100(mood + 4);
        exp += 1;
        touch();
        static const char* kLines[] = {"嘿嘿，好痒～再摸摸", "舒服……我最喜欢被摸头了",
                                       "唔…再摸一会儿嘛"};
        out.ok = true;
        out.code = "PET";
        out.say = kLines[static_cast<int>((now > 0 ? now : 0) % 3)];
    } else if (action == "story" || action == "song" || action == "teach") {
        /* 免费互动每天限次。本地也要管住，否则离线就成了「无限刷心情」，
         * 家长布置的任务体系会失去意义。计数器存在 pet.counters 里，与云端同结构。 */
        const char* key = action == "story" ? "story" : (action == "song" ? "sing" : "teach");
        const char* label = action == "story" ? "讲故事" : (action == "song" ? "唱歌" : "学东西");
        int64_t day = DayStart(now > 0 ? now : 0);

        cJSON* counters = cJSON_GetObjectItem(p, "counters");
        if (!cJSON_IsObject(counters) || GetI64(counters, "day", 0) != day) {
            cJSON_DeleteItemFromObject(p, "counters");
            counters = cJSON_AddObjectToObject(p, "counters");
            cJSON_AddNumberToObject(counters, "day", static_cast<double>(day));
        }
        int used = GetInt(counters, key, 0);

        if (used >= kDailyFunCap) {
            out.ok = false;
            out.code = "DAILY_CAP";
            out.say = std::string("今天已经") + label + " " + std::to_string(used) +
                      " 次啦，我们先做点别的，或者把今天的事情做完再来～";
        } else {
            cJSON_DeleteItemFromObject(counters, key);
            cJSON_AddNumberToObject(counters, key, used + 1);

            if (action == "story") {
                const char* topic = args ? GetStr(args, "topic", "") : "";
                mood = Clamp100(mood + 10);
                energy = Max0(energy - 2);
                exp += kExpPerAction * 2;
                touch();
                out.ok = true;
                out.code = "STORY";
                std::string t = (topic != nullptr && topic[0] != '\0') ? topic : "恐龙";
                out.say = "好呀，我给你讲一个关于" + t + "的故事——从前有一只小宠物，它……";
            } else if (action == "song") {
                mood = Clamp100(mood + 12);
                energy = Max0(energy - 3);
                exp += kExpPerAction * 2;
                touch();
                out.ok = true;
                out.code = "SONG";
                out.say = "啦啦啦～我唱歌给你听！";
            } else {
                const char* what = args ? GetStr(args, "what", "") : "";
                mood = Clamp100(mood + 8);
                energy = Max0(energy - 2);
                exp += kExpPerAction * 2;
                touch();
                out.ok = true;
                out.code = "TEACH";
                out.say = (what != nullptr && what[0] != '\0')
                              ? ("好呀，我想学" + std::string(what) + "！你教我吧～")
                              : "好呀，我想学新东西！你教我吧～";
            }
        }
    } else if (action == "chat") {
        mood = Clamp100(mood + 1);
        touch();
        out.ok = true;
        out.code = "CHAT";
        out.say = status_.mood_word;
    } else if (action == "coins") {
        out.ok = true;
        out.code = "COINS";
        out.say = "我现在有 " + std::to_string(coins) + " 枚游戏币。";
    } else if (action == "task_list") {
        out.ok = true;
        out.code = "TASKS";
        out.say = "";

    /* ---- 本地任务：离线时也完整可用，不让「养成」缺一半 ---- */
    } else if (action.compare(0, 5, "task_") == 0) {
        std::string title = args ? GetStr(args, "title", "") : "";
        cJSON* tasks = cJSON_GetObjectItem(root, "tasks");
        if (!cJSON_IsArray(tasks)) {
            cJSON_DeleteItemFromObject(root, "tasks");
            tasks = cJSON_AddArrayToObject(root, "tasks");
        }

        /* 找任务：优先在 pending 里找，避免同名旧任务被误命中 */
        auto findByTitle = [&](bool pending_only) -> cJSON* {
            cJSON* hit = nullptr;
            cJSON* it = nullptr;
            cJSON_ArrayForEach(it, tasks) {
                const char* t = GetStr(it, "title", "");
                if (t == nullptr || t[0] == '\0') {
                    continue;
                }
                if (strstr(t, title.c_str()) == nullptr) {
                    continue;
                }
                if (pending_only && strcmp(GetStr(it, "status", ""), "pending") != 0) {
                    continue;
                }
                hit = it;
                break;
            }
            return hit;
        };

        if (action == "task_publish") {
            if (title.empty()) {
                out.ok = false;
                out.code = "EMPTY";
                out.say = "任务名不能为空。";
            } else {
                int reward = args ? GetInt(args, "reward", 10) : 10;
                reward = reward < 1 ? 1 : (reward > 999 ? 999 : reward);
                char idbuf[32];
                snprintf(idbuf, sizeof(idbuf), "t%llx%x",
                         static_cast<long long>(now > 0 ? now : 0),
                         static_cast<unsigned>(cJSON_GetArraySize(tasks)));
                cJSON* t = cJSON_CreateObject();
                cJSON_AddStringToObject(t, "id", idbuf);
                cJSON_AddStringToObject(t, "title", title.c_str());
                cJSON_AddNumberToObject(t, "reward", reward);
                cJSON_AddStringToObject(t, "status", "pending");
                cJSON_AddNumberToObject(t, "createdAt", static_cast<double>(now > 0 ? now : 0));
                cJSON_AddItemToArray(tasks, t);
                /* 与云端 TASK_KEEP 一致，防止 state 撑爆 NVS */
                while (cJSON_GetArraySize(tasks) > 12) {
                    cJSON_DeleteItemFromArray(tasks, 0);
                }
                out.ok = true;
                out.code = "TASK_PUBLISH";
                out.say = "已发布任务「" + title + "」，奖励 " + std::to_string(reward) + " 枚游戏币。";
            }
        } else if (title.empty()) {
            out.ok = false;
            out.code = "EMPTY";
            out.say = "要告诉我任务名字，孩子才知道做哪一件。";
        } else if (action == "task_submit") {
            cJSON* t = findByTitle(true);
            if (t == nullptr) {
                t = findByTitle(false);
            }
            if (t == nullptr) {
                out.ok = false;
                out.code = "NOT_FOUND";
                out.say = "我没找到「" + title + "」这个任务哦，你可以问家长今天要做什么。";
            } else {
                const char* st = GetStr(t, "status", "");
                if (strcmp(st, "submitted") == 0) {
                    out.ok = false;
                    out.code = "DUPLICATE";
                    out.say = "「" + title + "」你已经报备过啦，等家长确认就好。";
                } else if (strcmp(st, "done") == 0) {
                    out.ok = false;
                    out.code = "DONE";
                    out.say = "「" + title + "」已经完成并且发过币啦，换一个任务试试？";
                } else {
                    int reward = GetInt(t, "reward", 10);
                    cJSON_DeleteItemFromObject(t, "status");
                    cJSON_AddStringToObject(t, "status", "submitted");
                    cJSON_DeleteItemFromObject(t, "submittedAt");
                    cJSON_AddNumberToObject(t, "submittedAt",
                                            static_cast<double>(now > 0 ? now : 0));
                    out.ok = true;
                    out.code = "TASK_SUBMIT";
                    out.say = "「" + title + "」我记下啦，等家长确认后给你发 " +
                              std::to_string(reward) + " 枚游戏币！";
                }
            }
        } else if (action == "task_approve") {
            cJSON* t = findByTitle(true);
            if (t == nullptr) {
                t = findByTitle(false);
            }
            if (t == nullptr) {
                out.ok = false;
                out.code = "NOT_FOUND";
                out.say = "没找到这个任务。";
            } else if (strcmp(GetStr(t, "status", ""), "done") == 0) {
                out.ok = false;
                out.code = "ALREADY";
                out.say = "该任务已经发过币了。";
            } else {
                int reward = GetInt(t, "reward", 10);
                cJSON_DeleteItemFromObject(t, "status");
                cJSON_AddStringToObject(t, "status", "done");
                cJSON_DeleteItemFromObject(t, "doneAt");
                cJSON_AddNumberToObject(t, "doneAt", static_cast<double>(now > 0 ? now : 0));
                coins += reward;
                out.ok = true;
                out.code = "TASK_APPROVE";
                out.say = "已确认「" + std::string(GetStr(t, "title", "")) + "」，发放 " +
                          std::to_string(reward) + " 枚游戏币。";
            }
        } else if (action == "task_reject") {
            cJSON* t = findByTitle(true);
            if (t == nullptr) {
                out.ok = false;
                out.code = "NOT_FOUND";
                out.say = "没找到这个任务。";
            } else {
                cJSON_DeleteItemFromObject(t, "status");
                cJSON_AddStringToObject(t, "status", "pending");
                cJSON_DeleteItemFromObject(t, "submittedAt");
                out.ok = true;
                out.code = "TASK_REJECT";
                out.say = "「" + title + "」先退回，让孩子再检查一下。";
            }
        } else {
            out.ok = false;
            out.code = "UNKNOWN_ACTION";
            out.say = "不认识的任务操作：" + action;
        }
    } else if (action == "reset") {
        /* 清掉内存和 NVS，下一次进来会按初值重建 */
        state_json_.clear();
        status_ = PetStatus();
        nvs_handle_t h = 0;
        if (nvs_open(kNvsNs, NVS_READWRITE, &h) == ESP_OK) {
            nvs_erase_key(h, kKeyState);
            nvs_commit(h);
            nvs_close(h);
        }
        out.ok = true;
        out.code = "RESET";
        out.say = "已经重新开始养啦。";
        if (args) cJSON_Delete(args);
        cJSON_Delete(root);
        return out;
    } else {
        out.ok = false;
        out.code = "UNKNOWN_ACTION";
        out.say = "不认识的指令：" + action;
    }

    commit();
    cJSON_Delete(root);
    if (args) {
        cJSON_Delete(args);
    }
    return out;
}

// ==================================================================== 任务清单

std::string PetCloud::TaskListText(bool include_done) {
    std::string payload = std::string("{\"include_done\":") + (include_done ? "true" : "false") + "}";
    /* 先让云端/本地跑一次 task_list（会刷新 state），再解析结果 */
    PetActionResult r = Act("task_list", payload);
    (void)r;

    std::lock_guard<std::mutex> lock(mutex_);
    if (state_json_.empty()) {
        return "现在还没有任务。";
    }
    cJSON* root = cJSON_Parse(state_json_.c_str());
    if (root == nullptr) {
        return "任务读取失败。";
    }
    cJSON* tasks = cJSON_GetObjectItem(root, "tasks");
    std::string out;
    int n = 0;
    if (cJSON_IsArray(tasks)) {
        cJSON* it = nullptr;
        cJSON_ArrayForEach(it, tasks) {
            const char* status = GetStr(it, "status", "");
            if (!include_done && strcmp(status, "done") == 0) {
                continue;
            }
            const char* title = GetStr(it, "title", "");
            int reward = GetInt(it, "reward", 0);
            const char* word = "待完成";
            if (strcmp(status, "submitted") == 0) {
                word = "等家长确认";
            } else if (strcmp(status, "done") == 0) {
                word = "已完成";
            }
            if (n > 0) out += "；";
            out += std::string(title) + "（" + word + "，奖励 " + std::to_string(reward) + " 枚）";
            n++;
        }
    }
    cJSON_Delete(root);
    if (n == 0) {
        return "现在还没有任务。";
    }
    return out;
}
