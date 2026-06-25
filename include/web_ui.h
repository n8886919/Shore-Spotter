#pragma once
// Embedded single-page control UI served by the shore station.
// The station joins the phone's hotspot (STA mode); open the station IP shown
// on its OLED at boot. Fully self-contained (no external CDN / map tiles) so it
// works even when the phone has no internet.
//
// Operator flow:
//   1. Drag the servo slider to aim the camera at the surfer (manual).
//   2. Press 開始追蹤 (Start) to lock the aim and auto-follow.
//   3. 暫停 (Pause) holds the camera; 恢復 (Resume) continues tracking.
// The radar shows the surfer's last 5 min path and the servo aim, both relative
// to the camera station (GPS + magnetometer).
static const char WEB_UI_HTML[] = R"rawlit(
<!DOCTYPE html>
<html lang="zh-Hant">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1">
<title>Shore Spotter</title>
<style>
:root{--bg:#0d1117;--card:#161b22;--line:#30363d;--fg:#e6edf3;--mut:#8b949e;
  --acc:#3b82f6;--ok:#3fb950;--warn:#d29922;--bad:#f85149;--trk:#f97316;--tgt:#22d3ee}
*{box-sizing:border-box}
html,body{margin:0;background:var(--bg);color:var(--fg);
  font-family:system-ui,-apple-system,"Segoe UI",Roboto,"Noto Sans TC",sans-serif}
header{display:flex;flex-wrap:wrap;align-items:center;gap:6px 14px;
  padding:8px 12px;background:var(--card);border-bottom:1px solid var(--line);
  font-size:12px;position:sticky;top:0;z-index:5}
header .t{font-weight:700;font-size:14px;margin-right:6px}
.pill{display:inline-flex;align-items:center;gap:5px;color:var(--mut)}
.pill b{color:var(--fg);font-variant-numeric:tabular-nums}
.dot{width:9px;height:9px;border-radius:50%;background:var(--mut);display:inline-block}
.dot.ok{background:var(--ok)}.dot.warn{background:var(--warn)}.dot.bad{background:var(--bad)}
main{max-width:980px;margin:0 auto;padding:12px;display:grid;gap:12px;
  grid-template-columns:1fr}
@media(min-width:760px){main{grid-template-columns:minmax(320px,1fr) 1fr}}
.card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:14px}
.card h2{margin:0 0 10px;font-size:13px;color:var(--mut);font-weight:600;
  text-transform:uppercase;letter-spacing:.04em}
#radarWrap{display:flex;justify-content:center}
canvas{width:100%;max-width:420px;height:auto;touch-action:none}
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
.grid{display:grid;grid-template-columns:repeat(2,1fr);gap:10px}
.kv{background:#0d1117;border:1px solid var(--line);border-radius:8px;padding:8px 10px}
.kv .k{font-size:11px;color:var(--mut)}
.kv .v{font-size:17px;font-weight:700;font-variant-numeric:tabular-nums;margin-top:2px}
.kv .v small{font-size:11px;color:var(--mut);font-weight:400}
#toast{position:fixed;left:50%;bottom:18px;transform:translateX(-50%);
  background:var(--bad);color:#fff;padding:9px 16px;border-radius:8px;font-size:13px;
  opacity:0;transition:opacity .25s;pointer-events:none;z-index:9}
#toast.show{opacity:1}
.lg{display:flex;gap:14px;font-size:11px;color:var(--mut);margin-top:8px;
  justify-content:center;flex-wrap:wrap}
.lg span{display:inline-flex;align-items:center;gap:5px}
.sw{width:18px;height:3px;border-radius:2px;display:inline-block}
</style>
</head>
<body>
<header>
  <span class="t">Shore Spotter</span>
  <span class="pill"><span id="dLink" class="dot"></span><b id="link">--</b></span>
  <span class="pill">RX <b id="age">--</b>s</span>
  <span class="pill"><span id="dGps" class="dot"></span>GPS <b id="sats">--</b></span>
  <span class="pill">羅盤 <b id="hdg">--</b>&deg;</span>
  <span class="pill">RSSI <b id="rssi">--</b></span>
  <span class="pill">Batt <b id="sbatt">--</b></span>
  <span class="pill">Uptime <b id="up">--</b></span>
