/**
 * 宠物养成核心 · 纯函数
 *
 * 设计原则：这一层不碰网络、不碰存储、不碰时间（时间一律由调用方传入 ts）。
 * 好处有三个：
 *   1. 云函数无状态，同一份代码在 SCF 和本机测试里行为完全一致；
 *   2. 衰减靠「时间戳差」而不是定时器，设备关机三天再开机也能正确补算；
 *   3. 可以单测。
 *
 * 状态文档结构（整份 JSON 在设备 NVS 里循环存取）：
 * {
 *   v: 版本号,
 *   pet: { name, energy, hunger, mood, sick, coins, exp,
 *          lastDecay, starvingSince, lastFeed, lastActionAt, createdAt, counters },
 *   tasks: [{ id, title, detail, reward, status, createdAt, submittedAt, doneAt, submitNote }],
 *   coinLog: [{ ts, delta, reason, actor }],
 *   meta: { updatedAt, deviceId }
 * }
 */

'use strict';

// ------------------------------------------------------------------ 配置

const DEFAULTS = {
  PET_NAME: '乔利',
  DECAY_STEP_HOURS: 1 / 120,  // 默认 30 秒一步（与设备端 PET_DECAY_STEP_SEC=30 同步）；游玩档可改回 0.5
  MAX_DECAY_STEPS: 2880,      // 单次最多补算 2880 步（24 小时），防止久不开机被瞬间清零
  SICK_AFTER_STARVING: 43200, // 饿到 0 持续 12 小时 → 生病
  DAILY_TASK_REWARD_CAP: 5,
  EXP_PER_ACTION: 2,
  EXP_PER_LEVEL: 50,
  TASK_NEEDS_PARENT_APPROVE: 1,
  COST_FEED: 2,
  COST_PLAY: 3,
  COST_HEAL: 10,
  COST_MEDICINE: 3,
  INIT_COINS: 20,
  DAILY_FUN_CAP: 3,           // 讲故事/唱歌/学东西的每日次数上限
  FULL_HUNGER: 99,
  ACTION_COOLDOWN_SEC: 2,
  // 体积预算：整份 state 要在设备 NVS 里存得下（目标 < 3KB），
  // 所以这两项比 CloudBase 版小得多。家庭实际场景够用。
  COIN_LOG_KEEP: 8,
  TASK_KEEP: 12,
};

const num = (v, d) => {
  const n = Number(v);
  return Number.isFinite(n) ? n : d;
};

/**
 * 合并环境变量成运行时配置。
 *
 * 两种写法都认，`PET_` 前缀优先：
 *   PET_INIT_COINS=50   （推荐，和 PET_KEY / PET_NAME 风格一致）
 *   INIT_COINS=50       （兼容旧写法）
 * 这样文档里写的变量名和实际生效的完全一致，不会出现「照文档配了却没动静」。
 */
function resolveConfig(env) {
  const src = env || {};
  const cfg = {};
  for (const k of Object.keys(DEFAULTS)) {
    const d = DEFAULTS[k];
    let raw = src['PET_' + k];
    if (raw === undefined) {
      raw = src[k];
    }
    cfg[k] = typeof d === 'number' ? num(raw, d) : (raw === undefined ? d : raw);
  }
  cfg.TASK_NEEDS_PARENT_APPROVE = Number(cfg.TASK_NEEDS_PARENT_APPROVE) ? 1 : 0;
  return cfg;
}

const nowSec = () => Math.floor(Date.now() / 1000);

/** 上海时区当天 00:00（秒）。SCF 运行时是 UTC，直接用 getHours 会差 8 小时。 */
function dayStartSec(ts) {
  const OFFSET = 8 * 3600;
  return Math.floor((ts + OFFSET) / 86400) * 86400 - OFFSET;
}

const clamp100 = (n) => Math.max(0, Math.min(100, n));
const rand = () => Math.random().toString(36).slice(2, 6);
const norm = (s) => String(s == null ? '' : s).trim().toLowerCase();

// ------------------------------------------------------------------ 物品库

/**
 * 食物。数值相对 INIT_COINS=20 的新手礼包设计：
 * 主食 2-4 币，甜点 5 币（心情涨得多、饱食涨得少，故意做成「不划算但开心」）。
 */
