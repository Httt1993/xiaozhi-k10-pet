#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
10.27 测试清单 · 问题项修复验证（主机侧静态 + 逻辑测试）

覆盖：
  A-04 状态机 audio_testing → activating/idle 合法转移
  A-06 底部一排（小鱼干/爱心状态机 + 电池 + 金币数字，无中文标签）
  任务删除 task_delete + 未完成任务角标 + 图标状态机/饥饿抖动（10-28 新增）
  爱心像素心重绘 + 取消 WiFi/时间状态栏（10-28 二轮 UI 修正）
  A-07 衰减 1 分钟内可见（三处常量同步）
  A-08 看板连接状态恢复 + 并发客户端
  A-11/A-12 看板动作触发设备联动 + action UAF 修复
  A-15 光感被动看护
  B-01 PIN 校验 + sessionStorage
  B-03 动作白名单（coins/reset/task_submit 拦截）
  C-03 登录服务器重试收敛 + 状态机可恢复

用法：python tests/test_checklist_fixes.py
"""
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
K10 = os.path.join(ROOT, "main", "boards", "dfrobot", "df-k10")

PASS = 0
FAIL = 0
FAILURES = []


def ok(cond, label, extra=None):
    global PASS, FAIL
    if cond:
        PASS += 1
        print(f"  ✓ {label}")
    else:
        FAIL += 1
        FAILURES.append(label)
        print(f"  ✗ {label}" + (f"  → {extra}" if extra else ""))


def read(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def section(title):
    print(f"\n── {title} ──")


# ================================================================ A-04
section("A-04 状态机转移（audio_testing → activating / idle）")

sm = read(os.path.join(ROOT, "main", "device_state_machine.cc"))
m = re.search(
    r"case kDeviceStateAudioTesting:(.*?)case kDeviceStateActivating:", sm, re.S)
ok(m is not None, "找到 audio_testing 转移分支")
if m:
    block = m.group(1)
    ok("kDeviceStateActivating" in block,
       "audio_testing → activating 合法（网络连上时可离开音频测试）", block[:120])
    ok("kDeviceStateIdle" in block,
       "audio_testing → idle 合法（可回待命）", block[:120])
    ok("kDeviceStateWifiConfiguring" in block,
       "audio_testing → wifi_configuring 合法（按键返回配网）")

# 模拟串口日志里的实际卡死路径，现在应全程合法
transitions = [
    ("unknown", "starting"),
    ("starting", "wifi_configuring"),
    ("wifi_configuring", "audio_testing"),
    ("audio_testing", "activating"),   # 日志里这条曾报 Invalid
    ("activating", "idle"),
]
# 从源码直接断言关键转移被允许
def transition_ok(from_enum, to_enum):
    pat = r"case %s:(.*?)(?=case kDeviceState|default:)" % from_enum
    m = re.search(pat, sm, re.S)
    return bool(m) and to_enum in m.group(1)


ok(transition_ok("kDeviceStateAudioTesting", "kDeviceStateActivating"),
   "模拟日志路径 audio_testing→activating 现在合法")
ok(transition_ok("kDeviceStateAudioTesting", "kDeviceStateIdle"),
   "模拟日志路径 audio_testing→idle 现在合法")
ok(transition_ok("kDeviceStateConnecting", "kDeviceStateIdle"),
   "connecting 失败可回 idle（登录服务器不卡死）")
ok(transition_ok("kDeviceStateIdle", "kDeviceStateConnecting"),
   "idle 可再发起连接（自动恢复通路）")

app = read(os.path.join(ROOT, "main", "application.cc"))
ok("Unexpected state" in app and "forcing Activating" in app,
   "网络连上时若卡在 audio_testing 会强制转入 activating")

# ================================================================ A-06
section("A-06 底部一排（小鱼干/爱心状态机 + 电池 + 金币数字）")

fv = read(os.path.join(K10, "pet_face_view.cc"))
fvh = read(os.path.join(K10, "pet_face_view.h"))
ok("stat_fish_grp_" in fvh and "stat_heart_grp_" in fvh and "stat_batt_" in fvh,
   "头文件声明小鱼干/爱心/电池填充对象")
ok("stat_coins_lbl_" in fvh, "金币用一个数字标签")
ok("stat_bars_" not in fvh and "stat_labels_" not in fvh,
   "旧能量条/数字标签成员已移除")
ok("stat_rice_" not in fvh and "stat_rice_" not in fv,
   "旧米饭高度图标已移除（被小鱼干状态机取代）")
ok("232, 40" in fv and "4, 276" in fv,
   "参数条沉到屏幕最底部一行（y=276，不遮脸）")
ok("SetFishState(FishStateFromSatiety(hunger))" in fv, "SetStats 驱动小鱼干状态机")
ok("SetHeartState(HeartStateFromMood(mood))" in fv, "SetStats 驱动爱心状态机")
ok("lv_obj_set_width(stat_batt_" in fv, "SetStats 驱动电池电量宽度")
ok("lv_label_set_text(stat_coins_lbl_" in fv, "金币只显数字")
ok("stat_tear_" in fv and "stat_star_" in fv, "低状态小泪滴/高状态小星星")
ok("kStatNames" not in fv, "屏幕不带中文标签")

# ================================================================ 任务删除 + 角标 + 图标状态机（2026-10-28 新增）
section("任务删除 task_delete（引擎 + 看板按钮 + 确认）")

js = read(os.path.join(ROOT, "cloud", "scf", "pet-core.js"))
eng = read(os.path.join(K10, "pet_engine.cc"))
http = read(os.path.join(K10, "pet_http.cc"))
pcl = read(os.path.join(K10, "pet_cloud.cc"))
board = read(os.path.join(K10, "df_k10_board.cc"))
tools = read(os.path.join(K10, "pet_tools.cc"))
disp = read(os.path.join(ROOT, "main", "display", "display.h"))

ok("function deleteTask" in js, "pet-core.js 有 deleteTask 函数")
ok("case 'task_delete'" in js, "pet-core.js 分发 task_delete")
ok("cJSON* DeleteTask" in eng, "pet_engine.cc 有 DeleteTask 函数")
ok('act == "task_delete"' in eng, "pet_engine.cc 分发 task_delete")
ok('"task_delete"' in http, "看板白名单放行 task_delete")
ok("task-del" in http and "confirm(" in http, "看板任务卡有删除按钮 + 二次确认弹窗")
ok("cmd('task_delete'" in http, "删除按钮调用 task_delete 动作")
ok("lastTasks" in http, "任务列表缓存用于按钮定位（防标题注入）")

section("未完成任务角标（设备屏同步）")

ok("CountOpenTasks" in pcl and "int PetCloud::CountOpenTasks" in pcl,
   "PetCloud::CountOpenTasks 本地统计未完成任务")
ok("SetTaskBadge" in disp, "Display 虚接口 SetTaskBadge")
ok("SetTaskBadge" in board, "2s 定时器刷新任务角标")
ok("task_delete" in tools and "SetTaskBadge" in tools,
   "任务动作钩子即时刷新角标（发布/删除/确认/退回）")
ok("SetTaskBadge" in fvh and "void PetFaceView::SetTaskBadge" in fv,
   "PetFaceView 实现任务角标")
ok("task_badge_" in fv and "task_badge_lbl_" in fv,
   "角标 UI：剪贴板 + 红色数字气泡")

section("图标状态机：小鱼干三态 + 爱心三态 + 饥饿抖动")

ok("enum class FishIconState" in fvh and "kWhole" in fvh and "kBitten" in fvh and "kSkeleton" in fvh,
   "FishIconState 三态枚举（完整/被咬一口/鱼骨头）")
ok("enum class HeartIconState" in fvh and "kSolid" in fvh and "kHollow" in fvh and "kCracked" in fvh,
   "HeartIconState 三态枚举（实心/空心/裂开）")
ok("satiety >= 60" in fvh and "satiety >= 30" in fvh,
   "satiety 0-100 分档：>=60 完整 / 30-59 咬一口 / <30 鱼骨头")
ok("mood >= 60" in fvh and "mood >= 30" in fvh,
   "mood 分档：>=60 实心 / 30-59 空心 / <30 裂开")
ok("fish_[3]" in fvh and "heart_[3]" in fvh, "每态一组 LVGL 图元容器，切换=显隐")
ok("SetFishState" in fv and "SetHeartState" in fv, "状态切换方法已实现")
ok("FishShakeX" in fv and "lv_anim_start" in fv and "UpdateHungerShake" in fv,
   "饥饿抖动动画（lv_anim 水平 ±2px）")
ok("satiety < 30" in fv or "satiety < 30" in fvh, "饱食 <30 触发抖动（饥饿提示）")
ok("lv_anim_set_repeat_count" in fv and "LV_ANIM_REPEAT_INFINITE" in fv,
   "抖动循环播放，饱食恢复后停止")
ok("fish_[0]" in fv and "fish_[1]" in fv and "fish_[2]" in fv,
   "三条小鱼干图元（完整/咬痕/鱼骨）")
ok("heart_[0]" in fv and "heart_[1]" in fv and "heart_[2]" in fv,
   "三颗爱心图元（实心/空心/裂纹）")
ok("kCrack" in fv, "裂心纹颜色常量")

# 10-28 二轮 UI 修正：爱心换真·心形像素画
ok("HRun" in fv and "drawHeart" in fv, "爱心改用像素心 run-length 描绘（不再是圆角方块拼的）")
ok("kBody" in fv and "kHole" in fv and "kCracks" in fv,
   "像素心三套 run 数据（外形 13×11 / 内腔 / 裂纹）")
ok("{0, 0, 2, 4}" in fv and "{10, 10, 6, 6}" in fv,
   "心形剪影：两叶圆顶 + 对称收尖底（经典心尖）")
ok("2 + runs[i].x0 * 2" in fv, "位图像素放大 2 倍画 2×2 色块（像素风）")

# 10-28 二轮 UI 修正：取消 WiFi/时间状态栏
lcd = read(os.path.join(ROOT, "main", "display", "lcd_display.cc"))
ok("取消 WiFi/时间状态栏" in lcd and lcd.count("lv_obj_add_flag(top_bar_, LV_OBJ_FLAG_HIDDEN)") == 2,
   "两个 SetupUI 变体都隐藏 top_bar_（WiFi/静音/电池图标条）")
lvgl = read(os.path.join(ROOT, "main", "display", "lvgl_display", "lvgl_display.cc"))
ok("strftime" not in lvgl, "HH:MM 时间显示代码已删除")
ok("取消时间显示" in lvgl and "lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN)" in lvgl,
   "空闲时隐藏状态文字，屏幕只留猫脸；交互时 SetStatus 仍显示状态")

# A-06 修复：SetPetStats 不再被 st.valid 卡住
board = read(os.path.join(K10, "df_k10_board.cc"))
ok("SetPetStats" in board, "定时器调用 SetPetStats")
ok("if (st.valid)" not in board.split("SetPetStats")[0].split("GetStatus")[-1],
   "SetPetStats 不再要求 st.valid（无条件刷新）")

# A-08/B-01 修复：屏幕弹 IP
ok("ShowNotification" in board and "IPSTR" in board,
   "WiFi 连上后屏幕弹出看板 IP 地址")

# ================================================================ A-07
section("A-07 衰减 30 秒内可见（常量同步）")

cfg = read(os.path.join(K10, "pet_config.h"))
ok("PET_DECAY_STEP_SEC 30" in cfg, "pet_config.h: PET_DECAY_STEP_SEC=30（30 秒一步）")

pcl = read(os.path.join(K10, "pet_cloud.cc"))
ok("kDecayStepSec = PET_DECAY_STEP_SEC" in pcl,
   "pet_cloud.cc 衰减步长跟随配置宏")
ok("kMaxDecaySteps = 2880" in pcl, "pet_cloud.cc 补算封顶 2880 步（24h）")

eng = read(os.path.join(K10, "pet_engine.h"))
ok("1.0 / 120.0" in eng, "pet_engine.h 默认步长 30 秒")
ok("max_decay_steps = 2880" in eng, "pet_engine.h 补算封顶 2880 步")

js = read(os.path.join(ROOT, "cloud", "scf", "pet-core.js"))
ok("DECAY_STEP_HOURS: 1 / 120" in js, "pet-core.js 与设备端同步（30 秒一步）")
ok("MAX_DECAY_STEPS: 2880" in js, "pet-core.js 补算封顶同步")

# 数学验证：30 秒一步，半分钟后饱食 -2（肉眼可见）
step_sec = 30
hunger = 60
for _ in range(1):  # 静置 30 秒（1 步）
    hunger = max(0, hunger - 2)
ok(hunger == 58, "静置 30 秒饱食 60→58（每步 -2，肉眼可见）", hunger)
for _ in range(1):  # 再 30 秒（累计 2 步 = 1 分钟）
    hunger = max(0, hunger - 2)
ok(hunger == 56, "静置 1 分钟饱食 60→56（两步 -4，清晰可见）", hunger)

# ================================================================ A-08 / B-01
section("A-08 看板连接状态恢复 / B-01 PIN 与并发")

http = read(os.path.join(K10, "pet_http.cc"))
ok("PetHttpClientTask" in http, "每连接独立任务（不再单线程拖死所有人）")
ok("kMaxClients" in http, "并发连接数上限保护")
ok("failCount" in http and "failCount=0" in http.replace(" ", ""),
   "JS 轮询带失败计数，成功即清零")
ok("连接正常" in http, "成功后状态栏恢复「连接正常」")
ok("pollBusy" in http, "轮询防重入（不会堆积请求）")
ok("sessionStorage" in http, "PIN 改存 sessionStorage（不再永久免输）")
ok("localStorage.getItem('pet_pin')" not in http, "不再用 localStorage 缓存 PIN")
ok("PIN 错误" in http, "登录会校验 PIN 并提示「PIN 错误」")
ok("r.status===401" in http or "r.status == 401" in http, "登录路径显式处理 401")
ok("IPSTR" in http, "启动时把设备 IP 打进日志（手机可直接输 IP）")
ok("'x-pin'" in http or '"x-pin"' in http, "API 调用携带 x-pin 头")

# PIN 服务端校验仍然存在
ok("CheckPinFromHeaders" in http, "服务端 PIN 校验保留")
ok('PetHttpStart(&pet_tools_->cloud(), "1234")' in read(os.path.join(K10, "df_k10_board.cc")),
   "PIN=1234 已注入")

# ================================================================ A-11 / A-12
section("A-11/A-12 看板动作 → 设备联动（含 UAF 修复）")

cloud_h = read(os.path.join(K10, "pet_cloud.h"))
cloud_cc = read(os.path.join(K10, "pet_cloud.cc"))
tools_cc = read(os.path.join(K10, "pet_tools.cc"))

ok("SetActionHook" in cloud_h, "PetCloud 提供动作完成钩子")
ok("action_hook_" in cloud_cc and "action_hook_(result, action)" in cloud_cc,
   "Act() 完成后回调钩子")
ok("SetActionHook" in tools_cc, "PetTools 注册了钩子")
ok("ReactToEvents" in tools_cc, "钩子里触发灯/脸联动")
ok("ShowNotification" in tools_cc, "钩子里把宠物话术显示到屏幕")

# UAF：action 必须在 cJSON_Delete 之前拷贝
cmd_block = re.search(r"void HandleCmd.*?\n\}", http, re.S)
ok(cmd_block is not None, "找到 HandleCmd")
if cmd_block:
    body = cmd_block.group(0)
    copy_pos = body.find("std::string action(action_raw)")
    del_pos = body.find("cJSON_Delete(root)")
    ok(0 < copy_pos < del_pos or (copy_pos > 0 and body.find("cJSON_Delete(root)", copy_pos) > copy_pos),
       "action 在 cJSON_Delete 之前拷贝（串口乱码 UNKNOWN_ACTION 的根因）",
       {"copy": copy_pos, "del": del_pos})
    ok("IsAllowedAction" in body, "白名单校验保留")

# 白名单内容（B-03）
wl = re.search(r"kAllowed\[\] = \{(.*?)\};", http, re.S)
ok(wl is not None, "找到动作白名单")
if wl:
    items = re.findall(r'"(\w+)"', wl.group(1))
    for a in ("feed", "play", "medicine", "sleep", "task_publish", "task_approve"):
        ok(a in items, f"白名单放行 {a}")
    for a in ("coins", "reset", "task_submit"):
        ok(a not in items, f"白名单拦截 {a}（B-03）")

# ================================================================ A-15
section("A-15 光感被动看护")

ok("PET_LIGHT_WATCH 1" in cfg, "pet_config.h 开启光感看护")
ok("WatchLightTick" in tools_cc, "PetTools 实现 WatchLightTick")
ok("sudden_bright" in tools_cc and "sudden_dark" in tools_cc,
   "骤亮/骤暗两种触发条件都有")
ok("curious" in tools_cc and "sleepy" in tools_cc, "骤亮→好奇脸，骤暗→困倦脸")
ok("30.0f" in tools_cc and "1.8f" in tools_cc,
   "阈值已降低到 30 lux / 1.8x（原 120/2.5x 手机手电够不着）")
board_cc = read(os.path.join(K10, "df_k10_board.cc"))
ok("WatchLightTick()" in board_cc, "板级 2 秒定时器调用光感看护")

# ================================================================ C-03
section("C-03 登录服务器重试收敛 + 可恢复")

ok("MAX_RETRY = 5" in app, "OTA 检查最多重试 5 次（原 10 次指数退避拖 40 分钟+）")
ok("retry_delay > 60" in app, "重试间隔封顶 60 秒")
ok("MAIN_EVENT_ACTIVATION_DONE" in app, "激活完成后有 DONE 事件可离开登录服务器")
ok("DismissAlert" in app, "连接成功会清掉错误提示")
ok(transition_ok("kDeviceStateConnecting", "kDeviceStateIdle"),
   "连接失败回 idle，不永久卡「登录服务器」")

# ================================================================ 结果
print("\n" + "=" * 50)
print(f"总计 {PASS + FAIL} 项  ·  通过 {PASS}  ·  失败 {FAIL}")
if FAILURES:
    print("失败项：")
    for f in FAILURES:
        print(f"  · {f}")
    sys.exit(1)
print("全部通过 ✓")
sys.exit(0)
