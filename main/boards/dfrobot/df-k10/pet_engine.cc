/*
 * pet_engine.cc —— pet-core.js 的 C++ 本地移植（逐函数对应）
 * =================================================================
 * 数值/规则唯一权威来源：cloud/scf/pet-core.js。本文件函数命名与 JS 一致，
 * 便于对照回归（feed / applyDecay / submitTask / trimState / run / pack ...）。
 *
 * 移植要点：
 *   - 状态是同一份 cJSON 树（对应 JS 的 doc 对象），NVS 存 JSON 字符串，格式不变；
 *   - 纯函数，时间 ts 由调用方传入；不做网络/存储/时钟；
 *   - run() 返回 SCF 风格响应，可直接替代 PetCloud 里的 HTTP 调用。
 */

#include "pet_engine.h"

#include <cJSON.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace pet_engine {
namespace {

/* ============================ 全局配置 ============================ */

Config g_cfg = DefaultConfig();

/* ============================ cJSON 小工具 ============================ */

cJSON* JGet(cJSON* o, const char* k) {
    return o ? cJSON_GetObjectItem(o, k) : nullptr;
}
bool Has(cJSON* o, const char* k) { return JGet(o, k) != nullptr; }
int JInt(cJSON* o, const char* k, int def) {
    cJSON* it = JGet(o, k);
    return cJSON_IsNumber(it) ? it->valueint : def;
}
int64_t JI64(cJSON* o, const char* k, int64_t def) {
    cJSON* it = JGet(o, k);
    return cJSON_IsNumber(it) ? (int64_t)(it->valuedouble + (it->valuedouble < 0 ? -0.5 : 0.5)) : def;
}
double JNum(cJSON* o, const char* k, double def) {
    cJSON* it = JGet(o, k);
    return cJSON_IsNumber(it) ? it->valuedouble : def;
}
const char* JStr(cJSON* o, const char* k, const char* def) {
    cJSON* it = JGet(o, k);
    return cJSON_IsString(it) ? it->valuestring : def;
}
bool JBool(cJSON* o, const char* k, bool def) {
    cJSON* it = JGet(o, k);
    return cJSON_IsBool(it) ? cJSON_IsTrue(it) : def;
}

/* 覆盖式写入：先删后加，键不存在也能创建（与 pet_cloud.cc 的 SetInt 一致）。 */
void SetInt(cJSON* o, const char* k, int v) {
    cJSON_DeleteItemFromObject(o, k);
    cJSON_AddNumberToObject(o, k, v);
}
void SetI64(cJSON* o, const char* k, int64_t v) {
    cJSON_DeleteItemFromObject(o, k);
    cJSON_AddNumberToObject(o, k, (double)v);
}
void SetNum(cJSON* o, const char* k, double v) {
    cJSON_DeleteItemFromObject(o, k);
    cJSON_AddNumberToObject(o, k, v);
}
void SetStr(cJSON* o, const char* k, const char* v) {
    cJSON_DeleteItemFromObject(o, k);
    cJSON_AddStringToObject(o, k, v ? v : "");
}
void SetBool(cJSON* o, const char* k, bool v) {
    cJSON_DeleteItemFromObject(o, k);
    cJSON_AddBoolToObject(o, k, v);
}
void SetNull(cJSON* o, const char* k) {
    cJSON_DeleteItemFromObject(o, k);
    cJSON_AddNullToObject(o, k);
}
void EnsureI64(cJSON* o, const char* k, int64_t def) {
    if (!Has(o, k)) SetI64(o, k, def);
}
void EnsureInt(cJSON* o, const char* k, int def) {
    if (!Has(o, k)) SetInt(o, k, def);
}
void EnsureStr(cJSON* o, const char* k, const char* def) {
    if (!Has(o, k)) SetStr(o, k, def);
}

/* ============================ 通用工具（对应 JS） ============================ */

int64_t NowSec() { return (int64_t)time(nullptr); }

/* 上海时区当天 00:00（秒）。对应 pet-core.js dayStartSec。 */
int64_t DayStartSec(int64_t ts) {
    const int64_t OFF = 8 * 3600;
    return ((ts + OFF) / 86400) * 86400 - OFF;
}

int Clamp100(double n) {
    int v = (int)std::lround(n);
    return std::max(0, std::min(100, v));
}

/* trim + tolower。对应 norm()。 */
std::string Norm(const char* s) {
    std::string t = s ? s : "";
    while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) t.erase(t.begin());
    while (!t.empty() && (t.back() == ' ' || t.back() == '\t' || t.back() == '\n' || t.back() == '\r')) t.pop_back();
    std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return t;
}

int Rr(int n) { return n <= 0 ? 0 : (rand() % n); }

/* ============================ 物品库（对应 FOODS / MEDS） ============================ */

struct Item {
    const char* id;
    const char* name;
    const char* aliases[6];  // nullptr 结尾
    int hunger, mood, energy, cost;
    int cure, tonic;
    const char* line;
};

const Item FOODS[] = {
    {"cookie", "小饼干", {"饼干", "曲奇", "cookie", nullptr}, 25, 5, 3, 2, 0, 0, "咔嚓咔嚓……小饼干最好吃啦！"},
    {"apple", "苹果", {"苹果片", "果子", nullptr}, 20, 8, 2, 2, 0, 0, "苹果甜甜的，脆脆的，我还要！"},
    {"milk", "牛奶", {"奶", "热牛奶", "酸奶", nullptr}, 15, 5, 8, 3, 0, 0, "咕嘟咕嘟……喝完牛奶身上暖暖的～"},
    {"carrot", "胡萝卜", {"萝卜", "红萝卜", nullptr}, 22, 2, 2, 2, 0, 0, "胡萝卜……好吧，为了眼睛亮亮的，我吃！"},
    {"noodles", "热汤面", {"汤面", "面条", "面", "汤", nullptr}, 30, 6, 6, 4, 0, 0, "呼——好烫！不过热汤面最暖肚子了～"},
    {"cake", "小蛋糕", {"蛋糕", "布丁", "甜点", "甜品", nullptr}, 18, 14, 4, 5, 0, 0, "哇！是蛋糕！今天是什么好日子呀～"},
};
const int FOODS_N = (int)(sizeof(FOODS) / sizeof(FOODS[0]));

const Item MEDS[] = {
    {"fever", "退烧药", {"退烧", "退热药", nullptr}, 0, 6, 4, 4, 1, 0, "药有点苦……但是头不晕了，谢谢你！"},
    {"cold", "感冒冲剂", {"感冒药", "感冒灵", "冲剂", nullptr}, 0, 4, 2, 3, 1, 0, "甜甜的冲剂，喝完鼻子通啦～"},
    {"vitamin", "维生素", {"维C", "维他命", "vc", nullptr}, 0, 4, 6, 2, 0, 1, "维C酸酸甜甜的，吃完感觉有力气啦！"},
};
const int MEDS_N = (int)(sizeof(MEDS) / sizeof(MEDS[0]));

/* 按别名匹配，命中「最长」的那个键。对应 pickByAlias。 */
const Item* PickByAlias(const Item* list, int n, const char* text) {
    std::string t = Norm(text);
    if (t.empty()) return nullptr;
    const Item* best = nullptr;
    size_t bestLen = 0;
    for (int i = 0; i < n; i++) {
        const Item& it = list[i];
        const char* keys[10];
        int nk = 0;
        keys[nk++] = it.id;
        keys[nk++] = it.name;
        for (int a = 0; a < 6 && it.aliases[a]; a++) keys[nk++] = it.aliases[a];
        for (int k = 0; k < nk; k++) {
            std::string kk = Norm(keys[k]);
            if (!kk.empty() && t.find(kk) != std::string::npos && kk.size() > bestLen) {
                best = &it;
                bestLen = kk.size();
            }
        }
    }
    return best;
}

const Item* FindFood(const char* t) { return PickByAlias(FOODS, FOODS_N, t); }
const Item* FindMed(const char* t) { return PickByAlias(MEDS, MEDS_N, t); }

