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
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const SCF_DIR = path.join(__dirname, 'scf');
const OUT_DIR = path.join(__dirname, 'dist');
const OUT_ZIP = path.join(OUT_DIR, 'pet-cloud-scf.zip');

// 要打进包的文件（顺序固定，便于比对）
const FILES = ['index.js', 'pet-core.js', 'package.json'];

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
} catch (err) {
  check(false, 'GET 自检页', err.message);
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

/** 固定时间戳，保证同样内容打出来的 zip 字节一致（便于校验） */
const DOS_TIME = (12 << 11) | (0 << 5) | (0 >> 1);
const DOS_DATE = ((2026 - 1980) << 9) | (1 << 5) | 1;

function makeZip(entries) {
  const locals = [];
  const centrals = [];
  let offset = 0;

  for (const e of entries) {
    const name = Buffer.from(e.name.replace(/\\/g, '/'), 'utf8');
    const data = e.data;
    const crc = crc32(data);

    const lh = Buffer.alloc(30);
    lh.writeUInt32LE(0x04034b50, 0);
    lh.writeUInt16LE(20, 4);          // version needed
    lh.writeUInt16LE(0x0800, 6);      // flags: UTF-8
    lh.writeUInt16LE(0, 8);           // store
    lh.writeUInt16LE(DOS_TIME, 10);
    lh.writeUInt16LE(DOS_DATE, 12);
    lh.writeUInt32LE(crc, 14);
    lh.writeUInt32LE(data.length, 18);
    lh.writeUInt32LE(data.length, 22);
    lh.writeUInt16LE(name.length, 26);
    lh.writeUInt16LE(0, 28);
    locals.push(lh, name, data);

    const ch = Buffer.alloc(46);
    ch.writeUInt32LE(0x02014b50, 0);
    ch.writeUInt16LE(20, 4);          // version made by
    ch.writeUInt16LE(20, 6);          // version needed
    ch.writeUInt16LE(0x0800, 8);
    ch.writeUInt16LE(0, 10);
    ch.writeUInt16LE(DOS_TIME, 12);
    ch.writeUInt16LE(DOS_DATE, 14);
    ch.writeUInt32LE(crc, 16);
    ch.writeUInt32LE(data.length, 20);
    ch.writeUInt32LE(data.length, 24);
    ch.writeUInt16LE(name.length, 28);
    ch.writeUInt16LE(0, 30);          // extra
    ch.writeUInt16LE(0, 32);          // comment
    ch.writeUInt16LE(0, 34);          // disk
    ch.writeUInt16LE(0, 36);          // internal attr
    ch.writeUInt32LE(0, 38);          // external attr
    ch.writeUInt32LE(offset, 42);
    centrals.push(Buffer.concat([ch, name]));

    offset += lh.length + name.length + data.length;
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
while (p < zip.length - 22 && zip.readUInt32LE(p) === 0x04034b50) {
  const nameLen = zip.readUInt16LE(p + 26);
  const compSize = zip.readUInt32LE(p + 18);
  const name = zip.slice(p + 30, p + 30 + nameLen).toString('utf8');
  if (name.includes('\\') || name.startsWith('/') || name.includes('../')) badPath = true;
  seen.push(name);
  p += 30 + nameLen + compSize;
}
check(!badPath, 'zip 内路径全部使用正斜杠且无目录前缀');
check(FILES.every((f) => seen.includes(f)), '条目与预期一致：' + seen.join(', '));

fs.mkdirSync(OUT_DIR, { recursive: true });
fs.writeFileSync(OUT_ZIP, zip);

const kb = (zip.length / 1024).toFixed(1);
check(zip.length > 1024 && zip.length < 1024 * 1024, `产物大小合理（${kb} KB）`);

// ---------------------------------------------------------------- 汇总
console.log('\n' + '─'.repeat(52));
if (checkFail > 0) die(`自检未通过（失败 ${checkFail} 项），未生成 zip`);

console.log(`自检全部通过（${checkPass} 项）`);
console.log(`\n产物：${OUT_ZIP}`);
console.log(`大小：${kb} KB`);
console.log('\n下一步：');
console.log('  1. 腾讯云 SCF 控制台 → 新建函数 → 本地上传 zip → 选上面这个文件');
console.log('  2. 执行超时改 10 秒，内存 128 MB，入口 index.main_handler');
console.log('  3. 环境变量可设 PET_KEY（建议）、PET_NAME（可选）');
console.log('  4. 创建函数 URL，把地址和口令填进固件的 pet_config.h');
console.log('  5. 浏览器打开函数 URL，能看到中文自检页就说明活了');
