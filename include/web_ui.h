#pragma once
// Embedded single-page control UI served by the shore station.
// The station joins the phone's hotspot (STA mode); open the station IP shown
// on its OLED at boot. The page itself (HTML/CSS/JS) is fully self-contained and
// loads with no internet. The optional 地圖 (map) view additionally streams
// OpenStreetMap raster tiles directly in the browser — those appear only when
// the phone also has mobile data; offline it falls back to tracks + scale bar.
//
// Operator flow (no separate calibrate step):
//   手動 (Manual): drag the overlaid slider to aim the camera at the surfer.
//   自動 (Auto):   locks the current manual aim as "facing the surfer" and
//                  auto-follows; pressing it again re-locks from the latest aim.
// Layout: full-height, no-scroll, two top tabs — 雷達 (radar/map canvas with the
//   雷達/地圖 + 手動/自動 toggles and the slider overlaid at the bottom) and
//   資訊 (live telemetry grid). The canvas auto-sizes to its container, and the
//   surfer marker carries a GPS-status tag (sats / HDOP / Good-Normal-Bad).
static const char WEB_UI_HTML[] = R"rawlit(
<!DOCTYPE html>
<html lang="zh-Hant">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,viewport-fit=cover">
<meta name="mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">
<meta name="theme-color" content="#0d1117">
<meta name="apple-mobile-web-app-title" content="Shore Spotter">
<link rel="manifest" href="/manifest.json">
<title>Shore Spotter</title>
<style>
:root{--bg:#0d1117;--card:#161b22;--line:#30363d;--fg:#e6edf3;--mut:#8b949e;
  --acc:#3b82f6;--ok:#3fb950;--warn:#d29922;--bad:#f85149;--trk:#f97316;--tgt:#22d3ee}
*{box-sizing:border-box}
html,body{margin:0;height:100%;background:var(--bg);color:var(--fg);
  font-family:system-ui,-apple-system,"Segoe UI",Roboto,"Noto Sans TC",sans-serif}
body{height:100dvh;display:flex;flex-direction:column;overflow:hidden}
.tabs{display:flex;background:var(--card);border-bottom:1px solid var(--line);flex:none}
.tabs button{flex:1;border:0;border-bottom:2px solid transparent;background:none;
  color:var(--mut);padding:12px 4px;font-size:15px;font-weight:700;cursor:pointer;
  font-family:inherit}
.tabs button.on{color:var(--fg);border-bottom-color:var(--acc)}
.statusbar{display:flex;align-items:center;gap:10px 14px;padding:6px 12px;
  background:var(--card);border-bottom:1px solid var(--line);font-size:11px;
  overflow-x:auto;white-space:nowrap;flex:none}
.statusbar::-webkit-scrollbar{display:none}
.brand{font-size:12px;font-weight:700;margin-right:2px}
.pill{display:inline-flex;align-items:center;gap:5px;color:var(--mut);flex:none}
.pill b{color:var(--fg);font-variant-numeric:tabular-nums}
.dot{width:9px;height:9px;border-radius:50%;background:var(--mut);display:inline-block}
.dot.ok{background:var(--ok)}.dot.warn{background:var(--warn)}.dot.bad{background:var(--bad)}
main{flex:1;position:relative;overflow:hidden}
.page{position:absolute;inset:0;display:none;flex-direction:column;padding:10px;gap:10px}
.page.on{display:flex}
#pgInfo{overflow:auto}
#pgCtrl{justify-content:center}
.panel{width:100%;max-width:520px;margin:0 auto}
.card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:16px}
.card h2{margin:0 0 12px;font-size:13px;color:var(--mut);font-weight:600;
  text-transform:uppercase;letter-spacing:.04em}
.radarHead{display:flex;align-items:center;gap:10px;flex:none;
  font-size:13px;color:var(--mut);font-weight:600}
.radarHead .segwrap{display:inline-flex}
.hint2{margin-left:auto;font-size:11px;color:var(--mut);font-weight:400;
  text-align:right;line-height:1.3}
#radarWrap{flex:1;min-height:0;position:relative}
#radar{position:absolute;inset:0;width:100%;height:100%;touch-action:none}
.sliderOverlay{position:absolute;left:50%;transform:translateX(-50%);bottom:8px;
  width:calc(100% - 16px);max-width:800px;z-index:2;
  display:flex;align-items:center;gap:12px;padding:8px 12px;
  background:rgba(13,17,23,.74);border:1px solid var(--line);border-radius:10px}