std::string FoodNames() {
    std::string s;
    for (int i = 0; i < FOODS_N; i++) { if (i) s += "、"; s += FOODS[i].name; }
    return s;
}
std::string MedNames() {
    std::string s;
    for (int i = 0; i < MEDS_N; i++) { if (i) s += "、"; s += MEDS[i].name; }
    return s;
}

/* ============================ 初始化 / 规范化 ============================ */

/* 对应 initDoc。返回新建 doc（调用方 free）。 */
cJSON* InitDoc(const Config& cfg, int64_t ts) {
    int startCoins = std::max(0, (int)std::lround(cfg.init_coins));
    cJSON* doc = cJSON_CreateObject();
    cJSON_AddNumberToObject(doc, "v", 0);

    cJSON* p = cJSON_CreateObject();
    cJSON_AddItemToObject(doc, "pet", p);
    cJSON_AddStringToObject(p, "name", cfg.pet_name.c_str());
    cJSON_AddNumberToObject(p, "energy", 80);
    cJSON_AddNumberToObject(p, "hunger", 60);
    cJSON_AddNumberToObject(p, "mood", 80);
    cJSON_AddNumberToObject(p, "sick", 0);
    cJSON_AddNumberToObject(p, "coins", startCoins);
    cJSON_AddNumberToObject(p, "exp", 0);
    cJSON_AddNumberToObject(p, "lastDecay", (double)ts);
    cJSON_AddNumberToObject(p, "starvingSince", 0);
    cJSON_AddNumberToObject(p, "lastFeed", 0);
    cJSON_AddNumberToObject(p, "lastActionAt", 0);
    cJSON_AddNumberToObject(p, "cmdAck", 0);
    cJSON_AddNumberToObject(p, "createdAt", (double)ts);
    cJSON* counters = cJSON_CreateObject();
    cJSON_AddItemToObject(p, "counters", counters);
    cJSON_AddNumberToObject(counters, "day", (double)DayStartSec(ts));

    cJSON_AddItemToObject(doc, "tasks", cJSON_CreateArray());
    cJSON* coinLog = cJSON_CreateArray();
    cJSON_AddItemToObject(doc, "coinLog", coinLog);
    if (startCoins > 0) {
        cJSON* c = cJSON_CreateObject();
        cJSON_AddNumberToObject(c, "ts", (double)ts);
        cJSON_AddNumberToObject(c, "delta", startCoins);
        cJSON_AddStringToObject(c, "reason", "新手礼包");
        cJSON_AddStringToObject(c, "actor", "parent");
        cJSON_AddItemToArray(coinLog, c);
    }
    cJSON* meta = cJSON_CreateObject();
    cJSON_AddItemToObject(doc, "meta", meta);
    cJSON_AddNumberToObject(meta, "updatedAt", (double)ts);
    return doc;
}

/* 对应 normalizeDoc（就地补齐/夹取）。doc 需为对象。 */
void NormalizeDocInPlace(cJSON* doc, const Config& cfg, int64_t ts) {
    EnsureInt(doc, "v", 0);
    cJSON* p = JGet(doc, "pet");
    if (!p || !cJSON_IsObject(p)) {
        cJSON_DeleteItemFromObject(doc, "pet");
        p = cJSON_CreateObject();
        cJSON_AddItemToObject(doc, "pet", p);
    }
    EnsureStr(p, "name", cfg.pet_name.c_str());
    EnsureInt(p, "energy", 80);
    EnsureInt(p, "hunger", 60);
    EnsureInt(p, "mood", 80);
    EnsureInt(p, "sick", 0);
    EnsureInt(p, "coins", cfg.init_coins);
    EnsureInt(p, "exp", 0);
    EnsureI64(p, "lastDecay", ts);
    EnsureI64(p, "starvingSince", 0);
    EnsureI64(p, "lastFeed", 0);
    EnsureI64(p, "lastActionAt", 0);
    EnsureI64(p, "cmdAck", 0);
    EnsureI64(p, "createdAt", ts);

    SetInt(p, "energy", Clamp100(JInt(p, "energy", 80)));
    SetInt(p, "hunger", Clamp100(JInt(p, "hunger", 60)));
    SetInt(p, "mood", Clamp100(JInt(p, "mood", 80)));
    SetInt(p, "coins", std::max(0, JInt(p, "coins", 0)));
    SetInt(p, "exp", std::max(0, JInt(p, "exp", 0)));
    SetInt(p, "sick", JInt(p, "sick", 0) ? 1 : 0);

    cJSON* c = JGet(p, "counters");
    if (!c || !cJSON_IsObject(c)) {
        cJSON_DeleteItemFromObject(p, "counters");
        c = cJSON_CreateObject();
        cJSON_AddItemToObject(p, "counters", c);
    }
    EnsureI64(c, "day", DayStartSec(ts));

    if (!cJSON_IsArray(JGet(doc, "tasks"))) {
        cJSON_DeleteItemFromObject(doc, "tasks");
        cJSON_AddItemToObject(doc, "tasks", cJSON_CreateArray());
    }
    if (!cJSON_IsArray(JGet(doc, "coinLog"))) {
        cJSON_DeleteItemFromObject(doc, "coinLog");
        cJSON_AddItemToObject(doc, "coinLog", cJSON_CreateArray());
    }
    cJSON* meta = JGet(doc, "meta");
    if (!meta || !cJSON_IsObject(meta)) {
        cJSON_DeleteItemFromObject(doc, "meta");
        meta = cJSON_CreateObject();
        cJSON_AddItemToObject(doc, "meta", meta);
    }
    SetI64(meta, "updatedAt", ts);

    // tasks：剔除无 id 的；coinLog：截到 COIN_LOG_KEEP
    cJSON* tasks = JGet(doc, "tasks");
    for (int i = cJSON_GetArraySize(tasks) - 1; i >= 0; i--) {
        cJSON* t = cJSON_GetArrayItem(tasks, i);
        if (!cJSON_IsObject(t) || !cJSON_IsString(JGet(t, "id"))) cJSON_DeleteItemFromArray(tasks, i);
    }
    cJSON* cl = JGet(doc, "coinLog");
    while (cJSON_GetArraySize(cl) > cfg.coin_log_keep) {
        cJSON_DeleteItemFromArray(cl, cJSON_GetArraySize(cl) - 1);
    }
}

/* ============================ 衰减（对应 applyDecay） ============================ */

int ApplyDecay(cJSON* doc, const Config& cfg, int64_t ts) {
    cJSON* p = JGet(doc, "pet");
    double stepSec = cfg.decay_step_hours * 3600.0;
    int64_t last = JI64(p, "lastDecay", ts);
    int64_t steps = (int64_t)std::floor((double)(ts - last) / stepSec);
    if (steps <= 0) {
        if (!JI64(p, "lastDecay", 0)) SetI64(p, "lastDecay", ts);
        return 0;
    }
    if (steps > cfg.max_decay_steps) steps = cfg.max_decay_steps;

    for (int64_t i = 0; i < steps; i++) {
        int64_t stepTs = last + (int64_t)((double)(i + 1) * stepSec);
        int hunger = std::max(0, JInt(p, "hunger", 0) - 2);
        SetInt(p, "hunger", hunger);
        SetInt(p, "energy", std::max(0, JInt(p, "energy", 0) - 1));
        int drop = (hunger <= 10) ? 4 : 1;
        SetInt(p, "mood", std::max(0, JInt(p, "mood", 0) - drop));
        if (JInt(p, "sick", 0)) SetInt(p, "mood", std::max(0, JInt(p, "mood", 0) - 3));

        if (hunger == 0) {
            int64_t since = JI64(p, "starvingSince", 0);
            if (!since) SetI64(p, "starvingSince", stepTs);
            else if (stepTs - since > cfg.sick_after_starving) SetInt(p, "sick", 1);
        } else {
            SetI64(p, "starvingSince", 0);
        }
    }
    SetI64(p, "lastDecay", last + (int64_t)((double)steps * stepSec));
    return (int)steps;
}

