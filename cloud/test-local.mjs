#!/usr/bin/env node
/**
 * 宠物云端引擎 · 本地端到端测试
 *
 * 不需要部署、不需要联网。跑的是一份完整「一天」的剧本：
 *   首次开机 → 饿了两小时 → 喂食 → 撑了 → 没钱 → 饿到生病 → 吃错药 →
 *   对症吃药 → 玩耍 → 语音连击被节流 → 讲故事刷到上限 → 家长发任务 →
 *   孩子报备 → 家长确认发币 → 设备时钟错乱保护
 *
 * 用法：node test-local.mjs
 */

import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);
const core = require('./scf/pet-core.js');

let pass = 0;
let fail = 0;
const failures = [];

function ok(cond, label, extra) {
  if (cond) {
    pass++;
    console.log(`  ✓ ${label}`);
  } else {
    fail++;
    failures.push(label);
    console.log(`  ✗ ${label}${extra ? '  → ' + JSON.stringify(extra) : ''}`);
  }
}

function eq(actual, expect, label) {
  ok(actual === expect, label, { actual, expect });
}

/** 用固定「现在」调一次，避免测试自己跑着跑着跨过 2 秒节流窗口 */
function call(action, state, args, tsOverride, env) {
  const ts = tsOverride === undefined ? Math.floor(Date.now() / 1000) : tsOverride;
  return core.run({ action, state, args: args || {}, ts }, env || {});
}

function section(title) {
  console.log(`\n── ${title} ──`);
}

// ==================================================================== 1. 初始化
section('1. 首次开机');

const t0 = Math.floor(Date.now() / 1000);
let r = call('ping', null, {}, t0);
ok(r.ok && r.code === 'PONG', 'ping 通');
ok(Array.isArray(r.catalog.foods) && r.catalog.foods.length === 6, '物品库有 6 种食物');
ok(r.catalog.meds.length === 3, '药品库有 3 种药');

r = call('sync', null, {}, t0);
ok(r.ok, 'state=null 自动初始化');
eq(r.status.hunger, 60, '初始饱食度 60');
eq(r.status.energy, 80, '初始精力 80');
eq(r.status.mood, 80, '初始心情 80');
eq(r.status.coins, 20, '新手礼包 20 枚金币');
eq(r.status.level, 1, '初始等级 1');
eq(r.status.sick, false, '初始健康');
ok(r.fresh === true, '标记为首次创建');
eq(r.state.v, 1, '状态版本号 = 1');
ok(Array.isArray(r.events), '返回事件数组');
ok(typeof r.say === 'string' && r.say.length > 0, '没有动作时也有一句状态心声');

let state = r.state;

// ==================================================================== 2. 时间衰减
section('2. 时间衰减（关机也照饿）');

// 把 lastDecay 拨回 1 小时前
let s = JSON.parse(JSON.stringify(state));
s.pet.lastDecay = t0 - 3600;
r = call('sync', s, {}, t0);
eq(r.decay_steps, 2, '1 小时 = 2 步（每 0.5 小时一步）');
eq(r.status.hunger, 56, '饱食 60 → 56（每步 -2）');
eq(r.status.energy, 78, '精力 80 → 78（每步 -1）');
eq(r.status.mood, 78, '心情 80 → 78（每步 -1）');

// 24 小时以上的封顶
s = JSON.parse(JSON.stringify(state));
s.pet.lastDecay = t0 - 100 * 3600;
r = call('sync', s, {}, t0);
eq(r.decay_steps, 48, '超过 24 小时封顶为 48 步，不会一次清零');

// 余数不丢：1.25 小时只走 2 步，但 lastDecay 只推进 1 小时
s = JSON.parse(JSON.stringify(state));
s.pet.lastDecay = t0 - 4500;
r = call('sync', s, {}, t0);
eq(r.decay_steps, 2, '1.25 小时走 2 步');
eq(r.state.pet.lastDecay, t0 - 4500 + 3600, 'lastDecay 只推进整步，余数保留');

// ==================================================================== 3. 喂食
section('3. 喂食');

state = call('sync', null, {}, t0).state;
state.pet.lastActionAt = 0;

r = call('feed', state, { food: '苹果' }, t0 + 100);
ok(r.ok && r.kind === 'food', '喂苹果成功');
eq(r.status.hunger, 80, '饱食 60 + 20 = 80');
eq(r.status.coins, 18, '金币 20 - 2 = 18');
ok(r.say.includes('苹果'), '话术里提到苹果', r.say);
state = r.state;

// 别名
state.pet.lastActionAt = 0;
r = call('feed', state, { food: '红萝卜' }, t0 + 200);
ok(r.ok && r.item === 'carrot', '「红萝卜」正确命中胡萝卜（最长别名优先）');
state = r.state;

// 不认识的词退回默认（注意先把肚子腾出来，否则会先撞上「吃饱了」）
s = JSON.parse(JSON.stringify(r.state));
s.pet.hunger = 40;
s.pet.lastActionAt = 0;
r = call('feed', s, { food: '外星陨石' }, t0 + 300);
ok(r.ok && r.item === 'cookie', '不认识的词退回小饼干');

