/* 线上链路验收：下发一条 pet 指令，盯 ack 是否回到云端 */
const BASE = 'https://1317918978-ebb04d018b.ap-guangzhou.tencentscf.com/?page=parent&api=1&key=qiaoli-2026';

const at = Date.now();
console.log('指令 at =', at, '(' + new Date(at).toLocaleString('zh-CN') + ')');

async function post() {
  const r = await fetch(BASE, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ action: 'pet', note: '链路验收-抚摸', at }),
  });
  const t = await r.text();
  console.log('POST', r.status, t.slice(0, 300));
}

async function get() {
  const r = await fetch(BASE);
  return await r.json();
}

const sleep = (ms) => new Promise((s) => setTimeout(s, ms));

(async () => {
  await post();

  const deadline = Date.now() + 9 * 60 * 1000;
  let last = '';
  let n = 0;
  while (Date.now() < deadline) {
    n++;
    let d;
    try {
      d = await get();
    } catch (e) {
      console.log(`[${n}] 拉取失败 ${e.message}`);
      await sleep(15000);
      continue;
    }
    const line =
      `[${n}] pending=${d.pending ? d.pending.length : '?'} ack=${d.ack} ` +
      `age=${d.age_sec} 状态=${d.status ? `体力${d.status.energy} 心情${d.status.mood}` : '空'}`;
    if (line !== last) {
      console.log(line + '  ' + new Date().toLocaleTimeString('zh-CN'));
      last = line;
    }
    if (Number(d.ack) >= at) {
      console.log('✅ 回执已到云端，链路打通');
      if (d.events && d.events[0]) console.log('   最新动态：', JSON.stringify(d.events[0]));
      return;
    }
    await sleep(15000);
  }
  console.log('⏱ 9 分钟内没等到回执（设备可能已关机或未联网）');
})();
