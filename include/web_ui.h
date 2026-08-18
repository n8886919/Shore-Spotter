#pragma once
// Embedded single-page control UI served by the shore station.
// The station joins the phone's hotspot (STA mode); open the station IP shown
// on its OLED at boot. The page itself (HTML/CSS/JS) is fully self-contained and
// loads with no internet. The optional 地圖 (map) view additionally streams
// OpenStreetMap raster tiles directly in the browser — those appear only when
// the phone also has mobile data; offline it falls back to tracks + scale bar.
//
// Operator flow:
//   One-time, on the 資訊 page: magnetometer hard-iron calibration (turn the rig
//   through one clockwise circle — which also tells the firmware which way the
//   board is mounted), then landmark calibration (centre a distant landmark in
//   the viewfinder and paste its coordinates — the only way to lock the aim, and
//   the only one that needs no compass). Both are solo, and together they
//   make mount_offset a constant that survives being packed up and set down at
//   another spot — so later sessions need no aiming at all.
//   手動 (Manual): drag the overlaid slider to aim the camera manually.
//   自動 (Auto):   locks the current manual aim as "facing the surfer" and
//                  auto-follows; pressing it again re-locks from the latest aim.
// Layout: full-height, no-scroll, two top tabs — 雷達 (radar/map canvas with the
//   雷達/地圖 + 手動/自動 toggles and the slider overlaid at the bottom) and
//   資訊 (live telemetry grid). The canvas auto-sizes to its container, and the
//   surfer marker carries a GPS-status tag (衛星 少/普通/好, 預期精度 ±N m,
//   Good/OK/Bad).
// GPS is reported in operator terms, not receiver terms: the satellite count
//   becomes 少/普通/好 and HDOP becomes 預期精度 in metres (see accM() below).
// The rolling firmware log is no longer a tab here — the 資訊 page has a button
//   that opens WEB_LOG_HTML (served at /log) in a separate browser tab, so it
//   can be watched next to the radar instead of replacing it.
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
<!-- 分頁圖示（見 tools/make_icon.py）。明確指定尺寸讓瀏覽器直接挑對，
     不必先去要 /favicon.ico 試試看 —— 那一趟往返打在只能同時服務一個連線的
     ESP32 上。 -->
<link rel="icon" type="image/png" sizes="32x32" href="/icon-32.png">
<link rel="icon" type="image/png" sizes="16x16" href="/icon-16.png">
<link rel="apple-touch-icon" href="/icon-192.png">
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
main{flex:1;position:relative;overflow:hidden}
.page{position:absolute;inset:0;display:none;flex-direction:column;padding:10px;gap:10px}
.page.on{display:flex}
#pgInfo{overflow:auto}
.card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:16px}
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
input[type=range]{flex:1;accent-color:var(--acc)}
input[type=range]:disabled{opacity:.4}
.ang{font-size:22px;font-weight:700;min-width:74px;text-align:right;
  font-variant-numeric:tabular-nums}
.ang small{font-size:12px;color:var(--mut);font-weight:400}
button{flex:1;min-width:96px;padding:11px 12px;border-radius:8px;border:1px solid var(--line);
  background:#21262d;color:var(--fg);font-size:14px;font-weight:600;cursor:pointer}
button:hover{border-color:var(--acc)}
button:disabled{opacity:.4;cursor:not-allowed}
#toast{position:fixed;left:50%;bottom:18px;transform:translateX(-50%);
  background:var(--bad);color:#fff;padding:9px 16px;border-radius:8px;font-size:13px;
  opacity:0;transition:opacity .25s;pointer-events:none;z-index:9}
#toast.show{opacity:1}
.lg{display:flex;gap:14px;font-size:11px;color:var(--mut);margin-top:8px;
  justify-content:center;flex-wrap:wrap}
.lg span{display:inline-flex;align-items:center;gap:5px}
.sw{width:18px;height:3px;border-radius:2px;display:inline-block}
.seg{padding:4px 12px;border:1px solid var(--line);background:#21262d;color:var(--mut);
  font-size:12px;font-weight:600;cursor:pointer;flex:none;min-width:0}
