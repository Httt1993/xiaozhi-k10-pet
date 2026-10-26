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
const store = require('./store');
const parent = require('./parent');

const VERSION = '1.1.0';

/**
 * 家长看板可以触发的动作。
 * 刻意不含 coins（自己给自己发币）、reset（误点清档）、
 * task_submit（那是孩子该做的事，家长代做就失去意义了）。
 */
const DASHBOARD_ACTIONS = new Set([
  'feed', 'play', 'medicine', 'heal', 'sleep', 'pet',
  'task_publish', 'task_approve', 'task_reject',
]);

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

<p style="margin:18px 0 6px"><a href="?page=parent"
  style="display:inline-block;background:#e8853a;color:#fff;text-decoration:none;
  padding:11px 20px;border-radius:11px;font-weight:600">打开家长看板 →</a></p>
<p class="muted">看板能看宠物此刻饿不饿、点按钮喂饭、布置和确认任务。
点下去的指令会排队，等设备下次联网时送达。</p>

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

/** 把看板指令翻成家长看得懂的一句话（存进事件流，也给提示语用） */
function describeCmd(cmd, args) {
  const a = args || {};
  switch (cmd) {
    case 'feed':
      return '家长喂了' + (a.food ? '「' + a.food + '」' : '一顿饭');
    case 'play':
      return '家长陪它玩了一会儿';
    case 'medicine':
      return '家长给了一次药' + (a.med ? '（' + a.med + '）' : '');
    case 'heal':
      return '家长带它去看病了';
    case 'sleep':
      return '家长哄它睡觉';
    case 'pet':
      return '家长摸了摸它的头';
    case 'task_publish':
      return '家长布置了任务「' + String(a.title || '').trim() + '」，奖励 ' + (a.reward || 0) + ' 币';
    case 'task_approve':
      return '家长确认「' + (a._title || '任务') + '」完成，发币';
    case 'task_reject':
      return '家长退回了「' + (a._title || '任务') + '」，还要再检查一下';
    default:
      return '家长执行了 ' + cmd;
  }
}

/**
 * 把家长看板排队的指令，按顺序压到设备刚上报的 state 上。
 *
 * 为什么在「进核心前」做而不是让核心自己查队列：
 *   pet-core 是纯函数（输入 state 出 state），不碰 IO，
 *   所以队列这一层必须留在 index.js。这样核心还能单测。
 *
 * 为什么还要 cmdAck：
 *   云函数可能同时活着好几个实例，队列只在其中一个实例的内存里，
 *   设备心跳落到别的实例就取不到 —— 指令会静默消失。
 *   所以看板会「没看到回执就重发」，同一条指令可能落到多个实例。
 *   state.pet.cmdAck 记的是「已处理到哪一号」，它存在设备 NVS 里、
 *   每次上报都带回来，所以是全网唯一可靠的去重依据：
 *   凡是 at <= cmdAck 的一律跳过，重复提交不会重复生效。
 */
function drainPending(env, state, ts) {
  const queue = store.takePending(env);
  if (!queue.length) return { state, applied: [] };

  let cur = state;
  const applied = [];
  let ack = Number(cur && cur.pet && cur.pet.cmdAck) || 0;

  /* 按时间排序：家长连点两下时，先点的先执行 */
  const ordered = queue.slice().sort((a, b) => (Number(a.at) || 0) - (Number(b.at) || 0));

  for (const cmd of ordered) {
    const action = String(cmd.action || '');
    const at = Number(cmd.at) || 0;

    if (at > 0 && at <= ack) {
      applied.push({ action, ok: true, skipped: true, at, note: cmd.note || '',
                     say: '这条已经执行过了（重复提交已跳过）' });
      continue;
    }

    if (!DASHBOARD_ACTIONS.has(action)) {
      applied.push({ action, ok: false, at, say: '该动作不允许从看板触发' });
      if (at > ack) ack = at;
      continue;
    }

    try {
      const r = core.run({ action, state: cur, args: cmd.args || {}, ts }, env);
      if (r.ok && r.state) {
        cur = r.state;
        applied.push({ action, ok: true, at, say: r.say || '', note: cmd.note || '' });
      } else {
        applied.push({ action, ok: false, at, say: r.say || r.code || '执行失败', note: cmd.note || '' });
      }
    } catch (err) {
      console.error('[pet] 队列指令执行异常', action, err && err.stack);
      applied.push({ action, ok: false, at, say: '执行异常：' + (err && err.message) });
    }

    /* 失败也要记 ack：否则看板会以为是没送到，一直重发同一条。
       失败原因（比如金币不够）重发一百次也一样失败。 */
    if (at > ack) {
      ack = at;
      if (cur && cur.pet) cur.pet.cmdAck = ack;
    }
  }

  if (cur && cur.pet && (Number(cur.pet.cmdAck) || 0) < ack) cur.pet.cmdAck = ack;
  return { state: cur, applied };
}