const FOODS = [
  { id: 'cookie', name: '小饼干', aliases: ['饼干', '曲奇', 'cookie'],
    hunger: 25, mood: 5, energy: 3, cost: 2, line: '咔嚓咔嚓……小饼干最好吃啦！' },
  { id: 'apple', name: '苹果', aliases: ['苹果片', '果子'],
    hunger: 20, mood: 8, energy: 2, cost: 2, line: '苹果甜甜的，脆脆的，我还要！' },
  { id: 'milk', name: '牛奶', aliases: ['奶', '热牛奶', '酸奶'],
    hunger: 15, mood: 5, energy: 8, cost: 3, line: '咕嘟咕嘟……喝完牛奶身上暖暖的～' },
  { id: 'carrot', name: '胡萝卜', aliases: ['萝卜', '红萝卜'],
    hunger: 22, mood: 2, energy: 2, cost: 2, line: '胡萝卜……好吧，为了眼睛亮亮的，我吃！' },
  { id: 'noodles', name: '热汤面', aliases: ['汤面', '面条', '面', '汤'],
    hunger: 30, mood: 6, energy: 6, cost: 4, line: '呼——好烫！不过热汤面最暖肚子了～' },
  { id: 'cake', name: '小蛋糕', aliases: ['蛋糕', '布丁', '甜点', '甜品'],
    hunger: 18, mood: 14, energy: 4, cost: 5, line: '哇！是蛋糕！今天是什么好日子呀～' },
];

/** 药品。cure=1 治病，tonic=1 只是保健品（没病也能吃，但治不了病）。 */
const MEDS = [
  { id: 'fever', name: '退烧药', aliases: ['退烧', '退热药'],
    cure: 1, mood: 6, energy: 4, cost: 4, line: '药有点苦……但是头不晕了，谢谢你！' },
  { id: 'cold', name: '感冒冲剂', aliases: ['感冒药', '感冒灵', '冲剂'],
    cure: 1, mood: 4, energy: 2, cost: 3, line: '甜甜的冲剂，喝完鼻子通啦～' },
  { id: 'vitamin', name: '维生素', aliases: ['维C', '维他命', 'vc'],
    cure: 0, mood: 4, energy: 6, cost: 2, tonic: 1, line: '维C酸酸甜甜的，吃完感觉有力气啦！' },
];

/**
 * 按别名匹配，命中「最长」的那个键。
 * 这样「红萝卜」不会被「萝卜」抢先命中，「感冒灵」也不会退化成别的。
 */
function pickByAlias(list, text) {
  const t = norm(text);
  if (!t) return null;
  let best = null;
  let bestLen = 0;
  for (const it of list) {
    const keys = [it.id, it.name].concat(it.aliases || []);
    for (const k of keys) {
      const kk = norm(k);
      if (kk && t.includes(kk) && kk.length > bestLen) {
        best = it;
        bestLen = kk.length;
      }
    }
  }
  return best;
}

const findFood = (t) => pickByAlias(FOODS, t);
const findMed = (t) => pickByAlias(MEDS, t);
const foodNames = () => FOODS.map((f) => f.name).join('、');
const medNames = () => MEDS.map((m) => m.name).join('、');

// ------------------------------------------------------------------ 初始化

function initDoc(cfg, ts) {
  const t = num(ts, nowSec());
  const startCoins = Math.max(0, Math.round(cfg.INIT_COINS || 0));
  return {
    v: 0,
    pet: {
      name: cfg.PET_NAME,
      energy: 80,
      hunger: 60,
      mood: 80,
      sick: 0,
      coins: startCoins,
      exp: 0,
      lastDecay: t,
      starvingSince: 0,
      lastFeed: 0,
      lastActionAt: 0,
      /* 家长看板指令的「已处理到哪一号」。存在设备 NVS 里、每次上报带回来，
         所以它比云函数内存可靠——看板靠它判断指令有没有送达。 */
      cmdAck: 0,
      createdAt: t,
      counters: { day: dayStartSec(t) },
    },
    tasks: [],
    coinLog: startCoins > 0
      ? [{ ts: t, delta: startCoins, reason: '新手礼包', actor: 'parent' }]
      : [],
    meta: { updatedAt: t },
  };
}

/** 老版本 / 缺字段的文档补齐，避免任何一处 undefined 把计算带成 NaN */
function normalizeDoc(doc, cfg, ts) {
  const base = initDoc(cfg, ts);
  if (!doc || typeof doc !== 'object' || !doc.pet) return base;
  const p = Object.assign({}, base.pet, doc.pet);
  for (const k of ['energy', 'hunger', 'mood']) {
    p[k] = clamp100(num(p[k], base.pet[k]));
  }
  p.coins = Math.max(0, Math.round(num(p.coins, 0)));
  p.exp = Math.max(0, Math.round(num(p.exp, 0)));
  p.sick = p.sick ? 1 : 0;
  p.lastDecay = num(p.lastDecay, ts);
  p.starvingSince = num(p.starvingSince, 0);
  p.cmdAck = Math.max(0, Math.round(num(p.cmdAck, 0)));
  p.counters = p.counters && typeof p.counters === 'object' ? p.counters : base.pet.counters;
  return {
    v: num(doc.v, 0),
    pet: p,
    tasks: Array.isArray(doc.tasks) ? doc.tasks.filter((x) => x && x.id) : [],
    coinLog: Array.isArray(doc.coinLog) ? doc.coinLog.slice(0, cfg.COIN_LOG_KEEP) : [],
    meta: Object.assign({ updatedAt: ts }, doc.meta || {}),
  };
}