.seg:first-child{border-radius:6px 0 0 6px}
.seg:last-child{border-radius:0 6px 6px 0;border-left:0}
.seg.on{background:var(--acc);border-color:var(--acc);color:#fff}
.infoCols{display:grid;grid-template-columns:1fr 1fr;gap:10px}
.col{overflow:visible}
.col h3{margin:0 0 4px;font-size:14px;font-weight:700}
.sub{font-size:10px;color:var(--mut);text-transform:uppercase;letter-spacing:.04em;
  margin:9px 0 2px;border-bottom:1px solid var(--line);padding-bottom:2px}
.r{display:flex;justify-content:space-between;align-items:baseline;gap:6px;
  font-size:12px;padding:2px 0}
.r span{color:var(--mut)}
.calCard h3{margin:0 0 4px;font-size:14px;font-weight:700}
.calRow{display:flex;gap:8px;align-items:center;margin-top:8px;flex-wrap:wrap}
.calRow input[type=text]{flex:1;min-width:170px;padding:10px;border-radius:8px;
  border:1px solid var(--line);background:#0d1117;color:var(--fg);font-size:13px}
.hint3{display:block;margin-top:6px;font-size:11px;color:var(--mut);line-height:1.45}
.bar{height:6px;border-radius:3px;background:#21262d;overflow:hidden;margin-top:8px}
.bar>i{display:block;height:100%;width:0;background:var(--acc);transition:width .2s}
.r b{font-variant-numeric:tabular-nums}
.cmp{width:100%;border-collapse:collapse;font-size:13px}
.cmp th,.cmp td{padding:5px 8px;border-bottom:1px solid var(--line);text-align:right;
  font-variant-numeric:tabular-nums}
.cmp tr:last-child td{border-bottom:0}
.cmp th:first-child,.cmp td:first-child{text-align:left;color:var(--mut);font-weight:400}
.cmp thead th{color:var(--fg);font-weight:700}
/* 現場提醒訊息列：釘在最上方、兩個分頁都看得到。內容來自 /api/status 的
   alerts 陣列（措辭與門檻都在韌體端，見 main.cpp 的 appendAlertsJson）。 */
.alertBar{flex:none;display:none;flex-direction:column;background:var(--card);
  border-bottom:1px solid var(--line)}
.alertBar.on{display:flex}
.ahead{display:flex;align-items:center;gap:8px;padding:10px 12px;font-size:13px;
  font-weight:700;cursor:pointer;-webkit-user-select:none;user-select:none}
.ahead .chev{margin-left:auto;color:var(--mut);font-size:11px;font-weight:400}
.alertBar.err .ahead{color:var(--bad)}
.alertBar.warn .ahead{color:var(--warn)}
.alist{display:flex;flex-direction:column;gap:1px;max-height:34vh;overflow:auto;
  border-top:1px solid var(--line)}
.alertBar.fold .alist{display:none}
.al{display:flex;gap:9px;padding:9px 12px;background:var(--bg)}
.al .dot{flex:none;width:8px;height:8px;border-radius:50%;margin-top:5px}
.al.error .dot{background:var(--bad)}
.al.warn .dot{background:var(--warn)}
.al .txt{min-width:0}
.al .t{font-size:13px;font-weight:700;margin-bottom:3px}
.al.error .t{color:var(--bad)}
.al.warn .t{color:var(--warn)}
.al .d{font-size:12px;color:var(--mut);line-height:1.55}
</style>
</head>
<body>
<div id="alertBar" class="alertBar"></div>
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
          <tr><td>預期精度</td><td id="sAcc">--</td><td id="cAcc">--</td></tr>
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
    <div class="card calCard" style="flex:none">
      <h3>校正（各做一次即可）</h3>

      <div class="sub">1 · 磁力計 hard-iron</div>
      <div class="r"><span>狀態</span><b id="mcState">--</b></div>
      <div class="r"><span>擬合殘差（總）</span><b id="mcRes">--</b></div>
      <div class="r"><span>├ 橢圓（soft iron／不正交）</span><b id="mcEll">--</b></div>
      <div class="r"><span>└ 散射（晃動／雜訊）</span><b id="mcSct">--</b></div>
      <div class="r"><span>磁場強度</span><b id="mcFld">--</b></div>
      <div class="r"><span>上次轉過角度</span><b id="mcSwp">--</b></div>
      <div class="r"><span>水平面軸</span><b id="mcAxes">--</b></div>
      <div class="bar"><i id="mcBar"></i></div>
      <div class="calRow"><button id="btnMagCal" type="button">開始磁力計校正</button></div>
      <span class="hint3">按下後把整台機器放腳架上，<b>順時針（從上往下看）水平轉一整圈</b>。
        <b>不必剛好 360°</b>——進度條是 10° 分格的覆蓋率，滿 34/36 格（≈340°）就會自己完成，
        多轉、來回修、中途停手都沒關係。<b>速度也不必平滑</b>，忽快忽慢、停一下都行，只要淨方向一致。
        唯一的下限是別快到 <b>2 秒一圈</b>（取樣 20 Hz，快過 200°/s 才會跳格）；
        建議 10~30 秒。
        真正會傷品質的是<b>轉的時候板子跟著晃</b>——要在腳架雲台上轉，不要手捧著轉。
        板子<b>平放或立起來都可以</b>，這一圈同時判斷哪兩軸是水平面（見「水平面軸」）；
        但<b>方向要對</b>，反了 heading 的正負會相反。
        殘差看下面兩項哪個大：<b>橢圓</b>大 = soft iron（servo 鋼齒輪）或板子沒擺正交，
        轉得再漂亮也不會變好，要移動板子；<b>散射</b>大 = 轉的時候在晃或吃到 servo 電流。</span>

      <div class="sub" style="margin-top:16px">2 · 地標校正（鎖定 mount_offset，做一次就永久有效）</div>
      <div class="r"><span>目前 mount_offset</span><b id="mcOff">--</b></div>
      <div class="r"><span>與校正姿勢的方位差</span><b id="mcPose">--</b></div>
      <div class="r"><span>└ 該姿勢差造成的指向誤差</span><b id="mcPoseErr">--</b></div>

      <div class="calRow">
        <input id="lmCoord" type="text" placeholder="25.033611, 121.565000">
        <button id="btnLmCal" type="button">用目前鏡頭指向校正</button>
      </div>
      <span class="hint3">servo 設 90°，從<b>觀景窗</b>把 1 km 以外的地標對到畫面正中央，
        貼上該地標座標（Google Maps 右鍵可複製）再按鈕。需要攝影站有 GPS fix，
        但不需要追蹤器在場、不需要第二個人，也<b>完全不涉及羅盤與磁偏角</b>
        （座標算出來的就是真方位），照準精度 0.1° 級。
        <b>盡量選跟浪區同方向的地標</b>（外海的燈塔、防波堤端、離岸礁、遠處岬角）：
        校正姿勢與拍攝姿勢的方位差越小，上面那個「姿勢差造成的指向誤差」就越接近 0，
        因為磁力計的橢圓誤差會自己抵銷掉。先做完步驟 1。</span>

    </div>
    <div class="card" style="flex:none;display:flex;align-items:center;gap:10px;flex-wrap:wrap">
      <button id="btnClear" type="button">清除軌跡</button>
      <button id="btnExport" type="button">匯出 GPX（可上傳 Strava）</button>
      <span id="exportHint" class="hint2" style="margin-left:0">網頁最多保留過去 2 小時軌跡，雷達／地圖仍只顯示最近 5 分鐘；匯出後會自動清空。GPX 檔請自行到 Strava 網頁上傳</span>
    </div>
    <div class="card" style="flex:none;display:flex;align-items:center;gap:10px;flex-wrap:wrap">
      <button id="btnLog" type="button">開啟執行紀錄（新分頁）</button>
      <span class="hint2" style="margin-left:0">韌體的滾動 log（4 KB 環形緩衝）。開在另一個分頁，雷達畫面就不用讓位；那個分頁切到背景時會自動停止輪詢，避免和這裡搶 ESP32 的單一連線</span>
    </div>
  </section>
</main>

<div id="toast"></div>

<script>
var $=function(id){return document.getElementById(id);};
var last={track:null,status:null,alerts:[]};
var dragging=false;
var servoPendingAngle=null;
var servoSendTimer=0;
var servoSending=false;
var servoDragEnded=false;
var SERVO_SEND_INTERVAL_MS=75;
var viewMode='radar';
var page='radar';
var hist=[];               // client-accumulated path (server no longer stores it)
var HIST_RETAIN_MS=2*60*60*1000; // keep up to 2h in memory (for CSV export)
var RADAR_WINDOW_MS=5*60*1000;   // 雷達／地圖畫面固定只顯示最近 5 分鐘
var radarZoom=1;           // multiplicative zoom for radar view
var mapZoomDelta=0;        // additive zoom delta (in zoom levels) for map view
var pinchBaseRadarZoom=1;
var pinchBaseMapZoomDelta=0;
var pinchStartDist=0;
var activePointers={};

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
// Send at most one request at a time and collapse rapid slider events to the
// newest angle. This keeps motion responsive without flooding the ESP32.
//
// The timeout matters more than it looks. The ESP's web server serves one client
// at a time and closes the connection per request, so an aim POST can be left
// hanging while /api/track or /api/log holds the socket. Without a timeout that
// single hung fetch leaves servoSending stuck true for good: every later drag is
// dropped in silence, the slider and the angle readout still move, and the
// camera never does. Only a page reload brings it back — which is exactly what
// "manual mode stopped working" looks like from the beach.
var SERVO_SEND_TIMEOUT_MS=2000;
var lastServoWarn='',lastServoWarnMs=0;
// Aim failures used to be swallowed whole, so a firmware 409 ("pause tracking
// first") or 503 ("servo PWM unavailable") was indistinguishable from a dead
// slider. Show the reason, but collapse repeats — one drag can fail 20 times.
function servoWarn(m){
  var t=Date.now();
  if(m===lastServoWarn&&t-lastServoWarnMs<5000)return;
  lastServoWarn=m;lastServoWarnMs=t;toast(m);
}
// Always settles, never rejects, so the queue below cannot deadlock.
function postServoAngle(v){
  var ac=(typeof AbortController!=='undefined')?new AbortController():null;
  var timer=setTimeout(function(){if(ac)ac.abort();},SERVO_SEND_TIMEOUT_MS);
  return fetch('/api/servo?angle='+encodeURIComponent(v),
               ac?{method:'POST',signal:ac.signal}:{method:'POST'})
    .then(function(r){
      return r.json().catch(function(){return{};}).then(function(j){
        if(!r.ok||j.ok===false)servoWarn(j.error||('HTTP '+r.status));
      });
    })
    .catch(function(e){
      servoWarn(e&&e.name==='AbortError'?'角度指令逾時，岸上端沒回應'
                                        :'角度指令送不出去，檢查 WiFi');
    })
    .then(function(){clearTimeout(timer);});
}
function sendPendingServoAngle(){
  servoSendTimer=0;
  if(servoSending||servoPendingAngle===null)return;
  var v=servoPendingAngle;
  servoPendingAngle=null;
  servoSending=true;
  postServoAngle(v).then(function(){
    servoSending=false;
    if(servoPendingAngle!==null){
      if(servoDragEnded)sendPendingServoAngle();
      else servoSendTimer=setTimeout(sendPendingServoAngle,SERVO_SEND_INTERVAL_MS);
    }else if(servoDragEnded){
      servoDragEnded=false;
      dragging=false;
    }
  });
}
function queueServoAngle(v,isFinal){
  servoPendingAngle=v;
  if(isFinal)servoDragEnded=true;
  if(servoSending)return;
  if(servoSendTimer)clearTimeout(servoSendTimer);
  servoSendTimer=isFinal?0:setTimeout(sendPendingServoAngle,SERVO_SEND_INTERVAL_MS);
  if(isFinal)sendPendingServoAngle();
}
$('sld').addEventListener('input',function(){
  dragging=true;
  servoDragEnded=false;
  $('angTxt').textContent=this.value;
  queueServoAngle(this.value,false);
});
$('sld').addEventListener('change',function(){
  queueServoAngle(this.value,true);
});
// 手動 = move servo live with the slider; 自動 = lock the current aim as "facing the
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
// The log lives on its own page (/log) in a separate browser tab, so watching it
// no longer means giving up the radar.
$('btnLog').onclick=function(){window.open('/log','_blank');};
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

function clamp(v,min,max){return Math.max(min,Math.min(max,v));}
function setRadarZoomAbs(v){radarZoom=clamp(v,0.35,20);redraw();}
function setMapZoomDeltaAbs(v){mapZoomDelta=clamp(v,-6,6);redraw();}
function zoomByFactor(f){
  if(!(f>0))return;
  if(viewMode==='radar')setRadarZoomAbs(radarZoom*f);
  else setMapZoomDeltaAbs(mapZoomDelta+Math.log(f)/Math.LN2);
}
function pointerDist(a,b){
  var dx=a.x-b.x,dy=a.y-b.y;
  return Math.sqrt(dx*dx+dy*dy);
}
function firstTwoPointers(){
  var ks=Object.keys(activePointers);
  if(ks.length<2)return null;
  return[activePointers[ks[0]],activePointers[ks[1]]];
}
function beginPinch(){
  var pair=firstTwoPointers();
  if(!pair)return;
  pinchStartDist=pointerDist(pair[0],pair[1]);
  pinchBaseRadarZoom=radarZoom;
  pinchBaseMapZoomDelta=mapZoomDelta;
}

var radarCanvas=$('radar');
radarCanvas.addEventListener('wheel',function(ev){
  if(page!=='radar')return;
  ev.preventDefault();
  zoomByFactor(Math.exp(-ev.deltaY*0.0015));
},{passive:false});

radarCanvas.addEventListener('pointerdown',function(ev){
  activePointers[ev.pointerId]={x:ev.clientX,y:ev.clientY};
  if(Object.keys(activePointers).length===2){
    beginPinch();
    try{radarCanvas.setPointerCapture(ev.pointerId);}catch(_e){}
  }
});

radarCanvas.addEventListener('pointermove',function(ev){
  if(!activePointers[ev.pointerId])return;
  activePointers[ev.pointerId]={x:ev.clientX,y:ev.clientY};
  if(Object.keys(activePointers).length!==2||pinchStartDist<=0)return;
  var pair=firstTwoPointers();
  if(!pair)return;
  var d=pointerDist(pair[0],pair[1]);
  if(!(d>0))return;
  var factor=d/pinchStartDist;
  if(viewMode==='radar')setRadarZoomAbs(pinchBaseRadarZoom*factor);
  else setMapZoomDeltaAbs(pinchBaseMapZoomDelta+Math.log(factor)/Math.LN2);
});

function endPointer(ev){
  delete activePointers[ev.pointerId];
  if(Object.keys(activePointers).length<2){
    pinchStartDist=0;
  }else{
    beginPinch();
  }
}
radarCanvas.addEventListener('pointerup',endPointer);
radarCanvas.addEventListener('pointercancel',endPointer);
radarCanvas.addEventListener('pointerleave',endPointer);

function applyMode(sv){
  var auto=(sv.mode==='tracking');
  $('mManual').classList.toggle('on',!auto);
  $('mAuto').classList.toggle('on',auto);
  $('sld').disabled=auto;
  if(!dragging&&document.activeElement!==$('sld')){
    $('sld').value=Math.round(sv.angle);$('angTxt').textContent=Math.round(sv.angle);
  }
}

// HDOP is a dimensionless multiplier, which nobody on a beach can act on.
// Horizontal accuracy ~= HDOP x UERE, and 2.5 m is the usual 1-sigma UERE for a
// consumer single-band module like the ATGM336H, so HDOP 1.2 reads as +-3 m.
// The wire format still carries HDOP (protocol.h unchanged) — this is display
// only, so the number stays comparable with any other GPS tool.
var UERE_M=2.5;
function accM(hdop){return hdop>=0?hdop*UERE_M:null;}
// Whole metres only: UERE is a rule of thumb, so a decimal would claim accuracy
// the estimate does not have.
function fmtAcc(hdop){var a=accM(hdop);
  return a==null?'--':('\u00b1'+Math.round(a)+' m');}
// Satellite count as words for the same reason: 8 vs 11 changes no decision,
// "夠不夠" does. Thresholds match gpsGrade() below so the two never disagree.
function satWord(sats){
  if(sats<0)return '--';
  if(sats>=8)return '好';
  if(sats>=6)return '普通';
  return '少';
}
// GPS quality grade (same thresholds as the firmware, shared by both ends).
function gpsGrade(sats,hdop){
  if(sats<0||hdop<0||sats<=0)return 'miss';
  if(sats<4)return 'bad';
  if(hdop<=1.5&&sats>=8)return 'good';
  if(hdop<=3&&sats>=6)return 'ok';
  return 'bad';
}
// Leader line + small info card placed next to a marker (px,py) on the canvas.
function drawGpsTag(x,px,py,W,H,title,sats,hdop){
  var g=gpsGrade(sats,hdop);
  var col=g==='good'?'#3fb950':g==='ok'?'#d29922':g==='bad'?'#f85149':'#8b949e';
  var l1=title;
  var l2='衛星 '+satWord(sats)+'   '+fmtAcc(hdop);
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
// Full buffer keeps up to HIST_RETAIN_MS (2h) for CSV export; drawing only
// ever looks at the most recent RADAR_WINDOW_MS (5 min) slice via recentHist().
function pushHist(d){
  if(!d.linked||!d.client.fix)return;
  var e={t:Date.now(),lat:d.client.lat,lon:d.client.lon,sfix:d.server.fix?1:0,
         slat:d.server.fix?d.server.lat:0,slon:d.server.fix?d.server.lon:0};
  var l=hist[hist.length-1];
  if(l&&l.lat===e.lat&&l.lon===e.lon&&l.slat===e.slat&&l.slon===e.slon)return;
  hist.push(e);
  var cutoff=Date.now()-HIST_RETAIN_MS;
  while(hist.length&&hist[0].t<cutoff)hist.shift();
}
// Slice of hist within the last windowMs, walked backwards so it stays O(window
// size) instead of scanning the whole 2h buffer every redraw.
function recentHist(windowMs){
  var cutoff=Date.now()-windowMs,out=[];
  for(var i=hist.length-1;i>=0;i--){
    if(hist[i].t<cutoff)break;
    out.unshift(hist[i]);
  }
  return out;
}
// 清除目前保留的全部軌跡（雷達／地圖立即選回空白）。
function clearHist(){
  hist=[];
  toast('已清除軌跡');
  redraw();
}
$('btnClear').onclick=clearHist;

// GPX 匯出（Strava 可直接上傳的格式）：只包 Surfer（client）的軌跡點，
// 匯出完成後自動清空線上網頁保留的線上軌跡（避免重複上傳）。
function escXml(s){return String(s).replace(/[<>&'"]/g,function(c){
  return{'<':'&lt;','>':'&gt;','&':'&amp;',"'":'&apos;','"':'&quot;'}[c];});}
function exportTrackGpx(){
  if(!hist.length){toast('尚無軌跡資料可匯出');return;}
  var name='Shore Spotter '+new Date(hist[0].t).toISOString();
  var lines=['<?xml version="1.0" encoding="UTF-8"?>',
    '<gpx version="1.1" creator="Shore Spotter" xmlns="http://www.topografix.com/GPX/1/1">',
    '  <metadata><time>'+new Date().toISOString()+'</time></metadata>',
    '  <trk><name>'+escXml(name)+'</name><trkseg>'];
  hist.forEach(function(e){
    lines.push('    <trkpt lat="'+e.lat+'" lon="'+e.lon+'"><time>'+
      new Date(e.t).toISOString()+'</time></trkpt>');
  });
  lines.push('  </trkseg></trk>','</gpx>');
  var blob=new Blob([lines.join('\n')],{type:'application/gpx+xml'});
  var url=URL.createObjectURL(blob);
  var a=document.createElement('a');
  var ts=new Date().toISOString().replace(/[:.]/g,'-');
  a.href=url;a.download='shorespotter_track_'+ts+'.gpx';
  document.body.appendChild(a);a.click();document.body.removeChild(a);
  URL.revokeObjectURL(url);
  clearHist();
}
$('btnExport').onclick=exportTrackGpx;

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
    $('sSat').textContent=satWord(d.server.satellites);
    $('sAcc').textContent=fmtAcc(d.server.hdop);
    $('sBattV').textContent=(d.server.batt_pct>=0?d.server.batt_pct+'%':'--')+
      (d.server.charging?' \u26a1':'');
    $('sTemp').textContent=d.server.temp_c==null?'--':d.server.temp_c+'°C';
    $('sHum').textContent=d.server.humidity_pct==null?'--':d.server.humidity_pct+'%';
    $('sHdg').textContent=d.mag.online&&d.mag.heading>=0?d.mag.heading.toFixed(0)+'°':'--';
    $('mcOff').textContent=d.servo.calibrated?d.servo.mount_offset_deg.toFixed(1)+'°':'未校正';
    $('mcPose').textContent=d.servo.pose_delta_deg==null?'--':
      (d.servo.pose_delta_deg>0?'+':'')+d.servo.pose_delta_deg.toFixed(0)+'°';
    $('mcPoseErr').textContent=d.servo.pose_err_deg==null?'--':
      '\u2264'+d.servo.pose_err_deg.toFixed(2)+'°';
    // --- Client block ---
    $('cFix').textContent=d.client.fix?'有':'無';
    $('cSat').textContent=satWord(d.client.satellites);
    $('cAcc').textContent=fmtAcc(d.client.hdop);
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

// ---- 現場提醒 -------------------------------------------------------------
// 判斷與措辭全在韌體（main.cpp 的 appendAlertsJson / include/alerts.h），這裡只
// 負責畫。好處是 OLED 之後要顯示同一組提醒時不必再實作一次規則。
var alertsFold=false, alertsSig='';
function escHtml(t){
  return String(t).replace(/[&<>"]/g,function(c){
    return c==='&'?'&amp;':c==='<'?'&lt;':c==='>'?'&gt;':'&quot;';});
}
function renderAlerts(list){
  var bar=$('alertBar');
  list=list||[];
  // 每 3 秒重畫一次會把使用者正在捲動的清單捲回頂端，所以內容沒變就不動它。
  var sig=alertsFold+'|'+list.map(function(a){return a.id+a.level+a.detail;}).join('|');
  if(sig===alertsSig)return;
  alertsSig=sig;
  if(!list.length){bar.className='alertBar';bar.innerHTML='';return;}
  var nErr=0;
  for(var i=0;i<list.length;i++)if(list[i].level==='error')nErr++;
  var head='<div class="ahead"><span>'+(nErr?'⛔':'⚠')+'</span><span>'+
    (nErr?nErr+' 項要立刻處理'+(list.length>nErr?('，另有 '+(list.length-nErr)+' 項提醒'):'')
        :list.length+' 項提醒')+
    '</span><span class="chev">'+(alertsFold?'展開':'收合')+'</span></div>';
  var items='';
  for(var j=0;j<list.length;j++){
    var a=list[j];
    items+='<div class="al '+(a.level==='error'?'error':'warn')+'">'+
      '<span class="dot"></span><div class="txt"><div class="t">'+escHtml(a.title)+
      '</div><div class="d">'+escHtml(a.detail)+'</div></div></div>';
  }
  bar.className='alertBar on '+(nErr?'err':'warn')+(alertsFold?' fold':'');
  bar.innerHTML=head+'<div class="alist">'+items+'</div>';
  bar.firstChild.onclick=function(){alertsFold=!alertsFold;alertsSig='';renderAlerts(last.alerts);};
}

function refreshStatus(){
  fetch('/api/status').then(function(r){return r.json();}).then(function(s){
    last.status=s;
    last.alerts=s.alerts||[];
    renderAlerts(last.alerts);
    $('sRssi').textContent=s.lora.rssi?s.lora.rssi.toFixed(0)+' dBm':'--';
    $('sSnr').textContent=s.lora.snr!=null?s.lora.snr.toFixed(1)+' dB':'--';
    $('sDrop').textContent=(s.lora.drop_rate*100).toFixed(1)+'%';
    $('sUp').textContent=fmtUptime(s.health.uptime_s);
  }).catch(function(){});
}

function fmtUptime(s){var h=Math.floor(s/3600),m=Math.floor((s%3600)/60);
  return h>0?(h+'h'+m+'m'):(m+'m'+(s%60)+'s');}
// Battery %: 0% at 3.2 V, 100% at 4.15 V (matches the firmware scale).
function battPct(mv){return Math.max(0,Math.min(100,Math.round((mv-3200)*100/950)));}

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
  var recent=recentHist(RADAR_WINDOW_MS);
  if(st.fix){
    recent.forEach(function(p){var en=toEN(st,p);
      maxR=Math.max(maxR,Math.hypot(en.e,en.n));});
    if(d.client.fix){var cur0=toEN(st,d.client);maxR=Math.max(maxR,Math.hypot(cur0.e,cur0.n));}
  }
  maxR*=1.1;
  var visR=maxR/radarZoom;
  // 圈圈（同心圓格線）與旁邊文字的亮度：調這兩個 alpha（0~1，越大越亮）即可。
  var RING_ALPHA=0.5, LABEL_ALPHA=1;
  // grid rings + scale labels
  x.strokeStyle='rgba(255,255,255,'+RING_ALPHA+')';
  x.fillStyle='rgba(255,255,255,'+LABEL_ALPHA+')';x.font='10px system-ui';x.textAlign='left';
  for(var k=1;k<=3;k++){var rr=R*k/3;x.beginPath();x.arc(cx,cy,rr,0,7);x.stroke();
    x.fillText((visR*k/3).toFixed(0)+'m',cx+4,cy-rr+12);}
  // cross + compass
  x.strokeStyle='rgba(255,255,255,'+RING_ALPHA+')';x.beginPath();
  x.moveTo(cx-R,cy);x.lineTo(cx+R,cy);x.moveTo(cx,cy-R);x.lineTo(cx,cy+R);x.stroke();
  x.fillStyle='#fff';x.font='11px system-ui';x.textAlign='center';
  x.fillText('N',cx,cy-R-8);x.fillText('S',cx,cy+R+14);
  x.fillText('E',cx+R+10,cy+4);x.fillText('W',cx-R-10,cy+4);
  x.textAlign='left';
  var sc=R/visR;
  function plot(e,n){return[cx+e*sc,cy-n*sc];}
  // surfer path (faded by age: oldest dim, newest bright)
  var h=recent;
  if(st.fix&&h.length>1){
    for(var i=1;i<h.length;i++){
      var a=toEN(st,h[i-1]),b=toEN(st,h[i]);
      var pa=plot(a.e,a.n),pb=plot(b.e,b.n);
      x.strokeStyle='rgba(139,148,158,'+(0.15+0.6*i/h.length).toFixed(2)+')';
      x.lineWidth=2;x.beginPath();x.moveTo(pa[0],pa[1]);x.lineTo(pb[0],pb[1]);x.stroke();
    }
  }
  // servo aim lines (angle increases CCW; compass bearing increases CW)
  var hdg=(d.mag.online&&d.mag.heading>=0)?d.mag.heading:0;
  var off=d.servo.mount_offset_deg||0;
  function aim(angle,col,w){
    var brg=((hdg-angle+off)%360+360)%360,r=brg*Math.PI/180;
    x.strokeStyle=col;x.lineWidth=w;x.beginPath();x.moveTo(cx,cy);
    x.lineTo(cx+Math.sin(r)*R,cy-Math.cos(r)*R);x.stroke();
  }
  // Servo 目前：畫成一個 5 度扇形雷達波束（半徑方向漸層 + 發光邊緣），
  // 比單一細線更有「雷達掃描」的感覺；halfWidthDeg 可調整扇形寬度。
  function aimSector(angle,rgb,halfWidthDeg){
    var brg=((hdg-angle+off)%360+360)%360;
    var a0=(brg-halfWidthDeg-90)*Math.PI/180,a1=(brg+halfWidthDeg-90)*Math.PI/180;
    var grad=x.createRadialGradient(cx,cy,0,cx,cy,R);
    grad.addColorStop(0,'rgba('+rgb+',0.04)');
    grad.addColorStop(0.6,'rgba('+rgb+',0.30)');
    grad.addColorStop(1,'rgba('+rgb+',0.88)');
    x.save();
    x.shadowColor='rgba('+rgb+',0.9)';x.shadowBlur=10;
    x.fillStyle=grad;
    x.beginPath();x.moveTo(cx,cy);x.arc(cx,cy,R,a0,a1);x.closePath();x.fill();
    x.restore();
    x.strokeStyle='rgba('+rgb+',0.95)';x.lineWidth=1.5;
    x.beginPath();x.arc(cx,cy,R,a0,a1);x.stroke();
  }
  if(d.servo.calibrated){
    aim(d.servo.target,'rgba(34,211,238,.55)',2);
    aimSector(d.servo.angle,'249,115,22',2.5);   // 5° 扇形 (±2.5°)
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
  var h=recentHist(RADAR_WINDOW_MS),cli=[],srv=[];
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
  // Centre on the LIVE station/surfer midpoint, not on the track's bounding box.
  // Using the box made the view drift away as the trail grew, so the two things
  // you actually care about slid off-centre while old track pulled the camera.
  var minSpan=0.0009,midLat,midLon;
  if(d.server.fix&&d.client.fix){
    midLat=(d.server.lat+d.client.lat)/2;midLon=(d.server.lon+d.client.lon)/2;
  }else if(d.server.fix){midLat=d.server.lat;midLon=d.server.lon;}
  else if(d.client.fix){midLat=d.client.lat;midLon=d.client.lon;}
  else{ // no live fix at all — fall back to the old track-box centre
    var bl=1e9,bh=-1e9,gl=1e9,gh=-1e9;
    all.forEach(function(p){bl=Math.min(bl,p.lat);bh=Math.max(bh,p.lat);
      gl=Math.min(gl,p.lon);gh=Math.max(gh,p.lon);});
    midLat=(bl+bh)/2;midLon=(gl+gh)/2;
  }
  // Span symmetric about that centre and just wide enough to hold both live
  // points, so auto-zoom still frames them but the centre never moves with the
  // trail. Pinch (mapZoomDelta) still overrides to see more track.
  var halfLat=minSpan/2,halfLon=minSpan/2;
  [d.server.fix?d.server:null,d.client.fix?d.client:null].forEach(function(p){
    if(!p)return;
    halfLat=Math.max(halfLat,Math.abs(p.lat-midLat));
    halfLon=Math.max(halfLon,Math.abs(p.lon-midLon));
  });
  var minLat=midLat-halfLat,maxLat=midLat+halfLat;
  var minLon=midLon-halfLon,maxLon=midLon+halfLon;
  var m=12,z=2;
  for(var zz=19;zz>=2;zz--){
    var w=Math.abs(lonToX(maxLon,zz)-lonToX(minLon,zz));
    var ht=Math.abs(latToY(minLat,zz)-latToY(maxLat,zz));
    if(w<=W-2*m&&ht<=H-2*m){z=zz;break;}
  }
  var zf=clamp(z+mapZoomDelta,2,19);
  var zi=Math.floor(zf),frac=zf-zi,zoomScale=Math.pow(2,frac);
  var cwx=lonToX(midLon,zi),cwy=latToY(midLat,zi);
  function plot(p){return[W/2+(lonToX(p.lon,zi)-cwx)*zoomScale,
                           H/2+(latToY(p.lat,zi)-cwy)*zoomScale];}
  // draw OSM tiles (loads only when the phone has internet; fails silently offline)
  var n=Math.pow(2,zi),originX=cwx-W/(2*zoomScale),originY=cwy-H/(2*zoomScale);
  var tx0=Math.floor(originX/256),ty0=Math.floor(originY/256);
  var tx1=Math.floor((cwx+W/(2*zoomScale))/256),ty1=Math.floor((cwy+H/(2*zoomScale))/256);
  var anyTile=false;
  for(var ty=ty0;ty<=ty1;ty++){
    if(ty<0||ty>=n)continue;
    for(var tx=tx0;tx<=tx1;tx++){
      var wtx=((tx%n)+n)%n;
      var img=getTile(zi,wtx,ty);
      var dx=Math.round((tx*256-originX)*zoomScale),dy=Math.round((ty*256-originY)*zoomScale);
      if(img._ok){x.drawImage(img,dx,dy,Math.ceil(256*zoomScale),Math.ceil(256*zoomScale));anyTile=true;}
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
  var mpp=156543.03392*Math.cos(midLat*Math.PI/180)/Math.pow(2,zf);
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
// ---- calibration ----
var magPollTimer=0;
function renderMagCal(m){
  var st={idle:'未校正',collecting:'量測中…',done:'已校正',failed:'失敗'}[m.state]||m.state;
  if(!m.online)st='無磁力計';
  else if(m.state==='failed'&&m.error)st='失敗：'+m.error;
  $('mcState').textContent=st;
  $('mcRes').textContent=m.residual_deg==null?'--':m.residual_deg.toFixed(2)+'°';
  $('mcEll').textContent=m.ellipse_deg==null?'--':m.ellipse_deg.toFixed(2)+'°';
  $('mcSct').textContent=m.scatter_deg==null?'--':m.scatter_deg.toFixed(2)+'°';
  $('mcSwp').textContent=m.sweep_deg?Math.round(m.sweep_deg)+'°':'--';
  $('mcFld').textContent=m.field_gauss==null?'--':m.field_gauss.toFixed(3)+' G';
  $('mcAxes').textContent=m.calibrated?(m.axes||'--'):'未判定（預設 X,Y）';
  var pct=m.state==='collecting'?m.coverage_pct:(m.calibrated?100:0);
  $('mcBar').style.width=pct+'%';
  $('btnMagCal').textContent=m.state==='collecting'
    ?('轉圈中… '+m.coverage_pct+'%（點此取消）'):'開始磁力計校正';
}
function pollMagCal(){
  fetch('/api/mag/calibrate').then(function(r){return r.json();}).then(function(m){
    renderMagCal(m);
    if(m.state==='collecting'){magPollTimer=setTimeout(pollMagCal,300);}
    else{
      magPollTimer=0;
      if(m.state==='done'){
        toast('校正完成：殘差 '+m.residual_deg.toFixed(2)+'°（橢圓 '+
          m.ellipse_deg.toFixed(2)+'° + 散射 '+m.scatter_deg.toFixed(2)+
          '°），轉過 '+Math.round(m.sweep_deg)+'°，水平面軸 '+m.axes);
        if(m.ellipse_deg>1&&m.ellipse_deg>2*m.scatter_deg)
          setTimeout(function(){toast('橢圓為主：soft iron 或板子沒擺正交，轉得再順也沒用，要移動板子');},2800);
        else if(m.scatter_deg>1&&m.scatter_deg>2*m.ellipse_deg)
          setTimeout(function(){toast('散射為主：轉的時候板子在晃，改在腳架雲台上轉並保持水平');},2800);
      }
      else if(m.state==='failed')toast('校正失敗：'+(m.error||'未知'));
    }
  }).catch(function(){magPollTimer=0;});
}
$('btnMagCal').onclick=function(){
  var busy=magPollTimer!==0;
  post('/api/mag/calibrate'+(busy?'?action=cancel':'')).then(function(m){
    renderMagCal(m);
    if(magPollTimer){clearTimeout(magPollTimer);magPollTimer=0;}
    if(m.state==='collecting'){toast('開始轉圈：慢慢順時針水平轉一整圈');pollMagCal();}
  }).catch(function(){});
};
// Accepts "25.033611, 121.565000" — the exact format Google Maps copies.
$('btnLmCal').onclick=function(){
  var m=/(-?\d+(?:\.\d+)?)\s*[, ]\s*(-?\d+(?:\.\d+)?)/.exec($('lmCoord').value||'');
  if(!m){toast('座標格式看不懂，例如 25.033611, 121.565000');return;}
  post('/api/track/calibrate?lat='+m[1]+'&lon='+m[2]).then(function(j){
    $('mcOff').textContent=j.mount_offset_deg.toFixed(1)+'°';
    toast('已鎖定 mount_offset '+j.mount_offset_deg.toFixed(1)+'°（地標方位 '+
      j.bearing.toFixed(1)+'°、距離 '+j.distance_m+' m）');
    if(j.warning)setTimeout(function(){toast(j.warning);},2800);
    refresh();
  }).catch(function(){});
};

refreshStatus();          // 提醒不要等到第一個 3 秒週期才出現
setInterval(refresh,1000);
setInterval(refreshStatus,3000);
fetch('/api/mag/calibrate').then(function(r){return r.json();})
  .then(renderMagCal).catch(function(){});
</script>
</body>
</html>
)rawlit";

// Standalone log viewer served at /log, opened from the 資訊 page in its own
// browser tab. It used to be a third tab inside WEB_UI_HTML, which meant reading
// the log cost you the radar; a separate tab can sit next to it instead.
//
// The firmware keeps a 4 KB ring buffer and hands back only what this page has
// not seen yet, keyed on an absolute byte offset, so polling stays cheap.
// It polls only while the tab is actually on screen: the ESP32's WebServer
// serves one connection at a time, so a forgotten background log tab would keep
// stealing turns from the radar tab's 1 Hz /api/track.
static const char WEB_LOG_HTML[] = R"loglit(
<!DOCTYPE html>
<html lang="zh-Hant">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#0d1117">
<link rel="icon" type="image/png" sizes="32x32" href="/icon-32.png">
<link rel="icon" type="image/png" sizes="16x16" href="/icon-16.png">
<title>Shore Spotter · 紀錄</title>
<style>
:root{--bg:#0d1117;--card:#161b22;--line:#30363d;--fg:#e6edf3;--mut:#8b949e;--acc:#3b82f6}
*{box-sizing:border-box}
html,body{margin:0;height:100%;background:var(--bg);color:var(--fg);
  font-family:system-ui,-apple-system,"Segoe UI",Roboto,"Noto Sans TC",sans-serif}
body{height:100dvh;display:flex;flex-direction:column;overflow:hidden;padding:10px;gap:10px}
.card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:12px}
.head{flex:none;display:flex;align-items:center;gap:12px;flex-wrap:wrap}
.head button{padding:9px 14px;border-radius:8px;border:1px solid var(--line);
  background:#21262d;color:var(--fg);font-size:13px;font-weight:600;cursor:pointer;
  font-family:inherit}
.head button:hover{border-color:var(--acc)}
.head label{font-size:12px;color:var(--mut);display:flex;align-items:center;gap:5px}
.head #stat{font-size:11px;color:var(--mut);margin-left:auto}
#box{flex:1;min-height:0;margin:0;overflow:auto;white-space:pre-wrap;word-break:break-all;
  font:11px/1.45 ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;color:var(--fg)}
</style>
</head>
<body>
<div class="card head">
  <button id="btnClear" type="button">清除</button>
  <label><input id="follow" type="checkbox" checked> 自動捲到最新</label>
  <span id="stat">--</span>
</div>
<pre id="box" class="card"></pre>

<script>
var $=function(id){return document.getElementById(id);};
var next=0, timer=0, busy=false;
function pump(){
  if(busy)return;
  busy=true;
  fetch('/api/log?from='+next).then(function(r){
    var n=parseInt(r.headers.get('X-Log-Next')||'0',10);
    var dropped=r.headers.get('X-Log-Dropped')==='1';
    return r.text().then(function(t){return{t:t,n:n,d:dropped};});
  }).then(function(o){
    var box=$('box');
    // Follow only if already pinned to the bottom, so reading scrollback is not
    // yanked away every 2 s.
    var atEnd=box.scrollTop+box.clientHeight>=box.scrollHeight-24;
    if(o.d&&next!==0)box.textContent+='\n--- 略過部分紀錄（緩衝已滿或裝置重開）---\n';
    if(o.t)box.textContent+=o.t;
    if(box.textContent.length>60000)box.textContent=box.textContent.slice(-40000);
    next=o.n;
    $('stat').textContent=o.n+' bytes';
    if($('follow').checked&&atEnd)box.scrollTop=box.scrollHeight;
  }).catch(function(){}).then(function(){busy=false;});
}
function start(){if(!timer){pump();timer=setInterval(pump,2000);}}
function stop(){if(timer){clearInterval(timer);timer=0;}}
document.addEventListener('visibilitychange',function(){
  if(document.hidden)stop();else start();
});
$('btnClear').onclick=function(){
  fetch('/api/log',{method:'POST'})
    .then(function(){$('box').textContent='';next=0;pump();})
    .catch(function(){});
};
start();
</script>
</body>
</html>
)loglit";
