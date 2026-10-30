#include "pet_http.h"
#include "pet_engine.h"
#include "pet_remind.h"
#include "pet_tools.h"
#include "pet_engage.h"
#include "boot_diag.h"

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
#include <cstdint>
#include <atomic>
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

/* ───────── 状态 ───────── */
PetCloud* g_cloud = nullptr;
PetTools* g_tools = nullptr;
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

/* 循环发送直到全部字节发出（send() 不保证一次发完大 body） */
static bool SendAll(int fd, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int n = send(fd, data + sent, len - sent, 0);
        if (n <= 0) {
            ESP_LOGW(TAG, "SendAll: send() returned %d at %u/%u", n,
                     (unsigned)sent, (unsigned)len);
            return false;
        }
        sent += (size_t)n;
    }
    return true;
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
    SendAll(fd, header, (size_t)hlen);
    SendAll(fd, body.c_str(), body.size());
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
        "task_delete",
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
.stat-row{display:grid;grid-template-columns:repeat(4,1fr);gap:6px}
.stat{background:#faf6f1;border-radius:12px;padding:8px 4px;text-align:center}
.stat .ico{font-size:28px;line-height:1.3}
.stat .label{font-size:11px;color:#a08060;margin-top:2px}
.stat .value{font-size:13px;font-weight:600;color:#806030}
.stat .value.big{font-size:26px;color:#ba7517;margin-top:6px}
.bar{height:8px;background:#eee;border-radius:4px;margin:6px 2px 2px;overflow:hidden}
.bar .fill{height:100%;border-radius:4px;transition:width .5s}
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
.task-item{display:flex;justify-content:space-between;align-items:center;gap:8px;
padding:8px 0;border-bottom:1px solid #f0e8e0;font-size:14px}
.task-item:last-child{border:none}
.task-title{flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.task-right{display:flex;align-items:center;gap:6px;flex:none}
.badge{font-size:11px;padding:2px 8px;border-radius:8px;color:#fff;flex:none}
.badge.pending{background:#e8b04b}.badge.submitted{background:#6bb5e8}
.badge.done{background:#5cb87a}
.task-coins{color:#e89b4b;font-weight:700;font-size:12.5px;flex:none}
.task-ok{border:none;background:#5cb87a;color:#fff;font-size:12px;font-weight:600;
padding:5px 12px;border-radius:8px;cursor:pointer;flex:none}
.task-ok:hover{background:#4a9e63}.task-ok:active{transform:scale(.95)}
.task-back{border:none;background:#e8b04b;color:#fff;font-size:12px;font-weight:600;
padding:5px 12px;border-radius:8px;cursor:pointer;flex:none}
.task-back:hover{background:#d19a38}.task-back:active{transform:scale(.95)}
.task-del{border:none;background:#f0dcc8;color:#b07050;width:26px;height:26px;
border-radius:8px;font-size:13px;cursor:pointer;flex:none}
.task-del:hover{background:#e8c4a8}.task-del:active{background:#d8b090}
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
<p style="color:#a08060;margin-bottom:12px">请输入 PIN 口令（默认 1234）</p>
<input id="pin-input" type="password" maxlength="16" placeholder="PIN">
<button onclick="doLogin()">进入</button>
<p id="pin-err" style="color:#e06060;font-size:13px;min-height:18px;margin-top:8px"></p>
</div>
</div>
<div class="wrap" id="main" style="display:none">
<h1>🐱 AI-Buddy</h1>
<div class="card">
<div class="pet-face" id="face">😺</div>
<div class="stat-row">
<div class="stat"><div class="ico">🍚</div>
<div class="bar"><div class="fill" id="b-hunger" style="background:#e89b4b"></div></div>
<div class="label">饱饱 <span class="value" id="v-hunger">-</span></div></div>
<div class="stat"><div class="ico">💗</div>
<div class="bar"><div class="fill" id="b-mood" style="background:#e88080"></div></div>
<div class="label">开心 <span class="value" id="v-mood">-</span></div></div>
<div class="stat"><div class="ico">🔋</div>
<div class="bar"><div class="fill" id="b-energy" style="background:#6bb5e8"></div></div>
<div class="label">活力 <span class="value" id="v-energy">-</span></div></div>
<div class="stat"><div class="ico">🪙</div>
<div class="value big" id="v-coins">-</div>
<div class="label">金币</div></div>
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
/* PIN 改存 sessionStorage：关掉标签页就要重新输，避免「电脑端不用输 PIN」的观感（B-01） */
let PIN=sessionStorage.getItem('pet_pin')||'';
let seq=parseInt(localStorage.getItem('pet_seq')||'1');
let pollBusy=false, failCount=0;
const $=id=>document.getElementById(id);
function showLogin(msg){$('pin-overlay').style.display='flex';$('main').style.display='none';
if(msg)$('pin-err').textContent=msg}
function showMain(){$('pin-overlay').style.display='none';$('main').style.display='block';
if(!window.__pollTimer){poll();window.__pollTimer=setInterval(poll,3000)}}
async function doLogin(){const v=$('pin-input').value.trim();
if(!v){showLogin('请输入 PIN');return}
try{
const r=await fetch('/api/state',{headers:{'x-pin':v}});
if(r.status===401){showLogin('PIN 错误，请重试');return}
if(!r.ok){showLogin('设备未就绪（'+r.status+'），稍后重试');return}
PIN=v;sessionStorage.setItem('pet_pin',PIN);showMain();
}catch(e){showLogin('连不上设备，检查是否在同一 WiFi')}}
async function api(path,opt){opt=opt||{};
opt.headers=Object.assign({'x-pin':PIN},opt.headers||{});
const r=await fetch(path,opt);
if(r.status===401){sessionStorage.removeItem('pet_pin');PIN='';
showLogin('PIN 已失效，请重新输入');throw new Error('NO_PIN')}
return r.json()}
async function poll(){if(pollBusy)return;pollBusy=true;try{
const d=await api('/api/state');
failCount=0;$('status-bar').textContent='k10-pet.local:8080 · 连接正常';
if(!d.status){pollBusy=false;return}
const s=d.status;
$('v-hunger').textContent=s.hunger;$('b-hunger').style.width=Math.min(100,s.hunger)+'%';
$('v-mood').textContent=s.mood;$('b-mood').style.width=Math.min(100,s.mood)+'%';
$('v-energy').textContent=s.energy;$('b-energy').style.width=Math.min(100,s.energy)+'%';
$('v-coins').textContent=s.coins;
$('face').textContent=s.sick?'🤒':s.hunger<=15?'🥺':s.mood<=20?'😢':s.mood>=80?'😺':'🙂';
$('say').textContent=d.say||'…';
renderTasks(d.tasks||[]);
}catch(e){failCount++;
/* 连续失败才提示，成功一次就恢复 —— 修「一直显示连接失败重试中」（A-08） */
if(failCount>=2)$('status-bar').textContent='⚠️ 连接失败，重试中…（'+failCount+' 次）'}
finally{pollBusy=false}}
let lastTasks=[];
function renderTasks(tasks){const el=$('task-list');lastTasks=tasks;
if(!tasks.length){el.innerHTML='<div style="color:#b09070;text-align:center;padding:12px">暂无任务</div>';return}
el.innerHTML=tasks.map((t,i)=>{const st=t.status||'pending';
const sm=st==='pending'?'待完成':st==='submitted'?'等确认':'已完成';
/* 确认按钮：待完成/等确认都给 —— 孩子做完不用先报备小智，家长看一眼直接点确认发币；
   退回按钮：只在等确认时给（孩子报备了但没做完）；已完成只剩删除 */
const okBtn=(st==='done')?'':'<button class="task-ok" data-ok="'+i+'">✓ 确认</button>';
const backBtn=(st==='submitted')?'<button class="task-back" data-no="'+i+'">退回</button>':'';
return '<div class="task-item"><div class="task-title">'+t.title+'</div>'+
'<div class="task-right"><span class="badge '+st+'">'+sm+'</span>'+
'<span class="task-coins">'+t.reward+'币</span>'+okBtn+backBtn+
'<button class="task-del" data-i="'+i+'" title="删除任务">✕</button></div></div>';}).join('');
el.querySelectorAll('.task-ok').forEach(b=>{b.onclick=()=>{
const t=lastTasks[+b.dataset.ok];if(!t)return;
if(!confirm('确认「'+t.title+'」完成并发 '+t.reward+' 币？'))return;
cmd('task_approve',{task_id:t.id||'',title:t.title});};});
el.querySelectorAll('.task-back').forEach(b=>{b.onclick=()=>{
const t=lastTasks[+b.dataset.no];if(!t)return;
cmd('task_reject',{task_id:t.id||'',title:t.title});};});
el.querySelectorAll('.task-del').forEach(b=>{b.onclick=()=>{
const t=lastTasks[+b.dataset.i];if(!t)return;
if(!confirm('确定删除任务「'+t.title+'」吗？\n删除后不可恢复。'))return;
cmd('task_delete',{task_id:t.id||'',title:t.title});};});}
function deleteTask(i){/* 兜底入口，与按钮等价 */
const t=lastTasks[i];if(!t)return;
if(!confirm('确定删除任务「'+t.title+'」吗？\n删除后不可恢复。'))return;
cmd('task_delete',{task_id:t.id||'',title:t.title})}
async function cmd(action,args){try{
const d=await api('/api/cmd',{method:'POST',
headers:{'Content-Type':'application/json'},
body:JSON.stringify({action,args:args||{},seq:seq++})});
localStorage.setItem('pet_seq',seq);
$('say').textContent=d.say||(d.ok?'完成！':'失败');
if(d.ok===false)$('say').textContent='⚠️ '+(d.say||d.code||'失败');
poll();
}catch(e){$('say').textContent='⚠️ 指令发送失败'}}
function publishTask(){const title=prompt('任务名称：');
if(!title)return;const r=prompt('奖励金币数：','10');
cmd('task_publish',{title,reward:parseInt(r)||10})}
/* 已有 PIN 则先校验再进主界面；过期就弹登录框 */
if(PIN){doLoginWithSaved()}else{showLogin('')}
async function doLoginWithSaved(){try{
const r=await fetch('/api/state',{headers:{'x-pin':PIN}});
if(r.ok){showMain()}else{sessionStorage.removeItem('pet_pin');PIN='';showLogin('')}
}catch(e){showLogin('')}}
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

    /* 静默门状态（《完善设计》SECTION 04）：看板显示「静默/交互中」徽标用 */
    {
        int64_t now_ms = esp_timer_get_time() / 1000;
        cJSON_AddStringToObject(root, "attention",
                                pet_engage::Engaged(now_ms) ? "engaged" : "silent");
    }

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

    const char* action_raw = cJSON_GetObjectItem(root, "action")
                             ? cJSON_GetObjectItem(root, "action")->valuestring : nullptr;
    int64_t seq = cJSON_GetObjectItem(root, "seq")
                      ? (int64_t)cJSON_GetObjectItem(root, "seq")->valuedouble : 0;

    if (!action_raw || !IsAllowedAction(action_raw)) {
        cJSON_Delete(root);
        SendJson(fd, 403, "{\"ok\":false,\"code\":\"ACTION_BLOCKED\"}");
        return;
    }
    /* 复制 action，避免 cJSON_Delete(root) 后悬空指针 */
    std::string action(action_raw);

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
    ESP_LOGI(TAG, "cmd=%s seq=%lld ok=%d code=%s", action.c_str(), (long long)seq,
             result.ok ? 1 : 0, result.code.c_str());
}

/* GET /api/map —— 状态→表情+LED 映射表（看板画脸小样/灯色圆点，保证两端一致） */
void HandleMap(int fd) {
    if (g_tools == nullptr) {
        SendJson(fd, 500, "{\"ok\":false,\"code\":\"NOT_READY\"}");
        return;
    }
    SendJson(fd, 200, g_tools->GetStateMapJson());
}

/* GET /api/remind —— 提醒链状态（级别/免打扰/补播队列） */
void HandleRemind(int fd, const std::string& headers) {
    if (!CheckPinFromHeaders(headers)) {
        SendJson(fd, 401, "{\"ok\":false,\"code\":\"NO_PIN\"}");
        return;
    }
    SendJson(fd, 200, pet_remind::StatusJson(NowSec() * 1000));
}

/* POST /api/remind/cmd —— 提醒指令：cheer / dnd_on / dnd_off / checkin / calm */
void HandleRemindCmd(int fd, const std::string& headers, const std::string& body) {
    if (!CheckPinFromHeaders(headers)) {
        SendJson(fd, 401, "{\"ok\":false,\"code\":\"NO_PIN\"}");
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
    const char* cmd = cJSON_GetObjectItem(root, "cmd")
                          ? cJSON_GetObjectItem(root, "cmd")->valuestring : nullptr;
    const char* arg = cJSON_GetObjectItem(root, "arg")
                          ? cJSON_GetObjectItem(root, "arg")->valuestring : nullptr;
    bool ok = pet_remind::Command(cmd, arg, NowSec() * 1000);
    cJSON_Delete(root);
    SendJson(fd, ok ? 200 : 400,
             ok ? "{\"ok\":true}" : "{\"ok\":false,\"code\":\"BAD_CMD\"}");
}

/* ───────── HTTP 服务主循环 ───────── */

/*
 * 每个连接一个独立小任务：手机/电脑浏览器会同时开 2~3 条连接
 * （HTML + 轮询 + 预连接），单线程 accept 一旦被慢客户端卡住 5 秒，
 * 其他人全部超时 —— 就是「看板过一会儿显示连接失败」「手机端打不开」的根源（A-08/B-01）。
 */
constexpr int kMaxClients = 4;
std::atomic<int> g_active_clients{0};

static void PetHttpClientTask(void* arg) {
    int client_fd = (int)(intptr_t)arg;
    HttpRequest req;
    if (ReadHttpRequest(client_fd, req)) {
        ESP_LOGI(TAG, "%s %s", req.method.c_str(), req.uri.c_str());

        if (req.uri == "/" || req.uri == "/index.html") {
            HandleDashboard(client_fd);
        } else if (req.uri == "/api/info" || req.uri.find("/api/info?") == 0) {
            HandleInfo(client_fd);
        } else if (req.uri == "/api/state" || req.uri.find("/api/state?") == 0) {
            HandleState(client_fd, req.headers);
        } else if (req.uri == "/api/map" || req.uri.find("/api/map?") == 0) {
            HandleMap(client_fd);
        } else if (req.uri == "/api/remind" || req.uri.find("/api/remind?") == 0) {
            HandleRemind(client_fd, req.headers);
        } else if (req.uri == "/api/remind/cmd" && req.method == "POST") {
            HandleRemindCmd(client_fd, req.headers, req.body);
        } else if (req.uri == "/api/cmd" && req.method == "POST") {
            HandleCmd(client_fd, req.headers, req.body);
        } else {
            SendJson(client_fd, 404, "{\"ok\":false,\"code\":\"NOT_FOUND\"}");
        }
    }
    close(client_fd);
    g_active_clients.fetch_sub(1);
    vTaskDelete(nullptr);
}

static void PetHttpTask(void* arg) {
    /* 等 WiFi 拿到 IP（最多 60 秒） */
    esp_netif_ip_info_t ip = {};
    for (int i = 0; i < 300; i++) {
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

    /* 手机浏览器经常解析不了 .local —— 把 IP 地址打出来，手机直接输 IP 也能开（B-01） */
    if (ip.ip.addr != 0) {
        ESP_LOGI(TAG, "PET_PATCH: 看板地址 http://" IPSTR ":%d  (或 http://k10-pet.local:%d)",
                 IP2STR(&ip.ip), kPort, kPort);
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
    if (listen(listen_fd, kMaxClients + 2) < 0) {
        ESP_LOGE(TAG, "listen() failed: errno=%d", errno);
        close(listen_fd);
        vTaskDelete(nullptr);
        return;
    }

    g_listen_fd = listen_fd;
    g_running = true;
    ESP_LOGI(TAG, "HTTP dashboard ready: http://k10-pet.local:%d", kPort);
    BootDiag_SetStage(BOOT_STAGE_HTTP_READY);   /* 诊断打点：本地看板已就绪 */

    /* 接受循环：每连接派发独立任务 */
    while (g_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            if (g_running) vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        /* 设置超时，防止慢速客户端卡住 */
        struct timeval tv = {.tv_sec = 3, .tv_usec = 0};
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        if (g_active_clients.load() >= kMaxClients) {
            /* 过载保护：直接回 503 并关掉，好过把所有人一起拖死 */
            SendJson(client_fd, 503, "{\"ok\":false,\"code\":\"BUSY\"}");
            close(client_fd);
            continue;
        }

        g_active_clients.fetch_add(1);
        BaseType_t ok = xTaskCreate(PetHttpClientTask, "pet_httpc", 6144,
                                    (void*)(intptr_t)client_fd, 4, nullptr);
        if (ok != pdPASS) {
            g_active_clients.fetch_sub(1);
            SendJson(client_fd, 503, "{\"ok\":false,\"code\":\"BUSY\"}");
            close(client_fd);
        }
    }

    close(listen_fd);
    g_listen_fd = -1;
    vTaskDelete(nullptr);
}

}  // namespace

/* ───────── 对外接口 ───────── */

int PetHttpStart(PetCloud* cloud, const char* pin, PetTools* tools) {
    if (g_running) {
        ESP_LOGW(TAG, "HTTP server already running");
        return 0;
    }
    g_cloud = cloud;
    g_tools = tools;
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