// ------------------------------------------------------------------ 衰减

/**
 * 按时间戳差补算衰减。返回补算的步数。
 *
 * 关键点：循环里用「这一步对应的时间点」，而不是统一的 ts。
 * 否则一步就跨过 12 小时饥饿线时，生病判定会算错。
 */
function applyDecay(doc, cfg, ts) {
  const p = doc.pet;
  const stepSec = cfg.DECAY_STEP_HOURS * 3600;
  const last = num(p.lastDecay, ts);
  let steps = Math.floor((ts - last) / stepSec);
  if (steps <= 0) {
    if (!p.lastDecay) p.lastDecay = ts;
    return 0;
  }
  steps = Math.min(steps, cfg.MAX_DECAY_STEPS);

  for (let i = 0; i < steps; i++) {
    const stepTs = last + (i + 1) * stepSec;
    p.hunger = Math.max(0, p.hunger - 2);
    p.energy = Math.max(0, p.energy - 1);
    p.mood = Math.max(0, p.mood - (p.hunger <= 10 ? 4 : 1));
    if (p.sick) p.mood = Math.max(0, p.mood - 3);

    if (p.hunger === 0) {
      if (!p.starvingSince) p.starvingSince = stepTs;
      else if (stepTs - p.starvingSince > cfg.SICK_AFTER_STARVING) p.sick = 1;
    } else {
      p.starvingSince = 0;
    }
  }
  // 只推进完整走完的步数，余数保留，避免反复取整累积误差
  p.lastDecay = last + steps * stepSec;
  return steps;
}

// ------------------------------------------------------------------ 状态

function dailyUsed(p, key, ts) {
  if (!p.counters || p.counters.day !== dayStartSec(ts)) return 0;
  return p.counters[key] || 0;
}

function bumpDaily(p, key, ts) {
  const day = dayStartSec(ts);
  if (!p.counters || p.counters.day !== day) p.counters = { day };
  p.counters[key] = (p.counters[key] || 0) + 1;
  return p.counters[key];
}

function getStatus(doc, cfg, ts) {
  const p = doc.pet;
  const cap = cfg.DAILY_FUN_CAP;
  const t = num(ts, nowSec());
  return {
    name: p.name,
    energy: p.energy,
    hunger: p.hunger,
    mood: p.mood,
    sick: !!p.sick,
    coins: p.coins,
    exp: p.exp,
    level: Math.floor(p.exp / cfg.EXP_PER_LEVEL) + 1,
    exp_in_level: p.exp % cfg.EXP_PER_LEVEL,
    exp_per_level: cfg.EXP_PER_LEVEL,
    fun_left: {
      story: Math.max(0, cap - dailyUsed(p, 'story', t)),
      sing: Math.max(0, cap - dailyUsed(p, 'sing', t)),
      teach: Math.max(0, cap - dailyUsed(p, 'teach', t)),
      cap,
    },
  };
}

/** 宠物此刻的心声（给模型当口吻参考，不要求原样复述） */
function moodWord(st) {
  if (st.sick) return '我有点难受……头昏昏的，能不能给我看看医生呀？';
  if (st.hunger <= 15) return '我肚子饿得咕咕叫了，快给我点吃的吧～';
  if (st.mood <= 20) return '有点无聊呢，陪我玩一会儿好不好？';
  if (st.mood >= 80 && st.energy >= 70) return '我今天状态超好！';
  return '感觉还不错～';
}

/**
 * 状态事件：固件拿去做「灯 + 脸」的自动联动，不需要模型参与。
 * 顺序按严重程度排，固件取第一个即最高优先级。
 */
function statusEvents(st) {
  const ev = [];
  if (st.sick) ev.push('sick');
  if (st.hunger <= 15) ev.push('starving');
  else if (st.hunger <= 35) ev.push('hungry');
  if (st.energy <= 20) ev.push('tired');
  if (st.mood <= 20) ev.push('sad');
  else if (st.mood <= 40) ev.push('bored');
  if (!ev.length && st.mood >= 80 && st.energy >= 70) ev.push('great');
  return ev;
}

// ------------------------------------------------------------------ 内部工具

function pushCoinLog(doc, cfg, ts, delta, reason, actor) {
  doc.coinLog.unshift({ ts, delta, reason, actor });
  if (doc.coinLog.length > cfg.COIN_LOG_KEEP) doc.coinLog.length = cfg.COIN_LOG_KEEP;
}

function addExp(doc, cfg, amount) {
  doc.pet.exp += amount == null ? cfg.EXP_PER_ACTION : amount;
}

