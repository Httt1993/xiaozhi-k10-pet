/**
 * 腾讯云 SCF 入口 · 小智 K10 宠物云端引擎
 *
 * 触发方式有两种，同一个函数都兼容：
 *   1. HTTP（函数 URL / API 网关）—— 设备和小程序都走这里；
 *   2. 定时触发 —— 只做心跳自检，返回存活信息，不做状态演化
 *      （因为状态存在设备 NVS 里，云函数本身无状态，这是刻意的设计）。
 *
 * 环境变量（都可选）：
 *   PET_KEY        共享口令。设置后所有请求必须带 X-Pet-Key 或 ?key=
 *   PET_NAME       宠物名字，默认「乔利」
 *   ...其余数值见 pet-core.js 的 DEFAULTS，同名覆盖
 */

'use strict';

const core = require('./pet-core');

const VERSION = '1.0.0';

const CORS = {
  'Access-Control-Allow-Origin': '*',
  'Access-Control-Allow-Headers': 'Content-Type, X-Pet-Key',
  'Access-Control-Allow-Methods': 'GET, POST, OPTIONS',
};

function json(statusCode, obj) {
  return {
    statusCode,
    headers: Object.assign({ 'Content-Type': 'application/json; charset=utf-8' }, CORS),
    body: JSON.stringify(obj),
    isBase64Encoded: false,
  };
}

function html(statusCode, body) {
  return {
    statusCode,
    headers: Object.assign({ 'Content-Type': 'text/html; charset=utf-8' }, CORS),
    body,
    isBase64Encoded: false,
  };
}

/** 统一取出 method / headers / query / body，兼容 SCF 原生事件和 API 网关事件 */
function parseEvent(event) {
  const e = event || {};

  // 定时触发器
  if (e.Type === 'Timer' || e.triggerName || e.TriggerName) {
    return { timer: true, method: 'TIMER' };
  }

  let headers = e.headers || {};
  // SCF 原生可能把 header 名转成小写，也可能保留原样，两种都兜一下
  const lower = {};
  for (const k of Object.keys(headers)) lower[String(k).toLowerCase()] = headers[k];

  let query = e.queryString || e.queryStringParameters || {};
  let rawBody = e.body;
  if (rawBody && e.isBase64Encoded) {
    rawBody = Buffer.from(rawBody, 'base64').toString('utf8');
  }

  let body = null;
  if (rawBody) {
    if (typeof rawBody === 'string') {
      try {
        body = JSON.parse(rawBody);
      } catch (_) {
        // 允许表单式传参：a=feed&food=苹果
        body = {};
        for (const pair of rawBody.split('&')) {
          const i = pair.indexOf('=');
          if (i > 0) {
            body[decodeURIComponent(pair.slice(0, i))] = decodeURIComponent(pair.slice(i + 1).replace(/\+/g, ' '));
          }
        }
      }
    } else {
      body = rawBody;
    }
  }

  return {
    timer: false,
    method: String(e.httpMethod || e.requestContext?.httpMethod || 'POST').toUpperCase(),
    path: e.path || e.requestContext?.path || '/',
    headers: lower,
    query,
    body,
  };
}

