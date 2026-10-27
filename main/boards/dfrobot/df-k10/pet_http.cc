#include "pet_http.h"
#include "pet_engine.h"

#include <cJSON.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_timer.h>
#include <mdns.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#define TAG "PetHttp"

namespace {

/* ───────── 配置 ───────── */
constexpr int kPort = 8080;  /* 80 被小智 OTA/WebSocket 占了（EADDRINUSE），改 8080 */
constexpr int kMaxPinLen = 16;
constexpr int kSeqCacheSize = 32;
constexpr int kMaxRequestBody = 1024;

/* ───────── 状态 ───────── */
PetCloud* g_cloud = nullptr;
httpd_handle_t g_server = nullptr;
char g_pin[kMaxPinLen + 1] = {0};

/* seq 幂等缓存：{seq → response} */
struct SeqEntry {
    int64_t seq = -1;
    std::string response;
};
SeqEntry g_seq_cache[kSeqCacheSize];
int g_seq_idx = 0;

/* ───────── 工具 ───────── */

int64_t NowSec() {
    time_t t = time(nullptr);
    return (t > 1600000000) ? static_cast<int64_t>(t) : 0;
}

/** 从请求里取 PIN：优先 x-pin 头，其次 ?pin= */
bool CheckPin(httpd_req_t* req) {
    if (g_pin[0] == '\0') {
        return true;  /* 未设 PIN = 免认证 */
    }
    char buf[kMaxPinLen + 1] = {0};
    if (httpd_req_get_hdr_value_str(req, "x-pin", buf, sizeof(buf)) == ESP_OK) {
        return strcmp(buf, g_pin) == 0;
    }
    /* 查询参数 */
    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen > 0 && qlen < 128) {
        char q[128] = {0};
        if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
            char pin_q[kMaxPinLen + 1] = {0};
            if (httpd_query_key_value(q, "pin", pin_q, sizeof(pin_q)) == ESP_OK) {
                return strcmp(pin_q, g_pin) == 0;
            }
        }
    }
    return false;
}

/** 发 JSON 响应 */
void SendJson(httpd_req_t* req, int status, const std::string& body) {
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_status(req, status == 200 ? "200 OK" :
                                 status == 401 ? "401 Unauthorized" :
                                 status == 403 ? "403 Forbidden" :
                                 status == 400 ? "400 Bad Request" : "500 Internal Server Error");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, body.c_str(), body.size());
}

/** 家长指令白名单 */
bool IsAllowedAction(const char* action) {
    static const char* kAllowed[] = {
        "sync", "feed", "play", "heal", "medicine", "sleep",
        "story", "song", "teach", "pet", "chat",
        "task_list", "task_publish", "task_approve", "task_reject",
    };
    for (const char* a : kAllowed) {
        if (strcmp(action, a) == 0) return true;
    }
    return false;
}