// 撑了
s = JSON.parse(JSON.stringify(r.state));
s.pet.hunger = 99;
s.pet.lastActionAt = 0;
r = call('feed', s, { food: '苹果' }, t0 + 400);
ok(!r.ok && r.code === 'FULL', '饱食 99 时拒绝喂食');

// 没钱
s = JSON.parse(JSON.stringify(r.state));
s.pet.coins = 0;
s.pet.hunger = 50;
s.pet.lastActionAt = 0;
r = call('feed', s, { food: '小蛋糕' }, t0 + 500);
ok(!r.ok && r.code === 'NO_COIN', '金币不够时拒绝');

// ==================================================================== 4. 生病链
section('4. 饿到生病');

s = core.initDoc(core.resolveConfig({}), t0);
s.pet.hunger = 0;
s.pet.lastDecay = t0 - 13 * 3600;
s.pet.starvingSince = t0 - 13 * 3600;
s.pet.lastActionAt = 0;
r = call('sync', s, {}, t0);
ok(r.status.sick === true, '饿满 12 小时后生病');
ok(r.events.includes('sick'), '事件里带 sick，固件会据此变红灯');

const sickState = r.state;

// 没病吃药
s = JSON.parse(JSON.stringify(call('sync', null, {}, t0).state));
s.pet.lastActionAt = 0;
r = call('medicine', s, { medicine: '退烧药' }, t0 + 600);
ok(!r.ok && r.code === 'NOT_SICK', '没生病时拒绝吃药');
ok(r.say.includes('肚子会疼'), '解释为什么要拒绝', r.say);

// 保健品：没病能吃
s.pet.lastActionAt = 0;
r = call('medicine', s, { medicine: '维生素' }, t0 + 700);
ok(r.ok && r.kind === 'tonic', '没病也能吃维生素（保健品）');

// 生病吃保健品
s = JSON.parse(JSON.stringify(sickState));
s.pet.lastActionAt = 0;
r = call('medicine', s, { medicine: '维生素' }, t0 + 800);
ok(!r.ok && r.code === 'WRONG_MED', '生病时吃维生素无效');

// 对症
s = JSON.parse(JSON.stringify(sickState));
s.pet.lastActionAt = 0;
const coinsBefore = s.pet.coins;
r = call('medicine', s, { medicine: '退烧药' }, t0 + 900);
ok(r.ok && r.kind === 'cure', '对症吃药治好');
eq(r.status.sick, false, '病好了');
eq(r.status.coins, coinsBefore - 4, '扣 4 枚药费');
ok(r.events.includes('sick') === false, '事件里不再有 sick');

// 看病（更贵，一次治好）
s = JSON.parse(JSON.stringify(sickState));
s.pet.coins = 20;
s.pet.lastActionAt = 0;
r = call('heal', s, {}, t0 + 1000);
ok(r.ok && r.status.sick === false, '花钱看病也治好');

// ==================================================================== 5. 互动与节流
section('5. 玩耍 / 节流 / 免费次数上限');

s = JSON.parse(JSON.stringify(call('sync', null, {}, t0).state));
s.pet.lastActionAt = 0;
r = call('play', s, {}, t0 + 1100);
ok(r.ok && r.status.mood === 98, '玩耍心情 +18');
eq(r.status.coins, 17, '玩耍扣 3 枚');

// 节流：同一个 ts 连调两次
const playState = r.state;
r = call('play', playState, {}, t0 + 1100);
ok(!r.ok && r.code === 'TOO_FAST', '2 秒内连击被节流（挡语音误识别）');

// 没精力
s = JSON.parse(JSON.stringify(call('sync', null, {}, t0).state));
s.pet.energy = 5;
s.pet.lastActionAt = 0;
r = call('play', s, {}, t0 + 1200);
ok(!r.ok && r.code === 'TIRED', '精力不足时拒绝玩耍');

// 免费互动上限
s = JSON.parse(JSON.stringify(call('sync', null, {}, t0).state));
let lastOk = true;
for (let i = 0; i < 3; i++) {
  s.pet.lastActionAt = 0;
  r = call('story', s, { topic: '恐龙' }, t0 + 2000 + i * 10);
  lastOk = lastOk && r.ok;
  s = r.state;
}
ok(lastOk, '讲故事前 3 次都成功');
s.pet.lastActionAt = 0;
r = call('story', s, {}, t0 + 2100);
ok(!r.ok && r.code === 'DAILY_CAP', '第 4 次触发每日上限');
eq(r.status.fun_left.story, 0, '剩余次数归零');