/* ============================ 状态 / 事件 / 心声 ============================ */

int DailyUsed(cJSON* p, const char* key, int64_t ts) {
    cJSON* c = JGet(p, "counters");
    if (!c || JI64(c, "day", 0) != DayStartSec(ts)) return 0;
    return JInt(c, key, 0);
}

int BumpDaily(cJSON* p, const char* key, int64_t ts) {
    int64_t day = DayStartSec(ts);
    cJSON* c = JGet(p, "counters");
    if (!c || JI64(c, "day", 0) != day) {
        cJSON_DeleteItemFromObject(p, "counters");
        c = cJSON_CreateObject();
        cJSON_AddItemToObject(p, "counters", c);
        cJSON_AddNumberToObject(c, "day", (double)day);
    }
    int v = JInt(c, key, 0) + 1;
    SetInt(c, key, v);
    return v;
}

/* 对应 getStatus。返回新建对象。 */
cJSON* GetStatus(cJSON* doc, const Config& cfg, int64_t ts) {
    cJSON* p = JGet(doc, "pet");
    int cap = cfg.daily_fun_cap;
    cJSON* st = cJSON_CreateObject();
    cJSON_AddStringToObject(st, "name", JStr(p, "name", cfg.pet_name.c_str()));
    cJSON_AddNumberToObject(st, "energy", JInt(p, "energy", 0));
    cJSON_AddNumberToObject(st, "hunger", JInt(p, "hunger", 0));
    cJSON_AddNumberToObject(st, "mood", JInt(p, "mood", 0));
    cJSON_AddBoolToObject(st, "sick", JInt(p, "sick", 0) != 0);
    cJSON_AddNumberToObject(st, "coins", JInt(p, "coins", 0));
    cJSON_AddNumberToObject(st, "exp", JInt(p, "exp", 0));
    cJSON_AddNumberToObject(st, "level", JInt(p, "exp", 0) / cfg.exp_per_level + 1);
    cJSON_AddNumberToObject(st, "exp_in_level", JInt(p, "exp", 0) % cfg.exp_per_level);
    cJSON_AddNumberToObject(st, "exp_per_level", cfg.exp_per_level);
    cJSON* fl = cJSON_CreateObject();
    cJSON_AddItemToObject(st, "fun_left", fl);
    cJSON_AddNumberToObject(fl, "story", std::max(0, cap - DailyUsed(p, "story", ts)));
    cJSON_AddNumberToObject(fl, "sing", std::max(0, cap - DailyUsed(p, "sing", ts)));
    cJSON_AddNumberToObject(fl, "teach", std::max(0, cap - DailyUsed(p, "teach", ts)));
    cJSON_AddNumberToObject(fl, "cap", cap);
    return st;
}

/* 对应 moodWord。 */
std::string MoodWordFromStatus(cJSON* st) {
    if (JBool(st, "sick", false)) return "我有点难受……头昏昏的，能不能给我看看医生呀？";
    if (JInt(st, "hunger", 100) <= 15) return "我肚子饿得咕咕叫了，快给我点吃的吧～";
    if (JInt(st, "mood", 100) <= 20) return "有点无聊呢，陪我玩一会儿好不好？";
    if (JInt(st, "mood", 0) >= 80 && JInt(st, "energy", 0) >= 70) return "我今天状态超好！";
    return "感觉还不错～";
}

/* 对应 statusEvents。返回新建数组。 */
cJSON* StatusEventsFromStatus(cJSON* st) {
    cJSON* ev = cJSON_CreateArray();
    bool sick = JBool(st, "sick", false);
    int hunger = JInt(st, "hunger", 100);
    int energy = JInt(st, "energy", 100);
    int mood = JInt(st, "mood", 100);
    if (sick) cJSON_AddItemToArray(ev, cJSON_CreateString("sick"));
    if (hunger <= 15) cJSON_AddItemToArray(ev, cJSON_CreateString("starving"));
    else if (hunger <= 35) cJSON_AddItemToArray(ev, cJSON_CreateString("hungry"));
    if (energy <= 20) cJSON_AddItemToArray(ev, cJSON_CreateString("tired"));
    if (mood <= 20) cJSON_AddItemToArray(ev, cJSON_CreateString("sad"));
    else if (mood <= 40) cJSON_AddItemToArray(ev, cJSON_CreateString("bored"));
    if (cJSON_GetArraySize(ev) == 0 && mood >= 80 && energy >= 70)
        cJSON_AddItemToArray(ev, cJSON_CreateString("great"));
    return ev;
}

/* ============================ 内部工具 ============================ */

void PushCoinLog(cJSON* doc, const Config& cfg, int64_t ts, int delta, const char* reason, const char* actor) {
    cJSON* cl = JGet(doc, "coinLog");
    cJSON* c = cJSON_CreateObject();
    cJSON_AddNumberToObject(c, "ts", (double)ts);
    cJSON_AddNumberToObject(c, "delta", delta);
    cJSON_AddStringToObject(c, "reason", reason);
    cJSON_AddStringToObject(c, "actor", actor);
    cJSON_InsertItemInArray(cl, 0, c);
    while (cJSON_GetArraySize(cl) > cfg.coin_log_keep) {
        cJSON_DeleteItemFromArray(cl, cJSON_GetArraySize(cl) - 1);
    }
}

void AddExp(cJSON* doc, const Config& cfg, int amount) {
    cJSON* p = JGet(doc, "pet");
    SetInt(p, "exp", JInt(p, "exp", 0) + amount);
}

/* 对应 throttle：返回 nullptr 表示放行；否则返回 TOO_FAST 结果。 */
cJSON* Throttle(cJSON* doc, const Config& cfg, int64_t ts) {
    if (cfg.action_cooldown_sec <= 0) return nullptr;
    int64_t last = JI64(JGet(doc, "pet"), "lastActionAt", 0);
    if (ts - last < cfg.action_cooldown_sec) {
        cJSON* r = cJSON_CreateObject();
        cJSON_AddBoolToObject(r, "ok", false);
        cJSON_AddStringToObject(r, "code", "TOO_FAST");
        cJSON_AddStringToObject(r, "say", "慢一点点啦，我还没反应过来～");
        return r;
    }
    return nullptr;
}

void Touch(cJSON* doc, int64_t ts) {
    SetI64(JGet(doc, "pet"), "lastActionAt", ts);
}

/* 结果构造 */
cJSON* Ok(const char* kind) {
    cJSON* r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    if (kind) cJSON_AddStringToObject(r, "kind", kind);
    return r;
}
cJSON* Err(const char* code, const char* say) {
    cJSON* r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", false);
    cJSON_AddStringToObject(r, "code", code);
    cJSON_AddStringToObject(r, "say", say);
    return r;
}

/* ============================ 动作 ============================ */

cJSON* Feed(cJSON* doc, const Config& cfg, int64_t ts, const char* foodRef) {
    ApplyDecay(doc, cfg, ts);
    cJSON* fast = Throttle(doc, cfg, ts);
    if (fast) return fast;

    const Item* food = FindFood(foodRef);
    if (!food) food = FindFood("小饼干");
    int cost = food->cost ? food->cost : cfg.cost_feed;
    cJSON* p = JGet(doc, "pet");
    int hunger = JInt(p, "hunger", 0);

    if (hunger >= cfg.full_hunger) {
        return Err("FULL", "我肚子已经圆滚滚啦，实在吃不下了……等会儿再喂我好不好？");
    }
    int coins = JInt(p, "coins", 0);
    if (coins < cost) {
        char buf[160];
        snprintf(buf, sizeof buf, "我的游戏币不够啦，%s要 %d 枚呢。现在只有 %d 枚。", food->name, cost, coins);
        return Err("NO_COIN", buf);
    }

    SetInt(p, "coins", coins - cost);
    SetInt(p, "hunger", Clamp100(hunger + food->hunger));
    SetInt(p, "mood", Clamp100(JInt(p, "mood", 0) + food->mood));
    SetInt(p, "energy", Clamp100(JInt(p, "energy", 0) + food->energy));
    SetI64(p, "lastFeed", ts);
    AddExp(doc, cfg, cfg.exp_per_action);
    PushCoinLog(doc, cfg, ts, -cost, (std::string("喂食：") + food->name).c_str(), "child");
    Touch(doc, ts);

    char say[220];
    snprintf(say, sizeof say, "%s（饱食 +%d，心情 +%d）", food->line, food->hunger, food->mood);
    cJSON* r = Ok("food");
    cJSON_AddStringToObject(r, "item", food->id);
    cJSON_AddStringToObject(r, "say", say);
    return r;
}