</header>

<main>
  <section class="card">
    <h2>追蹤雷達（過去 5 分鐘）</h2>
    <div id="radarWrap"><canvas id="radar" width="420" height="420"></canvas></div>
    <div class="lg">
      <span><i class="sw" style="background:var(--trk)"></i>Servo 目前</span>
      <span><i class="sw" style="background:var(--tgt)"></i>Servo 目標</span>
      <span><i class="sw" style="background:var(--bad)"></i>Surfer</span>
      <span><i class="sw" style="background:var(--mut)"></i>路徑</span>
    </div>
  </section>

  <section class="card">
    <h2>Servo 控制</h2>
    <div class="modeRow">
      <span>模式</span><span id="mode" class="badge idle">--</span>
      <span id="calib" class="pill" style="margin-left:auto">校正 <b>--</b></span>
    </div>
    <div class="sliderRow">
      <input id="sld" type="range" min="0" max="180" step="1" value="90">
      <div class="ang"><span id="angTxt">90</span><small>&deg;</small></div>
    </div>
    <div class="btns">
      <button id="bStart" class="go">開始追蹤</button>
      <button id="bPause" class="pause" style="display:none">暫停</button>
      <button id="bResume" style="display:none">恢復</button>
      <button id="bStop" style="display:none">停止</button>
    </div>
    <div class="hint">先拖動滑桿讓鏡頭對準 surfer，再按「開始追蹤」鎖定基準並自動跟隨。
      追蹤中滑桿停用；按「暫停」可定住鏡頭並重新手動微調。</div>
  </section>

  <section class="card" style="grid-column:1/-1">
    <h2>即時資訊</h2>
    <div class="grid">
      <div class="kv"><div class="k">距離</div><div class="v"><span id="dist">--</span><small> m</small></div></div>
      <div class="kv"><div class="k">方位（站→surfer）</div><div class="v"><span id="brg">--</span><small> &deg;</small></div></div>
      <div class="kv"><div class="k">Surfer 速度</div><div class="v"><span id="spd">--</span><small> km/h</small></div></div>
      <div class="kv"><div class="k">Surfer 航向</div><div class="v"><span id="crs">--</span><small> &deg;</small></div></div>
      <div class="kv"><div class="k">加速度</div><div class="v"><span id="acc">--</span><small> m/s&sup2;</small></div></div>
      <div class="kv"><div class="k">Surfer 電量</div><div class="v"><span id="cbatt">--</span><small> mV</small></div></div>
      <div class="kv"><div class="k">LoRa SNR</div><div class="v"><span id="snr">--</span><small> dB</small></div></div>
      <div class="kv"><div class="k">丟包率</div><div class="v"><span id="drop">--</span><small> %</small></div></div>
      <div class="kv"><div class="k">站體溫度</div><div class="v"><span id="temp">--</span><small> &deg;C</small></div></div>
      <div class="kv"><div class="k">可用記憶體</div><div class="v"><span id="heap">--</span><small> KB</small></div></div>
    </div>
  </section>
</main>

<div id="toast"></div>

<script>
var $=function(id){return document.getElementById(id);};
var last={track:null,status:null};
var dragging=false;

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
$('bStart').onclick=function(){post('/api/track/start').then(refresh);};
$('bPause').onclick=function(){post('/api/track/pause').then(refresh);};
$('bResume').onclick=function(){post('/api/track/resume').then(refresh);};
$('bStop').onclick=function(){post('/api/track/stop').then(refresh);};

function setDot(el,state){el.className='dot '+state;}

function applyMode(sv){
  var m=sv.mode;
  $('mode').textContent={tracking:'追蹤中',paused:'已暫停',manual:'手動',idle:'待機'}[m]||m;
  $('mode').className='badge '+m;
  $('calib').querySelector('b').textContent=sv.calibrated?'已鎖定':'未校正';
  var tracking=(m==='tracking');
  $('sld').disabled=tracking;
  $('bStart').style.display=tracking?'none':'';
  $('bPause').style.display=tracking?'':'none';
  $('bResume').style.display=(!tracking&&sv.calibrated)?'':'none';
  $('bStop').style.display=(m==='paused'||m==='manual')&&sv.calibrated?'':'none';
  if(!dragging&&document.activeElement!==$('sld')){
    $('sld').value=Math.round(sv.angle);$('angTxt').textContent=Math.round(sv.angle);
  }
}