// 摸头不限次
s = JSON.parse(JSON.stringify(call('sync', null, {}, t0).state));
let petAllOk = true;
let petCoinsSame = true;
const startCoins = s.pet.coins;
for (let i = 0; i < 10; i++) {
  s.pet.lastActionAt = 0;
  r = call('pet', s, {}, t0 + 3000 + i * 10);
  petAllOk = petAllOk && r.ok;
  s = r.state;
}
petCoinsSame = s.pet.coins === startCoins;
ok(petAllOk, '摸头可以无限次');
ok(petCoinsSame, '摸头不花金币（是陪伴不是刷数值）');

// ==================================================================== 6. 任务
section('6. 任务闭环');

state = call('sync', null, {}, t0).state;
r = call('task_publish', state, { title: '整理房间', detail: '把玩具收进箱子', reward: 10 }, t0 + 4000);
ok(r.ok && r.task.status === 'pending', '家长发布任务');
const taskId = r.task.id;
state = r.state;

r = call('task_list', state, {}, t0 + 4010);
ok(r.tasks.length === 1 && r.tasks[0].title === '整理房间', '孩子能查到任务');

r = call('task_submit', state, { title: '整理房间', note: '收好了' }, t0 + 4020);
ok(r.ok && r.task.status === 'submitted', '孩子报备完成，等家长确认');
ok(r.say.includes('等家长确认'), '话术说明还在等确认', r.say);
state = r.state;
const coinsBeforeTask = state.pet.coins;

// 重复报备
r = call('task_submit', state, { title: '整理房间' }, t0 + 4030);
ok(!r.ok && r.code === 'DUPLICATE', '同一任务不能重复报备');

// 家长确认
r = call('task_approve', state, { task_id: taskId }, t0 + 4040);
ok(r.ok && r.coins === coinsBeforeTask + 10, '家长确认后发 10 枚金币');
state = r.state;

// 幂等
r = call('task_approve', state, { task_id: taskId }, t0 + 4050);
ok(!r.ok && r.code === 'ALREADY', '重复确认不会重复发币');

// 不存在的任务
r = call('task_submit', state, { title: '登月' }, t0 + 4060);
ok(!r.ok && r.code === 'NOT_FOUND', '报备不存在的任务会被拒');

// ==================================================================== 7. 时钟保护
section('7. 设备时钟错乱保护');

s = JSON.parse(JSON.stringify(call('sync', null, {}, t0).state));
s.pet.lastDecay = t0;
// 设备时间比服务器快 3 小时，且 state 的 lastDecay 按错误时间算
r = core.run({ action: 'sync', state: s, args: {}, ts: t0 + 3 * 3600 }, {});
ok(Math.abs(r.skew_sec + 3 * 3600) <= 2, '上报时钟偏差（服务器 - 设备 ≈ -3h）', r.skew_sec);
ok((r.decay_steps || 0) === 0, '偏差超过 5 分钟时改用服务器时间，不会误算 6 步衰减');

// 偏差在 5 分钟内时信任设备时间
r = core.run({ action: 'sync', state: s, args: {}, ts: t0 + 120 }, {});
ok(Math.abs(r.skew_sec + 120) <= 2, '小偏差照常上报', r.skew_sec);
eq(r.decay_steps, undefined, '偏差 2 分钟不足以走一步衰减');

// ==================================================================== 8. 状态体积
section('8. 状态体积（要装进 NVS）');

let big = call('sync', null, {}, t0).state;
for (let i = 0; i < 40; i++) {
  big = call('task_publish', big, { title: '任务' + i, reward: 5 }, t0 + 5000 + i).state;
}
const size = JSON.stringify(big).length;
ok(size < 3200, `40 个任务后 state 仍只有 ${size} B（< 3200 B）`);
eq(big.tasks.length, 12, '未完成任务保留 12 条（TASK_KEEP 上限）');
ok(big.tasks.every((t) => t.status === 'pending'), '被裁掉的都是已完成的旧任务');

// ==================================================================== 9. 异常输入
section('9. 异常输入');

r = call('sync', { garbage: true }, {}, t0);
ok(r.ok, '残缺 state 不崩，自动补齐');

r = call('eat_moon', call('sync', null, {}, t0).state, {}, t0 + 6000);
ok(!r.ok && r.code === 'UNKNOWN_ACTION', '不认识的 action 被拒且不崩');

r = call('feed', call('sync', null, {}, t0).state, {}, t0 + 6100);
ok(r.ok && r.item === 'cookie', '不传 food 参数时用默认食物');

// ==================================================================== 10. 配置覆盖
section('10. 环境变量覆盖数值');

r = core.run({ action: 'sync', state: null, args: {}, ts: t0 }, { PET_NAME: '小白', INIT_COINS: '99' });
eq(r.status.name, '小白', 'PET_NAME 可覆盖');
eq(r.status.coins, 99, 'INIT_COINS 可覆盖');

// ==================================================================== 汇总
console.log('\n' + '─'.repeat(50));
console.log(`总计 ${pass + fail} 项  ·  通过 ${pass}  ·  失败 ${fail}`);
if (fail > 0) {
  console.log('\n失败项：');
  for (const f of failures) console.log('  · ' + f);
  process.exit(1);
}
console.log('全部通过 ✓');
