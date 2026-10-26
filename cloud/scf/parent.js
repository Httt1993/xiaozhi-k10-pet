/**
 * 家长看板（手机优先的网页）
 *
 * 它不是给设备看的，是给家长手机浏览器看的。所以：
 *   - 单页、无外部资源（国内手机打开不依赖任何 CDN，断网也能显示壳）
 *   - 按钮点下去不是直接改状态，而是「排队」等设备下次联网时执行
 *   - 页面里所有脚本用单引号拼接，避免和外层模板字符串打架
 */

'use strict';

/** 页面里要用到的样式，单独拎出来，方便和 HTML 结构对照着改 */
const CSS = `
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
body{margin:0;background:#f6f2ea;color:#2a2723;
  font:15px/1.6 -apple-system,BlinkMacSystemFont,"Segoe UI","PingFang SC","Microsoft YaHei",sans-serif}
.wrap{max-width:520px;margin:0 auto;padding:16px 16px 40px}
header{display:flex;align-items:center;gap:10px;margin:6px 0 16px}
.avatar{width:52px;height:52px;border-radius:16px;flex:none;
  background:radial-gradient(circle at 50% 38%,#ffd9a6,#ffb066 62%,#e08b3c);
  display:flex;align-items:center;justify-content:center;font-size:26px;
  box-shadow:0 4px 14px rgba(224,139,60,.28)}
h1{font-size:19px;margin:0;line-height:1.25}
.sub{color:#8b8175;font-size:12.5px;margin-top:2px}
.dot{display:inline-block;width:7px;height:7px;border-radius:50%;margin-right:4px;vertical-align:middle}
.live{background:#3fb950}.stale{background:#d29922}.dead{background:#c9c3ba}

.card{background:#fff;border-radius:16px;padding:16px;margin-bottom:12px;
  box-shadow:0 1px 3px rgba(60,45,30,.06),0 8px 24px rgba(60,45,30,.04)}
.card h2{font-size:13px;margin:0 0 12px;color:#8b8175;font-weight:600;letter-spacing:.06em}

.bars{display:grid;gap:11px}
.bar .row{display:flex;justify-content:space-between;font-size:13.5px;margin-bottom:4px}
.bar .row b{font-variant-numeric:tabular-nums}
.track{height:9px;border-radius:99px;background:#efe9df;overflow:hidden}
.fill{height:100%;border-radius:99px;transition:width .45s ease}
.f1{background:linear-gradient(90deg,#f0a04b,#e8853a)}
.f2{background:linear-gradient(90deg,#63c9b4,#3fa88f)}
.f3{background:linear-gradient(90deg,#9d8cf0,#7b66dd)}

.meta{display:flex;gap:8px;margin-top:14px;flex-wrap:wrap}
.chip{background:#f6f2ea;border-radius:10px;padding:7px 11px;font-size:12.5px;color:#6b6258}
.chip b{color:#2a2723;font-size:14px;margin-left:3px}
.chip.warn{background:#fdecec;color:#b4423a}
.chip.warn b{color:#b4423a}

.acts{display:grid;grid-template-columns:repeat(4,1fr);gap:8px}
.act{border:1px solid #ece5d9;background:#fffdf9;border-radius:13px;padding:11px 4px;
  font:inherit;font-size:12.5px;color:#4a423a;cursor:pointer;transition:.15s}
.act span{display:block;font-size:20px;line-height:1.2;margin-bottom:3px}
.act:active{transform:scale(.96);background:#f7f1e6}
.act[disabled]{opacity:.45}

label{display:block;font-size:12.5px;color:#8b8175;margin:0 0 5px}
input{width:100%;font:inherit;padding:10px 12px;border:1px solid #e6dfd3;border-radius:11px;
  background:#fffdfa;color:#2a2723;outline:none}
input:focus{border-color:#e0a45c;box-shadow:0 0 0 3px rgba(224,164,92,.14)}
.form{display:grid;gap:10px}
.form .line{display:grid;grid-template-columns:1fr 92px;gap:8px}

button.primary{width:100%;border:0;border-radius:12px;padding:12px;font:inherit;
  font-weight:600;background:#e8853a;color:#fff;cursor:pointer}
button.primary:active{background:#d1742c}
button.primary[disabled]{background:#d9d2c6;cursor:default}

.task{border:1px solid #efe8dc;border-radius:12px;padding:11px 12px;margin-bottom:8px}
.task .t{font-size:14px;display:flex;justify-content:space-between;gap:8px;align-items:baseline}
.task .t em{font-style:normal;color:#c98a2e;font-size:12.5px;flex:none}
.task .s{font-size:12px;color:#8b8175;margin-top:3px}
.mini{display:flex;gap:8px;margin-top:9px}
.mini button{flex:1;border:1px solid #e6dfd3;background:#fffdfa;border-radius:9px;
  padding:7px;font:inherit;font-size:12.5px;color:#4a423a;cursor:pointer}
.mini button.ok{background:#eef8f0;border-color:#cbe8d2;color:#2f7a45}
.mini button.no{background:#fdf0ef;border-color:#f0d5d2;color:#b4423a}

.ev{font-size:12.5px;color:#6b6258;padding:7px 0;border-bottom:1px dashed #efe8dc;
  display:flex;gap:8px}
.ev:last-child{border-bottom:0}
.ev time{color:#b0a698;flex:none;font-variant-numeric:tabular-nums}
.empty{color:#b0a698;font-size:13px;text-align:center;padding:10px 0}

#gate{position:fixed;inset:0;background:rgba(246,242,234,.96);display:none;
  align-items:center;justify-content:center;padding:24px;z-index:9}
#gate .box{background:#fff;border-radius:16px;padding:20px;width:100%;max-width:340px;
  box-shadow:0 12px 40px rgba(60,45,30,.14)}
#gate h3{margin:0 0 6px;font-size:16px}
#gate p{margin:0 0 14px;font-size:13px;color:#8b8175}

#toast{position:fixed;left:50%;bottom:28px;transform:translate(-50%,14px);
  background:#2a2723;color:#fff;font-size:13px;padding:10px 16px;border-radius:99px;
  opacity:0;pointer-events:none;transition:.25s;max-width:88vw;z-index:10}
#toast.on{opacity:1;transform:translate(-50%,0)}
footer{text-align:center;color:#b0a698;font-size:11.5px;margin-top:18px;line-height:1.7}
`;

