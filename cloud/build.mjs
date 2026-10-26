#!/usr/bin/env node
/**
 * 打包成可直接上传腾讯云 SCF 的 zip
 *
 * 为什么自己拼 zip 而不用系统工具：
 *   Windows 上 PowerShell 压出来的路径是 `scf\index.js`（反斜杠），
 *   ZIP 规范要求 `/`。Linux 解压时会把整串当成一个文件名，
 *   云函数启动直接 MODULE_NOT_FOUND。这个坑踩过一次，不再踩第二次。
 *
 * 自检 5 项，任何一项不过就不产出 zip —— 拿到包就说明包本身没问题。
 *
 * 用法：node build.mjs
 */

import fs from 'node:fs';
import path from 'node:path';
import zlib from 'node:zlib';
import crypto from 'node:crypto';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const SCF_DIR = path.join(__dirname, 'scf');
const OUT_DIR = path.join(__dirname, 'dist');
const OUT_ZIP = path.join(OUT_DIR, 'pet-cloud-scf.zip');
const STAMP = path.join(OUT_DIR, 'scf.stamp.json');

// 要打进包的文件（顺序固定，便于比对）
const FILES = ['index.js', 'pet-core.js', 'store.js', 'parent.js', 'package.json'];

/* ---------------------------------------------------------------- 指纹
 * 只看 mtime 会骗人：改完又改回来、系统时钟跳一下、从 git 拉一份旧文件覆盖，
 * mtime 都可能是"新"的。所以改成按内容算 sha256。
 */
function sha256(buf) {
  return crypto.createHash('sha256').update(buf).digest('hex');
}

function sourceDigest() {
  const h = crypto.createHash('sha256');
  const files = {};
  const missing = [];
  for (const f of FILES) {
    const p = path.join(SCF_DIR, f);
    if (!fs.existsSync(p)) {
      missing.push(f);
      continue;
    }
    const buf = fs.readFileSync(p);
    files[f] = { size: buf.length, sha256: sha256(buf) };
    h.update(f).update('\0').update(buf).update('\0');
  }
  return { digest: h.digest('hex'), files, missing };
}

function readVersion() {
  try {
    const src = fs.readFileSync(path.join(SCF_DIR, 'index.js'), 'utf8');
    const m = src.match(/const\s+VERSION\s*=\s*['"]([^'"]+)['"]/);
    return m ? m[1] : 'unknown';
  } catch (_) {
    return 'unknown';
  }
}

let checkPass = 0;
let checkFail = 0;

function check(cond, label, detail) {
  if (cond) {
    checkPass++;
    console.log(`  ✓ ${label}`);
  } else {
    checkFail++;
    console.log(`  ✗ ${label}${detail ? '  → ' + detail : ''}`);
  }
}

function die(msg) {
  console.error(`\n✗ ${msg}`);
  process.exit(1);
}

/* ---------------------------------------------------------------- --check
 * 只读校验，不打包、不跑冒烟测试，一秒出结果，适合"上传前先看一眼"。
 *   node build.mjs --check          人话输出
 *   node build.mjs --check --json   给脚本用
 * 退出码：0 = 包是最新的；1 = 过期/缺失
 */