/* ───────── 看板 HTML（内嵌单文件） ───────── */
const char kDashboardHtml[] = R"rawliteral(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>AI-Buddy 家长看板</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:-apple-system,BlinkMacSystemFont,"Microsoft YaHei",sans-serif;
background:#f5f0eb;color:#3d2e1e;min-height:100vh}
.wrap{max-width:420px;margin:0 auto;padding:16px}
h1{text-align:center;font-size:20px;padding:16px 0 8px}
.card{background:#fff;border-radius:16px;padding:16px;margin-bottom:12px;
box-shadow:0 2px 8px rgba(0,0,0,.08)}
.pet-face{text-align:center;font-size:64px;padding:8px 0}
.stat-grid{display:grid;grid-template-columns:1fr 1fr;gap:8px}
.stat{background:#faf6f1;border-radius:10px;padding:10px;text-align:center}
.stat .label{font-size:12px;color:#a08060}
.stat .value{font-size:22px;font-weight:700;margin-top:2px}
.bar{height:6px;background:#eee;border-radius:3px;margin-top:4px;overflow:hidden}
.bar .fill{height:100%;border-radius:3px;transition:width .5s}
.say-box{background:#fff8e8;border-radius:10px;padding:10px;font-size:14px;
margin-top:10px;min-height:36px;color:#806030}
.btn-row{display:grid;grid-template-columns:1fr 1fr 1fr;gap:8px}
.btn{border:none;border-radius:10px;padding:12px 8px;font-size:14px;
cursor:pointer;color:#fff;font-weight:600;transition:transform .1s}
.btn:active{transform:scale(.95)}
.btn.feed{background:#e89b4b}.btn.play{background:#6bb5e8}
.btn.sleep{background:#8e7cc3}.btn.heal{background:#e06060}
.btn.story{background:#e8b04b}.btn.pet{background:#e88080}
.btn.task{background:#5cb87a}.btn.wide{grid-column:span 2}
.task-item{display:flex;justify-content:space-between;align-items:center;
padding:8px 0;border-bottom:1px solid #f0e8e0;font-size:14px}
.task-item:last-child{border:none}
.badge{font-size:11px;padding:2px 8px;border-radius:8px;color:#fff}
.badge.pending{background:#e8b04b}.badge.submitted{background:#6bb5e8}
.badge.done{background:#5cb87a}
#pin-overlay{position:fixed;inset:0;background:rgba(245,240,235,.97);
display:flex;align-items:center;justify-content:center;z-index:10}
#pin-box{text-align:center}
#pin-box input{font-size:24px;padding:10px;border:2px solid #e89b4b;
border-radius:12px;text-align:center;width:160px;letter-spacing:8px}
#pin-box button{display:block;margin:12px auto;padding:10px 32px;
background:#e89b4b;color:#fff;border:none;border-radius:12px;font-size:16px}
.status-bar{text-align:center;font-size:11px;color:#b09070;padding:8px}
</style>
</head>
<body>
<div id="pin-overlay">
<div id="pin-box">
<div style="font-size:48px">🐱</div>
<h2 style="margin:8px 0">AI-Buddy 家长看板</h2>
<p style="color:#a08060;margin-bottom:12px">请输入 PIN 口令</p>
<input id="pin-input" type="password" maxlength="16" placeholder="PIN">
<button onclick="doLogin()">进入</button>
</div>
</div>
<div class="wrap" id="main" style="display:none">
<h1>🐱 AI-Buddy</h1>
<div class="card">
<div class="pet-face" id="face">😺</div>
<div class="stat-grid">
<div class="stat"><div class="label">饱食</div><div class="value" id="v-hunger">-</div>
<div class="bar"><div class="fill" id="b-hunger" style="background:#e89b4b"></div></div></div>
<div class="stat"><div class="label">心情</div><div class="value" id="v-mood">-</div>
<div class="bar"><div class="fill" id="b-mood" style="background:#e88080"></div></div></div>
<div class="stat"><div class="label">精力</div><div class="value" id="v-energy">-</div>
<div class="bar"><div class="fill" id="b-energy" style="background:#6bb5e8"></div></div></div>
<div class="stat"><div class="label">金币</div><div class="value" id="v-coins">-</div></div>
</div>
<div class="say-box" id="say">加载中…</div>
</div>
<div class="card">
<div class="btn-row">
<button class="btn feed" onclick="cmd('feed',{food:'apple'})">🍎 喂食</button>
<button class="btn play" onclick="cmd('play')">🎮 玩耍</button>
<button class="btn sleep" onclick="cmd('sleep')">😴 睡觉</button>
<button class="btn heal" onclick="cmd('heal')">💊 治病</button>
<button class="btn story" onclick="cmd('story')">📖 讲故事</button>
<button class="btn pet" onclick="cmd('pet')">🤚 摸摸头</button>
</div>
</div>
<div class="card">
<div style="display:flex;justify-content:space-between;align-items:center;margin-bottom:8px">
<b>📋 任务列表</b>
<button class="btn task" style="padding:6px 14px;font-size:12px" onclick="publishTask()">+ 发任务</button>
</div>
<div id="task-list"><div style="color:#b09070;text-align:center;padding:12px">暂无任务</div></div>
</div>
<div class="status-bar" id="status-bar">k10-pet.local · 本地直连</div>
</div>
<script>
let PIN=localStorage.getItem('pet_pin')||'';
let seq=parseInt(localStorage.getItem('pet_seq')||'1');
const $=id=>document.getElementById(id);
function doLogin(){PIN=$('pin-input').value.trim();
if(PIN){localStorage.setItem('pet_pin',PIN);$('pin-overlay').style.display='none';
$('main').style.display='block';poll();setInterval(poll,3000);}
}
if(PIN){$('pin-overlay').style.display='none';$('main').style.display='block';
poll();setInterval(poll,3000);}
async function api(path,opt){opt=opt||{};
opt.headers=Object.assign({'x-pin':PIN},opt.headers||{});
const r=await fetch(path,opt);return r.json();}
async function poll(){try{
const d=await api('/api/state');if(!d.status)return;
const s=d.status;
$('v-hunger').textContent=s.hunger;$('b-hunger').style.width=Math.min(100,s.hunger)+'%';
$('v-mood').textContent=s.mood;$('b-mood').style.width=Math.min(100,s.mood)+'%';
$('v-energy').textContent=s.energy;$('b-energy').style.width=Math.min(100,s.energy)+'%';
$('v-coins').textContent=s.coins;
$('face').textContent=s.sick?'🤒':s.hunger<=15?'🥺':s.mood<=20?'😢':s.mood>=80?'😺':'🙂';
$('say').textContent=d.say||'…';
renderTasks(d.tasks||[]);
}catch(e){$('status-bar').textContent='⚠️ 连接失败，重试中…'}}
function renderTasks(tasks){const el=$('task-list');
if(!tasks.length){el.innerHTML='<div style="color:#b09070;text-align:center;padding:12px">暂无任务</div>';return}
el.innerHTML=tasks.map(t=>`<div class="task-item"><span>${t.title}</span>
<div><span class="badge ${t.status}">${t.status==='pending'?'待完成':t.status==='submitted'?'等确认':'已完成'}</span>
<span style="margin-left:6px;color:#e89b4b;font-weight:700">${t.reward}币</span></div></div>`).join('')}
async function cmd(action,args){try{
const d=await api('/api/cmd',{method:'POST',
headers:{'Content-Type':'application/json'},
body:JSON.stringify({action,args:args||{},seq:seq++})});
localStorage.setItem('pet_seq',seq);
$('say').textContent=d.say||(d.ok?'完成！':'失败');poll();
}catch(e){$('say').textContent='⚠️ 指令发送失败'}}
function publishTask(){const title=prompt('任务名称：');
if(!title)return;const r=prompt('奖励金币数：','10');
cmd('task_publish',{title,reward:parseInt(r)||10})}
</script>
</body>
</html>)rawliteral";

/* ───────── 路由处理 ───────── */

/* GET / → 看板 HTML */
esp_err_t HandleDashboard(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, kDashboardHtml, sizeof(kDashboardHtml) - 1);
    return ESP_OK;
}

/* GET /api/info → 设备信息（免 PIN） */
esp_err_t HandleInfo(httpd_req_t* req) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "device", "K10 Pet");
    cJSON_AddStringToObject(root, "model", "df-k10");
    cJSON_AddStringToObject(root, "engine", "pet_engine (local)");
    cJSON_AddNumberToObject(root, "uptime_sec", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddStringToObject(root, "pin_required", g_pin[0] ? "true" : "false");
    char* text = cJSON_PrintUnformatted(root);
    std::string body = text ? text : "{}";
    if (text) cJSON_free(text);
    cJSON_Delete(root);
    SendJson(req, 200, body);
    return ESP_OK;
}

/* GET /api/state → 宠物状态（PIN） */
esp_err_t HandleState(httpd_req_t* req) {
    if (!CheckPin(req)) {
        SendJson(req, 401, "{\"ok\":false,\"code\":\"NO_PIN\"}");
        return ESP_OK;
    }
    if (!g_cloud) {
        SendJson(req, 500, "{\"ok\":false,\"code\":\"NOT_READY\"}");
        return ESP_OK;
    }

    /* 走 pet_engine sync：衰减 + 状态快照，不改变任何东西 */
    PetStatus st = g_cloud->GetStatus();

    cJSON* root = cJSON_CreateObject();
    cJSON* status = cJSON_AddObjectToObject(root, "status");
    cJSON_AddStringToObject(status, "name", st.name.c_str());
    cJSON_AddNumberToObject(status, "energy", st.energy);
    cJSON_AddNumberToObject(status, "hunger", st.hunger);
    cJSON_AddNumberToObject(status, "mood", st.mood);
    cJSON_AddNumberToObject(status, "coins", st.coins);
    cJSON_AddNumberToObject(status, "exp", st.exp);
    cJSON_AddNumberToObject(status, "level", st.level);
    cJSON_AddBoolToObject(status, "sick", st.sick);

    cJSON* events = cJSON_AddArrayToObject(root, "events");
    for (const auto& e : st.events) {
        cJSON_AddItemToArray(events, cJSON_CreateString(e.c_str()));
    }

    cJSON_AddStringToObject(root, "say", st.mood_word.c_str());
    cJSON_AddNumberToObject(root, "server_ts", (double)NowSec());

    /* 任务清单 */
    {
        std::string tasks_json = g_cloud->GetTasksJson(true);
        cJSON* tasks = cJSON_Parse(tasks_json.c_str());
        if (tasks && cJSON_IsArray(tasks)) {
            cJSON_AddItemToObject(root, "tasks", tasks);
        } else {
            if (tasks) cJSON_Delete(tasks);
            cJSON_AddArrayToObject(root, "tasks");
        }
    }

    char* text = cJSON_PrintUnformatted(root);
    std::string body = text ? text : "{}";
    if (text) cJSON_free(text);
    cJSON_Delete(root);
    SendJson(req, 200, body);
    return ESP_OK;
}

/* POST /api/cmd → 执行动作（PIN + seq 幂等） */
esp_err_t HandleCmd(httpd_req_t* req) {
    if (!CheckPin(req)) {
        SendJson(req, 401, "{\"ok\":false,\"code\":\"NO_PIN\"}");
        return ESP_OK;
    }
    if (!g_cloud) {
        SendJson(req, 500, "{\"ok\":false,\"code\":\"NOT_READY\"}");
        return ESP_OK;
    }

    /* 读 body */
    if (req->content_len <= 0 || req->content_len > kMaxRequestBody) {
        SendJson(req, 400, "{\"ok\":false,\"code\":\"BAD_REQUEST\"}");
        return ESP_OK;
    }
    std::string body(req->content_len, 0);
    int received = httpd_req_recv(req, &body[0], req->content_len);
    if (received <= 0) {
        SendJson(req, 400, "{\"ok\":false,\"code\":\"RECV_FAIL\"}");
        return ESP_OK;
    }
    body.resize(received);

    /* 解析 JSON */
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
        SendJson(req, 400, "{\"ok\":false,\"code\":\"BAD_JSON\"}");
        return ESP_OK;
    }

    const char* action = cJSON_GetObjectItem(root, "action")
                             ? cJSON_GetObjectItem(root, "action")->valuestring : nullptr;
    int64_t seq = cJSON_GetObjectItem(root, "seq")
                      ? (int64_t)cJSON_GetObjectItem(root, "seq")->valuedouble : 0;

    if (!action || !IsAllowedAction(action)) {
        cJSON_Delete(root);
        SendJson(req, 403, "{\"ok\":false,\"code\":\"ACTION_BLOCKED\"}");
        return ESP_OK;
    }

    /* seq 幂等：命中缓存直接返回上次结果 */
    if (seq > 0) {
        for (auto& e : g_seq_cache) {
            if (e.seq == seq) {
                cJSON_Delete(root);
                SendJson(req, 200, e.response);
                return ESP_OK;
            }
        }
    }

    /* args */
    std::string args_json = "{}";
    cJSON* args = cJSON_GetObjectItem(root, "args");
    if (cJSON_IsObject(args)) {
        char* at = cJSON_PrintUnformatted(args);
        if (at) {
            args_json = at;
            cJSON_free(at);
        }
    }
    cJSON_Delete(root);

    /* 调 pet_cloud → pet_engine */
    PetActionResult result = g_cloud->Act(action, args_json);

    /* 构建响应 */
    cJSON* resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", result.ok);
    cJSON_AddStringToObject(resp, "code", result.code.c_str());
    cJSON_AddStringToObject(resp, "say", result.say.c_str());
    cJSON_AddNumberToObject(resp, "coins", result.coins);
    cJSON* events = cJSON_AddArrayToObject(resp, "events");
    for (const auto& e : result.events) {
        cJSON_AddItemToArray(events, cJSON_CreateString(e.c_str()));
    }
    cJSON_AddStringToObject(resp, "from", "pet_engine");

    char* text = cJSON_PrintUnformatted(resp);
    std::string response = text ? text : "{}";
    if (text) cJSON_free(text);
    cJSON_Delete(resp);

    /* 存 seq 缓存 */
    if (seq > 0) {
        g_seq_cache[g_seq_idx].seq = seq;
        g_seq_cache[g_seq_idx].response = response;
        g_seq_idx = (g_seq_idx + 1) % kSeqCacheSize;
    }

    SendJson(req, 200, response);
    ESP_LOGI(TAG, "cmd=%s seq=%lld ok=%d code=%s", action, (long long)seq,
             result.ok ? 1 : 0, result.code.c_str());
    return ESP_OK;
}

}  // namespace

/* ───────── 对外接口 ───────── */

/* 延迟启动任务：等 TCP/IP 栈就绪后再起 mDNS + httpd。
 * 直接在 InitializeIot() 里起会因 lwIP 尚未启动而 assert 崩溃
 * （tcpip_send_msg_wait_sem Invalid mbox）。 */
static void PetHttpTask(void* arg) {
    /* 等待 WiFi/TCP-IP 就绪：轮询 netif 是否有 IP，最多等 60 秒 */
    for (int i = 0; i < 300; i++) {
        esp_netif_ip_info_t ip;
        esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    /* 额外再等 500ms 让 lwIP 完全稳定 */
    vTaskDelay(pdMS_TO_TICKS(500));

    PetCloud* cloud = (PetCloud*)arg;

    /* 启动 mDNS */
    esp_err_t err = mdns_init();
    if (err == ESP_OK) {
        mdns_hostname_set("k10-pet");
        mdns_instance_name_set("K10 Pet Board");
        mdns_service_add("K10-Pet-HTTP", "_http", "_tcp", kPort, nullptr, 0);
        ESP_LOGI(TAG, "mDNS: k10-pet.local");
    } else {
        ESP_LOGW(TAG, "mdns_init failed: %s (HTTP 仍会启动，只是没有 .local 域名)", esp_err_to_name(err));
    }

    /* 启动 HTTP server */
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = kPort;
    config.max_uri_handlers = 8;
    config.stack_size = 8192;
    config.lru_purge_enable = true;

    err = httpd_start(&g_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        g_server = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    /* 注册路由 */
    const httpd_uri_t uri_dashboard = {
        .uri = "/", .method = HTTP_GET, .handler = HandleDashboard,
        .user_ctx = nullptr};
    const httpd_uri_t uri_info = {
        .uri = "/api/info", .method = HTTP_GET, .handler = HandleInfo,
        .user_ctx = nullptr};
    const httpd_uri_t uri_state = {
        .uri = "/api/state", .method = HTTP_GET, .handler = HandleState,
        .user_ctx = nullptr};
    const httpd_uri_t uri_cmd = {
        .uri = "/api/cmd", .method = HTTP_POST, .handler = HandleCmd,
        .user_ctx = nullptr};

    httpd_register_uri_handler(g_server, &uri_dashboard);
    httpd_register_uri_handler(g_server, &uri_info);
    httpd_register_uri_handler(g_server, &uri_state);
    httpd_register_uri_handler(g_server, &uri_cmd);

    ESP_LOGI(TAG, "HTTP dashboard ready: http://k10-pet.local (port %d)", kPort);
    vTaskDelete(nullptr);  /* 任务完成，自杀 */
}

int PetHttpStart(PetCloud* cloud, const char* pin) {
    if (g_server) {
        ESP_LOGW(TAG, "HTTP server already running");
        return 0;
    }
    g_cloud = cloud;
    if (pin && pin[0]) {
        strncpy(g_pin, pin, kMaxPinLen);
        g_pin[kMaxPinLen] = '\0';
    }

    /* 不能在这里直接 mdns_init + httpd_start，因为 WiFi/TCP-IP 可能还没起。
     * 起一个后台任务等网络就绪后再启动。 */
    ESP_LOGI(TAG, "HTTP server deferred, waiting for WiFi...");
    BaseType_t ok = xTaskCreate(PetHttpTask, "pet_http", 8192, cloud, 5, nullptr);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create PetHttpTask");
        return -1;
    }
    return 0;
}

void PetHttpStop() {
    if (g_server) {
        httpd_stop(g_server);
        g_server = nullptr;
        ESP_LOGI(TAG, "HTTP server stopped");
    }
}