const JS = `
var KEY_STORE='pet_key';
var Q_STORE='pet_cmdq';        /* 我在这个手机上点过的指令，等设备回执 */
var RETRY_MS=30000;            /* 没回执就重发：云函数可能同时活着好几个实例，
                                  指令只排在其中一个的内存里，重发是为了让每个活着的实例都拿到 */
var GIVE_UP_MS=8*60*1000;      /* 8 分钟还没确认就不再自动重发（不删，等设备下次上报再说） */
var data=null;

function el(id){return document.getElementById(id)}

/* ---- 本机指令队列：解决「云函数多实例导致指令静默丢失」 ---- */
function qload(){
  try{ var a=JSON.parse(localStorage.getItem(Q_STORE)||'[]'); return a instanceof Array ? a : []; }
  catch(e){ return []; }
}
function qsave(a){
  try{ localStorage.setItem(Q_STORE, JSON.stringify(a.slice(-20))); }catch(e){}
}
/* 设备已经把指令处理到 at 号了（这个值存在设备 NVS 里，最可靠） */
function ackNo(){ return (data && data.ack) ? data.ack : 0; }
function myPending(){
  var a=ackNo(), all=qload(), out=[];
  for(var i=0;i<all.length;i++){ if(!(all[i].at<=a)) out.push(all[i]); }
  return out;
}
function qdrop(){
  var a=ackNo(), all=qload(), keep=[];
  for(var i=0;i<all.length;i++){ if(!(all[i].at<=a)) keep.push(all[i]); }
  if(keep.length!==all.length) qsave(keep);
}
function qpost(it){
  var h={'Content-Type':'application/json'};
  if(q()) h['X-Pet-Key']=q();
  return fetch(apiUrl(),{method:'POST',headers:h,body:JSON.stringify({cmd:it.cmd,args:it.args,at:it.at})})
    .then(function(r){return r.json()});
}
/* 每条没回执的指令再发一次（同一个 at，云端会去重，不会重复生效） */
function qretry(){
  var now=Date.now(), all=qload();
  for(var i=0;i<all.length;i++){
    var it=all[i];
    if(it.at<=ackNo()) continue;
    if(now-it.at>GIVE_UP_MS) continue;
    qpost(it).catch(function(){});
  }
}

/* 口令优先级：地址栏 ?key= > 本机记住的 */
function q(){
  try{
    var u=new URL(location.href);
    var k=u.searchParams.get('key');
    if(k){ localStorage.setItem(KEY_STORE,k); return k; }
  }catch(e){}
  return localStorage.getItem(KEY_STORE)||'';
}

/* API 地址按当前页面地址推出来，不写死 /parent 前缀 ——
   函数 URL、API 网关 /release/ 前缀两种情况都能用 */
function apiUrl(){
  var u=new URL(location.href);
  u.searchParams.set('page','parent');
  u.searchParams.set('api','1');
  return u.pathname+u.search;
}

function toast(msg){
  var t=el('toast'); t.textContent=msg; t.classList.add('on');
  clearTimeout(t._h); t._h=setTimeout(function(){t.classList.remove('on')},2600);
}

function ago(sec){
  if(!sec||sec<0) return '未知';
  if(sec<60) return '刚刚';
  if(sec<3600) return Math.floor(sec/60)+' 分钟前';
  if(sec<86400) return Math.floor(sec/3600)+' 小时前';
  return Math.floor(sec/86400)+' 天前';
}

function fmtTime(ts){
  if(!ts) return '--:--';
  if(ts>1e12) ts=Math.floor(ts/1000);   // 排队指令的 at 是毫秒
  var d=new Date(ts*1000);
  var p=function(n){return n<10?'0'+n:''+n};
  return p(d.getHours())+':'+p(d.getMinutes());
}

function bar(label,val,cls){
  var v=Math.max(0,Math.min(100,Math.round(val||0)));
  return '<div class="bar"><div class="row"><span>'+label+'</span><b>'+v+'</b></div>'+
    '<div class="track"><div class="fill '+cls+'" style="width:'+v+'%"></div></div></div>';
}

function statusWord(st){
  if(st.sick) return '生病了';
  if(st.hunger<=15) return '饿坏了';
  if(st.mood<=20) return '有点闷';
  if(st.hunger<=35) return '有点饿';
  return '状态不错';
}

function render(){
  if(!data||!data.ok){ return; }
  var st=data.status;
  if(!st){
    el('body').innerHTML='<div class="card"><div class="empty">这一会儿还没有设备上报的数据。<br>'+
      '设备每 5 分钟自动上报一次，最多等 5 分钟；<br>'+
      '如果设备关机或断网，要等它开机连上 Wi-Fi 才会有。</div></div>';
    return;
  }
  var live=data.age_sec!=null&&data.age_sec<1800;
  var cls=data.age_sec==null?'dead':(live?'live':'stale');
  var html='';

  html+='<header><div class="avatar">🐱</div><div>'+
    '<h1>'+st.name+'</h1>'+
    '<div class="sub"><span class="dot '+cls+'"></span>'+statusWord(st)+
    ' · 数据 '+ago(data.age_sec)+'</div></div></header>';

  html+='<div class="card"><h2>身体状态</h2><div class="bars">'+
    bar('饱食度',st.hunger,'f1')+bar('精力',st.energy,'f2')+bar('心情',st.mood,'f3')+
    '</div><div class="meta">'+
    '<div class="chip">金币<b>'+st.coins+'</b></div>'+
    '<div class="chip">等级<b>Lv.'+st.level+'</b></div>'+
    '<div class="chip">经验<b>'+st.exp_in_level+'/'+st.exp_per_level+'</b></div>'+
    (st.sick?'<div class="chip warn">需要看病<b>是</b></div>':'')+
    '</div></div>';

  html+='<div class="card"><h2>照看一下</h2><div class="acts">'+
    '<button class="act" data-cmd="feed"><span>🍚</span>喂饭</button>'+
    '<button class="act" data-cmd="play"><span>🎾</span>陪玩</button>'+
    '<button class="act" data-cmd="medicine"><span>💊</span>吃药</button>'+
    '<button class="act" data-cmd="sleep"><span>🌙</span>哄睡</button>'+
    '</div></div>';

  var tasks=data.tasks||[];
  html+='<div class="card"><h2>今日任务</h2>';
  if(!tasks.length){
    html+='<div class="empty">还没布置任务</div>';
  }else{
    for(var i=0;i<tasks.length;i++){
      var t=tasks[i];
      var sm={pending:'等孩子完成',submitted:'等家长确认',done:'已完成'}[t.status]||t.status;
      html+='<div class="task"><div class="t"><span>'+t.title+'</span><em>+'+t.reward+'</em></div>'+
        '<div class="s">'+sm+'</div>';
      if(t.status==='submitted'){
        html+='<div class="mini">'+
          '<button class="ok" data-approve="'+t.id+'">确认完成，发币</button>'+
          '<button class="no" data-reject="'+t.id+'">还没做完</button></div>';
      }
      html+='</div>';
    }
  }
  html+='</div>';

  html+='<div class="card"><h2>布置新任务</h2><div class="form">'+
    '<div><label>任务内容</label><input id="ntitle" placeholder="比如：把书包收拾好" maxlength="40"></div>'+
    '<div class="line"><div><label>奖励金币</label><input id="nreward" value="10" inputmode="numeric"></div>'+
    '<div style="align-self:end"><button class="primary" id="npub">发布</button></div></div>'+
    '</div></div>';

  var evs=data.events||[];
  html+='<div class="card"><h2>最近动态</h2>';
  if(!evs.length) html+='<div class="empty">暂无</div>';
  for(var j=0;j<evs.length;j++){
    html+='<div class="ev"><time>'+fmtTime(evs[j].at)+'</time><span>'+evs[j].text+'</span></div>';
  }
  html+='</div>';

  /* 用本机队列而不是云端队列：云端那份只存在于某一个热实例里，
     换个实例就读不到，看着像「指令消失」，其实还在路上 */
  var mine=myPending();
  if(mine.length){
    html+='<div class="card"><h2>等待送达</h2>';
    for(var k=0;k<mine.length;k++){
      html+='<div class="ev"><time>'+fmtTime(mine[k].at)+'</time><span>'+
        (mine[k].note||mine[k].cmd)+' · 每 30 秒自动重发，设备心跳取到后变「已送达」</span></div>';
    }
    html+='</div>';
  }

  html+='<footer>指令先排队，设备下次心跳（最多 5 分钟）或孩子跟小智说话时送达；'+
    '没收到回执会自动重发（同一条不会重复生效）。<br>'+
    '云端快照存在云函数热实例内存里 —— 实例被回收后这一页会短暂变空，设备下次上报就回来，状态本身在设备上不会丢。</footer>';

  el('body').innerHTML=html;
  bind();
}

function bind(){
  var acts=document.querySelectorAll('.act');
  for(var i=0;i<acts.length;i++){
    acts[i].onclick=function(){ send(this.getAttribute('data-cmd'),{},'已排队'); };
  }
  var oks=document.querySelectorAll('[data-approve]');
  for(var a=0;a<oks.length;a++){
    oks[a].onclick=function(){ send('task_approve',{task_id:this.getAttribute('data-approve')},'已确认'); };
  }
  var nos=document.querySelectorAll('[data-reject]');
  for(var b=0;b<nos.length;b++){
    nos[b].onclick=function(){ send('task_reject',{task_id:this.getAttribute('data-reject')},'已退回'); };
  }
  var pub=el('npub');
  if(pub) pub.onclick=function(){
    var title=(el('ntitle').value||'').trim();
    if(!title){ toast('先写任务内容'); return; }
    var reward=parseInt(el('nreward').value,10);
    if(!(reward>0)) reward=10;
    send('task_publish',{title:title,reward:reward},'任务已发布');
    el('ntitle').value='';
  };
}

function send(cmd,args,okMsg){
  var it={cmd:cmd,args:args||{},at:Date.now(),note:okMsg};
  var all=qload(); all.push(it); qsave(all);
  qpost(it)
    .then(function(j){
      if(j.ok){ toast('已排队，正在送达（最多 5 分钟）'); refresh(); }
      else { toast(j.say||'没成功'); }
    })
    .catch(function(){ toast('网络好像不通，这条会自己重试'); });
}

function refresh(){
  var h={};
  if(q()) h['X-Pet-Key']=q();
  fetch(apiUrl(),{headers:h})
    .then(function(r){
      if(r.status===401){ el('gate').style.display='flex'; return null; }
      return r.json();
    })
    .then(function(j){
      if(j){
        data=j;
        qdrop();     /* 设备回执到的指令从本机队列删掉 */
        render();
        el('gate').style.display='none';
      }
    })
    .catch(function(){});
}

el('gok').onclick=function(){
  var v=(el('gkey').value||'').trim();
  if(!v){ return; }
  localStorage.setItem(KEY_STORE,v);
  refresh();
};
refresh();
setInterval(refresh,15000);
setInterval(qretry,RETRY_MS);
qretry();
`;

function renderParentPage(petName) {
  const name = String(petName || '乔利');
  return `<!doctype html>
<html lang="zh-CN"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="theme-color" content="#f6f2ea">
<title>${name} · 家长看板</title>
<style>${CSS}</style>
</head><body>
<div class="wrap" id="body">
  <div class="card"><div class="empty">读取中…</div></div>
</div>

<div id="gate"><div class="box">
  <h3>输入家长口令</h3>
  <p>这台云函数开启了鉴权，输入部署时设置的 PET_KEY。</p>
  <input id="gkey" type="password" placeholder="口令" autocomplete="current-password">
  <div style="height:12px"></div>
  <button class="primary" id="gok">进入看板</button>
</div></div>

<div id="toast"></div>
<script>${JS}</script>
</body></html>`;
}

module.exports = { renderParentPage };