.sliderOverlay input[type=range]{flex:1;accent-color:var(--acc)}
.sliderOverlay input[type=range]:disabled{opacity:.45}
.modeRow{display:flex;align-items:center;gap:10px;margin-bottom:12px}
.badge{padding:3px 10px;border-radius:999px;font-size:12px;font-weight:700;
  border:1px solid var(--line)}
.badge.manual{color:var(--acc);border-color:var(--acc)}
.badge.tracking{color:var(--ok);border-color:var(--ok)}
.badge.paused{color:var(--warn);border-color:var(--warn)}
.badge.idle{color:var(--mut)}
.sliderRow{display:flex;align-items:center;gap:12px;margin:6px 0 14px}
input[type=range]{flex:1;accent-color:var(--acc)}
input[type=range]:disabled{opacity:.4}
.ang{font-size:22px;font-weight:700;min-width:74px;text-align:right;
  font-variant-numeric:tabular-nums}
.ang small{font-size:12px;color:var(--mut);font-weight:400}
.btns{display:flex;flex-wrap:wrap;gap:8px}
button{flex:1;min-width:96px;padding:11px 12px;border-radius:8px;border:1px solid var(--line);
  background:#21262d;color:var(--fg);font-size:14px;font-weight:600;cursor:pointer}
button:hover{border-color:var(--acc)}
button:disabled{opacity:.4;cursor:not-allowed}
button.go{background:var(--ok);border-color:var(--ok);color:#03210e}
button.pause{background:var(--warn);border-color:var(--warn);color:#241a00}
.hint{font-size:11px;color:var(--mut);margin-top:10px;line-height:1.5}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,1fr));gap:10px}
.kv{background:#0d1117;border:1px solid var(--line);border-radius:8px;padding:8px 10px}
.kv .k{font-size:11px;color:var(--mut)}
.kv .v{font-size:17px;font-weight:700;font-variant-numeric:tabular-nums;margin-top:2px}
.kv .v small{font-size:11px;color:var(--mut);font-weight:400}
#toast{position:fixed;left:50%;bottom:18px;transform:translateX(-50%);
  background:var(--bad);color:#fff;padding:9px 16px;border-radius:8px;font-size:13px;
  opacity:0;transition:opacity .25s;pointer-events:none;z-index:9}
#toast.show{opacity:1}
#prog{position:fixed;top:0;left:0;height:3px;width:0;background:var(--acc);
  z-index:10;opacity:0;transition:width .15s ease,opacity .3s;
  box-shadow:0 0 6px var(--acc)}
#prog.on{opacity:1}
.lg{display:flex;gap:14px;font-size:11px;color:var(--mut);margin-top:8px;
  justify-content:center;flex-wrap:wrap}
.lg span{display:inline-flex;align-items:center;gap:5px}
.sw{width:18px;height:3px;border-radius:2px;display:inline-block}
.seg{padding:4px 12px;border:1px solid var(--line);background:#21262d;color:var(--mut);
  font-size:12px;font-weight:600;cursor:pointer;flex:none;min-width:0}