/** 简易节流：2 秒内重复动作直接拒绝，挡住语音误识别连击刷币 */
function throttle(doc, cfg, ts) {
  if (cfg.ACTION_COOLDOWN_SEC <= 0) return null;
  if (ts - (doc.pet.lastActionAt || 0) < cfg.ACTION_COOLDOWN_SEC) {
    return { ok: false, code: 'TOO_FAST', say: '慢一点点啦，我还没反应过来～' };
  }
  return null;
}

function touch(doc, ts) {
  doc.pet.lastActionAt = ts;
}

// ------------------------------------------------------------------ 动作

function feed(doc, cfg, ts, foodRef) {
  applyDecay(doc, cfg, ts);
  const fast = throttle(doc, cfg, ts);
  if (fast) return fast;

  const food = findFood(foodRef) || findFood('小饼干');
  const cost = Number.isFinite(food.cost) ? food.cost : cfg.COST_FEED;
  const p = doc.pet;

  if (p.hunger >= cfg.FULL_HUNGER) {
    return { ok: false, code: 'FULL', say: '我肚子已经圆滚滚啦，实在吃不下了……等会儿再喂我好不好？' };
  }
  if (p.coins < cost) {
    return { ok: false, code: 'NO_COIN', say: `我的游戏币不够啦，${food.name}要 ${cost} 枚呢。现在只有 ${p.coins} 枚。` };
  }

  p.coins -= cost;
  p.hunger = clamp100(p.hunger + food.hunger);
  p.mood = clamp100(p.mood + food.mood);
  p.energy = clamp100(p.energy + food.energy);
  p.lastFeed = ts;
  addExp(doc, cfg);
  pushCoinLog(doc, cfg, ts, -cost, '喂食：' + food.name, 'child');
  touch(doc, ts);
  return {
    ok: true,
    kind: 'food',
    item: food.id,
    say: `${food.line}（饱食 +${food.hunger}，心情 +${food.mood}）`,
  };
}

/**
 * 喂药。故意和喂食分成两条链路，因为「要不要吃药」本身就是判断力练习：
 *   没生病 → 拒绝（乱吃药肚子疼）；保健品 → 能吃但不治病；对症 → 治好。
 */
function giveMedicine(doc, cfg, ts, medRef) {
  applyDecay(doc, cfg, ts);
  const fast = throttle(doc, cfg, ts);
  if (fast) return fast;

  const med = findMed(medRef);
  const p = doc.pet;
  if (!med) {
    return {
      ok: false, code: 'NO_MED',
      say: `我的药箱里没有这个药呀。现在有：${medNames()}。`,
      meds: MEDS.map((m) => m.name),
    };
  }
  const cost = Number.isFinite(med.cost) ? med.cost : cfg.COST_MEDICINE;

  if (!p.sick) {
    if (med.tonic) {
      if (p.coins < cost) {
        return { ok: false, code: 'NO_COIN', say: `维生素要 ${cost} 枚游戏币，我的币不够啦。` };
      }
      p.coins -= cost;
      p.mood = clamp100(p.mood + med.mood);
      p.energy = clamp100(p.energy + med.energy);
      addExp(doc, cfg);
      pushCoinLog(doc, cfg, ts, -cost, '保健：' + med.name, 'child');
      touch(doc, ts);
      return { ok: true, kind: 'tonic', item: med.id, say: `${med.line}（我没生病，这个只是补身体～）` };
    }
    return {
      ok: false, code: 'NOT_SICK',
      say: '我现在没生病呀，乱吃药肚子会疼的。要是不舒服，先问问我哪里难受～',
    };
  }

  if (!med.cure) {
    return {
      ok: false, code: 'WRONG_MED',
      say: `${med.name}只能保健，治不了病哦。我这次是真的不舒服，得吃退烧药或者感冒冲剂。`,
    };
  }
  if (p.coins < cost) {
    return { ok: false, code: 'NO_COIN', say: `${med.name}要 ${cost} 枚游戏币，我的币不够啦……` };
  }

  p.coins -= cost;
  p.sick = 0;
  p.starvingSince = 0;
  p.mood = clamp100(p.mood + med.mood);
  p.energy = clamp100(p.energy + med.energy);
  addExp(doc, cfg, cfg.EXP_PER_ACTION * 3);
  pushCoinLog(doc, cfg, ts, -cost, '服药：' + med.name, 'child');
  touch(doc, ts);
  return { ok: true, kind: 'cure', item: med.id, say: `${med.line} 我的病好啦！` };
}