/** 家长看板要的那一份 JSON（已经算好 level、任务、待送达） */
function parentSnapshot(env) {
  const st = store.load(env);
  const now = core.nowSec();
  const cfg = core.resolveConfig(env);
  const out = {
    ok: true,
    backend: store.backend(env),
    server_ts: now,
    age_sec: st.updatedAt ? Math.max(0, now - st.updatedAt) : null,
    source: st.source || '',
    state_present: !!st.state,
    status: null,
    tasks: [],
    events: st.events,
    pending: st.pending,
    /* 设备已经把指令处理到哪一号（毫秒）。看板拿它和本地指令的 at 比，
       判断「已送达」还是「还在路上、该重发」。 */
    ack: 0,
    has_key: !!process.env.PET_KEY,
    pet_name: (process.env.PET_NAME || core.DEFAULTS.PET_NAME),
  };
  if (!st.state) return out;

  try {
    const doc = core.normalizeDoc(st.state, cfg);
    out.status = core.getStatus(doc, cfg, now);
    out.ack = Number(doc.pet && doc.pet.cmdAck) || 0;
    out.tasks = (doc.tasks || [])
      .filter((t) => t && t.id && t.status !== 'done')
      .sort((a, b) => (b.createdAt || 0) - (a.createdAt || 0))
      .slice(0, 12)
      .map((t) => ({
        id: t.id,
        title: t.title,
        reward: t.reward,
        status: t.status,
        submittedAt: t.submittedAt || 0,
      }));
  } catch (err) {
    console.warn('[pet] 快照解析失败：' + (err && err.message));
  }
  return out;
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

  /* 家长看板识别：?page=parent 或路径末尾 /parent 都认 */
  const isParent = String(req.query.page || '') === 'parent' || /\/parent\/?$/.test(req.path || '');
  const isParentApi = isParent && String(req.query.api || '') === '1';

  /* 页面壳子不含任何宠物数据，先给出去，免得没口令时看到一片白 */
  if (isParent && !isParentApi && req.method === 'GET') {
    return html(200, parent.renderParentPage(process.env.PET_NAME || core.DEFAULTS.PET_NAME));
  }

  if (req.method === 'GET' && !isParent) {
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

  /* ---------------- 家长看板接口 ---------------- */
  if (isParentApi) {
    if (req.method === 'GET') {
      return json(200, parentSnapshot(process.env));
    }

    const body = req.body || {};
    const cmd = String(body.cmd || body.action || '');
    if (!DASHBOARD_ACTIONS.has(cmd)) {
      return json(400, { ok: false, code: 'BAD_CMD', say: '看板不支持这个操作：' + (cmd || '(空)') });
    }
    const args = body.args && typeof body.args === 'object' ? body.args : {};
    if (cmd === 'task_publish' && !String(args.title || '').trim()) {
      return json(400, { ok: false, code: 'NO_TITLE', say: '任务内容不能为空。' });
    }

    /* 确认/退回时把任务名带上，事件流里写「家长确认「收拾书包」完成」比「确认任务」有用得多 */
    if ((cmd === 'task_approve' || cmd === 'task_reject') && args.task_id) {
      const snap = parentSnapshot(process.env);
      const hit = (snap.tasks || []).find((t) => t.id === args.task_id);
      if (hit) args._title = hit.title;
    }

    const note = describeCmd(cmd, args);
    /* 看板重发时会带上原来的 at（毫秒），云端据此去重；
       首次提交没带，就用此刻。
       注意：body.at 只用于「同一条指令的标识」，不参与任何数值计算，
       所以伪造它最多让自己那条被当成旧指令跳过，改不了宠物状态。 */
    const clientAt = Number(body.at) > 0 ? Math.round(Number(body.at)) : 0;
    const at = clientAt || Date.now();
    const n = store.pushPending(process.env, cmd, args, note, at);
    /* 这里刻意不写事件流：排队中的指令由看板的「等待送达」区展示，
       事件流只记真实发生过的事，否则一条指令会留下「已排队」+「已送达」两条噪音。 */
    console.log('[pet] 家长看板排队 ' + cmd + ' at=' + at + ' 队列长度=' + n);
    return json(200, { ok: true, queued: n, at, say: note + '，已排队' });
  }

  const body = req.body || {};
  const action = body.action || req.query.action || 'sync';
  const deviceTs = body.ts || body.client_ts;

  // 小体量请求，不设流控；这里只挡明显异常的体积，防误传整个固件
  if (JSON.stringify(body).length > 64 * 1024) {
    return json(413, { ok: false, code: 'TOO_LARGE', say: '请求体过大，state 应该在 4KB 以内。' });
  }

  /* 设备这次请求顺路当一次「送货」：把家长看板排队的指令压进它上报的 state。
     没有 state（首次初始化）就跳过 —— 队列留着，等有 state 再送。 */
  let incoming = body.state === undefined ? null : body.state;
  let applied = [];
  if (incoming && typeof incoming === 'object') {
    const merged = drainPending(process.env, incoming, deviceTs);
    incoming = merged.state;
    applied = merged.applied;
  }

  const t0 = Date.now();
  let out;
  try {
    out = core.run(
      {
        action,
        state: incoming,
        args: body.args || {},
        ts: deviceTs,
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

  /* 给家长看板留一份快照。写失败不影响设备，store 内部已兜住异常。 */
  if (out && out.state) {
    store.updateSnapshot(process.env, out.state, { deviceTs, source: action });
  }
  for (const a of applied) {
    store.addEvent(
      process.env,
      (a.ok ? '已送达：' : '未生效：') + (a.note || a.action) + (a.ok ? '' : '（' + (a.say || '') + '）'),
      'deliver'
    );
  }
  if (applied.length) {
    out.applied = applied;
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
