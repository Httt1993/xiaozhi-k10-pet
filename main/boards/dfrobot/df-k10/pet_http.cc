#include "pet_http.h"
#include "pet_engine.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_timer.h>
#include <mdns.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

/* 原生 BSD socket（不走 esp_http_server，避开 ctrl socket 资源冲突） */
#include <lwip/sockets.h>
#include <lwip/netdb.h>

#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#define TAG "PetHttp"

namespace {

/* ───────── 配置 ───────── */
constexpr int kPort = 8080;
constexpr int kMaxPinLen = 16;
constexpr int kSeqCacheSize = 32;
constexpr int kMaxRequestBody = 2048;
constexpr int kMaxRequestLine = 512;
constexpr int kBacklog = 4;

/* ───────── 状态 ───────── */
PetCloud* g_cloud = nullptr;
int g_listen_fd = -1;
volatile bool g_running = false;
char g_pin[kMaxPinLen + 1] = {0};

/* seq 幂等缓存 */
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

/* 简易 HTTP 响应发送 */
void SendHttpResponse(int fd, int status, const char* content_type,
                      const std::string& body) {
    const char* status_text =
        status == 200 ? "OK" :
        status == 401 ? "Unauthorized" :
        status == 403 ? "Forbidden" :
        status == 400 ? "Bad Request" :
        status == 404 ? "Not Found" : "Internal Server Error";
    char header[256];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n\r\n",
        status, status_text, content_type, (int)body.size());
    send(fd, header, hlen, 0);
    send(fd, body.c_str(), body.size(), 0);
}

void SendJson(int fd, int status, const std::string& body) {
    SendHttpResponse(fd, status, "application/json; charset=utf-8", body);
}

/* PIN 校验 */
bool CheckPinFromHeaders(const std::string& headers) {
    if (g_pin[0] == '\0') return true;
    /* x-pin header */
    size_t pos = headers.find("x-pin:");
    if (pos == std::string::npos) pos = headers.find("X-Pin:");
    if (pos != std::string::npos) {
        size_t start = headers.find_first_not_of(" \t", pos + 6);
        size_t end = headers.find("\r\n", start);
        if (start != std::string::npos) {
            std::string val = headers.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (val == g_pin) return true;
        }
    }
    /* ?pin= query */
    pos = headers.find("pin=");
    if (pos != std::string::npos) {
        size_t start = pos + 4;
        size_t end = headers.find_first_of(" &\r\n", start);
        std::string val = headers.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (val == g_pin) return true;
    }
    return false;
}

/* 家长指令白名单 */
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
<div class="status-bar" id="status-bar">k10-pet.local:8080 · 本地直连</div>
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

/* ───────── 请求处理 ───────── */

struct HttpRequest {
    std::string method;
    std::string uri;
    std::string headers;
    std::string body;
};

/* 从 socket 读一个完整 HTTP 请求 */
bool ReadHttpRequest(int fd, HttpRequest& req) {
    char buf[2048];
    int total = 0;
    std::string raw;

    /* 读到 \r\n\r\n 为止（headers 结束） */
    while (total < (int)sizeof(buf) - 1) {
        int n = recv(fd, buf + total, sizeof(buf) - 1 - total, 0);
        if (n <= 0) return false;
        total += n;
        buf[total] = '\0';
        if (strstr(buf, "\r\n\r\n")) break;
    }
    raw.assign(buf, total);

    /* 解析请求行: METHOD URI HTTP/1.x */
    size_t line_end = raw.find("\r\n");
    if (line_end == std::string::npos) return false;
    std::string request_line = raw.substr(0, line_end);
    size_t sp1 = request_line.find(' ');
    size_t sp2 = request_line.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) return false;
    req.method = request_line.substr(0, sp1);
    req.uri = request_line.substr(sp1 + 1, sp2 - sp1 - 1);

    /* headers = 从第一行之后到 \r\n\r\n */
    size_t hdr_start = line_end + 2;
    size_t hdr_end = raw.find("\r\n\r\n", hdr_start);
    if (hdr_end == std::string::npos) return false;
    req.headers = raw.substr(hdr_start, hdr_end - hdr_start);

    /* body（Content-Length） */
    size_t body_start = hdr_end + 4;
    int content_length = 0;
    size_t cl_pos = req.headers.find("Content-Length:");
    if (cl_pos == std::string::npos) cl_pos = req.headers.find("content-length:");
    if (cl_pos != std::string::npos) {
        content_length = atoi(req.headers.c_str() + cl_pos + 15);
    }
    if (content_length > 0 && content_length <= kMaxRequestBody) {
        req.body = raw.substr(body_start);
        /* 可能还没读完 body */
        while ((int)req.body.size() < content_length) {
            int n = recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            req.body.append(buf, n);
        }
        if ((int)req.body.size() > content_length) {
            req.body.resize(content_length);
        }
    }
    return true;
}

/* 处理 GET / → 看板 HTML */
void HandleDashboard(int fd) {
    SendHttpResponse(fd, 200, "text/html; charset=utf-8",
                     std::string(kDashboardHtml, sizeof(kDashboardHtml) - 1));
}

/* GET /api/info */
void HandleInfo(int fd) {
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
    SendJson(fd, 200, body);
}

/* GET /api/state */
void HandleState(int fd, const std::string& headers) {
    if (!CheckPinFromHeaders(headers)) {
        SendJson(fd, 401, "{\"ok\":false,\"code\":\"NO_PIN\"}");
        return;
    }
    if (!g_cloud) {
        SendJson(fd, 500, "{\"ok\":false,\"code\":\"NOT_READY\"}");
        return;
    }

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
    SendJson(fd, 200, body);
}