if (process.argv.includes('--check')) {
  const asJson = process.argv.includes('--json');
  const now = sourceDigest();
  const ver = readVersion();
  const out = { ok: false, version: ver, zip: OUT_ZIP, problems: [] };

  if (now.missing.length) {
    out.problems.push('源文件缺失：' + now.missing.join(', '));
  }
  if (!fs.existsSync(OUT_ZIP)) {
    out.problems.push('还没有 zip，先跑 node build.mjs');
  }
  if (!fs.existsSync(STAMP)) {
    out.problems.push('zip 存在但缺少指纹记录 dist/scf.stamp.json（老包，无法判定），重跑 node build.mjs 即可');
  }

  if (!out.problems.length) {
    let stamp;
    try {
      stamp = JSON.parse(fs.readFileSync(STAMP, 'utf8'));
    } catch (err) {
      out.problems.push('指纹文件读不动：' + err.message);
    }

    if (stamp) {
      out.built_at = stamp.built_at;
      out.built_digest = stamp.digest;
      out.digest = now.digest;

      // 1) 源文件有没有动过
      if (stamp.digest !== now.digest) {
        const changed = Object.keys(now.files).filter((f) => {
          const old = (stamp.files || {})[f];
          return !old || old.sha256 !== now.files[f].sha256;
        });
        const added = Object.keys(now.files).filter((f) => !(stamp.files || {})[f]);
        const detail = Array.from(new Set(changed.concat(added)));
        out.problems.push('源码已改动，zip 过期：' + (detail.join(', ') || '未知文件'));
      }

      // 2) zip 本身有没有被替换/手工改过
      const zipBuf = fs.readFileSync(OUT_ZIP);
      const zipSha = sha256(zipBuf);
      out.zip_sha256 = zipSha;
      out.zip_size = zipBuf.length;
      if (stamp.zip_sha256 && stamp.zip_sha256 !== zipSha) {
        out.problems.push('zip 内容与打包记录不一致（被替换或损坏），重跑 node build.mjs');
      }

      // 3) 版本号对不上也要提醒
      if (stamp.version && stamp.version !== ver) {
        out.problems.push(`版本号不一致：源码 v${ver}，打包时 v${stamp.version}`);
      }
    }
  }

  out.ok = out.problems.length === 0;

  if (asJson) {
    console.log(JSON.stringify(out, null, 2));
  } else {
    console.log('');
    console.log(`  源码版本   v${ver}`);
    console.log(`  源码指纹   ${(out.digest || now.digest).slice(0, 16)}…`);
    if (out.built_at) {
      console.log(`  打包时间   ${out.built_at}`);
      console.log(`  包内指纹   ${String(out.built_digest).slice(0, 16)}…`);
      console.log(`  包大小     ${(out.zip_size / 1024).toFixed(1)} KB`);
    }
    console.log('');
    if (out.ok) {
      console.log('  ✓ 本机 zip 是最新的，可以直接上传');
      console.log(`    ${OUT_ZIP}`);
    } else {
      console.log('  ✗ 不能直接上传：');
      for (const p of out.problems) console.log('    · ' + p);
      console.log('');
      console.log('  修法：node build.mjs   （重打包 + 40 项自检）');
    }
    console.log('');
  }
  process.exit(out.ok ? 0 : 1);
}

// ---------------------------------------------------------------- 1. 文件齐不齐
console.log('\n自检 1/5 · 文件完整性');
for (const f of FILES) {
  const p = path.join(SCF_DIR, f);
  check(fs.existsSync(p), `${f} 存在`, p);
  if (fs.existsSync(p)) {
    const size = fs.statSync(p).size;
    check(size > 0 && size < 200 * 1024, `${f} 体积合理（${size} B）`);
  }
}
if (checkFail > 0) die('文件不全，先补齐再打包');

// ---------------------------------------------------------------- 2. 零外部依赖
console.log('\n自检 2/5 · 依赖');
const pkg = JSON.parse(fs.readFileSync(path.join(SCF_DIR, 'package.json'), 'utf8'));
const deps = Object.keys(pkg.dependencies || {});
check(deps.length === 0, '不依赖任何第三方包（上传时无需在线安装依赖）', deps.join(','));
check(pkg.main === 'index.js', '入口指向 index.js', pkg.main);

// ---------------------------------------------------------------- 3. 语法可加载
console.log('\n自检 3/5 · 模块加载');
const require = createRequire(path.join(__dirname, 'x.js'));
let core;
let handler;
try {
  core = require('./scf/pet-core.js');
  check(true, 'pet-core.js 可加载');
  check(typeof core.run === 'function', 'pet-core 导出 run()');
} catch (err) {
  check(false, 'pet-core.js 可加载', err.message);
}
try {
  const idx = require('./scf/index.js');
  handler = idx.main_handler;
  check(typeof handler === 'function', 'index.js 导出 main_handler');
} catch (err) {
  check(false, 'index.js 可加载', err.message);
}
if (checkFail > 0) die('模块加载失败，检查语法');

// ---------------------------------------------------------------- 4. 冒烟调用
console.log('\n自检 4/5 · 冒烟调用');

const postEvent = (body, headers) => ({
  httpMethod: 'POST',
  headers: Object.assign({ host: 'scf.example.com', 'x-forwarded-proto': 'https' }, headers || {}),
  body: typeof body === 'string' ? body : JSON.stringify(body),
  path: '/',
});