function play(doc, cfg, ts) {
  applyDecay(doc, cfg, ts);
  const fast = throttle(doc, cfg, ts);
  if (fast) return fast;

  const p = doc.pet;
  if (p.sick) return { ok: false, code: 'SICK', say: '我现在病着呢，玩不动啦……先给我治治病好不好？' };
  if (p.coins < cfg.COST_PLAY) {
    return { ok: false, code: 'NO_COIN', say: `游戏币不够啦，玩一次要 ${cfg.COST_PLAY} 枚。现在只有 ${p.coins} 枚。` };
  }
  if (p.energy < 10) return { ok: false, code: 'TIRED', say: '我有点累了，想睡一会儿，明天再玩好不好？' };

  p.coins -= cfg.COST_PLAY;
  p.mood = clamp100(p.mood + 18);
  p.energy = Math.max(0, p.energy - 8);
  p.hunger = Math.max(0, p.hunger - 3);
  addExp(doc, cfg);
  pushCoinLog(doc, cfg, ts, -cfg.COST_PLAY, '互动玩耍', 'child');
  touch(doc, ts);
  return { ok: true, kind: 'play', say: '太好玩啦！我最喜欢和你一起玩～' };
}

function heal(doc, cfg, ts) {
  applyDecay(doc, cfg, ts);
  const fast = throttle(doc, cfg, ts);
  if (fast) return fast;

  const p = doc.pet;
  if (!p.sick) return { ok: false, code: 'HEALTHY', say: '我现在很健康哦，不需要看病～' };
  if (p.coins < cfg.COST_HEAL) {
    return { ok: false, code: 'NO_COIN', say: `治病要 ${cfg.COST_HEAL} 枚游戏币，我的币不够啦……` };
  }
  p.coins -= cfg.COST_HEAL;
  p.sick = 0;
  p.starvingSince = 0;
  p.mood = clamp100(p.mood + 20);
  p.energy = clamp100(p.energy + 10);
  addExp(doc, cfg, cfg.EXP_PER_ACTION * 3);
  pushCoinLog(doc, cfg, ts, -cfg.COST_HEAL, '看病治病', 'child');
  touch(doc, ts);
  return { ok: true, kind: 'heal', say: '谢谢医生！我感觉好多啦，又活蹦乱跳了～' };
}

function sleep(doc, cfg, ts) {
  applyDecay(doc, cfg, ts);
  const p = doc.pet;
  p.energy = clamp100(p.energy + 20);
  p.hunger = Math.max(0, p.hunger - 5);
  touch(doc, ts);
  return { ok: true, kind: 'sleep', say: '那我先睡一会儿啦，呼呼～' };
}

// 免费互动：不花游戏币，但每天限次。既是内容成本，也是防「一晚上把宠物刷满」。

function freeAction(doc, cfg, ts, opt) {
  const p = doc.pet;
  const used = dailyUsed(p, opt.key, ts);
  const cap = cfg.DAILY_FUN_CAP;
  if (used >= cap) {
    return {
      ok: false, code: 'DAILY_CAP',
      say: `今天已经${opt.label} ${used} 次啦，我们先做点别的，或者把今天的事情做完再来～`,
    };
  }
  bumpDaily(p, opt.key, ts);
  p.mood = clamp100(p.mood + (opt.mood || 0));
  p.energy = clamp100(p.energy + (opt.energy || 0));
  p.hunger = clamp100(p.hunger + (opt.hunger || 0));
  addExp(doc, cfg, opt.exp);
  touch(doc, ts);
  return { ok: true, kind: opt.key, say: opt.say, left_today: cap - used - 1 };
}

function tellStory(doc, cfg, ts, topic) {
  applyDecay(doc, cfg, ts);
  const fast = throttle(doc, cfg, ts);
  if (fast) return fast;
  const t = String(topic || '').trim();
  const themes = ['恐龙', '太空', '小兔子', '大海', '小汽车', '公主', '恐龙蛋', '会飞的鲸鱼'];
  const pick = t || themes[Math.floor(Math.random() * themes.length)];
  return freeAction(doc, cfg, ts, {
    key: 'story', label: '讲故事', mood: 10, energy: -2,
    exp: cfg.EXP_PER_ACTION * 2, code: 'story',
    say: `好呀，我给你讲一个关于${pick}的故事——从前有一只小宠物，它……（接下来你来讲这段${pick}的故事）`,
  });
}

function singSong(doc, cfg, ts) {
  applyDecay(doc, cfg, ts);
  const fast = throttle(doc, cfg, ts);
  if (fast) return fast;
  return freeAction(doc, cfg, ts, {
    key: 'sing', label: '唱歌', mood: 12, energy: -3,
    exp: cfg.EXP_PER_ACTION * 2, code: 'sing',
    say: '啦啦啦～我唱歌给你听！',
  });
}