cJSON* GiveMedicine(cJSON* doc, const Config& cfg, int64_t ts, const char* medRef) {
    ApplyDecay(doc, cfg, ts);
    cJSON* fast = Throttle(doc, cfg, ts);
    if (fast) return fast;

    const Item* med = FindMed(medRef);
    cJSON* p = JGet(doc, "pet");
    if (!med) {
        cJSON* r = Err("NO_MED", (std::string("我的药箱里没有这个药呀。现在有：") + MedNames() + "。").c_str());
        cJSON* arr = cJSON_CreateArray();
        cJSON_AddItemToObject(r, "meds", arr);
        for (int i = 0; i < MEDS_N; i++) cJSON_AddItemToArray(arr, cJSON_CreateString(MEDS[i].name));
        return r;
    }
    int cost = med->cost ? med->cost : cfg.cost_medicine;

    if (!JInt(p, "sick", 0)) {
        if (med->tonic) {
            int coins = JInt(p, "coins", 0);
            if (coins < cost) {
                char buf[128];
                snprintf(buf, sizeof buf, "维生素要 %d 枚游戏币，我的币不够啦。", cost);
                return Err("NO_COIN", buf);
            }
            SetInt(p, "coins", coins - cost);
            SetInt(p, "mood", Clamp100(JInt(p, "mood", 0) + med->mood));
            SetInt(p, "energy", Clamp100(JInt(p, "energy", 0) + med->energy));
            AddExp(doc, cfg, cfg.exp_per_action);
            PushCoinLog(doc, cfg, ts, -cost, (std::string("保健：") + med->name).c_str(), "child");
            Touch(doc, ts);
            cJSON* r = Ok("tonic");
            cJSON_AddStringToObject(r, "item", med->id);
            cJSON_AddStringToObject(r, "say", (std::string(med->line) + "（我没生病，这个只是补身体～）").c_str());
            return r;
        }
        return Err("NOT_SICK", "我现在没生病呀，乱吃药肚子会疼的。要是不舒服，先问问我哪里难受～");
    }

    if (!med->cure) {
        return Err("WRONG_MED", (std::string(med->name) + "只能保健，治不了病哦。我这次是真的不舒服，得吃退烧药或者感冒冲剂。").c_str());
    }
    int coins = JInt(p, "coins", 0);
    if (coins < cost) {
        return Err("NO_COIN", (std::string(med->name) + "要 " + std::to_string(cost) + " 枚游戏币，我的币不够啦……").c_str());
    }

    SetInt(p, "coins", coins - cost);
    SetInt(p, "sick", 0);
    SetI64(p, "starvingSince", 0);
    SetInt(p, "mood", Clamp100(JInt(p, "mood", 0) + med->mood));
    SetInt(p, "energy", Clamp100(JInt(p, "energy", 0) + med->energy));
    AddExp(doc, cfg, cfg.exp_per_action * 3);
    PushCoinLog(doc, cfg, ts, -cost, (std::string("服药：") + med->name).c_str(), "child");
    Touch(doc, ts);
    cJSON* r = Ok("cure");
    cJSON_AddStringToObject(r, "item", med->id);
    cJSON_AddStringToObject(r, "say", (std::string(med->line) + " 我的病好啦！").c_str());
    return r;
}

cJSON* Play(cJSON* doc, const Config& cfg, int64_t ts) {
    ApplyDecay(doc, cfg, ts);
    cJSON* fast = Throttle(doc, cfg, ts);
    if (fast) return fast;

    cJSON* p = JGet(doc, "pet");
    if (JInt(p, "sick", 0)) return Err("SICK", "我现在病着呢，玩不动啦……先给我治治病好不好？");
    int coins = JInt(p, "coins", 0);
    if (coins < cfg.cost_play) {
        char buf[128];
        snprintf(buf, sizeof buf, "游戏币不够啦，玩一次要 %d 枚。现在只有 %d 枚。", cfg.cost_play, coins);
        return Err("NO_COIN", buf);
    }
    if (JInt(p, "energy", 0) < 10) return Err("TIRED", "我有点累了，想睡一会儿，明天再玩好不好？");

    SetInt(p, "coins", coins - cfg.cost_play);
    SetInt(p, "mood", Clamp100(JInt(p, "mood", 0) + 18));
    SetInt(p, "energy", std::max(0, JInt(p, "energy", 0) - 8));
    SetInt(p, "hunger", std::max(0, JInt(p, "hunger", 0) - 3));
    AddExp(doc, cfg, cfg.exp_per_action);
    PushCoinLog(doc, cfg, ts, -cfg.cost_play, "互动玩耍", "child");
    Touch(doc, ts);
    cJSON* r = Ok("play");
    cJSON_AddStringToObject(r, "say", "太好玩啦！我最喜欢和你一起玩～");
    return r;
}

cJSON* Heal(cJSON* doc, const Config& cfg, int64_t ts) {
    ApplyDecay(doc, cfg, ts);
    cJSON* fast = Throttle(doc, cfg, ts);
    if (fast) return fast;

    cJSON* p = JGet(doc, "pet");
    if (!JInt(p, "sick", 0)) return Err("HEALTHY", "我现在很健康哦，不需要看病～");
    int coins = JInt(p, "coins", 0);
    if (coins < cfg.cost_heal) {
        std::string s = std::string("治病要 ") + std::to_string(cfg.cost_heal) + " 枚游戏币，我的币不够啦……";
        return Err("NO_COIN", s.c_str());
    }
    SetInt(p, "coins", coins - cfg.cost_heal);
    SetInt(p, "sick", 0);
    SetI64(p, "starvingSince", 0);
    SetInt(p, "mood", Clamp100(JInt(p, "mood", 0) + 20));
    SetInt(p, "energy", Clamp100(JInt(p, "energy", 0) + 10));
    AddExp(doc, cfg, cfg.exp_per_action * 3);
    PushCoinLog(doc, cfg, ts, -cfg.cost_heal, "看病治病", "child");
    Touch(doc, ts);
    cJSON* r = Ok("heal");
    cJSON_AddStringToObject(r, "say", "谢谢医生！我感觉好多啦，又活蹦乱跳了～");
    return r;
}

cJSON* SleepAct(cJSON* doc, const Config& cfg, int64_t ts) {
    ApplyDecay(doc, cfg, ts);
    cJSON* p = JGet(doc, "pet");
    SetInt(p, "energy", Clamp100(JInt(p, "energy", 0) + 20));
    SetInt(p, "hunger", std::max(0, JInt(p, "hunger", 0) - 5));
    Touch(doc, ts);
    cJSON* r = Ok("sleep");
    cJSON_AddStringToObject(r, "say", "那我先睡一会儿啦，呼呼～");
    return r;
}