let smokeOk = true;
try {
  const r = await handler(postEvent({ action: 'sync', state: null }), {});
  const body = JSON.parse(r.body);
  smokeOk =
    r.statusCode === 200 &&
    body.ok === true &&
    body.status.hunger === 60 &&
    body.status.coins === 20 &&
    body.state && body.state.pet;
  check(smokeOk, 'POST {action:sync,state:null} 返回 200 且完成初始化',
    r.statusCode + ' ' + (r.body || '').slice(0, 120));
  check(body.events === undefined ? false : Array.isArray(body.events), '返回 events 数组');
  check(typeof body.say === 'string' && body.say.length > 0, '返回话术');
} catch (err) {
  check(false, '冒烟调用', err.message);
}

// 喂食链路
try {
  const s = JSON.parse((await handler(postEvent({ action: 'sync', state: null }), {})).body).state;
  s.pet.lastActionAt = 0;
  const r = await handler(postEvent({ action: 'feed', state: s, args: { food: '苹果' } }), {});
  const body = JSON.parse(r.body);
  check(body.ok && body.status.hunger === 80 && body.status.coins === 18, '喂苹果：饱食 +20 / 金币 -2');
} catch (err) {
  check(false, '喂食链路', err.message);
}

// 浏览器直接打开（GET）要能看到自检页
try {
  const r = await handler({ httpMethod: 'GET', headers: { host: 'scf.example.com' }, path: '/' }, {});
  check(r.statusCode === 200 && String(r.body).includes('宠物云端引擎'), 'GET 返回中文自检页');
  check(String(r.body).includes('page=parent'), '自检页里有家长看板入口');
} catch (err) {
  check(false, 'GET 自检页', err.message);
}

// 家长看板：页面壳 + 空快照 + 排队 → 设备上报时送达
try {
  const r = await handler(
    { httpMethod: 'GET', headers: { host: 'scf.example.com' }, path: '/', queryString: { page: 'parent' } },
    {}
  );
  check(
    r.statusCode === 200 && String(r.body).includes('家长看板') && String(r.body).includes('<style>'),
    'GET ?page=parent 返回看板页'
  );
} catch (err) {
  check(false, 'GET 家长看板', err.message);
}

