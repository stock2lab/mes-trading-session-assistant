"use strict";

const STORAGE_KEY = "mes-companion-web-v1";
const VERSION = 1;
const MAX_TRADES = 5;
const LOSS_LIMIT_HALF_R = -4;
const INTERVALS = [1, 3, 5, 15];

const HOLIDAYS = {
  "2026-01-01": ["closed", 0, "元旦"], "2026-01-19": ["early", 780, "马丁·路德·金纪念日"],
  "2026-02-16": ["early", 780, "总统日"], "2026-04-03": ["closed", 0, "耶稣受难日"],
  "2026-05-25": ["early", 780, "阵亡将士纪念日"], "2026-06-19": ["early", 780, "六月节"],
  "2026-07-03": ["early", 780, "独立日假期"], "2026-09-07": ["early", 780, "劳动节"],
  "2026-11-26": ["early", 780, "感恩节"], "2026-11-27": ["early", 780, "感恩节次日"],
  "2026-12-24": ["early", 780, "圣诞前夕"], "2026-12-25": ["closed", 0, "圣诞节"],
  "2027-01-01": ["closed", 0, "元旦"], "2027-01-18": ["early", 780, "马丁·路德·金纪念日"],
  "2027-02-15": ["early", 780, "总统日"], "2027-03-26": ["closed", 0, "耶稣受难日"],
  "2027-05-31": ["early", 780, "阵亡将士纪念日"], "2027-06-18": ["early", 780, "六月节假期"],
  "2027-07-05": ["early", 780, "独立日假期"], "2027-09-06": ["early", 780, "劳动节"],
  "2027-11-25": ["early", 780, "感恩节"], "2027-11-26": ["early", 780, "感恩节次日"],
  "2027-12-24": ["closed", 0, "圣诞节假期"]
};

const defaults = { version: VERSION, interval: 5, sound: false, positionAlert: true, positionAlertMinutes: 30, day: "", trades: [], openPosition: null, cooldownUntil: 0, oneTradeOverride: false };
let state = loadState();
let customHalfR = 5;
let lastBarKey = "";
let lastAlertKey = "";
let pendingConfirm = null;
let holdTimer = null;

const $ = id => document.getElementById(id);
const $$ = selector => [...document.querySelectorAll(selector)];

function loadState() {
  try { return { ...defaults, ...JSON.parse(localStorage.getItem(STORAGE_KEY) || "{}") }; }
  catch { return { ...defaults }; }
}
function saveState() { localStorage.setItem(STORAGE_KEY, JSON.stringify(state)); }
function pad(value) { return String(value).padStart(2, "0"); }
function formatDuration(seconds) { const s = Math.max(0, Math.floor(seconds)); return `${pad(Math.floor(s / 60))}:${pad(s % 60)}`; }
function formatR(halfR) { const value = halfR / 2; return `${value > 0 ? "+" : ""}${value.toFixed(1)}R`; }
function toast(message) { const el = $("toast"); el.textContent = message; el.classList.add("show"); clearTimeout(el._timer); el._timer = setTimeout(() => el.classList.remove("show"), 2200); }