/* 免费互动：不花币、每日限次。对应 freeAction。 */
cJSON* FreeAction(cJSON* doc, const Config& cfg, int64_t ts,
                  const char* key, const char* label, int mood, int energy, int hunger,
                  int exp, const char* say) {
    cJSON* p = JGet(doc, "pet");
    int used = DailyUsed(p, key, ts);
    int cap = cfg.daily_fun_cap;
    if (used >= cap) {
        char buf[160];
        snprintf(buf, sizeof buf, "今天已经%s %d 次啦，我们先做点别的，或者把今天的事情做完再来～", label, used);
        return Err("DAILY_CAP", buf);
    }
    BumpDaily(p, key, ts);
    SetInt(p, "mood", Clamp100(JInt(p, "mood", 0) + mood));
    SetInt(p, "energy", Clamp100(JInt(p, "energy", 0) + energy));
    SetInt(p, "hunger", Clamp100(JInt(p, "hunger", 0) + hunger));
    AddExp(doc, cfg, exp);
    Touch(doc, ts);
    cJSON* r = Ok(key);
    cJSON_AddStringToObject(r, "say", say);
    cJSON_AddNumberToObject(r, "left_today", cap - used - 1);
    return r;
}

cJSON* TellStory(cJSON* doc, const Config& cfg, int64_t ts, const char* topic) {
    ApplyDecay(doc, cfg, ts);
    cJSON* fast = Throttle(doc, cfg, ts);
    if (fast) return fast;
    std::string t = topic ? Norm(topic) : "";
    const char* themes[] = {"恐龙", "太空", "小兔子", "大海", "小汽车", "公主", "恐龙蛋", "会飞的鲸鱼"};
    int n = (int)(sizeof(themes) / sizeof(themes[0]));
    std::string pick = !t.empty() ? t : themes[Rr(n)];
    std::string say = std::string("好呀，我给你讲一个关于") + pick + "的故事——从前有一只小宠物，它……（接下来你来讲这段" + pick + "的故事）";
    return FreeAction(doc, cfg, ts, "story", "讲故事", 10, -2, 0, cfg.exp_per_action * 2, say.c_str());
}

cJSON* SingSong(cJSON* doc, const Config& cfg, int64_t ts) {
    ApplyDecay(doc, cfg, ts);
    cJSON* fast = Throttle(doc, cfg, ts);
    if (fast) return fast;
    return FreeAction(doc, cfg, ts, "sing", "唱歌", 12, -3, 0, cfg.exp_per_action * 2, "啦啦啦～我唱歌给你听！");
}

cJSON* Teach(cJSON* doc, const Config& cfg, int64_t ts, const char* what) {
    ApplyDecay(doc, cfg, ts);
    cJSON* fast = Throttle(doc, cfg, ts);
    if (fast) return fast;
    std::string w = what ? Norm(what) : "";
    std::string say = w.empty() ? "好呀，我想学新东西！你教我吧～"
                                : ("好呀，我想学" + w + "！你教我吧～");
    return FreeAction(doc, cfg, ts, "teach", "学东西", 8, -2, 0, cfg.exp_per_action * 2, say.c_str());
}

cJSON* PetHead(cJSON* doc, const Config& cfg, int64_t ts) {
    ApplyDecay(doc, cfg, ts);
    cJSON* fast = Throttle(doc, cfg, ts);
    if (fast) return fast;
    cJSON* p = JGet(doc, "pet");
    SetInt(p, "mood", Clamp100(JInt(p, "mood", 0) + 4));
    AddExp(doc, cfg, 1);
    Touch(doc, ts);
    const char* lines[] = {"嘿嘿，好痒～再摸摸", "舒服……我最喜欢被摸头了", "唔…再摸一会儿嘛"};
    cJSON* r = Ok("pet");
    cJSON_AddStringToObject(r, "say", lines[Rr(3)]);
    return r;
}

cJSON* Chat(cJSON* doc, const Config& cfg, int64_t ts, const char* text) {
    ApplyDecay(doc, cfg, ts);
    cJSON* p = JGet(doc, "pet");
    SetInt(p, "mood", Clamp100(JInt(p, "mood", 0) + 1));
    Touch(doc, ts);
    cJSON* r = Ok("chat");
    cJSON_AddStringToObject(r, "say", "");
    std::string e = text ? text : "";
    if (e.size() > 60) e = e.substr(0, 60);
    cJSON_AddStringToObject(r, "echo", e.c_str());
    return r;
}

/* ============================ 任务 ============================ */

cJSON* ListTasks(cJSON* doc, bool includeDone) {
    cJSON* out = cJSON_CreateArray();
    cJSON* tasks = JGet(doc, "tasks");
    std::vector<cJSON*> keep;
    int n = cJSON_GetArraySize(tasks);
    for (int i = 0; i < n; i++) {
        cJSON* t = cJSON_GetArrayItem(tasks, i);
        const char* status = JStr(t, "status", "");
        if (!includeDone && !(strcmp(status, "pending") == 0 || strcmp(status, "submitted") == 0)) continue;
        keep.push_back(t);
    }
    std::sort(keep.begin(), keep.end(), [](cJSON* a, cJSON* b) {
        return JI64(a, "createdAt", 0) > JI64(b, "createdAt", 0);
    });
    if (keep.size() > 20) keep.resize(20);
    for (cJSON* t : keep) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", JStr(t, "id", ""));
        cJSON_AddStringToObject(o, "title", JStr(t, "title", ""));
        cJSON_AddStringToObject(o, "detail", JStr(t, "detail", ""));
        cJSON_AddNumberToObject(o, "reward", JInt(t, "reward", 0));
        cJSON_AddStringToObject(o, "status", JStr(t, "status", ""));
        cJSON_AddItemToArray(out, o);
    }
    return out;
}

cJSON* SubmitTask(cJSON* doc, const Config& cfg, int64_t ts, const char* title, const char* note) {
    std::string q = title ? Norm(title) : "";
    // JS 用原始 title 的 trim 做空判，这里等价
    std::string raw = title ? title : "";
    while (!raw.empty() && (raw.front() == ' ' || raw.front() == '\t')) raw.erase(raw.begin());
    if (raw.empty()) return Err("EMPTY", "你想报备哪个任务呀？告诉我任务名字。");

    int64_t dayStart = DayStartSec(ts);
    cJSON* tasks = JGet(doc, "tasks");
    int todayCount = 0;
    int n = cJSON_GetArraySize(tasks);
    for (int i = 0; i < n; i++) {
        if (JI64(cJSON_GetArrayItem(tasks, i), "submittedAt", 0) >= dayStart) todayCount++;
    }
    if (todayCount >= cfg.daily_task_reward_cap) {
        char buf[128];
        snprintf(buf, sizeof buf, "今天已经报备 %d 个任务啦，先休息一下吧。", todayCount);
        return Err("CAP", buf);
    }

    auto isPending = [](cJSON* t) { return strcmp(JStr(t, "status", ""), "pending") == 0; };
    cJSON* matched = nullptr;
    // 四级匹配，顺序与 JS 一致
    for (int i = 0; i < n && !matched; i++) {
        cJSON* t = cJSON_GetArrayItem(tasks, i);
        if (isPending(t) && Norm(JStr(t, "title", "")) == q) matched = t;
    }
    for (int i = 0; i < n && !matched; i++) {
        cJSON* t = cJSON_GetArrayItem(tasks, i);
        if (isPending(t) && Norm(JStr(t, "title", "")).find(q) != std::string::npos) matched = t;
    }
    for (int i = 0; i < n && !matched; i++) {
        cJSON* t = cJSON_GetArrayItem(tasks, i);
        if (isPending(t) && Norm(JStr(t, "detail", "")).find(q) != std::string::npos) matched = t;
    }
    for (int i = 0; i < n && !matched; i++) {
        cJSON* t = cJSON_GetArrayItem(tasks, i);
        if (Norm(JStr(t, "title", "")) == q) matched = t;
    }
    for (int i = 0; i < n && !matched; i++) {
        cJSON* t = cJSON_GetArrayItem(tasks, i);
        if (Norm(JStr(t, "title", "")).find(q) != std::string::npos) matched = t;
    }

    if (!matched) {
        return Err("NOT_FOUND", (std::string("我没找到「") + raw + "」这个任务哦，你可以问家长今天要做什么。").c_str());
    }
    const char* st = JStr(matched, "status", "");
    if (strcmp(st, "submitted") == 0) {
        return Err("DUPLICATE", (std::string("「") + JStr(matched, "title", "") + "」你已经报备过啦，等家长确认就好。").c_str());
    }
    if (strcmp(st, "done") == 0) {
        return Err("DONE", (std::string("「") + JStr(matched, "title", "") + "」已经完成并且发过币啦，换一个任务试试？").c_str());
    }

    bool needApprove = cfg.task_needs_parent_approve != 0;
    SetStr(matched, "status", needApprove ? "submitted" : "done");
    SetI64(matched, "submittedAt", ts);
    SetStr(matched, "submitNote", note ? note : "");
    if (needApprove) SetNull(matched, "doneAt");
    else SetI64(matched, "doneAt", ts);

    if (!needApprove) {
        cJSON* p = JGet(doc, "pet");
        SetInt(p, "coins", JInt(p, "coins", 0) + JInt(matched, "reward", 0));
        PushCoinLog(doc, cfg, ts, JInt(matched, "reward", 0),
                    (std::string("完成任务：") + JStr(matched, "title", "")).c_str(), "child");
    }
    cJSON* r = Ok("task_submit");
    int reward = JInt(matched, "reward", 0);
    const char* titleS = JStr(matched, "title", "");
    if (needApprove) {
        std::string say = std::string("「") + titleS + "」我记下啦，等家长确认后给你发 " + std::to_string(reward) + " 枚游戏币！";
        cJSON_AddStringToObject(r, "say", say.c_str());
    } else {
        std::string say = std::string("太棒了，「") + titleS + "」完成，奖励你 " + std::to_string(reward) + " 枚游戏币！";
        cJSON_AddStringToObject(r, "say", say.c_str());
    }
    cJSON* task = cJSON_CreateObject();
    cJSON_AddItemToObject(r, "task", task);
    cJSON_AddStringToObject(task, "id", JStr(matched, "id", ""));
    cJSON_AddStringToObject(task, "title", titleS);
    cJSON_AddNumberToObject(task, "reward", reward);
    cJSON_AddStringToObject(task, "status", JStr(matched, "status", ""));
    return r;
}