function teach(doc, cfg, ts, what) {
  applyDecay(doc, cfg, ts);
  const fast = throttle(doc, cfg, ts);
  if (fast) return fast;
  const w = String(what || '').trim();
  return freeAction(doc, cfg, ts, {
    key: 'teach', label: '学东西', mood: 8, energy: -2,
    exp: cfg.EXP_PER_ACTION * 2, code: 'teach',
    say: w ? `好呀，我想学${w}！你教我吧～` : '好呀，我想学新东西！你教我吧～',
  });
}

/** 摸头：零成本、不限次，但只加一点点心情 —— 是陪伴，不是刷数值 */
function petHead(doc, cfg, ts) {
  applyDecay(doc, cfg, ts);
  const fast = throttle(doc, cfg, ts);
  if (fast) return fast;
  const p = doc.pet;
  p.mood = clamp100(p.mood + 4);
  addExp(doc, cfg, 1);
  touch(doc, ts);
  const lines = ['嘿嘿，好痒～再摸摸', '舒服……我最喜欢被摸头了', '唔…再摸一会儿嘛'];
  return { ok: true, kind: 'pet', say: lines[Math.floor(Math.random() * lines.length)] };
}

/** 闲聊：不加数值，只把状态带回去，让模型自己组织语言 */
function chat(doc, cfg, ts, text) {
  applyDecay(doc, cfg, ts);
  const p = doc.pet;
  p.mood = clamp100(p.mood + 1);
  touch(doc, ts);
  return { ok: true, kind: 'chat', say: '', echo: String(text || '').slice(0, 60) };
}

// ------------------------------------------------------------------ 任务

function listTasks(doc, cfg, includeDone) {
  return doc.tasks
    .filter((t) => includeDone || t.status === 'pending' || t.status === 'submitted')
    .slice()
    .sort((a, b) => (b.createdAt || 0) - (a.createdAt || 0))
    .slice(0, 20)
    .map((t) => ({ id: t.id, title: t.title, detail: t.detail, reward: t.reward, status: t.status }));
}

/**
 * 孩子报备任务完成。防刷币三道闸：
 *   1) 同一任务只能报备一次；
 *   2) 每天最多报备 DAILY_TASK_REWARD_CAP 次；
 *   3) 默认要家长确认才真正发币。
 */
function submitTask(doc, cfg, ts, title, note) {
  const q = String(title || '').trim();
  if (!q) return { ok: false, code: 'EMPTY', say: '你想报备哪个任务呀？告诉我任务名字。' };

  const dayStart = dayStartSec(ts);
  const todayCount = doc.tasks.filter((t) => (t.submittedAt || 0) >= dayStart).length;
  if (todayCount >= cfg.DAILY_TASK_REWARD_CAP) {
    return { ok: false, code: 'CAP', say: `今天已经报备 ${todayCount} 个任务啦，先休息一下吧。` };
  }

  const nq = norm(q);
  const pend = (t) => t.status === 'pending';
  const matched =
    doc.tasks.find((t) => pend(t) && norm(t.title) === nq) ||
    doc.tasks.find((t) => pend(t) && norm(t.title).includes(nq)) ||
    doc.tasks.find((t) => pend(t) && norm(t.detail).includes(nq)) ||
    doc.tasks.find((t) => norm(t.title) === nq) ||
    doc.tasks.find((t) => norm(t.title).includes(nq));

  if (!matched) {
    return { ok: false, code: 'NOT_FOUND', say: `我没找到「${q}」这个任务哦，你可以问家长今天要做什么。` };
  }
  if (matched.status === 'submitted') {
    return { ok: false, code: 'DUPLICATE', say: `「${matched.title}」你已经报备过啦，等家长确认就好。` };
  }
  if (matched.status === 'done') {
    return { ok: false, code: 'DONE', say: `「${matched.title}」已经完成并且发过币啦，换一个任务试试？` };
  }

  const needApprove = !!cfg.TASK_NEEDS_PARENT_APPROVE;
  matched.status = needApprove ? 'submitted' : 'done';
  matched.submittedAt = ts;
  matched.submitNote = note || '';
  matched.doneAt = needApprove ? null : ts;

  if (!needApprove) {
    doc.pet.coins += matched.reward;
    pushCoinLog(doc, cfg, ts, matched.reward, '完成任务：' + matched.title, 'child');
  }
  return {
    ok: true,
    kind: 'task_submit',
    say: needApprove
      ? `「${matched.title}」我记下啦，等家长确认后给你发 ${matched.reward} 枚游戏币！`
      : `太棒了，「${matched.title}」完成，奖励你 ${matched.reward} 枚游戏币！`,
    task: { id: matched.id, title: matched.title, reward: matched.reward, status: matched.status },
  };
}