function nyParts(date = new Date()) {
  const parts = new Intl.DateTimeFormat("en-CA", { timeZone: "America/New_York", year: "numeric", month: "2-digit", day: "2-digit", weekday: "short", hour: "2-digit", minute: "2-digit", second: "2-digit", hourCycle: "h23", timeZoneName: "short" }).formatToParts(date);
  const get = type => parts.find(p => p.type === type)?.value || "";
  return { year:+get("year"), month:+get("month"), day:+get("day"), weekday:get("weekday"), hour:+get("hour"), minute:+get("minute"), second:+get("second"), zone:get("timeZoneName") };
}
function dayKey(p) { return `${p.year}-${pad(p.month)}-${pad(p.day)}`; }
function scheduleFor(p) {
  if (p.weekday === "Sat" || p.weekday === "Sun") return { type:"closed", close:0, name:"周末" };
  const item = HOLIDAYS[dayKey(p)];
  return item ? { type:item[0], close:item[1], name:item[2] } : { type:"regular", close:960, name:"常规交易日" };
}
function tradeTotal() { return state.trades.reduce((sum, t) => sum + t.halfR, 0); }
function lossStreak() { let n=0; for (let i=state.trades.length-1;i>=0&&state.trades[i].halfR<0;i--) n++; return n; }
function ensureTradingDay(p) {
  const key = dayKey(p);
  if (state.day !== key) { state = { ...state, day:key, trades:[], openPosition:null, cooldownUntil:0, oneTradeOverride:false }; saveState(); }
}
function sessionInfo(p, nowMs = Date.now()) {
  const schedule = scheduleFor(p); const minutes = p.hour * 60 + p.minute;
  if (schedule.type === "closed") return { phase:"closed", tone:"danger", title:"市场休市", detail:schedule.name, next:"NEXT SESSION", canEnter:false, lockReason:`${schedule.name}，今天不记录新交易。` };
  if (minutes < 570) return { phase:"preopen", tone:"neutral", title:"等待纽约开盘", detail:`距离 09:30 ET 还有 ${formatDuration(570*60-(minutes*60+p.second))}`, next:"09:30 ET", canEnter:false, lockReason:"尚未到 09:30 ET 交易窗口。" };
  if (schedule.type === "early" && minutes >= schedule.close) return { phase:"closed", tone:"danger", title:"假日时段已结束", detail:`${schedule.name} · 13:00 ET 锁定`, next:"CLOSED", canEnter:false, lockReason:`${schedule.name} 提前收盘时段已经结束。` };
  if (schedule.type === "early") return { phase:"holiday", tone:"warning", title:"假日缩短交易时段", detail:`${schedule.name} · 13:00 ET 锁定`, next:"13:00 ET", canEnter:true };
  if (minutes < 780) return { phase:"open", tone:"open", title:"交易窗口开放", detail:"保持耐心，只执行计划内交易", next:"13:00 ET", canEnter:true };
  if (minutes < 840) return { phase:"wrap", tone:"warning", title:"进入收尾窗口", detail:"13:00 已过，14:00 ET 前结束常规交易", next:"14:00 ET", canEnter:true };
  if (state.oneTradeOverride) return { phase:"override", tone:"warning", title:"单笔例外已解锁", detail:"本次开仓后例外权限立即失效", next:"ONE TRADE", canEnter:true };
  return { phase:"locked", tone:"danger", title:"交易时段已锁定", detail:"14:00 ET · 常规交易已经结束", next:"LOCKED", canEnter:false, lockReason:"常规交易已经超过 14:00 ET。可长按 5 秒，仅解锁一笔例外交易。", allowOverride:true };
}
function entryStatus(info, nowMs = Date.now()) {
  if (state.openPosition) return { allowed:false, reason:"已有一笔持仓尚未结束。" };
  if (state.trades.length >= MAX_TRADES) return { allowed:false, reason:"今日已完成 5 笔交易，达到每日上限。" };
  if (tradeTotal() <= LOSS_LIMIT_HALF_R) return { allowed:false, reason:"今日结果已达到 -2R，触发每日止损。" };
  if (state.cooldownUntil > nowMs) return { allowed:false, reason:`冷静期剩余 ${formatDuration((state.cooldownUntil-nowMs)/1000)}。` };
  if (!info.canEnter) return { allowed:false, reason:info.lockReason || info.detail, allowOverride:info.allowOverride };
  return { allowed:true, reason:"可以记录新的交易" };
}
function barInfo(p, schedule) {
  const total = state.interval * 60; const elapsed = p.minute * 60 + p.second; const left = total - (elapsed % total) || total;
  const minutes = p.hour*60+p.minute; const close = schedule.type === "early" ? schedule.close : 960;
  let number = "—"; if (schedule.type !== "closed" && minutes >= 570) number = Math.max(1, Math.floor(((minutes-570)*60+p.second)/total)+1); if (minutes >= close) number = Math.max(1, Math.ceil((close-570)/state.interval));
  return { left, total, progress:((total-left)/total)*100, number };
}
function localClock(date) {
  const time = new Intl.DateTimeFormat("zh-CN", {hour:"2-digit",minute:"2-digit",second:"2-digit",hourCycle:"h23"}).format(date);
  const zone = Intl.DateTimeFormat().resolvedOptions().timeZone || "LOCAL";
  const offset = -date.getTimezoneOffset(), sign=offset>=0?"+":"-";
  return { time, zone:zone.split("/").pop().replaceAll("_"," ").toUpperCase(), offset:`UTC${sign}${Math.floor(Math.abs(offset)/60)}${offset%60?":"+pad(Math.abs(offset)%60):""}` };
}
function setText(id, value) { const el=$(id); if(el) el.textContent=value; }