cJSON* PublishTask(cJSON* doc, const Config& cfg, int64_t ts, const char* title, const char* detail, double reward) {
    std::string raw = title ? title : "";
    while (!raw.empty() && (raw.front() == ' ' || raw.front() == '\t')) raw.erase(raw.begin());
    if (raw.empty()) return Err("EMPTY", "任务名不能为空。");
    int r = (int)std::lround(reward);
    if (r < 1) r = 1;
    if (r > 999) r = 999;

    cJSON* task = cJSON_CreateObject();
    char id[40];
    snprintf(id, sizeof id, "t%lld%03x", (long long)ts, (unsigned)(rand() & 0xfff));
    cJSON_AddStringToObject(task, "id", id);
    cJSON_AddStringToObject(task, "title", raw.c_str());
    std::string d = detail ? detail : "";
    if (d.size() > 200) d = d.substr(0, 200);
    cJSON_AddStringToObject(task, "detail", d.c_str());
    cJSON_AddNumberToObject(task, "reward", r);
    cJSON_AddStringToObject(task, "status", "pending");
    cJSON_AddNumberToObject(task, "createdAt", (double)ts);
    cJSON_AddNullToObject(task, "submittedAt");
    cJSON_AddNullToObject(task, "doneAt");
    cJSON_AddStringToObject(task, "submitNote", "");

    cJSON* tasks = JGet(doc, "tasks");
    cJSON_InsertItemInArray(tasks, 0, task);
    while (cJSON_GetArraySize(tasks) > cfg.task_keep) {
        cJSON_DeleteItemFromArray(tasks, cJSON_GetArraySize(tasks) - 1);
    }
    cJSON* res = Ok("task_publish");
    std::string say = std::string("已发布任务「") + raw + "」，奖励 " + std::to_string(r) + " 枚游戏币。";
    cJSON_AddStringToObject(res, "say", say.c_str());
    cJSON_AddItemToObject(res, "task", cJSON_Duplicate(task, 1));
    return res;
}

cJSON* ApproveTask(cJSON* doc, const Config& cfg, int64_t ts, const char* taskId, const char* title) {
    cJSON* tasks = JGet(doc, "tasks");
    int n = cJSON_GetArraySize(tasks);
    cJSON* t = nullptr;
    if (taskId && *taskId) {
        for (int i = 0; i < n; i++) {
            if (strcmp(JStr(cJSON_GetArrayItem(tasks, i), "id", ""), taskId) == 0) { t = cJSON_GetArrayItem(tasks, i); break; }
        }
    }
    if (!t && title && *title) {
        std::string nt = Norm(title);
        for (int i = 0; i < n; i++) {
            cJSON* x = cJSON_GetArrayItem(tasks, i);
            if (Norm(JStr(x, "title", "")).find(nt) != std::string::npos) { t = x; break; }
        }
    }
    if (!t) return Err("NOT_FOUND", "没找到这个任务。");
    if (strcmp(JStr(t, "status", ""), "done") == 0) return Err("ALREADY", "该任务已经发过币了。");

    SetStr(t, "status", "done");
    SetI64(t, "doneAt", ts);
    cJSON* p = JGet(doc, "pet");
    int coins = JInt(p, "coins", 0) + JInt(t, "reward", 0);
    SetInt(p, "coins", coins);
    PushCoinLog(doc, cfg, ts, JInt(t, "reward", 0), (std::string("完成任务：") + JStr(t, "title", "")).c_str(), "parent");
    cJSON* r = Ok("task_approve");
    cJSON_AddStringToObject(r, "say", (std::string("已确认「") + JStr(t, "title", "") + "」，发放 " + std::to_string(JInt(t, "reward", 0)) + " 枚游戏币。").c_str());
    cJSON_AddNumberToObject(r, "coins", coins);
    return r;
}

cJSON* RejectTask(cJSON* doc, int64_t ts, const char* taskId, const char* title) {
    cJSON* tasks = JGet(doc, "tasks");
    int n = cJSON_GetArraySize(tasks);
    cJSON* t = nullptr;
    if (taskId && *taskId) {
        for (int i = 0; i < n; i++) {
            if (strcmp(JStr(cJSON_GetArrayItem(tasks, i), "id", ""), taskId) == 0) { t = cJSON_GetArrayItem(tasks, i); break; }
        }
    }
    if (!t && title && *title) {
        std::string nt = Norm(title);
        for (int i = 0; i < n; i++) {
            cJSON* x = cJSON_GetArrayItem(tasks, i);
            if (Norm(JStr(x, "title", "")).find(nt) != std::string::npos) { t = x; break; }
        }
    }
    if (!t) return Err("NOT_FOUND", "没找到这个任务。");
    if (strcmp(JStr(t, "status", ""), "submitted") != 0) return Err("BAD_STATE", "这个任务现在不是待确认状态。");
    SetStr(t, "status", "pending");
    SetNull(t, "submittedAt");
    cJSON* r = Ok("task_reject");
    cJSON_AddStringToObject(r, "say", (std::string("「") + JStr(t, "title", "") + "」先退回，让孩子再检查一下。").c_str());
    return r;
}