function refresh(){
  return fetch('/api/track').then(function(r){return r.json();}).then(function(d){
    last.track=d;
    var linked=d.linked;
    $('link').textContent=linked?'ONLINE':'NO SIGNAL';
    setDot($('dLink'),linked?'ok':'bad');
    $('age').textContent=d.client.last_rx_sec>=0?d.client.last_rx_sec:'--';
    setDot($('dGps'),d.server.fix?'ok':'bad');
    $('hdg').textContent=d.mag.online&&d.mag.heading>=0?d.mag.heading.toFixed(0):'--';
    $('sbatt').textContent=d.telemetry.batt_mv>0?d.telemetry.batt_mv+' mV':'--';
    applyMode(d.servo);

    $('brg').textContent=d.bearing>=0?d.bearing.toFixed(0):'--';
    $('cbatt').textContent=d.telemetry.batt_mv>0?d.telemetry.batt_mv:'--';
    $('spd').textContent=d.client.fix?(d.client.speed_cms*0.036).toFixed(1):'--';
    $('crs').textContent=d.client.fix?(d.client.course_deg10/10).toFixed(0):'--';
    $('acc').textContent=d.client.fix?(d.client.accel_cms2/100).toFixed(2):'--';
    $('temp').textContent=d.server.temp_c==null?'--':d.server.temp_c.toFixed(1);

    var dist=geoDist(d.server,d.client);
    $('dist').textContent=(d.server.fix&&d.client.fix&&dist!=null)?dist.toFixed(0):'--';
    drawRadar(d,dist);
  }).catch(function(){});
}

function refreshStatus(){
  fetch('/api/status').then(function(r){return r.json();}).then(function(s){
    last.status=s;
    $('sats').textContent=s.server_gps.satellites>=0?s.server_gps.satellites:'--';
    $('rssi').textContent=s.lora.rssi?s.lora.rssi.toFixed(0):'--';
    $('snr').textContent=s.lora.snr!=null?s.lora.snr.toFixed(1):'--';
    $('drop').textContent=(s.lora.drop_rate*100).toFixed(1);
    $('heap').textContent=(s.health.heap_free/1024).toFixed(0);
    $('up').textContent=fmtUptime(s.health.uptime_s);
  }).catch(function(){});
}

function fmtUptime(s){var h=Math.floor(s/3600),m=Math.floor((s%3600)/60);
  return h>0?(h+'h'+m+'m'):(m+'m'+(s%60)+'s');}

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
  var c=$('radar'),x=c.getContext('2d'),W=c.width,H=c.height;
  var cx=W/2,cy=H/2,R=Math.min(W,H)/2-26;
  x.clearRect(0,0,W,H);
  var st=d.server,maxR=50;
  if(st.fix){
    (d.history||[]).forEach(function(p){var en=toEN(st,p);
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
  var h=d.history||[];
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
  // current surfer marker + distance label
  if(st.fix&&d.client.fix){
    var cur=toEN(st,d.client),p=plot(cur.e,cur.n);
    x.fillStyle='#f85149';x.beginPath();x.arc(p[0],p[1],6,0,7);x.fill();
    x.strokeStyle='#fff';x.lineWidth=1.5;x.stroke();
    if(dist!=null){x.fillStyle='#e6edf3';x.font='11px system-ui';
      x.fillText(dist.toFixed(0)+'m',p[0]+9,p[1]+4);}
  }
  // station centre
  x.fillStyle='#3b82f6';x.beginPath();x.arc(cx,cy,5,0,7);x.fill();
  if(!st.fix){x.fillStyle='#f85149';x.textAlign='center';
    x.fillText('攝影站無 GPS 定位',cx,cy+R/2);x.textAlign='left';}
}

refresh();refreshStatus();
setInterval(refresh,1000);
setInterval(refreshStatus,3000);
</script>
</body>
</html>
)rawlit";