function render(now = new Date()) {
  const p=nyParts(now); ensureTradingDay(p); const schedule=scheduleFor(p); const info=sessionInfo(p, now.getTime()); const entry=entryStatus(info, now.getTime()); const bar=barInfo(p,schedule); const local=localClock(now);
  setText("nyTime",`${pad(p.hour)}:${pad(p.minute)}:${pad(p.second)}`); setText("nyZone",p.zone); setText("localTime",local.time); setText("localZone",local.zone); setText("localOffset",local.offset);
  setText("marketDate",`${p.year}.${pad(p.month)}.${pad(p.day)} · ${schedule.name}`); setText("intervalLabel",`${state.interval} MIN`); setText("barNumber",bar.number); setText("countdown",formatDuration(bar.left)); $("barProgress").style.width=`${bar.progress}%`;
  $("countdown").classList.toggle("urgent",bar.left<=10); setText("countdownHint",info.phase==="closed"?"交易时段已结束":bar.left<=30?"当前 K 线即将结束":"按纽约时间计算");
  $("disciplineCard").dataset.tone=info.tone; setText("disciplineTitle",info.title); setText("disciplineDetail",info.detail); setText("nextMarker",info.next);
  const pill=$("sessionPill"); pill.className=`session-pill ${info.tone==="open"?"open":info.tone==="danger"?"locked":""}`; pill.querySelector("b").textContent=info.title;
  $$("[data-open-side]").forEach(b=>{b.disabled=false;b.classList.toggle("blocked",!entry.allowed);b.title=entry.allowed?"记录开仓":entry.reason}); setText("entryAvailability",entry.allowed?"交易窗口开放 · 可手动记录":entry.reason); setText("journalAvailability",entry.allowed?"确认交易计划后，手动记录方向。":"点击方向按钮可查看锁定原因。 "+entry.reason);
  setText("tradeCount",state.trades.length); const total=tradeTotal(); setText("totalR",formatR(total)); setText("lossStreak",lossStreak()); setText("cooldown",state.cooldownUntil>now.getTime()?formatDuration((state.cooldownUntil-now.getTime())/1000):"READY");
  $("totalR").style.color=total<0?"var(--red)":total>0?"var(--green)":"var(--text)";
  renderPosition(now); renderFocus(p,info,bar,now); renderHistory(); renderCalendar(p);
  setText("journalDate",new Intl.DateTimeFormat("zh-CN",{timeZone:"America/New_York",year:"numeric",month:"long",day:"numeric",weekday:"long"}).format(now));
  document.title=`${formatDuration(bar.left)} · ${info.title} · MES`;
  const barKey=`${state.interval}-${dayKey(p)}-${p.hour}-${Math.floor(p.minute/state.interval)}`;
  if(lastBarKey&&barKey!==lastBarKey&&state.sound) beep(false); lastBarKey=barKey;
  const alertKey=`${dayKey(p)}-${info.phase}`;
  if(lastAlertKey&&alertKey!==lastAlertKey&&state.sound&&["wrap","locked","closed"].includes(info.phase)) beep(true); lastAlertKey=alertKey;
}
function renderPosition(now) {
  const open=state.openPosition; $("positionEmpty").classList.toggle("hidden",!!open); $("positionLive").classList.toggle("hidden",!open); $("journalNoPosition").classList.toggle("hidden",!!open); $("journalOpenPosition").classList.toggle("hidden",!open);
  if(!open) return; const seconds=(now.getTime()-open.openedAt)/1000; const duration=formatDuration(seconds); const started=new Date(open.openedAt).toLocaleTimeString("zh-CN",{hour:"2-digit",minute:"2-digit"});
  ["positionSide","journalSide"].forEach(id=>{setText(id,open.side);$(id).classList.toggle("short",open.side==="SHORT")}); setText("positionDuration",duration); setText("journalDuration",duration); setText("positionOpenedAt",`${started} 开始`); setText("journalOpenedAt",`${started} 开始记录 · 结果由你手动确认`);
  const alert=state.positionAlert&&seconds>=state.positionAlertMinutes*60; $("positionCard").style.borderColor=alert?"rgba(255,200,87,.75)":"";
}
function renderFocus(p,info,bar,now) {
  setText("focusSession",info.title.toUpperCase()); setText("focusNyTime",`${pad(p.hour)}:${pad(p.minute)}:${pad(p.second)} ${p.zone}`); setText("focusInterval",`${state.interval} MIN`); setText("focusBar",bar.number); setText("focusCountdown",formatDuration(bar.left)); $("focusProgress").style.width=`${bar.progress}%`; setText("focusMessage",info.detail);
  if(state.openPosition){const duration=formatDuration((now.getTime()-state.openPosition.openedAt)/1000);setText("focusPosition",`${state.openPosition.side} OPEN · ${duration}`)}else setText("focusPosition","NO OPEN POSITION");
}
function renderHistory() {
  const list=$("historyList"); if(!state.trades.length) list.innerHTML='<div class="history-empty">今天还没有已完成的交易</div>'; else list.innerHTML=[...state.trades].reverse().map((t,i)=>`<div class="history-row"><span class="side-badge ${t.side==='SHORT'?'short':''}">${t.side}</span><div><b>TRADE ${state.trades.length-i}</b><small>${new Date(t.closedAt).toLocaleTimeString('zh-CN',{hour:'2-digit',minute:'2-digit'})} · ${formatDuration((t.closedAt-t.openedAt)/1000)}</small></div><strong class="${t.halfR>0?'positive':t.halfR<0?'negative':''}">${formatR(t.halfR)}</strong></div>`).join("");
  setText("historyTotal",formatR(tradeTotal()));
}
function renderCalendar(p) {
  const base=new Date(Date.UTC(p.year,p.month-1,p.day,16)); let found=null;
  for(let i=0;i<370;i++){const d=new Date(base.getTime()+i*86400000);const key=d.toISOString().slice(0,10);if(HOLIDAYS[key]){found={key,item:HOLIDAYS[key]};break}}
  if(found){setText("calendarHeadline",found.item[2]);setText("calendarDetail",found.item[0]==="closed"?"全天休市":"13:00 ET 提前锁定");setText("calendarDate",found.key.slice(5).replace("-"," / "))}else{setText("calendarHeadline","日历数据需要更新");setText("calendarDetail","未找到未来节假日，请核对 CME 官方安排");setText("calendarDate","CHECK")}
}
function openTrade(side) {
  const info=sessionInfo(nyParts()); const entry=entryStatus(info); if(!entry.allowed){showLock(entry.reason,entry.allowOverride);return}
  state.openPosition={side,openedAt:Date.now()}; if(state.oneTradeOverride)state.oneTradeOverride=false; saveState(); toast(`${side} 已开始计时`); navigate("dashboard"); render();
}
function closeTrade(halfR) {
  if(!state.openPosition)return; const closedAt=Date.now(); const streakBefore=lossStreak(); state.trades.push({...state.openPosition,closedAt,halfR}); state.openPosition=null; const nextStreak=halfR<0?streakBefore+1:0; state.cooldownUntil=closedAt+(nextStreak>=2?15:5)*60000; saveState(); toast(`已记录 ${formatR(halfR)}`); customHalfR=5; setText("customResult",formatR(customHalfR)); render();
}
function showLock(reason,allowOverride=false){setText("lockReason",reason);$("overrideButton").classList.toggle("hidden",!allowOverride);$("lockDialog").showModal()}
function askConfirm(title,text,action){setText("confirmTitle",title);setText("confirmText",text);pendingConfirm=action;$("confirmDialog").showModal()}
function navigate(view){$$('.view').forEach(v=>v.classList.toggle('active',v.dataset.view===view));$$('[data-nav]').forEach(b=>b.classList.toggle('active',b.dataset.nav===view));window.scrollTo({top:0,behavior:'smooth'});if(view==='focus'&&document.documentElement.requestFullscreen&&window.innerWidth<900)document.documentElement.requestFullscreen().catch(()=>{})}
function beep(urgent){try{const ctx=new (window.AudioContext||window.webkitAudioContext)();const tones=urgent?[880,660,880]:[760];tones.forEach((f,i)=>{const o=ctx.createOscillator(),g=ctx.createGain();o.connect(g);g.connect(ctx.destination);o.frequency.value=f;g.gain.setValueAtTime(.0001,ctx.currentTime+i*.13);g.gain.exponentialRampToValueAtTime(.13,ctx.currentTime+i*.13+.01);g.gain.exponentialRampToValueAtTime(.0001,ctx.currentTime+i*.13+.09);o.start(ctx.currentTime+i*.13);o.stop(ctx.currentTime+i*.13+.1)});setTimeout(()=>ctx.close(),700)}catch{}}
function exportData(){const blob=new Blob([JSON.stringify({app:"MES Companion Web",exportedAt:new Date().toISOString(),data:state},null,2)],{type:"application/json"});const a=document.createElement("a");a.href=URL.createObjectURL(blob);a.download=`mes-companion-${state.day||"backup"}.json`;a.click();URL.revokeObjectURL(a.href);toast("数据已导出")}