/* 家长删除任务（误发/作废）。按 id 精确删，找不到再按标题模糊删。 */
cJSON* DeleteTask(cJSON* doc, const char* taskId, const char* title) {
    cJSON* tasks = JGet(doc, "tasks");
    int n = cJSON_GetArraySize(tasks);
    cJSON* t = nullptr;
    int idx = -1;
    if (taskId && *taskId) {
        for (int i = 0; i < n; i++) {
            if (strcmp(JStr(cJSON_GetArrayItem(tasks, i), "id", ""), taskId) == 0) {
                t = cJSON_GetArrayItem(tasks, i);
                idx = i;
                break;
            }
        }
    }
    if (!t && title && *title) {
        std::string nt = Norm(title);
        for (int i = 0; i < n; i++) {
            cJSON* x = cJSON_GetArrayItem(tasks, i);
            if (Norm(JStr(x, "title", "")) == nt) { t = x; idx = i; break; }
        }
        if (!t) {
            for (int i = 0; i < n; i++) {
                cJSON* x = cJSON_GetArrayItem(tasks, i);
                if (Norm(JStr(x, "title", "")).find(nt) != std::string::npos) { t = x; idx = i; break; }
            }
        }
    }
    if (!t || idx < 0) return Err("NOT_FOUND", "没找到这个任务。");

    char id_buf[48] = {0};
    snprintf(id_buf, sizeof id_buf, "%s", JStr(t, "id", ""));
    std::string title_s = JStr(t, "title", "");
    cJSON_DeleteItemFromArray(tasks, idx);

    cJSON* r = Ok("task_delete");
    cJSON_AddStringToObject(r, "say", (std::string("好啦，任务「") + title_s + "」已经删掉了。").c_str());
    cJSON* task = cJSON_CreateObject();
    cJSON_AddItemToObject(r, "task", task);
    cJSON_AddStringToObject(task, "id", id_buf);
    cJSON_AddStringToObject(task, "title", title_s.c_str());
    return r;
}

/* ============================ 状态裁剪（对应 trimState） ============================ */

void TrimState(cJSON* doc, const Config& cfg) {
    cJSON* tasks = JGet(doc, "tasks");
    std::vector<cJSON*> live, done;
    int n = cJSON_GetArraySize(tasks);
    for (int i = 0; i < n; i++) {
        cJSON* t = cJSON_GetArrayItem(tasks, i);
        if (strcmp(JStr(t, "status", ""), "done") == 0) done.push_back(t);
        else live.push_back(t);
    }
    std::sort(done.begin(), done.end(), [](cJSON* a, cJSON* b) {
        return JI64(a, "doneAt", 0) > JI64(b, "doneAt", 0);
    });
    if (done.size() > 2) done.resize(2);
    std::vector<cJSON*> merged = live;
    merged.insert(merged.end(), done.begin(), done.end());
    std::sort(merged.begin(), merged.end(), [](cJSON* a, cJSON* b) {
        return JI64(a, "createdAt", 0) > JI64(b, "createdAt", 0);
    });
    if ((int)merged.size() > cfg.task_keep) merged.resize(cfg.task_keep);

    // 重建 tasks：每条只留必要字段（空字段不落盘）
    cJSON* fresh = cJSON_CreateArray();
    for (cJSON* t : merged) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", JStr(t, "id", ""));
        std::string title = JStr(t, "title", "");
        if (title.size() > 40) title = title.substr(0, 40);
        cJSON_AddStringToObject(o, "title", title.c_str());
        cJSON_AddNumberToObject(o, "reward", JInt(t, "reward", 0));
        cJSON_AddStringToObject(o, "status", JStr(t, "status", ""));
        cJSON_AddNumberToObject(o, "createdAt", JI64(t, "createdAt", 0));
        const char* detail = JStr(t, "detail", "");
        if (detail && *detail) {
            std::string d = detail;
            if (d.size() > 60) d = d.substr(0, 60);
            cJSON_AddStringToObject(o, "detail", d.c_str());
        }
        if (Has(t, "submittedAt") && !cJSON_IsNull(JGet(t, "submittedAt")))
            cJSON_AddNumberToObject(o, "submittedAt", JI64(t, "submittedAt", 0));
        if (Has(t, "doneAt") && !cJSON_IsNull(JGet(t, "doneAt")))
            cJSON_AddNumberToObject(o, "doneAt", JI64(t, "doneAt", 0));
        const char* note = JStr(t, "submitNote", "");
        if (note && *note) {
            std::string s = note;
            if (s.size() > 40) s = s.substr(0, 40);
            cJSON_AddStringToObject(o, "submitNote", s.c_str());
        }
        cJSON_AddItemToArray(fresh, o);
    }
    cJSON_DeleteItemFromObject(doc, "tasks");
    cJSON_AddItemToObject(doc, "tasks", fresh);

    // coinLog 精简
    cJSON* cl = JGet(doc, "coinLog");
    cJSON* clf = cJSON_CreateArray();
    int cn = std::min((int)cJSON_GetArraySize(cl), cfg.coin_log_keep);
    for (int i = 0; i < cn; i++) {
        cJSON* c = cJSON_GetArrayItem(cl, i);
        cJSON* o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "ts", JI64(c, "ts", 0));
        cJSON_AddNumberToObject(o, "delta", JInt(c, "delta", 0));
        std::string reason = JStr(c, "reason", "");
        if (reason.size() > 20) reason = reason.substr(0, 20);
        cJSON_AddStringToObject(o, "reason", reason.c_str());
        cJSON_AddItemToArray(clf, o);
    }
    cJSON_DeleteItemFromObject(doc, "coinLog");
    cJSON_AddItemToObject(doc, "coinLog", clf);
}

/* ============================ 统一入口（对应 run + pack） ============================ */

/* pack：把结果包成统一响应。res 是已构建的结果对象，本函数补 status/state/... 并返回它。 */
cJSON* Pack(cJSON* doc, const Config& cfg, int64_t ts, int64_t serverTs, int64_t skew, cJSON* res) {
    cJSON* meta = JGet(doc, "meta");
    if (!meta) { meta = cJSON_CreateObject(); cJSON_AddItemToObject(doc, "meta", meta); }
    SetI64(meta, "updatedAt", ts);
    if (!JInt(doc, "v", 0)) SetInt(doc, "v", 0);
    SetInt(doc, "v", JInt(doc, "v", 0) + 1);
    TrimState(doc, cfg);

    cJSON* st = GetStatus(doc, cfg, ts);
    cJSON_AddItemToObject(res, "status", st);
    cJSON_AddItemToObject(res, "state", cJSON_Duplicate(doc, 1));
    cJSON_AddNumberToObject(res, "server_ts", (double)serverTs);
    cJSON_AddNumberToObject(res, "skew_sec", (double)skew);
    cJSON_AddItemToObject(res, "events", StatusEventsFromStatus(st));
    std::string mw = MoodWordFromStatus(st);
    cJSON_AddStringToObject(res, "mood_word", mw.c_str());

    cJSON* say = JGet(res, "say");
    if (!cJSON_IsString(say) || !say->valuestring || !*(say->valuestring)) {
        SetStr(res, "say", mw.c_str());
    }
    return res;
}

}  // namespace

/* ============================ 对外实现 ============================ */

Config DefaultConfig() { return Config(); }
void SetConfig(const Config& cfg) { g_cfg = cfg; }
Config GetConfig() { return g_cfg; }