try {
  fs.mkdirSync(OUT_DIR, { recursive: true });
  /* 云函数读的是 process.env，测试就照着改，别造一个假的 env 对象 */
  const storeFile = path.join(OUT_DIR, '.smoke-store.json');
  process.env.PET_STORE_FILE = storeFile;
  const env = {};

  const apiGet = (q) => ({
    httpMethod: 'GET',
    headers: { host: 'scf.example.com' },
    path: '/',
    queryString: Object.assign({ page: 'parent', api: '1' }, q || {}),
  });
  const apiPost = (body) => ({
    httpMethod: 'POST',
    headers: { host: 'scf.example.com' },
    path: '/',
    queryString: { page: 'parent', api: '1' },
    body: JSON.stringify(body),
  });

  /* 先让设备上报一次，快照才有东西 */
  const s = JSON.parse((await handler(postEvent({ action: 'sync', state: null }), env)).body).state;
  await handler(postEvent({ action: 'sync', state: s }), env);

  const snap = JSON.parse((await handler(apiGet(), env)).body);
  check(snap.ok && snap.state_present && snap.status && snap.status.coins >= 0, '看板能读到设备快照');
  check(snap.backend === 'file', '快照后端识别为 file');

  /* 家长点「喂饭」→ 排队 */
  const q = JSON.parse((await handler(apiPost({ cmd: 'feed', args: { food: '苹果' } }), env)).body);
  check(q.ok && q.queued === 1, '看板点喂饭进入队列');

  /* 设备再上报 → 队列被消费，饱食度真的涨了 */
  const dres = await handler(postEvent({ action: 'sync', state: s }), env);
  const sDelivered = JSON.parse(dres.body).state;   /* 带 cmdAck 的那份，设备会存进 NVS */
  const after = JSON.parse((await handler(apiGet(), env)).body);
  check(after.pending.length === 0, '设备上报后队列被清空（指令已送达）');
  check(
    after.status && after.status.hunger === 80 && after.status.coins === 18,
    '排队指令真的生效：饱食 60→80、金币 20→18',
    after.status ? 'hunger=' + after.status.hunger + ' coins=' + after.status.coins : 'no status'
  );
  check(
    after.events.some((e) => String(e.text).indexOf('已送达') >= 0),
    '送达结果写进了家长事件流'
  );

  /* ---- 幂等与回执 ----
     云函数会同时活着好几个实例，指令只排在其中一个的内存里，
     设备心跳可能落到别的实例 -> 看板收不到回执就会重发。
     所以必须保证：同一条指令重发多少次，都只生效一次。 */
  const d1res = await handler(postEvent({ action: 'sync', state: sDelivered }), env);
  const ackAt = JSON.parse(d1res.body).state.pet.cmdAck;
  check(ackAt > 0, '送达后 state.pet.cmdAck 被写上（设备会把它存进 NVS）', { ackAt });

  /* 模拟看板重发同一条（同一时刻的 at），落到「另一个实例」 */
  const retry = JSON.parse((await handler(apiPost({ cmd: 'feed', args: { food: '苹果' }, at: ackAt }), env)).body);
  check(retry.ok && retry.at === ackAt, '重发沿用原来的 at（云端据此认出是同一条）');

  /* 设备带着 cmdAck 上报：这条必须被跳过，不能二次喂饭 */
  const d2res = await handler(postEvent({ action: 'sync', state: sDelivered }), env);
  const d2 = JSON.parse(d2res.body);
  check(
    Array.isArray(d2.applied) && d2.applied[0] && d2.applied[0].skipped === true,
    '重复指令被判为「已执行过」而不是再执行一次',
    d2.applied
  );
  check(d2.status.coins === 18 && d2.status.hunger === 80, '重发没有让金币和饱食度再变一次',
        'hunger=' + d2.status.hunger + ' coins=' + d2.status.coins);

  /* 队列层去重：同一条（同 action + 同 at）提交两次只排一条 */
  const atX = Date.now();
  await handler(apiPost({ cmd: 'play', args: {}, at: atX }), env);
  const dup = JSON.parse((await handler(apiPost({ cmd: 'play', args: {}, at: atX }), env)).body);
  check(dup.queued === 1, '同一条指令重复提交只排一条', { queued: dup.queued });

  /* 看板能读到回执号，用来判断「已送达」 */
  const snap2 = JSON.parse((await handler(apiGet(), env)).body);
  check(snap2.ack === ackAt, '看板能从快照里读到回执号', { ack: snap2.ack, ackAt });

  /* 非法指令要拒绝，避免看板被当成提币口 */
  const bad = await handler(apiPost({ cmd: 'coins', args: { delta: 999 } }), env);
  check(bad.statusCode === 400, '看板拒绝 coins 这类敏感动作');

  try {
    fs.unlinkSync(storeFile);
  } catch (_) {}
  delete process.env.PET_STORE_FILE;
} catch (err) {
  check(false, '家长看板链路', err.message + '\n' + err.stack);
}

// 定时触发不能崩
try {
  const r = await handler({ Type: 'Timer', TriggerName: 'pet-tick' }, {});
  check(r && r.ok === true && r.msg === 'pet-tick', '定时触发返回心跳');
} catch (err) {
  check(false, '定时触发', err.message);
}

// 鉴权
try {
  process.env.PET_KEY = 'test-key-123';
  const bad = await handler(postEvent({ action: 'sync', state: null }), {});
  check(bad.statusCode === 401, '口令不对时返回 401');
  const good = await handler(postEvent({ action: 'sync', state: null }, { 'x-pet-key': 'test-key-123' }), {});
  check(good.statusCode === 200, '口令正确时放行');
  delete process.env.PET_KEY;
} catch (err) {
  check(false, '鉴权', err.message);
  delete process.env.PET_KEY;
}

// 坏 JSON 不能崩
try {
  const r = await handler(postEvent('{不是json'), {});
  check(r.statusCode === 200 || r.statusCode === 400, '坏 JSON 有兜底（不抛异常）', String(r.statusCode));
} catch (err) {
  check(false, '坏 JSON 兜底', err.message);
}

if (checkFail > 0) die('冒烟测试未通过，不产出 zip');

// ---------------------------------------------------------------- 5. 拼 zip
console.log('\n自检 5/5 · 打包');

