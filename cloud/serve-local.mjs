#!/usr/bin/env node
/**
 * 本地试跑云函数（不需要腾讯云账号）
 *
 * 把 SCF 的 HTTP 事件格式在本地模拟出来，直接调 index.js 的 main_handler，
 * 所以行为和部署到云上一模一样 —— 可以先在本机把接口调通，再决定要不要上云。
 *
 * 用法：
 *   node serve-local.mjs            # 监听 8901
 *   node serve-local.mjs 9000       # 换端口
 *
 * 默认端口特意从 8899 挪到 8901：本机 8899 被小米电脑管家（MiPCAudio）占了，
 * 两个进程同时 listen 会出现「HTTP 解析失败 / 收到乱码」这种莫名其妙的现象。
 * 被占时脚本会直接报错并提示换端口，不会带着坏状态跑下去。
 *
 * 然后另开一个终端：
 *   curl -X POST http://127.0.0.1:8901/ -d '{"action":"sync","state":null}'
 *
 * 浏览器打开 http://127.0.0.1:8901/?page=parent 就是家长看板。
 */

import http from 'node:http';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

const require = createRequire(import.meta.url);
const __dirname = path.dirname(fileURLToPath(import.meta.url));

/* 本地跑就给快照找个落脚点：家长看板要能看到「刚刚上报的状态」，
   云上靠热实例内存撑着，本地没必要这么将就，直接写文件，重启也在。 */
if (!process.env.PET_STORE_FILE) {
  process.env.PET_STORE_FILE = path.join(__dirname, '.pet-store.local.json');
}

const { main_handler } = require(path.join(__dirname, 'scf/index.js'));

const PORT = Number(process.argv[2] || process.env.PORT || 8901);

const server = http.createServer(async (req, res) => {
  const chunks = [];
  for await (const c of req) {
    chunks.push(c);
    /* 防止误传大文件把内存打满 */
    if (chunks.reduce((n, b) => n + b.length, 0) > 64 * 1024) {
      res.writeHead(413).end('请求体过大');
      return;
    }
  }
  const body = Buffer.concat(chunks).toString('utf8');

  const url = new URL(req.url, `http://${req.headers.host || '127.0.0.1'}`);
  const query = {};
  for (const [k, v] of url.searchParams) query[k] = v;

  /* 组装成 SCF 函数 URL 的事件形状 */
  const event = {
    httpMethod: req.method,
    headers: Object.assign({}, req.headers, {
      host: req.headers.host || `127.0.0.1:${PORT}`,
      'x-forwarded-proto': 'http',
    }),
    queryString: query,
    body: body || undefined,
    path: url.pathname,
    isBase64Encoded: false,
  };

  const started = Date.now();
  try {
    const out = await main_handler(event, process.env);
    res.writeHead(out.statusCode || 200, out.headers || {});
    res.end(out.body || '');
    const ms = Date.now() - started;
    console.log(`${req.method} ${url.pathname}${url.search} → ${out.statusCode} · ${ms}ms`);
  } catch (err) {
    console.error('处理异常', err);
    res.writeHead(500, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ ok: false, code: 'INTERNAL', error: String(err && err.message) }));
  }
});

/* 端口被占时给出人话提示，而不是抛一个 EADDRINUSE 堆栈让人猜 */
server.on('error', (err) => {
  if (err && err.code === 'EADDRINUSE') {
    console.error(`\n✗ 端口 ${PORT} 已被别的程序占用。`);
    console.error('  换一个端口再跑，例如：node serve-local.mjs 8902\n');
    process.exit(1);
  }
  throw err;
});

server.listen(PORT, '127.0.0.1', () => {
  console.log(`宠物云端引擎 · 本地模式  http://127.0.0.1:${PORT}/`);
  console.log('');
  console.log(`家长看板     http://127.0.0.1:${PORT}/?page=parent`);
  console.log('自检页       http://127.0.0.1:' + PORT + '/');
  console.log('');
  console.log('命令行试一次初始化：');
  console.log(`  curl -X POST http://127.0.0.1:${PORT}/ -H 'Content-Type: application/json' \\`);
  console.log('       -d \'{"action":"sync","state":null}\'');
  console.log('');
  console.log(`快照文件：${process.env.PET_STORE_FILE}`);
  if (process.env.PET_KEY) {
    console.log(`已启用口令（来自环境变量 PET_KEY），请求需带 -H 'X-Pet-Key: ${process.env.PET_KEY}'`);
    console.log(`看板地址可以直接带口令：http://127.0.0.1:${PORT}/?page=parent&key=${process.env.PET_KEY}`);
  } else {
    console.log('未设 PET_KEY，本地调试不校验口令。');
  }
  console.log('按 Ctrl+C 停止。');
});