/** 家长发布任务。真机上由家长对小智说一句话触发，不需要网页后台。 */
function publishTask(doc, cfg, ts, title, detail, reward) {
  const t = String(title || '').trim();
  if (!t) return { ok: false, code: 'EMPTY', say: '任务名不能为空。' };
  const r = Math.max(1, Math.min(999, Math.round(num(reward, 10))));
  const task = {
    id: 't' + Date.now().toString(36) + rand(),
    title: t,
    detail: String(detail || '').slice(0, 200),
    reward: r,
    status: 'pending',
    createdAt: ts,
    submittedAt: null,
    doneAt: null,
    submitNote: '',
  };
  doc.tasks.unshift(task);
  if (doc.tasks.length > cfg.TASK_KEEP) doc.tasks.length = cfg.TASK_KEEP;
  return { ok: true, kind: 'task_publish', say: `已发布任务「${t}」，奖励 ${r} 枚游戏币。`, task };
}

/** 确认发币。幂等：已 done 的再点一次不会重复发币。 */
function approveTask(doc, cfg, ts, taskId, title) {
  const t = doc.tasks.find((x) => x.id === taskId) || doc.tasks.find((x) => norm(x.title).includes(norm(title || '')));
  if (!t) return { ok: false, code: 'NOT_FOUND', say: '没找到这个任务。' };
  if (t.status === 'done') return { ok: false, code: 'ALREADY', say: '该任务已经发过币了。' };

  t.status = 'done';
  t.doneAt = ts;
  doc.pet.coins += t.reward;
  pushCoinLog(doc, cfg, ts, t.reward, '完成任务：' + t.title, 'parent');
  return { ok: true, kind: 'task_approve', say: `已确认「${t.title}」，发放 ${t.reward} 枚游戏币。`, coins: doc.pet.coins };
}

function rejectTask(doc, cfg, ts, taskId, title) {
  const t = doc.tasks.find((x) => x.id === taskId) || doc.tasks.find((x) => norm(x.title).includes(norm(title || '')));
  if (!t) return { ok: false, code: 'NOT_FOUND', say: '没找到这个任务。' };
  if (t.status !== 'submitted') return { ok: false, code: 'BAD_STATE', say: '这个任务现在不是待确认状态。' };
  t.status = 'pending';
  t.submittedAt = null;
  return { ok: true, kind: 'task_reject', say: `「${t.title}」先退回，让孩子再检查一下。` };
}

// ------------------------------------------------------------------ 状态裁剪

/**
 * 控制 state 体积：整份 JSON 要在设备 NVS 里存得下（目标 < 3KB）。
 *
 * 裁剪策略不是「简单截断」——那样会把家长刚发的任务丢掉：
 *   未完成的任务（pending / submitted）优先保留，已完成只留最近 2 条；
 *   两者合计不超过 TASK_KEEP。
 * 金币流水只留最近 COIN_LOG_KEEP 条，且字段精简。
 */
function trimState(doc, cfg) {
  const live = doc.tasks.filter((t) => t.status !== 'done');
  const done = doc.tasks
    .filter((t) => t.status === 'done')
    .sort((a, b) => (b.doneAt || 0) - (a.doneAt || 0))
    .slice(0, 2);

  doc.tasks = live
    .concat(done)
    .sort((a, b) => (b.createdAt || 0) - (a.createdAt || 0))
    .slice(0, cfg.TASK_KEEP)
    // 空字段不落盘：单条任务能省 40-60 字节，十几条就是半 KB
    .map((t) => {
      const o = {
        id: t.id,
        title: String(t.title || '').slice(0, 40),
        reward: t.reward,
        status: t.status,
        createdAt: t.createdAt,
      };
      if (t.detail) o.detail = String(t.detail).slice(0, 60);
      if (t.submittedAt) o.submittedAt = t.submittedAt;
      if (t.doneAt) o.doneAt = t.doneAt;
      if (t.submitNote) o.submitNote = String(t.submitNote).slice(0, 40);
      return o;
    });

  doc.coinLog = doc.coinLog.slice(0, cfg.COIN_LOG_KEEP).map((c) => ({
    ts: c.ts,
    delta: c.delta,
    reason: String(c.reason || '').slice(0, 20),
  }));
  return doc;
}

// ------------------------------------------------------------------ 统一入口

/**
 * 唯一入口。固件只认这一个函数，不用关心内部有多少种动作。
 *
 * @param {object} req {
 *   action: string,      // sync / feed / play / heal / medicine / sleep / story / song /
 *                        // teach / pet / chat / task_list / task_submit / task_publish /
 *                        // task_approve / task_reject / coins / reset / ping
 *   state: object|null,  // 设备带来的完整状态；null = 首次，自动初始化
 *   args: object,        // 动作参数：food / title / reward / topic / what / task_id ...
 *   ts: number,          // 设备时间（秒）；不传则用服务器时间
 * }
 * @param {object} env 环境变量（可覆盖数值配置）
 */
