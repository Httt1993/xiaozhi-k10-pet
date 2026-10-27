#ifndef _PET_HTTP_H_
#define _PET_HTTP_H_

#include "pet_cloud.h"

/*
 * 家长看板 HTTP 服务（设备端）
 * =================================================================
 *
 * 起一个 esp_http_server，让手机浏览器直连 K10（mDNS: k10-pet.local），
 * 不经过任何云端——这是「全本地化方案」的看板入口。
 *
 * 路由：
 *   GET  /           看板 HTML（内嵌单文件，无外部依赖）
 *   GET  /api/info   设备信息（免 PIN）
 *   GET  /api/state  宠物状态（PIN）
 *   POST /api/cmd    执行动作（PIN + seq 幂等）
 *
 * 安全：
 *   PIN 走 x-pin 头或 ?pin= 查询参数（与模拟器一致）
 *   家长指令白名单：feed / play / heal / medicine / sleep / story / song /
 *   teach / pet / chat / task_publish / task_approve / task_reject / sync / task_list
 *   屏蔽：coins / reset / task_submit（孩子侧语音操作，不该从看板越权）
 *   seq 幂等：同一 seq 重复提交返回 cached 结果，不重复执行
 *
 * 依赖：esp_http_server（IDF 自带）、mdns（managed component）。
 * 调用方：df_k10_board.cc 在 WiFi 连上后调 PetHttpStart()。
 */

/** 启动 HTTP 看板服务（WiFi 就绪后调一次）。返回 0=成功。 */
int PetHttpStart(PetCloud* cloud, const char* pin);

/** 停止服务（可选，一般不需要）。 */
void PetHttpStop();

#endif  // _PET_HTTP_H_