.seg:first-child{border-radius:6px 0 0 6px}
.seg:last-child{border-radius:0 6px 6px 0;border-left:0}
.seg.on{background:var(--acc);border-color:var(--acc);color:#fff}
.infoCols{flex:1;min-height:0;display:grid;grid-template-columns:1fr 1fr;gap:10px}
.col{overflow:auto}
.col h3{margin:0 0 4px;font-size:14px;font-weight:700}
.sub{font-size:10px;color:var(--mut);text-transform:uppercase;letter-spacing:.04em;
  margin:9px 0 2px;border-bottom:1px solid var(--line);padding-bottom:2px}
.r{display:flex;justify-content:space-between;align-items:baseline;gap:6px;
  font-size:12px;padding:2px 0}
.r span{color:var(--mut)}
.r b{font-variant-numeric:tabular-nums}
.cmp{width:100%;border-collapse:collapse;font-size:13px}
.cmp th,.cmp td{padding:5px 8px;border-bottom:1px solid var(--line);text-align:right;
  font-variant-numeric:tabular-nums}
.cmp tr:last-child td{border-bottom:0}
.cmp th:first-child,.cmp td:first-child{text-align:left;color:var(--mut);font-weight:400}
.cmp thead th{color:var(--fg);font-weight:700}
</style>
</head>
<body>
<div id="prog"></div>
<nav class="tabs">
  <button id="tabRadar" class="on" type="button">雷達</button>
  <button id="tabInfo" type="button">資訊</button>
</nav>

<main>
  <section id="pgRadar" class="page on">
    <div class="radarHead">
      <span class="segwrap">
        <button id="vRadar" class="seg on" type="button">雷達</button>
        <button id="vMap" class="seg" type="button">地圖</button>
      </span>
      <span id="viewTitle" class="hint2">過去 5 分鐘</span>
    </div>
    <div class="radarHead">
      <span class="segwrap">
        <button id="mManual" class="seg on" type="button">手動</button>
        <button id="mAuto" class="seg" type="button">自動</button>
      </span>
      <span class="hint2">自動＝以目前手動角度當「正對 Surfer」並跟隨</span>
    </div>
    <div id="radarWrap">
      <canvas id="radar"></canvas>
      <div class="sliderOverlay">
        <input id="sld" type="range" min="0" max="180" step="1" value="90">
        <div class="ang"><span id="angTxt">90</span><small>&deg;</small></div>
      </div>
    </div>
    <div class="lg" id="lgRadar">
      <span><i class="sw" style="background:var(--trk)"></i>Servo 目前</span>
      <span><i class="sw" style="background:var(--tgt)"></i>Servo 目標</span>
      <span><i class="sw" style="background:var(--bad)"></i>Surfer</span>
      <span><i class="sw" style="background:var(--mut)"></i>路徑</span>
    </div>
    <div class="lg" id="lgMap" style="display:none">
      <span><i class="sw" style="background:var(--acc)"></i>攝影站路徑</span>
      <span><i class="sw" style="background:var(--bad)"></i>Surfer 路徑</span>
      <span>&#9675; 起點　&#9679; 目前</span>
    </div>
  </section>

  <section id="pgInfo" class="page">
    <div class="card" style="flex:none">
      <table class="cmp">
        <thead><tr><th></th><th>站 Server</th><th>Surfer Client</th></tr></thead>
        <tbody>
          <tr><td>定位</td><td id="sFix">--</td><td id="cFix">--</td></tr>
          <tr><td>衛星</td><td id="sSat">--</td><td id="cSat">--</td></tr>
          <tr><td>HDOP</td><td id="sHdop">--</td><td id="cHdop">--</td></tr>
          <tr><td>電量</td><td id="sBattV">--</td><td id="cBattV">--</td></tr>
          <tr><td>溫度</td><td id="sTemp">--</td><td id="cTemp">--</td></tr>
          <tr><td>濕度</td><td id="sHum">--</td><td id="cHum">--</td></tr>
        </tbody>
      </table>
    </div>
    <div class="infoCols">
      <div class="col card">
        <h3>站 專屬</h3>
        <div class="sub">LoRa（收 Surfer）</div>
        <div class="r"><span>RSSI</span><b id="sRssi">--</b></div>
        <div class="r"><span>SNR</span><b id="sSnr">--</b></div>
        <div class="r"><span>丟包率</span><b id="sDrop">--</b></div>
        <div class="sub">其他</div>
        <div class="r"><span>羅盤</span><b id="sHdg">--</b></div>
        <div class="r"><span>Uptime</span><b id="sUp">--</b></div>
      </div>
      <div class="col card">
        <h3>Surfer 專屬</h3>
        <div class="sub">位置（站→Surfer）</div>
        <div class="r"><span>距離</span><b id="cDist">--</b></div>
        <div class="r"><span>方位</span><b id="cBrg">--</b></div>
        <div class="sub">LoRa 連線</div>
        <div class="r"><span>連線</span><b id="cLink">--</b></div>
        <div class="r"><span>距上次封包</span><b id="cAge">--</b></div>
      </div>
    </div>
  </section>
</main>

<div id="toast"></div>

<script>
var $=function(id){return document.getElementById(id);};
var last={track:null,status:null};
var dragging=false;
var viewMode='radar';
var page='radar';
var hist=[];               // client-accumulated path (server no longer stores it)
var HIST_CAP=300;          // ~5 min at 1 pt/s

function toast(m){var t=$('toast');t.textContent=m;t.classList.add('show');
  clearTimeout(t._h);t._h=setTimeout(function(){t.classList.remove('show');},2600);}

function post(url){
  return fetch(url,{method:'POST'}).then(function(r){
    return r.json().catch(function(){return{};}).then(function(j){
      if(!r.ok||j.ok===false){toast(j.error||('HTTP '+r.status));throw j;}
      return j;});
  });
}

// ---- controls ----
$('sld').addEventListener('input',function(){
  dragging=true;$('angTxt').textContent=this.value;});
$('sld').addEventListener('change',function(){
  var v=this.value;
  post('/api/servo?angle='+v).catch(function(){}).then(function(){dragging=false;});
});
// 手動 = release servo to the slider; 自動 = lock the current aim as "facing the
// surfer" and auto-track (re-locks every time, so no separate calibrate step).
$('mManual').onclick=function(){
  if(last.track&&last.track.servo.mode==='tracking')post('/api/track/pause').then(refresh);
};
$('mAuto').onclick=function(){post('/api/track/start').then(refresh).catch(function(){});};

$('vRadar').onclick=function(){setView('radar');};
$('vMap').onclick=function(){setView('map');};
function setView(m){
  viewMode=m;
  $('vRadar').classList.toggle('on',m==='radar');
  $('vMap').classList.toggle('on',m==='map');
  $('lgRadar').style.display=m==='radar'?'':'none';
  $('lgMap').style.display=m==='map'?'':'none';
  $('viewTitle').textContent=m==='radar'?'過去 5 分鐘':'絕對位置·過去 5 分鐘';
  if(last.track)drawRadar(last.track,geoDist(last.track.server,last.track.client));
}

// ---- tabs + responsive canvas (fills its container, no fixed px) ----
function showPage(p){
  page=p;
  $('tabRadar').classList.toggle('on',p==='radar');
  $('tabInfo').classList.toggle('on',p==='info');
  $('pgRadar').classList.toggle('on',p==='radar');
  $('pgInfo').classList.toggle('on',p==='info');
  if(p==='radar')redraw();
}
$('tabRadar').onclick=function(){showPage('radar');};
$('tabInfo').onclick=function(){showPage('info');};
function fitCanvas(){
  var c=$('radar'),w=Math.round(c.clientWidth),h=Math.round(c.clientHeight);
  if(w>0&&h>0&&(c.width!==w||c.height!==h)){c.width=w;c.height=h;}
}
function redraw(){
  if(page!=='radar'||!last.track)return;
  fitCanvas();
  drawRadar(last.track,geoDist(last.track.server,last.track.client));
}
window.addEventListener('resize',redraw);

function setDot(el,state){el.className='dot '+state;}

function applyMode(sv){
  var auto=(sv.mode==='tracking');
  $('mManual').classList.toggle('on',!auto);
  $('mAuto').classList.toggle('on',auto);
  $('sld').disabled=auto;
  if(!dragging&&document.activeElement!==$('sld')){
    $('sld').value=Math.round(sv.angle);$('angTxt').textContent=Math.round(sv.angle);
  }
}

// GPS quality grade (same thresholds as the firmware, shared by both ends).
function gpsGrade(sats,hdop){
  if(sats<0||hdop<0||sats<=0)return 'miss';
  if(sats<4)return 'bad';
  if(hdop<=2&&sats>=6)return 'good';
  if(hdop<=5)return 'ok';
  return 'bad';
}
// Leader line + small info card placed next to a marker (px,py) on the canvas.
function drawGpsTag(x,px,py,W,H,title,sats,hdop){
  var g=gpsGrade(sats,hdop);
  var col=g==='good'?'#3fb950':g==='ok'?'#d29922':g==='bad'?'#f85149':'#8b949e';
  var l1=title;
  var l2=(sats>=0?('sats '+sats):'sats --')+'  HDOP '+(hdop>=0?hdop.toFixed(1):'--');
  var l3=g==='good'?'GPS Good':g==='ok'?'GPS OK':g==='bad'?'GPS Bad':'GPS Miss';
  x.font='11px system-ui';x.textBaseline='alphabetic';
  var bw=Math.max(x.measureText(l1).width,x.measureText(l2).width,
                  x.measureText(l3).width)+16,bh=46;
  var bx=px+16,by=py-bh-16;
  if(bx+bw>W-4)bx=px-16-bw;
  if(bx<4)bx=4;
  if(by<4)by=py+16;
  if(by+bh>H-4)by=H-4-bh;
  var ax=(bx+bw/2<px)?bx+bw:bx;        // line attaches to the near edge
  x.strokeStyle=col;x.lineWidth=1.5;x.beginPath();
  x.moveTo(px,py);x.lineTo(ax,by+bh/2);x.stroke();
  x.fillStyle='rgba(13,17,23,.85)';x.fillRect(bx,by,bw,bh);
  x.strokeStyle=col;x.strokeRect(bx,by,bw,bh);
  x.textAlign='left';
  x.fillStyle='#e6edf3';x.font='bold 11px system-ui';x.fillText(l1,bx+8,by+15);
  x.fillStyle='#8b949e';x.font='11px system-ui';x.fillText(l2,bx+8,by+29);
  x.fillStyle=col;x.font='bold 11px system-ui';x.fillText(l3,bx+8,by+43);
}

// /api/track is now small (no history); the browser accumulates the path itself.
function fetchTrack(){return fetch('/api/track').then(function(r){return r.json();});}

// Append the current sample to the local path (skip stale/duplicate points).
function pushHist(d){
  if(!d.linked||!d.client.fix)return;
  var e={lat:d.client.lat,lon:d.client.lon,sfix:d.server.fix?1:0,
         slat:d.server.fix?d.server.lat:0,slon:d.server.fix?d.server.lon:0};
  var l=hist[hist.length-1];
  if(l&&l.lat===e.lat&&l.lon===e.lon&&l.slat===e.slat&&l.slon===e.slon)return;
  hist.push(e);
  if(hist.length>HIST_CAP)hist.shift();
}

var refreshing=false;
function refresh(){
  if(refreshing)return Promise.resolve();
  refreshing=true;
  return fetchTrack().then(function(d){
    last.track=d;
    pushHist(d);
    applyMode(d.servo);
    var dist=geoDist(d.server,d.client);
    // --- Server block ---
    $('sFix').textContent=d.server.fix?'有':'無';
    $('sSat').textContent=d.server.satellites>=0?d.server.satellites:'--';
    $('sHdop').textContent=d.server.hdop>=0?d.server.hdop.toFixed(1):'--';
    $('sBattV').textContent=(d.server.batt_pct>=0?d.server.batt_pct+'%':'--')+
      (d.server.charging?' \u26a1':'');
    $('sTemp').textContent=d.server.temp_c==null?'--':d.server.temp_c+'°C';
    $('sHum').textContent=d.server.humidity_pct==null?'--':d.server.humidity_pct+'%';
    $('sHdg').textContent=d.mag.online&&d.mag.heading>=0?d.mag.heading.toFixed(0)+'°':'--';
    // --- Client block ---
    $('cFix').textContent=d.client.fix?'有':'無';
    $('cSat').textContent=d.client.satellites>=0?d.client.satellites:'--';
    $('cHdop').textContent=d.client.hdop>=0?d.client.hdop.toFixed(1):'--';
    $('cDist').textContent=(d.server.fix&&d.client.fix&&dist!=null)?dist.toFixed(0)+' m':'--';
    $('cBrg').textContent=d.bearing>=0?d.bearing.toFixed(0)+'°':'--';
    $('cLink').textContent=d.linked?'ONLINE':'離線';
    $('cAge').textContent=d.client.last_rx_sec>=0?d.client.last_rx_sec+' s':'--';
    $('cBattV').textContent=d.telemetry.batt_mv>0?battPct(d.telemetry.batt_mv)+'%':'--';
    $('cTemp').textContent=d.telemetry.temp_c==null?'--':d.telemetry.temp_c+'°C';
    $('cHum').textContent=d.telemetry.humidity_pct==null?'--':d.telemetry.humidity_pct+'%';
    if(page==='radar')drawRadar(d,dist);
  }).catch(function(){}).then(function(){refreshing=false;});
}

function refreshStatus(){
  fetch('/api/status').then(function(r){return r.json();}).then(function(s){
    last.status=s;
    $('sRssi').textContent=s.lora.rssi?s.lora.rssi.toFixed(0)+' dBm':'--';
    $('sSnr').textContent=s.lora.snr!=null?s.lora.snr.toFixed(1)+' dB':'--';
    $('sDrop').textContent=(s.lora.drop_rate*100).toFixed(1)+'%';
    $('sUp').textContent=fmtUptime(s.health.uptime_s);
  }).catch(function(){});
}

function fmtUptime(s){var h=Math.floor(s/3600),m=Math.floor((s%3600)/60);
  return h>0?(h+'h'+m+'m'):(m+'m'+(s%60)+'s');}
// Battery %: 0% at 3.2 V, 100% at 4.2 V (matches the firmware scale).
function battPct(mv){return Math.max(0,Math.min(100,Math.round((mv-3200)/10)));}

// equirectangular metres between two {lat,lon,fix} points
function geoDist(a,b){
  if(!a.fix||!b.fix)return null;
  var R=6371000,la=a.lat*Math.PI/180;
  var dx=(b.lon-a.lon)*Math.PI/180*Math.cos(la)*R;
  var dy=(b.lat-a.lat)*Math.PI/180*R;
  return Math.sqrt(dx*dx+dy*dy);
}
// local east/north metres of pt relative to station
function toEN(st,pt){
  var R=6371000,la=st.lat*Math.PI/180;
  return{e:(pt.lon-st.lon)*Math.PI/180*Math.cos(la)*R,
         n:(pt.lat-st.lat)*Math.PI/180*R};
}

function drawRadar(d,dist){
  if(viewMode==='map'){drawMap(d,dist);return;}
  fitCanvas();
  var c=$('radar'),x=c.getContext('2d'),W=c.width,H=c.height;
  var cx=W/2,cy=H/2,R=Math.min(W,H)/2-26;
  x.clearRect(0,0,W,H);
  var st=d.server,maxR=50;
  if(st.fix){
    hist.forEach(function(p){var en=toEN(st,p);
      maxR=Math.max(maxR,Math.hypot(en.e,en.n));});
    if(d.client.fix){var cur0=toEN(st,d.client);maxR=Math.max(maxR,Math.hypot(cur0.e,cur0.n));}
  }
  maxR*=1.1;
  // grid rings + scale labels
  x.strokeStyle='#21262d';x.fillStyle='#6e7681';x.font='10px system-ui';x.textAlign='left';
  for(var k=1;k<=3;k++){var rr=R*k/3;x.beginPath();x.arc(cx,cy,rr,0,7);x.stroke();
    x.fillText((maxR*k/3).toFixed(0)+'m',cx+4,cy-rr+12);}
  // cross + compass
  x.strokeStyle='#21262d';x.beginPath();
  x.moveTo(cx-R,cy);x.lineTo(cx+R,cy);x.moveTo(cx,cy-R);x.lineTo(cx,cy+R);x.stroke();
  x.fillStyle='#8b949e';x.font='11px system-ui';x.textAlign='center';
  x.fillText('N',cx,cy-R-8);x.fillText('S',cx,cy+R+14);
  x.fillText('E',cx+R+10,cy+4);x.fillText('W',cx-R-10,cy+4);
  x.textAlign='left';
  var sc=R/maxR;
  function plot(e,n){return[cx+e*sc,cy-n*sc];}
  // surfer path (faded by age: oldest dim, newest bright)
  var h=hist;
  if(st.fix&&h.length>1){
    for(var i=1;i<h.length;i++){
      var a=toEN(st,h[i-1]),b=toEN(st,h[i]);
      var pa=plot(a.e,a.n),pb=plot(b.e,b.n);
      x.strokeStyle='rgba(139,148,158,'+(0.15+0.6*i/h.length).toFixed(2)+')';
      x.lineWidth=2;x.beginPath();x.moveTo(pa[0],pa[1]);x.lineTo(pb[0],pb[1]);x.stroke();
    }
  }
  // servo aim lines (world bearing = heading + angle + mountOffset)
  var hdg=(d.mag.online&&d.mag.heading>=0)?d.mag.heading:0;
  var off=d.servo.mount_offset_deg||0;
  function aim(angle,col,w){
    var brg=((hdg+angle+off)%360+360)%360,r=brg*Math.PI/180;
    x.strokeStyle=col;x.lineWidth=w;x.beginPath();x.moveTo(cx,cy);
    x.lineTo(cx+Math.sin(r)*R,cy-Math.cos(r)*R);x.stroke();
  }
  if(d.servo.calibrated){
    aim(d.servo.target,'rgba(34,211,238,.55)',2);
    aim(d.servo.angle,'#f97316',3);
  }
  // current surfer marker + GPS status tag
  if(st.fix&&d.client.fix){
    var cur=toEN(st,d.client),p=plot(cur.e,cur.n);
    x.fillStyle='#f85149';x.beginPath();x.arc(p[0],p[1],6,0,7);x.fill();
    x.strokeStyle='#fff';x.lineWidth=1.5;x.stroke();
    drawGpsTag(x,p[0],p[1],W,H,
      'Surfer'+(dist!=null?(' '+dist.toFixed(0)+'m'):''),
      d.client.satellites,d.client.hdop);
  }
  // station centre
  x.fillStyle='#3b82f6';x.beginPath();x.arc(cx,cy,5,0,7);x.fill();
  if(!st.fix){x.fillStyle='#f85149';x.textAlign='center';
    x.fillText('攝影站無 GPS 定位',cx,cy+R/2);x.textAlign='left';}
}

// ---- absolute map: OpenStreetMap raster tiles + GPS tracks (Web Mercator) ----
// Tiles load straight from tile.openstreetmap.org; they appear only when the
// phone has internet (mobile data alongside the hotspot). Offline, tile loads
// fail silently and just the tracks + scale bar are shown over a dark backdrop.
function lonToX(lon,z){return (lon+180)/360*Math.pow(2,z)*256;}
function latToY(lat,z){var r=lat*Math.PI/180;
  return (1-Math.log(Math.tan(r)+1/Math.cos(r))/Math.PI)/2*Math.pow(2,z)*256;}
var tileCache={};
function getTile(z,tx,ty){
  var key=z+'/'+tx+'/'+ty,t=tileCache[key];
  if(t)return t;
  var img=new Image();img._ok=false;
  img.onload=function(){img._ok=true;
    if(viewMode==='map'&&last.track)
      drawMap(last.track,geoDist(last.track.server,last.track.client));};
  img.onerror=function(){img._ok=false;};
  img.src='https://'+'abc'[(tx+ty)%3]+'.tile.openstreetmap.org/'+z+'/'+tx+'/'+ty+'.png';
  tileCache[key]=img;return img;
}
function niceStep(v){
  if(v<=0)return 10;
  var p=Math.pow(10,Math.floor(Math.log(v)/Math.LN10)),f=v/p;
  var n=f<1.5?1:(f<3?2:(f<7?5:10));return n*p;
}
function drawPath(x,pts,plot,line,dot){
  if(pts.length===0)return;
  if(pts.length>1){
    x.strokeStyle=line;x.lineWidth=2;x.beginPath();
    for(var i=0;i<pts.length;i++){var p=plot(pts[i]);
      if(i===0)x.moveTo(p[0],p[1]);else x.lineTo(p[0],p[1]);}
    x.stroke();
  }
  var s=plot(pts[0]);x.strokeStyle=dot;x.lineWidth=2;x.beginPath();
  x.arc(s[0],s[1],5,0,7);x.stroke();           // start (hollow)
  var e=plot(pts[pts.length-1]);x.fillStyle=dot;x.beginPath();
  x.arc(e[0],e[1],6,0,7);x.fill();             // current (filled)
  x.strokeStyle='#fff';x.lineWidth=1.5;x.stroke();
}
function drawMap(d,dist){
  fitCanvas();
  var c=$('radar'),x=c.getContext('2d'),W=c.width,H=c.height;
  x.clearRect(0,0,W,H);
  var h=hist,cli=[],srv=[];
  for(var i=0;i<h.length;i++){
    if(h[i].lat||h[i].lon)cli.push({lat:h[i].lat,lon:h[i].lon});
    if(h[i].sfix)srv.push({lat:h[i].slat,lon:h[i].slon});
  }
  if(d.client.fix)cli.push({lat:d.client.lat,lon:d.client.lon});
  if(d.server.fix)srv.push({lat:d.server.lat,lon:d.server.lon});
  var all=cli.concat(srv);
  x.fillStyle='#0d1117';x.fillRect(0,0,W,H);
  if(all.length===0){
    x.fillStyle='#f85149';x.font='13px system-ui';x.textAlign='center';
    x.fillText('尚無 GPS 軌跡資料',W/2,H/2);x.textAlign='left';return;}
  // bounding box (deg) with a minimum span so a stationary cluster keeps context
  var minLat=1e9,maxLat=-1e9,minLon=1e9,maxLon=-1e9;
  all.forEach(function(p){minLat=Math.min(minLat,p.lat);maxLat=Math.max(maxLat,p.lat);
    minLon=Math.min(minLon,p.lon);maxLon=Math.max(maxLon,p.lon);});
  var midLat=(minLat+maxLat)/2,midLon=(minLon+maxLon)/2,minSpan=0.0009;
  if(maxLat-minLat<minSpan){minLat=midLat-minSpan/2;maxLat=midLat+minSpan/2;}
  if(maxLon-minLon<minSpan){minLon=midLon-minSpan/2;maxLon=midLon+minSpan/2;}
  var m=12,z=2;
  for(var zz=19;zz>=2;zz--){
    var w=Math.abs(lonToX(maxLon,zz)-lonToX(minLon,zz));
    var ht=Math.abs(latToY(minLat,zz)-latToY(maxLat,zz));
    if(w<=W-2*m&&ht<=H-2*m){z=zz;break;}
  }
  var cwx=lonToX(midLon,z),cwy=latToY(midLat,z);
  function plot(p){return[W/2+(lonToX(p.lon,z)-cwx),H/2+(latToY(p.lat,z)-cwy)];}
  // draw OSM tiles (loads only when the phone has internet; fails silently offline)
  var n=Math.pow(2,z),originX=cwx-W/2,originY=cwy-H/2;
  var tx0=Math.floor(originX/256),ty0=Math.floor(originY/256);
  var tx1=Math.floor((cwx+W/2)/256),ty1=Math.floor((cwy+H/2)/256);
  var anyTile=false;
  for(var ty=ty0;ty<=ty1;ty++){
    if(ty<0||ty>=n)continue;
    for(var tx=tx0;tx<=tx1;tx++){
      var wtx=((tx%n)+n)%n;
      var img=getTile(z,wtx,ty);
      var dx=Math.round(tx*256-originX),dy=Math.round(ty*256-originY);
      if(img._ok){x.drawImage(img,dx,dy,256,256);anyTile=true;}
    }
  }
  if(anyTile){x.fillStyle='rgba(13,17,23,0.12)';x.fillRect(0,0,W,H);}
  else{
    x.fillStyle='#8b949e';x.font='12px system-ui';x.textAlign='center';
    x.fillText('地圖底圖載入中…（手機需開行動數據）',W/2,18);x.textAlign='left';
  }
  // tracks on top
  drawPath(x,srv,plot,'rgba(59,130,246,0.95)','#3b82f6');
  drawPath(x,cli,plot,'rgba(248,81,73,0.95)','#f85149');
  if(cli.length&&d.client.fix){var lp=plot(cli[cli.length-1]);
    drawGpsTag(x,lp[0],lp[1],W,H,
      'Surfer'+(d.server.fix&&dist!=null?(' '+dist.toFixed(0)+'m'):''),
      d.client.satellites,d.client.hdop);}
  // scale bar (metres-per-pixel at this latitude & zoom)
  var mpp=156543.03392*Math.cos(midLat*Math.PI/180)/Math.pow(2,z);
  var step=niceStep(mpp*90),px2=step/mpp,bx=W-14-px2,by=H-18;
  x.strokeStyle='#fff';x.lineWidth=3;x.beginPath();
  x.moveTo(bx,by);x.lineTo(bx+px2,by);x.moveTo(bx,by-4);x.lineTo(bx,by+4);
  x.moveTo(bx+px2,by-4);x.lineTo(bx+px2,by+4);x.stroke();
  x.strokeStyle='#000';x.lineWidth=1;x.beginPath();
  x.moveTo(bx,by);x.lineTo(bx+px2,by);x.stroke();
  x.fillStyle='#fff';x.font='10px system-ui';x.textAlign='center';
  x.fillText(step<1000?step+' m':(step/1000)+' km',bx+px2/2,by-6);x.textAlign='left';
  // separation + attribution
  if(d.server.fix&&d.client.fix&&dist!=null){
    x.fillStyle='#fff';x.font='bold 12px system-ui';x.textAlign='left';
    x.fillText('站\u2194Surfer '+dist.toFixed(0)+' m',10,H-12);}
  x.fillStyle='rgba(230,237,243,.7)';x.font='9px system-ui';x.textAlign='right';
  x.fillText('© OpenStreetMap',W-6,12);x.textAlign='left';
}

showPage('radar');
refresh();refreshStatus();
setInterval(refresh,1000);
setInterval(refreshStatus,3000);
</script>
</body>
</html>
)rawlit";