bool Run(const char* state_json, const char* action, const char* args_json,
         int64_t client_ts, std::string& out_response_json,
         std::string& out_state_json, std::string& out_err) {
    const Config cfg = g_cfg;
    cJSON* args = (args_json && *args_json) ? cJSON_Parse(args_json) : nullptr;
    auto AStr = [&](const char* k) -> const char* {
        cJSON* it = args ? cJSON_GetObjectItem(args, k) : nullptr;
        return cJSON_IsString(it) ? it->valuestring : "";
    };
    auto ANum = [&](const char* k, double d) -> double {
        cJSON* it = args ? cJSON_GetObjectItem(args, k) : nullptr;
        return cJSON_IsNumber(it) ? it->valuedouble : d;
    };

    int64_t serverTs = NowSec();
    int64_t clientT = client_ts;
    int64_t skew = clientT > 0 ? serverTs - clientT : 0;
    int64_t ts = (clientT > 0 && std::llabs(skew) <= 300) ? clientT : serverTs;

    cJSON* doc = nullptr;
    bool fresh = !(state_json && *state_json);
    if (!fresh) doc = cJSON_Parse(state_json);
    if (!doc || !cJSON_IsObject(doc)) {
        cJSON_Delete(doc);
        doc = InitDoc(cfg, ts);
        fresh = true;
    } else {
        NormalizeDocInPlace(doc, cfg, ts);
    }

    std::string act = action ? Norm(action) : "sync";
    cJSON* res = nullptr;

    if (act == "ping") {
        cJSON* r = cJSON_CreateObject();
        cJSON_AddBoolToObject(r, "ok", true);
        cJSON_AddStringToObject(r, "code", "PONG");
        cJSON_AddStringToObject(r, "say", "");
        cJSON_AddStringToObject(r, "action", act.c_str());
        cJSON_AddNumberToObject(r, "server_ts", (double)serverTs);
        cJSON_AddBoolToObject(r, "fresh", fresh);
        cJSON* config = cJSON_CreateObject();
        cJSON_AddItemToObject(r, "config", config);
        cJSON_AddNumberToObject(config, "decay_step_hours", cfg.decay_step_hours);
        cJSON_AddNumberToObject(config, "full_hunger", cfg.full_hunger);
        cJSON_AddNumberToObject(config, "daily_fun_cap", cfg.daily_fun_cap);
        cJSON_AddNumberToObject(config, "exp_per_level", cfg.exp_per_level);
        cJSON* catalog = cJSON_CreateObject();
        cJSON_AddItemToObject(r, "catalog", catalog);
        cJSON* foods = cJSON_CreateArray();
        cJSON_AddItemToObject(catalog, "foods", foods);
        for (int i = 0; i < FOODS_N; i++) {
            cJSON* f = cJSON_CreateObject();
            cJSON_AddStringToObject(f, "name", FOODS[i].name);
            cJSON_AddNumberToObject(f, "cost", FOODS[i].cost);
            cJSON_AddItemToArray(foods, f);
        }
        cJSON* meds = cJSON_CreateArray();
        cJSON_AddItemToObject(catalog, "meds", meds);
        for (int i = 0; i < MEDS_N; i++) {
            cJSON* m = cJSON_CreateObject();
            cJSON_AddStringToObject(m, "name", MEDS[i].name);
            cJSON_AddNumberToObject(m, "cost", MEDS[i].cost);
            cJSON_AddBoolToObject(m, "cure", MEDS[i].cure != 0);
            cJSON_AddItemToArray(meds, m);
        }
        // ping 不走 pack，但仍输出规范化 state 供调用方落盘
        {
            char* tmp1 = cJSON_PrintUnformatted(doc);
            if (tmp1) { out_state_json = tmp1; cJSON_free(tmp1); }
            char* tmp2 = cJSON_PrintUnformatted(r);
            if (tmp2) { out_response_json = tmp2; cJSON_free(tmp2); }
        }
        cJSON_Delete(r);
        cJSON_Delete(doc);
        cJSON_Delete(args);
        return true;
    }

    if (act == "reset") {
        cJSON_Delete(doc);
        doc = InitDoc(cfg, ts);
        res = Ok(nullptr);
        cJSON_AddStringToObject(res, "code", "RESET");
        cJSON_AddStringToObject(res, "say", "已经重新开始养啦。");
        cJSON_AddBoolToObject(res, "fresh", true);
    } else {
        int steps = ApplyDecay(doc, cfg, ts);

        if (act == "sync") {
            res = Ok(nullptr);
            cJSON_AddStringToObject(res, "code", "SYNC");
            cJSON_AddStringToObject(res, "say", "");
        } else if (act == "feed") {
            res = Feed(doc, cfg, ts, AStr("food"));
        } else if (act == "medicine") {
            cJSON* m1 = args ? cJSON_GetObjectItem(args, "medicine") : nullptr;
            cJSON* m2 = args ? cJSON_GetObjectItem(args, "med") : nullptr;
            const char* mref = cJSON_IsString(m1) ? m1->valuestring : (cJSON_IsString(m2) ? m2->valuestring : "");
            res = GiveMedicine(doc, cfg, ts, mref);
        } else if (act == "play") {
            res = Play(doc, cfg, ts);
        } else if (act == "heal") {
            res = Heal(doc, cfg, ts);
        } else if (act == "sleep") {
            res = SleepAct(doc, cfg, ts);
        } else if (act == "story") {
            res = TellStory(doc, cfg, ts, AStr("topic"));
        } else if (act == "song") {
            res = SingSong(doc, cfg, ts);
        } else if (act == "teach") {
            res = Teach(doc, cfg, ts, AStr("what"));
        } else if (act == "pet") {
            res = PetHead(doc, cfg, ts);
        } else if (act == "chat") {
            res = Chat(doc, cfg, ts, AStr("text"));
        } else if (act == "task_list") {
            cJSON* it = args ? cJSON_GetObjectItem(args, "include_done") : nullptr;
            bool inc = cJSON_IsBool(it) ? cJSON_IsTrue(it) : (cJSON_IsNumber(it) ? it->valueint != 0 : false);
            res = Ok(nullptr);
            cJSON_AddStringToObject(res, "code", "TASKS");
            cJSON_AddStringToObject(res, "say", "");
            cJSON_AddItemToObject(res, "tasks", ListTasks(doc, inc));
        } else if (act == "task_submit") {
            res = SubmitTask(doc, cfg, ts, AStr("title"), AStr("note"));
        } else if (act == "task_publish") {
            res = PublishTask(doc, cfg, ts, AStr("title"), AStr("detail"), ANum("reward", 10));
        } else if (act == "task_approve") {
            res = ApproveTask(doc, cfg, ts, AStr("task_id"), AStr("title"));
        } else if (act == "task_reject") {
            res = RejectTask(doc, ts, AStr("task_id"), AStr("title"));
        } else if (act == "task_delete") {
            res = DeleteTask(doc, AStr("task_id"), AStr("title"));
        } else if (act == "coins") {
            res = Ok(nullptr);
            cJSON_AddStringToObject(res, "code", "COINS");
            int coins = JInt(JGet(doc, "pet"), "coins", 0);
            cJSON_AddStringToObject(res, "say", (std::string("我现在有 ") + std::to_string(coins) + " 枚游戏币。").c_str());
            cJSON_AddNumberToObject(res, "coins", coins);
        } else {
            res = Err("UNKNOWN_ACTION", (std::string("不认识的指令：") + act).c_str());
        }

        if (steps > 0) cJSON_AddNumberToObject(res, "decay_steps", steps);
    }

    cJSON_AddStringToObject(res, "action", act.c_str());
    cJSON_AddBoolToObject(res, "fresh", fresh);
    Pack(doc, cfg, ts, serverTs, skew, res);

    {
        char* tmp1 = cJSON_PrintUnformatted(res);
        if (tmp1) { out_response_json = tmp1; cJSON_free(tmp1); }
        char* tmp2 = cJSON_PrintUnformatted(doc);
        if (tmp2) { out_state_json = tmp2; cJSON_free(tmp2); }
    }
    cJSON_Delete(res);
    cJSON_Delete(doc);
    cJSON_Delete(args);
    return true;
}

bool GetStatusJson(const char* state_json, int64_t ts, std::string& out_status_json) {
    const Config cfg = g_cfg;
    if (!state_json || !*state_json) return false;
    cJSON* doc = cJSON_Parse(state_json);
    if (!doc || !cJSON_IsObject(doc)) {
        cJSON_Delete(doc);
        return false;
    }
    NormalizeDocInPlace(doc, cfg, ts);
    cJSON* st = GetStatus(doc, cfg, ts);
    {
        char* tmp = cJSON_PrintUnformatted(st);
        if (tmp) { out_status_json = tmp; cJSON_free(tmp); }
    }
    cJSON_Delete(st);
    cJSON_Delete(doc);
    return true;
}

std::string MoodWordOf(const char* state_json, int64_t ts) {
    std::string sj;
    if (!GetStatusJson(state_json, ts, sj)) return "感觉还不错～";
    cJSON* st = cJSON_Parse(sj.c_str());
    std::string s = MoodWordFromStatus(st);
    cJSON_Delete(st);
    return s;
}

}  // namespace pet_engine