const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let i = 0; i < 256; i++) {
    let c = i;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[i] = c >>> 0;
  }
  return t;
})();

function crc32(buf) {
  let c = 0xffffffff;
  for (let i = 0; i < buf.length; i++) c = CRC_TABLE[(c ^ buf[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

/**
 * 固定时间戳，保证同样内容打出来的 zip 字节一致（便于校验）
 */
const DOS_TIME = (12 << 11) | (0 << 5) | (0 >> 1);
const DOS_DATE = ((2026 - 1980) << 9) | (1 << 5) | 1;

/**
 * 压缩方式用 deflate（method=8），不用 store（method=0）。
 *
 * 最初写的是 store —— 结构合法、本地能解开，但腾讯云 BuildCodeViaSCF 那一步
 * 对"全不压缩"的包不友好，实测报 `EntryFile did not find in code or layers`。
 * 换成 deflate 顺带把 64 KB 压到 15 KB，上传更快，也更接近各大工具产出的常规包。
 */
function deflate(buf) {
  const out = zlib.deflateRawSync(buf, { level: 9 });
  // 极小文件压缩后可能反而更大，那就老实存原文，仍是合法的 zip
  return out.length < buf.length ? out : null;
}

function makeZip(entries) {
  const locals = [];
  const centrals = [];
  let offset = 0;

  for (const e of entries) {
    const name = Buffer.from(e.name.replace(/\\/g, '/'), 'utf8');
    const data = e.data;
    const crc = crc32(data);
    const packed = deflate(data);
    const body = packed || data;
    const method = packed ? 8 : 0;

    const lh = Buffer.alloc(30);
    lh.writeUInt32LE(0x04034b50, 0);
    lh.writeUInt16LE(20, 4);          // version needed
    lh.writeUInt16LE(0x0800, 6);      // flags: UTF-8
    lh.writeUInt16LE(method, 8);      // 8 = deflate, 0 = store
    lh.writeUInt16LE(DOS_TIME, 10);
    lh.writeUInt16LE(DOS_DATE, 12);
    lh.writeUInt32LE(crc, 14);
    lh.writeUInt32LE(body.length, 18);   // 压缩后
    lh.writeUInt32LE(data.length, 22);   // 原始
    lh.writeUInt16LE(name.length, 26);
    lh.writeUInt16LE(0, 28);
    locals.push(lh, name, body);

    const ch = Buffer.alloc(46);
    ch.writeUInt32LE(0x02014b50, 0);
    ch.writeUInt16LE(20, 4);          // version made by
    ch.writeUInt16LE(20, 6);          // version needed
    ch.writeUInt16LE(0x0800, 8);
    ch.writeUInt16LE(method, 10);
    ch.writeUInt16LE(DOS_TIME, 12);
    ch.writeUInt16LE(DOS_DATE, 14);
    ch.writeUInt32LE(crc, 16);
    ch.writeUInt32LE(body.length, 20);
    ch.writeUInt32LE(data.length, 24);
    ch.writeUInt16LE(name.length, 28);
    ch.writeUInt16LE(0, 30);          // extra
    ch.writeUInt16LE(0, 32);          // comment
    ch.writeUInt16LE(0, 34);          // disk
    ch.writeUInt16LE(0, 36);          // internal attr
    ch.writeUInt32LE(0, 38);          // external attr
    ch.writeUInt32LE(offset, 42);
    centrals.push(Buffer.concat([ch, name]));

    offset += lh.length + name.length + body.length;
  }

  const cd = Buffer.concat(centrals);
  const eocd = Buffer.alloc(22);
  eocd.writeUInt32LE(0x06054b50, 0);
  eocd.writeUInt16LE(0, 4);
  eocd.writeUInt16LE(0, 6);
  eocd.writeUInt16LE(entries.length, 8);
  eocd.writeUInt16LE(entries.length, 10);
  eocd.writeUInt32LE(cd.length, 12);
  eocd.writeUInt32LE(offset, 16);
  eocd.writeUInt16LE(0, 20);

  return Buffer.concat([Buffer.concat(locals), cd, eocd]);
}

const entries = FILES.map((f) => ({
  name: f,
  data: fs.readFileSync(path.join(SCF_DIR, f)),
}));

// package.json 里的 main 必须是 index.js，且不能带 "type":"module"（否则 CJS 会报错）
const pkgText = entries.find((e) => e.name === 'package.json').data.toString('utf8');
check(!/"type"\s*:\s*"module"/.test(pkgText), 'package.json 未声明 type:module（SCF 走 CommonJS）');

const zip = makeZip(entries);

// zip 结构自检
check(zip.readUInt32LE(0) === 0x04034b50, 'zip 以 PK\\x03\\x04 开头');
const eocdPos = zip.length - 22;
check(zip.readUInt32LE(eocdPos) === 0x06054b50, 'zip 以 EOCD 结尾');
check(zip.readUInt16LE(eocdPos + 8) === FILES.length, `zip 内含 ${FILES.length} 个条目`);

// 路径必须是正斜杠，且不能有目录前缀
let badPath = false;
let p = 0;
const seen = [];
const methods = [];
const badRoundtrip = [];
while (p < zip.length - 22 && zip.readUInt32LE(p) === 0x04034b50) {
  const nameLen = zip.readUInt16LE(p + 26);
  const method = zip.readUInt16LE(p + 8);
  const compSize = zip.readUInt32LE(p + 18);
  const name = zip.slice(p + 30, p + 30 + nameLen).toString('utf8');
  if (name.includes('\\') || name.startsWith('/') || name.includes('../')) badPath = true;
  methods.push(method);
  seen.push(name);

  // 反向验证：用标准 inflate 把这一个条目解回来，和源文件逐字节比
  const body = zip.slice(p + 30 + nameLen, p + 30 + nameLen + compSize);
  try {
    const raw = method === 8 ? zlib.inflateRawSync(body) : body;
    const src = fs.readFileSync(path.join(SCF_DIR, name));
    if (!raw.equals(src)) badRoundtrip.push(name + '（内容不一致）');
  } catch (err) {
    badRoundtrip.push(name + '（' + err.message + '）');
  }

  p += 30 + nameLen + compSize;
}
check(!badPath, 'zip 内路径全部使用正斜杠且无目录前缀');
check(FILES.every((f) => seen.includes(f)), '条目与预期一致：' + seen.join(', '));
check(badRoundtrip.length === 0, '每个条目用标准 inflate 都能原样还原（云上解压器能读）', badRoundtrip.join('; '));
check(
  methods.every((m) => m === 8),
  '全部条目使用 deflate（method=8），不是未压缩的 store',
  '实际 method=' + methods.join(',')
);

fs.mkdirSync(OUT_DIR, { recursive: true });
fs.writeFileSync(OUT_ZIP, zip);

const kb = (zip.length / 1024).toFixed(1);
check(zip.length > 1024 && zip.length < 1024 * 1024, `产物大小合理（${kb} KB）`);

// ---------------------------------------------------------------- 汇总
console.log('\n' + '─'.repeat(52));
if (checkFail > 0) die(`自检未通过（失败 ${checkFail} 项），未生成 zip`);

// 留一份指纹，供 `node build.mjs --check` 判定"本机 zip 是不是最新的"
const dg = sourceDigest();
fs.writeFileSync(
  STAMP,
  JSON.stringify(
    {
      version: readVersion(),
      built_at: new Date().toLocaleString('sv-SE', { timeZone: 'Asia/Shanghai' }).replace(' ', ' '),
      digest: dg.digest,
      files: dg.files,
      zip_sha256: sha256(zip),
      zip_size: zip.length,
      checks_passed: checkPass,
    },
    null,
    2
  ) + '\n'
);
console.log(`指纹已记录：${STAMP}`);

console.log(`自检全部通过（${checkPass} 项）`);
console.log(`\n产物：${OUT_ZIP}`);
console.log(`大小：${kb} KB`);
console.log('\n下一步：');
console.log('  1. 腾讯云 SCF 控制台 → 新建函数 → 本地上传 zip → 选上面这个文件');
console.log('  2. 执行超时改 10 秒，内存 128 MB，入口 index.main_handler');
console.log('  3. 环境变量可设 PET_KEY（建议）、PET_NAME（可选）');
console.log('  4. 创建函数 URL，把地址和口令填进固件的 pet_config.h');
console.log('  5. 浏览器打开函数 URL，能看到中文自检页就说明活了');