function bindEvents(){
  $$('[data-nav]').forEach(b=>b.addEventListener('click',()=>navigate(b.dataset.nav)));$("brandButton").addEventListener('click',()=>navigate('dashboard'));$("openJournalButton").addEventListener('click',()=>navigate('journal'));$("closePositionButton").addEventListener('click',()=>navigate('journal'));$("focusExit").addEventListener('click',()=>{if(document.fullscreenElement)document.exitFullscreen();navigate('dashboard')});
  $$('[data-open-side]').forEach(b=>b.addEventListener('click',()=>openTrade(b.dataset.openSide)));$$('#intervalPicker button').forEach(b=>b.addEventListener('click',()=>{state.interval=+b.dataset.interval;saveState();$$('#intervalPicker button').forEach(x=>x.classList.toggle('active',x===b));render()}));
  $$('#resultGrid button').forEach(b=>b.addEventListener('click',()=>closeTrade(+b.dataset.result)));$("customMinus").addEventListener('click',()=>{customHalfR=Math.max(-4,customHalfR-1);setText('customResult',formatR(customHalfR))});$("customPlus").addEventListener('click',()=>{customHalfR=Math.min(20,customHalfR+1);setText('customResult',formatR(customHalfR))});$("customConfirm").addEventListener('click',()=>closeTrade(customHalfR));
  $("cancelPositionButton").addEventListener('click',()=>askConfirm('取消未完成交易','这笔交易不会计入今日统计。',()=>{state.openPosition=null;saveState();render();toast('未完成交易已取消')}));$("undoButton").addEventListener('click',()=>{if(!state.trades.length)return toast('没有可撤销的记录');askConfirm('撤销上一笔','上一笔已完成交易将从今日统计中移除。',()=>{state.trades.pop();state.cooldownUntil=0;saveState();render();toast('已撤销上一笔')})});$("resetDayButton").addEventListener('click',()=>askConfirm('重置今天的数据','将删除今天的持仓和全部交易记录，此操作无法撤销。',()=>{state.trades=[];state.openPosition=null;state.cooldownUntil=0;state.oneTradeOverride=false;saveState();render();toast('今日数据已重置')}));
  $$('[data-close-dialog]').forEach(b=>b.addEventListener('click',()=>b.closest('dialog').close()));$("confirmAction").addEventListener('click',()=>{const action=pendingConfirm;pendingConfirm=null;$("confirmDialog").close();if(action)action()});
  const hold=$("overrideButton");const startHold=()=>{hold.textContent='继续按住…';holdTimer=setTimeout(()=>{state.oneTradeOverride=true;saveState();$("lockDialog").close();hold.textContent='按住 5 秒解锁一笔';toast('已解锁一笔例外交易');render()},5000)};const stopHold=()=>{clearTimeout(holdTimer);hold.textContent='按住 5 秒解锁一笔'};hold.addEventListener('pointerdown',startHold);['pointerup','pointerleave','pointercancel'].forEach(e=>hold.addEventListener(e,stopHold));
  $("soundButton").addEventListener('click',()=>{state.sound=!state.sound;saveState();syncSettings();if(state.sound)beep(false)});$("soundToggle").addEventListener('change',e=>{state.sound=e.target.checked;saveState();syncSettings();if(state.sound)beep(false)});$("positionAlertToggle").addEventListener('change',e=>{state.positionAlert=e.target.checked;saveState()});$("positionAlertMinutes").addEventListener('change',e=>{state.positionAlertMinutes=Math.min(180,Math.max(5,+e.target.value||30));saveState();syncSettings()});
  $("fullscreenButton").addEventListener('click',()=>document.fullscreenElement?document.exitFullscreen():document.documentElement.requestFullscreen().catch(()=>toast('浏览器不允许全屏')));$("exportButton").addEventListener('click',exportData);$("exportSettingsButton").addEventListener('click',exportData);$("importInput").addEventListener('change',async e=>{try{const parsed=JSON.parse(await e.target.files[0].text());const data=parsed.data||parsed;if(!Array.isArray(data.trades))throw new Error();state={...defaults,...data,version:VERSION};saveState();syncSettings();render();toast('数据导入成功')}catch{toast('无法读取这份备份文件')}e.target.value=''});
  document.addEventListener('visibilitychange',()=>{if(!document.hidden)render()});
}
function syncSettings(){$("soundToggle").checked=state.sound;$("positionAlertToggle").checked=state.positionAlert;$("positionAlertMinutes").value=state.positionAlertMinutes;$("soundButton").classList.toggle("active",state.sound);setText("soundIcon",state.sound?"♫":"♪");$$('#intervalPicker button').forEach(x=>x.classList.toggle('active',+x.dataset.interval===state.interval))}
function init(){bindEvents();syncSettings();render();setInterval(()=>render(),1000);if('serviceWorker'in navigator)window.addEventListener('load',()=>navigator.serviceWorker.register('./sw.js').catch(()=>{}))}
init();