function run(req, env) {
  const cfg = resolveConfig(env);
  const r = req || {};
  const args = r.args && typeof r.args === 'object' ? r.args : {};
  const action = String(r.action || 'sync').toLowerCase();

  const serverTs = nowSec();
  const clientTs = num(r.ts, 0);
  // 设备时间只作参考：偏差超过 5 分钟就一律以服务器时间为准，
  // 避免设备时钟错乱导致「一开机就饿了 20 小时」。
  // skew 记录的是「服务器 - 设备」，供固件判断自己时钟准不准。
  const skew = clientTs > 0 ? serverTs - clientTs : 0;
  const ts = clientTs > 0 && Math.abs(skew) <= 300 ? clientTs : serverTs;

  let doc = normalizeDoc(r.state, cfg, ts);
  const fresh = !r.state;

  if (action === 'ping') {
    return {
      ok: true, code: 'PONG', say: '', action,
      server_ts: serverTs, fresh,
      config: {
        decay_step_hours: cfg.DECAY_STEP_HOURS,
        full_hunger: cfg.FULL_HUNGER,
        daily_fun_cap: cfg.DAILY_FUN_CAP,
        exp_per_level: cfg.EXP_PER_LEVEL,
      },
      catalog: {
        foods: FOODS.map((f) => ({ name: f.name, cost: f.cost })),
        meds: MEDS.map((m) => ({ name: m.name, cost: m.cost, cure: !!m.cure })),
      },
    };
  }

  if (action === 'reset') {
    doc = initDoc(cfg, ts);
    return pack(doc, cfg, ts, serverTs, skew, { ok: true, code: 'RESET', say: '已经重新开始养啦。', action, fresh: true });
  }

  // 所有动作之前统一补算衰减 —— 保证「先饿后喂」的因果正确
  const steps = applyDecay(doc, cfg, ts);

  let res;
  switch (action) {
    case 'sync':
      res = { ok: true, code: 'SYNC', say: '' };
      break;
    case 'feed':
      res = feed(doc, cfg, ts, args.food);
      break;
    case 'medicine':
      res = giveMedicine(doc, cfg, ts, args.medicine || args.med);
      break;
    case 'play':
      res = play(doc, cfg, ts);
      break;
    case 'heal':
      res = heal(doc, cfg, ts);
      break;
    case 'sleep':
      res = sleep(doc, cfg, ts);
      break;
    case 'story':
      res = tellStory(doc, cfg, ts, args.topic);
      break;
    case 'song':
      res = singSong(doc, cfg, ts);
      break;
    case 'teach':
      res = teach(doc, cfg, ts, args.what);
      break;
    case 'pet':
      res = petHead(doc, cfg, ts);
      break;
    case 'chat':
      res = chat(doc, cfg, ts, args.text);
      break;
    case 'task_list':
      res = { ok: true, code: 'TASKS', say: '', tasks: listTasks(doc, cfg, !!args.include_done) };
      break;
    case 'task_submit':
      res = submitTask(doc, cfg, ts, args.title, args.note);
      break;
    case 'task_publish':
      res = publishTask(doc, cfg, ts, args.title, args.detail, args.reward);
      break;
    case 'task_approve':
      res = approveTask(doc, cfg, ts, args.task_id, args.title);
      break;
    case 'task_reject':
      res = rejectTask(doc, cfg, ts, args.task_id, args.title);
      break;
    case 'coins':
      res = { ok: true, code: 'COINS', say: `我现在有 ${doc.pet.coins} 枚游戏币。`, coins: doc.pet.coins };
      break;
    default:
      res = { ok: false, code: 'UNKNOWN_ACTION', say: `不认识的指令：${action}` };
      break;
  }

  if (steps > 0) res.decay_steps = steps;
  return pack(doc, cfg, ts, serverTs, skew, Object.assign({ action, fresh }, res));
}

/** 把结果包成统一响应：状态、话术、事件、新 state */
function pack(doc, cfg, ts, serverTs, skew, res) {
  doc.meta = Object.assign({}, doc.meta, { updatedAt: ts });
  if (!doc.v) doc.v = 0;
  doc.v += 1;
  trimState(doc, cfg);

  const st = getStatus(doc, cfg, ts);
  return Object.assign({}, res, {
    status: st,
    state: doc,
    server_ts: serverTs,
    skew_sec: skew,
    events: statusEvents(st),
    mood_word: moodWord(st),
    // 没有专门话术时（sync/chat/task_list），用状态心声兜底，模型总有话可说
    say: res.say || moodWord(st),
  });
}

module.exports = {
  DEFAULTS,
  FOODS,
  MEDS,
  resolveConfig,
  initDoc,
  normalizeDoc,
  applyDecay,
  getStatus,
  statusEvents,
  moodWord,
  run,
  nowSec,
  dayStartSec,
  foodNames,
  medNames,
};