/* POST /api/cmd */
void HandleCmd(int fd, const std::string& headers, const std::string& body) {
    if (!CheckPinFromHeaders(headers)) {
        SendJson(fd, 401, "{\"ok\":false,\"code\":\"NO_PIN\"}");
        return;
    }
    if (!g_cloud) {
        SendJson(fd, 500, "{\"ok\":false,\"code\":\"NOT_READY\"}");
        return;
    }
    if (body.empty() || body.size() > kMaxRequestBody) {
        SendJson(fd, 400, "{\"ok\":false,\"code\":\"BAD_REQUEST\"}");
        return;
    }

    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
        SendJson(fd, 400, "{\"ok\":false,\"code\":\"BAD_JSON\"}");
        return;
    }

    const char* action = cJSON_GetObjectItem(root, "action")
                             ? cJSON_GetObjectItem(root, "action")->valuestring : nullptr;
    int64_t seq = cJSON_GetObjectItem(root, "seq")
                      ? (int64_t)cJSON_GetObjectItem(root, "seq")->valuedouble : 0;

    if (!action || !IsAllowedAction(action)) {
        cJSON_Delete(root);
        SendJson(fd, 403, "{\"ok\":false,\"code\":\"ACTION_BLOCKED\"}");
        return;
    }

    /* seq 幂等 */
    if (seq > 0) {
        for (auto& e : g_seq_cache) {
            if (e.seq == seq) {
                cJSON_Delete(root);
                SendJson(fd, 200, e.response);
                return;
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

    /* 调 pet_engine */
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

    SendJson(fd, 200, response);
    ESP_LOGI(TAG, "cmd=%s seq=%lld ok=%d code=%s", action, (long long)seq,
             result.ok ? 1 : 0, result.code.c_str());
}

/* ───────── HTTP 服务主循环 ───────── */

static void PetHttpTask(void* arg) {
    /* 等 WiFi 拿到 IP（最多 60 秒） */
    for (int i = 0; i < 300; i++) {
        esp_netif_ip_info_t ip;
        esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    /* 额外等 2 秒让 lwIP 完全稳定 */
    vTaskDelay(pdMS_TO_TICKS(2000));

    /* mDNS */
    esp_err_t err = mdns_init();
    if (err == ESP_OK) {
        mdns_hostname_set("k10-pet");
        mdns_instance_name_set("K10 Pet Board");
        mdns_service_add("K10-Pet-HTTP", "_http", "_tcp", kPort, nullptr, 0);
        ESP_LOGI(TAG, "mDNS: k10-pet.local");
    } else {
        ESP_LOGW(TAG, "mdns_init failed: %s", esp_err_to_name(err));
    }

    /* 创建监听 socket */
    int listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_fd < 0) {
        ESP_LOGE(TAG, "socket() failed: errno=%d", errno);
        vTaskDelete(nullptr);
        return;
    }
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(kPort);

    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind(%d) failed: errno=%d", kPort, errno);
        close(listen_fd);
        vTaskDelete(nullptr);
        return;
    }
    if (listen(listen_fd, kBacklog) < 0) {
        ESP_LOGE(TAG, "listen() failed: errno=%d", errno);
        close(listen_fd);
        vTaskDelete(nullptr);
        return;
    }

    g_listen_fd = listen_fd;
    g_running = true;
    ESP_LOGI(TAG, "HTTP dashboard ready: http://k10-pet.local:%d", kPort);

    /* 接受循环 */
    while (g_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            if (g_running) vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        /* 设置超时，防止慢速客户端卡住 */
        struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        HttpRequest req;
        if (ReadHttpRequest(client_fd, req)) {
            ESP_LOGI(TAG, "%s %s", req.method.c_str(), req.uri.c_str());

            if (req.uri == "/" || req.uri == "/index.html") {
                HandleDashboard(client_fd);
            } else if (req.uri == "/api/info" || req.uri.find("/api/info?") == 0) {
                HandleInfo(client_fd);
            } else if (req.uri == "/api/state" || req.uri.find("/api/state?") == 0) {
                HandleState(client_fd, req.headers);
            } else if (req.uri == "/api/cmd" && req.method == "POST") {
                HandleCmd(client_fd, req.headers, req.body);
            } else {
                SendJson(client_fd, 404, "{\"ok\":false,\"code\":\"NOT_FOUND\"}");
            }
        }
        close(client_fd);
    }

    close(listen_fd);
    g_listen_fd = -1;
    vTaskDelete(nullptr);
}

}  // namespace

/* ───────── 对外接口 ───────── */

int PetHttpStart(PetCloud* cloud, const char* pin) {
    if (g_running) {
        ESP_LOGW(TAG, "HTTP server already running");
        return 0;
    }
    g_cloud = cloud;
    if (pin && pin[0]) {
        strncpy(g_pin, pin, kMaxPinLen);
        g_pin[kMaxPinLen] = '\0';
    }

    ESP_LOGI(TAG, "HTTP server deferred, waiting for WiFi...");
    BaseType_t ok = xTaskCreate(PetHttpTask, "pet_http", 8192, cloud, 5, nullptr);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create PetHttpTask");
        return -1;
    }
    return 0;
}

void PetHttpStop() {
    g_running = false;
    if (g_listen_fd >= 0) {
        close(g_listen_fd);
        g_listen_fd = -1;
    }
    ESP_LOGI(TAG, "HTTP server stopped");
}
