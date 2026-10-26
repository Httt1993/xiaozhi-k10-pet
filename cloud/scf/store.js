/**
 * 快照 + 家长指令队列
 *
 * 为什么需要这一层：
 *   宠物状态的真身在设备 NVS 里，云函数自己是无状态的。
 *   家长打开网页想看「我的宠物现在饿不饿」，就必须让云上留一份快照。
 *
 * 两个后端，靠环境变量自动选：
 *   1. file —— 设了 PET_STORE_FILE（本地 serve-local.mjs 会自动设）
 *      快照写在这个文件里，重启不丢，适合本机体验和调试。
 *   2. mem  —— 没设 PET_STORE_FILE（云上默认）
 *      存在模块级变量里。SCF 热实例会被复用，所以几分钟～几十分钟内
 *      家长看板能看到最新快照；实例回收后就看板变空，等设备下次上报自动恢复。
 *      不花钱、不依赖第三方，代价是「冷启动后看板空一会儿」。
 *
 * 想让它长期不丢，最省事的做法是把 PET_STORE_FILE 指到 SCF 的 /tmp：
 *   /tmp 在同一实例内可写，但实例回收照样丢 —— 真要长期留存就得挂 COS/数据库。
 *   对「孩子今天吃了几顿饭」这个量级的需求，mem 后端其实够用，所以默认不折腾。
 */

'use strict';

const fs = require('fs');

/* 模块级缓存：SCF 热实例复用期间有效 */
let mem = null;

function emptyStore() {
  return {
    state: null,        // 最近一次宠物状态快照
    updatedAt: 0,       // 快照时间（服务器时钟，秒）
    deviceAt: 0,        // 设备上报时间（设备时钟，秒）
    source: '',         // 快照来自哪个动作
    pending: [],        // 家长在看板点的指令，等设备下次联网时执行
    events: [],         // 最近若干条给家长看的事件
  };
}

function normalize(raw) {
  const base = emptyStore();
  if (!raw || typeof raw !== 'object') return base;
  if (raw.state && typeof raw.state === 'object') base.state = raw.state;
  base.updatedAt = Number(raw.updatedAt) || 0;
  base.deviceAt = Number(raw.deviceAt) || 0;
  base.source = typeof raw.source === 'string' ? raw.source : '';
  if (Array.isArray(raw.pending)) base.pending = raw.pending.slice(0, 20);
  if (Array.isArray(raw.events)) base.events = raw.events.slice(0, 30);
  return base;
}

function filePath(env) {
  const p = env && env.PET_STORE_FILE;
  return typeof p === 'string' && p.trim() ? p.trim() : '';
}

/** 读整份 store。任何异常都降级成空 store —— 家长看板打不开比看不到数据更糟。 */
function load(env) {
  const p = filePath(env);
  if (!p) return Object.assign({}, mem || emptyStore());

  try {
    if (fs.existsSync(p)) {
      const text = fs.readFileSync(p, 'utf8');
      if (text && text.trim()) return normalize(JSON.parse(text));
    }
  } catch (err) {
    console.warn('[pet-store] 读快照失败，按空处理：' + (err && err.message));
  }
  return emptyStore();
}

/** 写整份 store。写失败只告警，不影响设备请求。 */
function save(env, data) {
  const obj = normalize(data);
  mem = obj;
  const p = filePath(env);
  if (!p) return obj;

  try {
    fs.writeFileSync(p, JSON.stringify(obj));
  } catch (err) {
    console.warn('[pet-store] 写快照失败：' + (err && err.message));
  }
  return obj;
}

/** 设备上报后更新快照（不动 pending 队列） */
function updateSnapshot(env, state, meta) {
  const st = load(env);
  if (state && typeof state === 'object') st.state = state;
  st.updatedAt = Math.floor(Date.now() / 1000);
  if (meta && meta.deviceTs) st.deviceAt = Number(meta.deviceTs) || 0;
  if (meta && meta.source) st.source = meta.source;
  return save(env, st);
}

/** 家长在看板上点了一下：进队列，等设备联网时执行 */
function pushPending(env, action, args, note) {
  const st = load(env);
  if (st.pending.length >= 20) st.pending.shift();
  st.pending.push({
    action: String(action || ''),
    args: args && typeof args === 'object' ? args : {},
    note: String(note || ''),
    at: Math.floor(Date.now() / 1000),
  });
  save(env, st);
  return st.pending.length;
}

/**
 * 取出并清空队列。
 * 先清空再执行：即使这轮执行到一半崩了，也不会让同一条指令无限重复发币
 * （家长点「通过」两次导致双倍金币，是这类系统最典型的 bug）。
 */
function takePending(env) {
  const st = load(env);
  const queue = st.pending.slice();
  if (!queue.length) return [];
  st.pending = [];
  save(env, st);
  return queue;
}

/** 追加一条家长可见事件 */
function addEvent(env, text, kind) {
  const st = load(env);
  const now = Math.floor(Date.now() / 1000);
  st.events.unshift({ at: now, text: String(text || ''), kind: String(kind || 'info') });
  if (st.events.length > 30) st.events.length = 30;
  save(env, st);
}

module.exports = {
  emptyStore,
  load,
  save,
  updateSnapshot,
  pushPending,
  takePending,
  addEvent,
  backend: (env) => (filePath(env) ? 'file' : 'mem'),
};