/** 部署自检页：浏览器直接打开函数 URL 就能看到这个 */
function helpPage(endpoint) {
  return `<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>小智宠物云端引擎</title>
<style>
  body{font:15px/1.7 -apple-system,"Segoe UI","Microsoft YaHei",sans-serif;
       max-width:640px;margin:40px auto;padding:0 20px;color:#1b1b1f;background:#faf8f5}
  h1{font-size:22px;margin:0 0 4px}
  .tag{display:inline-block;background:#e8f5e9;color:#1b5e20;border-radius:999px;
       padding:2px 10px;font-size:12px;margin-bottom:18px}
  code{background:#f0ece6;padding:2px 6px;border-radius:5px;font-size:13px}
  pre{background:#2b2b2b;color:#e6e6e6;padding:14px;border-radius:10px;overflow:auto;font-size:12.5px;line-height:1.6}
  .muted{color:#6b6b73;font-size:13px}
  table{border-collapse:collapse;width:100%;font-size:13.5px;margin:10px 0}
  td,th{border-bottom:1px solid #e5e0d8;padding:6px 4px;text-align:left}
  th{color:#6b6b73;font-weight:500}
</style></head><body>
<h1>小智宠物云端引擎</h1>
<div class="tag">运行中 · v${VERSION}</div>
<p>这个地址是小智 K10 宠物的<b>状态演化服务</b>。设备每隔一段时间把宠物状态发上来，
这里按真实时间差算完（饿了多久、该不该生病、金币够不够），再把新状态和一句话回传。</p>
<p class="muted">云函数本身不存数据 —— 状态存在设备的 NVS 里，所以刷新、扩容、冷启动都不会丢进度。</p>

<table>
  <tr><th>项目</th><th>值</th></tr>
  <tr><td>服务地址</td><td><code>${endpoint}</code></td></tr>
  <tr><td>鉴权</td><td>${process.env.PET_KEY ? '已开启（需 X-Pet-Key）' : '未开启'}</td></tr>
  <tr><td>宠物名字</td><td>${(process.env.PET_NAME || core.DEFAULTS.PET_NAME)}</td></tr>
  <tr><td>衰减步长</td><td>每 ${core.DEFAULTS.DECAY_STEP_HOURS} 小时一步</td></tr>
</table>

<p>自己试一下（curl）：</p>
<pre>curl -X POST '${endpoint}' \\
  -H 'Content-Type: application/json' \\
  ${process.env.PET_KEY ? "-H 'X-Pet-Key: 你的口令' \\" : ''}
  -d '{"action":"sync","state":null}'</pre>
<p class="muted">第一次传 <code>state: null</code> 会自动初始化一只新宠物（${core.DEFAULTS.INIT_COINS} 枚新手金币）。</p>
<p class="muted">可用动作：sync / feed / medicine / play / heal / sleep / story / song / teach / pet /
chat / task_list / task_submit / task_publish / task_approve / task_reject / coins / reset / ping</p>
</body></html>`;
}

exports.main_handler = async (event, context) => {
  let req;
  try {
    req = parseEvent(event);
  } catch (err) {
    return json(400, { ok: false, code: 'BAD_EVENT', say: '请求格式看不懂：' + err.message });
  }

  // 定时触发：只做心跳，不演化状态（无状态设计的必然结果，不是偷懒）
  if (req.timer) {
    const r = core.run({ action: 'ping' }, process.env);
    console.log('[pet] 定时心跳', JSON.stringify({ server_ts: r.server_ts, version: VERSION }));
    return { ok: true, msg: 'pet-tick', version: VERSION, server_ts: r.server_ts };
  }

  if (req.method === 'OPTIONS') {
    return { statusCode: 204, headers: CORS, body: '', isBase64Encoded: false };
  }

  // 拿请求里的 URL 拼自检页（函数 URL 场景下 requestContext 里有真实域名）
  const host = req.headers['host'] || req.headers['x-forwarded-host'] || '你的函数地址';
  const proto = req.headers['x-forwarded-proto'] || 'https';
  const endpoint = `${proto}://${host}${req.path}`;

  if (req.method === 'GET') {
    return html(200, helpPage(endpoint));
  }

  // 鉴权
  const needKey = process.env.PET_KEY;
  if (needKey) {
    const got = req.headers['x-pet-key'] || req.query.key || req.query.pet_key;
    if (got !== needKey) {
      console.warn('[pet] 鉴权失败，来源 host=' + host);
      return json(401, { ok: false, code: 'UNAUTHORIZED', say: '口令不对，检查设备的 PET_CLOUD_KEY 是否和云函数一致。' });
    }
  }

  const body = req.body || {};
  const action = body.action || req.query.action || 'sync';

  // 小体量请求，不设流控；这里只挡明显异常的体积，防误传整个固件
  if (JSON.stringify(body).length > 64 * 1024) {
    return json(413, { ok: false, code: 'TOO_LARGE', say: '请求体过大，state 应该在 4KB 以内。' });
  }

  const t0 = Date.now();
  let out;
  try {
    out = core.run(
      {
        action,
        state: body.state === undefined ? null : body.state,
        args: body.args || {},
        ts: body.ts || body.client_ts,
      },
      process.env
    );
  } catch (err) {
    console.error('[pet] 执行异常', action, err && err.stack);
    return json(500, {
      ok: false,
      code: 'INTERNAL',
      say: '云端出了点问题，宠物先在本地待着。',
      error: String(err && err.message),
    });
  }

  const cost = Date.now() - t0;
  console.log(
    '[pet] ' + action +
    ' ok=' + out.ok + ' code=' + (out.code || '-') +
    ' hunger=' + out.status.hunger + ' energy=' + out.status.energy +
    ' mood=' + out.status.mood + ' sick=' + (out.status.sick ? 1 : 0) +
    ' coins=' + out.status.coins +
    ' decay=' + (out.decay_steps || 0) +
    ' state=' + JSON.stringify(out.state).length + 'B' +
    ' cost=' + cost + 'ms'
  );

  return json(200, out);
};
