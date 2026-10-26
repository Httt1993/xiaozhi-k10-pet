#!/usr/bin/env node
/**
 * 灌一批演示数据进本地看板，方便直接看效果
 * 用法：node seed-demo.mjs [端口]
 */
const PORT = process.argv[2] || 8899;
const BASE = `http://127.0.0.1:${PORT}/`;

async function call(method, url, body) {
  const opt = { method, headers: { 'Content-Type': 'application/json' } };
  if (body) opt.body = JSON.stringify(body);
  const r = await fetch(url, opt);
  const text = await r.text();
  try {
    return JSON.parse(text);
  } catch (_) {
    return { raw: text.slice(0, 200), status: r.status };
  }
}

const api = (m, b) => call(m, BASE + '?page=parent&api=1', b);

/* 1. 初始化一只新宠物 */
let r = await call('POST', BASE, { action: 'sync', state: null });
let state = r.state;
console.log('宠物名:', r.status.name, '| 饱食', r.status.hunger, '金币', r.status.coins);

/* 2. 让它饿一点、心情低一点，条形图才好看 */
state.pet.hunger = 42;
state.pet.energy = 66;
state.pet.mood = 78;
state.pet.exp = 130;
state.pet.coins = 34;
state.pet.lastDecay = Math.floor(Date.now() / 1000);
r = await call('POST', BASE, { action: 'sync', state });
state = r.state;

/* 3. 布置两个任务 */
await api('POST', { cmd: 'task_publish', args: { title: '把书包收拾好', reward: 10 } });
await api('POST', { cmd: 'task_publish', args: { title: '读 20 分钟绘本', reward: 15 } });

/* 4. 让孩子报备第一个任务（模拟设备侧） */
r = await call('POST', BASE, { action: 'sync', state });
state = r.state;
r = await call('POST', BASE, { action: 'task_submit', state, args: { title: '书包' } });
if (r.state) state = r.state;

/* 5. 再同步一次，让快照里带上任务 */
await call('POST', BASE, { action: 'sync', state });

/* 6. 最后排一条不消费的指令，方便看到看板上的「等待送达」区 */
await api('POST', { cmd: 'play', args: {} });

const snap = await api('GET');
console.log('快照后端:', snap.backend, '| 距今', snap.age_sec, '秒');
console.log('任务数:', snap.tasks.length, '| 待送达:', snap.pending.length);
console.log('看板地址: ' + BASE + '?page=parent');
