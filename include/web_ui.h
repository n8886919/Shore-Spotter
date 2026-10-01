#pragma once
// Embedded single-page control UI served by the shore station.
// Before UI changes, read docs/web-ui-design.md (confirmed user design contract).
// The station joins the phone's hotspot (STA mode); open the station IP shown
// on its OLED at boot. The page itself (HTML/CSS/JS) is fully self-contained and
// loads with no internet. The optional 地圖 (map) view additionally streams
// OpenStreetMap raster tiles directly in the browser — those appear only when
// the phone also has mobile data; offline it falls back to tracks + scale bar.
//
// Operator flow:
// Calibration: type the heading read from the compass attached to the camera.
// North=0, East=90; automatic start/resume preserves the saved reference.
// GPS and UART are exclusive modes; a missing input holds the current angle.
// Manual stops tracking and allows adjustment on the radar's outer ring.
// Layout: full-height radar/map with an overlaid Info button and
//   雷達/地圖 + 手動/GPS/UART controls over the canvas) and
//   資訊 (shared speed limit, GPS prediction, compass calibration and telemetry).
//   The canvas auto-sizes to its container, and the
//   surfer marker carries a GPS-status tag (衛星 少/普通/好, 預期精度 ±N m,
//   Good/OK/Bad).
// GPS is reported in operator terms, not receiver terms: the satellite count
//   becomes 少/普通/好 and HDOP becomes 預期精度 in metres (see accM() below).
// Manual is the boot default; GPS is selectable with current valid positions; quality gates prediction only.
static const char WEB_UI_HTML[] = R"rawlit(
<!DOCTYPE html>
<html lang="zh-Hant">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,viewport-fit=cover">
<meta name="mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">
<meta name="theme-color" content="#ffffff">
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
:root{--bg:#ffffff;--card:#f4f7f4;--line:#a6b5a9;--fg:#11251a;--mut:#405849;
  --acc:#076c38;--ok:#086a35;--warn:#825200;--bad:#b2241d;color-scheme:light}
*{box-sizing:border-box}
html,body{margin:0;height:100%;background:var(--bg);color:var(--fg);
  font-family:system-ui,-apple-system,"Segoe UI",Roboto,"Noto Sans TC",sans-serif}
body{height:100dvh;height:var(--app-height,100dvh);position:relative;display:flex;flex-direction:column;overflow:hidden}
main{flex:1;position:relative;overflow:hidden}
.page{position:absolute;inset:0;display:none;flex-direction:column;padding:10px;gap:10px}
.page.on{display:flex}
#pgInfo{overflow:auto;padding-bottom:64px}
#pgRadar{padding:0;gap:0;overflow:hidden}
#tabRadar{flex:none;align-self:flex-start;min-height:40px}
.card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:16px}
.radarHead{display:flex;align-items:center;gap:10px;flex:none;
  font-size:13px;color:var(--mut);font-weight:600}
.radarHead .segwrap{display:inline-flex}
.radarOverlay{position:absolute;inset:8px 8px auto;z-index:1;pointer-events:none;
  display:flex;justify-content:space-between;align-items:flex-start;gap:6px}
.radarOverlay .segwrap{pointer-events:auto;backdrop-filter:blur(3px);border-radius:9px;overflow:hidden}
.radarOverlay .seg{padding:10px 12px;font-size:18px;line-height:24px;min-height:46px;
  background:rgba(255,255,255,.76);border-color:rgba(143,169,151,.8)}
.radarOverlay .seg:first-child{border-radius:9px 0 0 9px}
.radarOverlay .seg:last-child{border-radius:0 9px 9px 0}
.radarOverlay .seg.on{background:rgba(7,108,56,.82);color:#fff}
.radarOverlay #tabInfo{pointer-events:auto;backdrop-filter:blur(3px);border-radius:9px}
@media(max-width:440px){.radarOverlay .seg{padding-inline:clamp(3px,calc(8.5vw - 24.2px),12px)}}
.hint2{margin-left:auto;font-size:12px;color:var(--mut);font-weight:400;
  text-align:right;line-height:1.3}
#radarWrap{flex:1;min-height:0;position:relative;overflow:hidden}
#radar{position:absolute;inset:0;width:100%;height:100%;touch-action:none}
.servoRing{position:absolute;inset:0;width:100%;height:100%;z-index:2;pointer-events:none;touch-action:none}
.servoRing .ringRail{fill:none;stroke:#f0fff5;stroke-width:12;stroke-linecap:round;filter:drop-shadow(0 1px 2px #002c17)}
.servoRing .ringAccent{fill:none;stroke:#14894b;stroke-width:3;stroke-linecap:round}
#servoRingHit{fill:none;stroke:transparent;stroke-width:44;pointer-events:stroke;cursor:grab}
#servoRingHandle{pointer-events:all;cursor:grab;filter:drop-shadow(0 2px 3px #002c17)}
.servoRing.is-dragging #servoRingHit,.servoRing.is-dragging #servoRingHandle{cursor:grabbing}
.servoRing.is-disabled .ringRail,.servoRing.is-disabled .ringAccent{opacity:.5}
.servoRing.is-disabled #servoRingHit,.servoRing.is-disabled #servoRingHandle{pointer-events:none;cursor:default}
#servoRingTicks text{font:600 11px system-ui;fill:#073820;stroke:#fff;stroke-width:3;paint-order:stroke}
#servoRingPreview{stroke:#076c38;stroke-width:2;stroke-dasharray:5 5}
#servoRingHandle text{font:700 13px system-ui;fill:#123b25;text-anchor:middle;font-variant-numeric:tabular-nums;user-select:none}
.ringInput{position:absolute;width:1px;height:1px;padding:0;border:0;clip-path:inset(50%);overflow:hidden}
.ringInput:focus-visible+.servoRing #servoRingHandle circle{stroke:#825200;stroke-width:5}
.signalPanel{position:absolute;left:8px;top:64px;z-index:3;width:124px;height:114px;
  display:flex;flex-direction:column;gap:6px;padding:6px;border:1px solid rgba(143,169,151,.65);
  border-radius:10px;background:rgba(255,255,255,.78);pointer-events:none}
.signalRow{display:flex;align-items:center;gap:8px;min-height:30px}
.signalDot{flex:none;width:22px;height:22px;border-radius:50%;background:#78847d;
  border:2px solid #fff;box-shadow:0 0 0 1px #65736b,inset 0 2px 3px rgba(255,255,255,.5)}
.signalRow[data-state=ok] .signalDot{background:#159b42;box-shadow:0 0 0 1px #076c38,0 0 7px #159b4255}
.signalRow[data-state=bad] .signalDot{background:#e13129;box-shadow:0 0 0 1px #a71913,0 0 7px #e1312955}
.signalLabel{display:flex;flex-direction:column;line-height:1.15;font-size:12px;font-weight:700}
.signalLabel small{margin-top:2px;font-size:10px;font-weight:500;color:var(--mut)}
button{flex:1;min-width:96px;padding:11px 12px;border-radius:8px;border:1px solid var(--line);
  background:#f2f6f2;color:var(--fg);font-size:14px;font-weight:600;cursor:pointer}
button:hover{border-color:var(--acc)}
button:disabled{opacity:.4;cursor:not-allowed}
#toast{position:fixed;left:50%;bottom:18px;transform:translateX(-50%);
  background:var(--bad);color:#fff;padding:9px 16px;border-radius:8px;font-size:13px;
  opacity:0;transition:opacity .25s;pointer-events:none;z-index:9}
#toast.show{opacity:1}
.seg{padding:4px 12px;border:1px solid var(--line);background:#f2f6f2;color:var(--fg);
  font-size:12px;font-weight:600;cursor:pointer;flex:none;min-width:0;border-radius:0}
.seg:first-child{border-radius:6px 0 0 6px}
.seg:last-child{border-radius:0 6px 6px 0}
.seg+.seg{border-left:0}
.seg.on{background:var(--acc);border-color:var(--acc);color:#fff}
.infoCols{display:grid;grid-template-columns:1fr 1fr;gap:10px}
.col{overflow:visible}
.col h3{margin:0 0 4px;font-size:14px;font-weight:700}
.sub{font-size:10px;color:var(--mut);text-transform:uppercase;letter-spacing:.04em;
  margin:9px 0 2px;border-bottom:1px solid var(--line);padding-bottom:2px}
.r{display:flex;justify-content:space-between;align-items:baseline;gap:6px;
  font-size:13px;padding:3px 0}
.r span{color:var(--mut)}
.calCard h3{margin:0 0 4px;font-size:14px;font-weight:700}
.calRow{display:flex;gap:8px;align-items:center;margin-top:8px;flex-wrap:wrap}
.calRow input[type=text],.calRow input[type=number]{flex:1;min-width:170px;padding:10px;border-radius:8px;
  border:1px solid var(--line);background:#ffffff;color:var(--fg);font-size:13px}
#cfgSpeed{width:130px;padding:8px;border:1px solid var(--line);border-radius:6px;background:#ffffff;color:var(--fg)}
#cfgPrediction{width:20px;height:20px;accent-color:var(--acc)}
.hint3{display:block;margin-top:6px;font-size:11px;color:var(--mut);line-height:1.45}
.extensionCard h3{margin:0 0 4px;font-size:14px;font-weight:700}
.extensionActions{display:flex;gap:8px;flex-wrap:wrap;margin-top:8px}
.extensionActions button{flex:0 1 auto;min-width:112px}
.extensionFields{display:grid;grid-template-columns:1fr 1fr 1fr;gap:7px;margin-top:8px}
.extensionFields input{min-width:0;width:100%;padding:9px;border:1px solid var(--line);border-radius:7px;background:#fff;color:var(--fg);font-size:13px}
.extensionStatus{margin-top:8px;font-size:12px;line-height:1.45;color:var(--mut)}
.extensionStatus b{color:var(--fg)}
@media(max-width:440px){.extensionFields{grid-template-columns:1fr 1fr}.extensionFields input:last-child{grid-column:span 2}}
.bar{height:6px;border-radius:3px;background:#d9e3dc;overflow:hidden;margin-top:8px}
.bar>i{display:block;height:100%;width:0;background:var(--acc);transition:width .2s}
.r b{font-variant-numeric:tabular-nums}
.cmp{width:100%;border-collapse:collapse;font-size:13px}
.cmp th,.cmp td{padding:5px 8px;border-bottom:1px solid var(--line);text-align:right;
  font-variant-numeric:tabular-nums}
.cmp tr:last-child td{border-bottom:0}
.cmp th:first-child,.cmp td:first-child{text-align:left;color:var(--mut);font-weight:400}
.cmp thead th{color:var(--fg);font-weight:700}
/* 現場提醒訊息列：頁底收合列，展開半透明覆蓋圖面，不擠壓內容。內容來自 /api/status 的
   alerts 陣列（措辭與門檻都在韌體端，見 main.cpp 的 appendAlertsJson）。 */
.alertBar{position:absolute;left:0;right:0;bottom:0;z-index:6;display:none;flex-direction:column;
  background:rgba(255,255,255,.76);border-top:1px solid rgba(143,169,151,.7)}
.alertBar.on{display:flex}
.ahead{display:flex;align-items:center;gap:8px;width:100%;min-height:48px;padding:6px 12px;font-size:13px;
  text-align:left;border:0;border-radius:0;background:transparent;
  font-weight:700;cursor:pointer;-webkit-user-select:none;user-select:none}
.ahead .chev{flex:none;margin-left:auto;padding:7px 10px;border-radius:7px;
  background:var(--acc);color:#fff;font-size:13px;font-weight:700;white-space:nowrap}
.ahead:focus-visible{outline:3px solid var(--acc);outline-offset:-3px}
.alertBar.err .ahead{color:var(--bad)}
.alertBar.warn .ahead{color:var(--warn)}
.alist{display:flex;flex-direction:column;order:-1;
  gap:1px;height:var(--alert-list-height,30dvh);overflow:auto}
.alertBar.fold .alist{display:none}
.al{display:flex;gap:9px;padding:9px 12px;background:transparent}
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
<main>
  <section id="pgRadar" class="page on">
    <div id="radarWrap">
      <canvas id="radar"></canvas>
      <input id="sld" class="ringInput" aria-label="Servo 外圈角度，方向鍵調整，放開生效" type="range" min="0" max="180" step="1" value="90" disabled>
      <svg id="servoRing" class="servoRing is-disabled" style="display:none" aria-hidden="true">
        <g id="servoRingTrack">
          <path id="servoRingRail" class="ringRail"></path>
          <path id="servoRingAccent" class="ringAccent"></path>
          <g id="servoRingTicks"></g>
          <line id="servoRingPreview" visibility="hidden"></line>
          <path id="servoRingHit"></path>
        </g>
        <g id="servoRingHandle">
          <circle r="22" fill="#fff" stroke="#0c7239" stroke-width="3"></circle>
          <text y="3"><tspan id="angTxt">90</tspan>°</text>
          <path id="servoRingGrip" d="M-6 10H6M-4 13H4" stroke="#0c7239" stroke-width="1.5" stroke-linecap="round"></path>
        </g>
      </svg>
      <div id="signalPanel" class="signalPanel" role="group" aria-label="GPS 與 LoRa 狀態">
        <div id="signalStation" class="signalRow" data-state="unknown"><i class="signalDot" aria-hidden="true"></i><span class="signalLabel">Station GPS<small id="signalStationText">等待資料</small></span></div>
        <div id="signalClient" class="signalRow" data-state="unknown"><i class="signalDot" aria-hidden="true"></i><span class="signalLabel">Client GPS<small id="signalClientText">等待資料</small></span></div>
        <div id="signalLora" class="signalRow" role="group" aria-label="LoRa 等待資料" data-state="unknown"><i class="signalDot" aria-hidden="true"></i><span class="signalLabel">LoRa<small id="signalLoraText">等待資料</small></span></div>
      </div>
      <div class="radarOverlay">
    <div class="radarHead">
      <button id="tabInfo" class="seg" type="button">資訊</button>
      <span class="segwrap">
        <button id="vRadar" class="seg on" type="button">雷達</button>
        <button id="vMap" class="seg" type="button">地圖</button>
      </span>
    </div>
    <div class="radarHead">
      <span class="segwrap">
        <button id="mManual" class="seg on" type="button">手動</button>
        <button id="mGps" class="seg" type="button" disabled>GPS</button>
        <button id="mUart" class="seg" type="button">UART</button>
      </span>
    </div>
      </div>
    </div>
  </section>

  <section id="pgInfo" class="page">
    <button id="tabRadar" type="button">← 返回雷達</button>
    <div class="card" style="flex:none">
      <table class="cmp">
        <thead><tr><th></th><th>站 Station</th><th>Surfer Client</th></tr></thead>
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
        <div class="r"><span>RSSI</span><b id="sRssi">--</b></div>
        <div class="r"><span>SNR</span><b id="sSnr">--</b></div>
        <div class="r"><span>丟包率</span><b id="sDrop">--</b></div>
        <div class="sub">其他</div>
        <div class="r"><span>韌體版本</span><b id="sVersion">--</b></div>
      </div>
      <div class="col card">
        <h3>Surfer 專屬</h3>
        <div class="r"><span>距離</span><b id="cDist">--</b></div>
        <div class="r"><span>方位</span><b id="cBrg">--</b></div>
        <div class="sub">LoRa 連線</div>
        <div class="r"><span>連線</span><b id="cLink">--</b></div>
        <div class="r"><span>距上次封包</span><b id="cAge">--</b></div>
      </div>
    </div>
    <div class="card calCard" style="flex:none">
      <h3>鏡頭指南針校正</h3>
      <div class="calRow">
        <button id="btnCompassCenter" type="button">回到 90°</button>
        <input id="compassBearing" type="number" inputmode="decimal" min="0" max="359.9" step="0.1" placeholder="指南針角度 0–359.9°" aria-label="鏡頭指南針角度">
        <button id="btnCompassCal" type="button" aria-describedby="compassCalState">校正</button>
      </div>
      <p class="hint3">開機採用 90° 偏移。需重新校正時，先回到 Servo 90° 並停穩，再填鏡頭朝向：北 0°、東 90°、南 180°、西 270°。其他方向填指南針實際讀數。</p>
      <p id="compassCalState" class="hint2" role="status" aria-live="polite">回到 90° 會切為手動；校正後再選 GPS。</p>
      <div class="r"><span>校正偏移</span><b id="mcOff">90.0°</b></div>
      <div class="r"><span>岸端 30 秒座標散布</span><b id="stationSpread">--</b></div>
      <div class="r"><span>磁偏角補償</span><b id="mcDeclination">--</b></div>

    </div>
    <div id="servoSpeedSettings" class="card calCard" style="flex:none">
      <h3>Servo 最高速度</h3>
      <div class="calRow">
        <input id="cfgSpeed" type="number" inputmode="decimal" min="1" max="90" step="0.1" value="30" disabled aria-label="Servo 最高速度，每秒度數">
        <span>°／秒</span>
      </div>
      <span class="hint3">手動、GPS、UART 共用。可調 1–90°／秒，修改後自動儲存，重開機保留。</span>
      <span id="speedSaveState" class="hint3">讀取速度中</span>
    </div>
    <div id="gpsPredictionSettings" class="card calCard" style="flex:none">
      <label class="calRow" for="cfgPrediction"><input id="cfgPrediction" type="checkbox" disabled aria-describedby="predictionHelp predictionSaveState"> GPS 位置預測（α）</label>
      <span id="predictionHelp" class="hint3">預設關閉。僅影響 GPS 模式。開啟（α＝1）：兩端品質合格且速度有效時，依接收後時間外推；品質差時仍追最新位置。關閉（α＝0）：追蹤最近收到的位置。切換可能改變追蹤目標，共用最高速度維持不變。修改後自動儲存，重開機保留。</span>
      <span id="predictionSaveState" class="hint3" role="status" aria-live="polite">讀取預測設定中</span>
    </div>
    <div id="stationPositionSettings" class="card extensionCard" style="flex:none">
      <h3>Station 位置</h3>
      <div class="r"><span>目前來源</span><b id="stationPositionSource">讀取中</b></div>
      <div class="r"><span>位置時間</span><b id="stationPositionTime">--</b></div>
      <div class="r"><span>精度</span><b id="stationPositionAccuracy">--</b></div>
      <div class="extensionActions">
        <button id="btnPhonePosition" type="button">用手機更新位置</button>
        <button id="btnGnssPosition" type="button">改用 Station GNSS</button>
      </div>
      <div class="extensionFields">
        <input id="stationPhoneLat" type="number" inputmode="decimal" step="any" placeholder="緯度 lat" aria-label="Station 手動緯度">
        <input id="stationPhoneLon" type="number" inputmode="decimal" step="any" placeholder="經度 lon" aria-label="Station 手動經度">
        <input id="stationPhoneAccuracy" type="number" inputmode="decimal" min="0" step="any" placeholder="精度 m" aria-label="Station 手動精度，公尺">
      </div>
      <div class="extensionActions"><button id="btnManualPhonePosition" type="button">送出手動位置</button></div>
      <p id="stationPositionState" class="extensionStatus" role="status" aria-live="polite">手機定位需從 HTTPS 手機定位頁明確按下開始；關閉或背景化後，Station 保留最後一次有效位置到重新開機。</p>
    </div>
    <div id="clientControlSettings" class="card extensionCard" style="flex:none">
      <h3>T096 Client 遠端控制</h3>
      <div class="r"><span>組別／頻率</span><b id="clientRadioGroup">--</b></div>
      <div class="r"><span>配對 Client</span><b id="clientPairedId">--</b></div>
      <div class="r"><span>裝置狀態</span><b id="clientControlState">讀取中</b></div>
      <div class="r"><span>距上次 STATE</span><b id="clientControlAge">--</b></div>
      <div class="r"><span>命令</span><b id="clientControlCommand">--</b></div>
      <div id="clientControlTest" class="extensionStatus">測試：--</div>
      <div class="extensionActions">
        <button id="btnClientStart" type="button" disabled>開始追蹤</button>
        <button id="btnClientStop" type="button" disabled>停止為待命</button>
        <button id="btnClientTest" type="button" disabled>LoRa RF 測試</button>
        <button id="btnClientStore" type="button" disabled>請求深度休眠</button>
      </div>
      <p id="clientControlHint" class="extensionStatus" role="status" aria-live="polite">等待 T096 Client 的近期 STATE；離線時不會假稱為待命或深度休眠。</p>
    </div>
  </section>
</main>

<div id="alertBar" class="alertBar"></div>
<div id="toast"></div>

<script>
var $=function(id){return document.getElementById(id);};
var last={track:null,status:null,alerts:[]};
var trackReceivedMs=null;
var dragging=false;
var ringGesture=null;
var servoPendingAngle=null;
var servoPendingIntent=null;
var servoSendTimer=0;
var servoLastSendMs=-Infinity;
var servoSending=false;
var servoRequest=Promise.resolve();
var controlChanging=false;
var servoDragEnded=false;
var servoGestureCancelled=false;
var viewMode='radar';
var page='radar';
var hist=[];               // client-accumulated path (station no longer stores it)
var HIST_RETAIN_MS=5*60*1000; // keep only the visible five-minute path
var RADAR_WINDOW_MS=5*60*1000;   // 雷達／地圖畫面固定只顯示最近 5 分鐘
var radarZoom=1;           // multiplicative zoom for radar view
var mapZoomDelta=0;        // additive zoom delta (in zoom levels) for map view
var pinchBaseRadarZoom=1;
var pinchBaseMapZoomDelta=0;
var pinchStartDist=0;
var activePointers={};

function toast(m){var t=$('toast');t.textContent=m;t.classList.add('show');
  clearTimeout(t._h);t._h=setTimeout(function(){t.classList.remove('show');},2600);}

var motionContext=null;
// One entry per observed board reboot during this page's lifetime. A delayed
// reply from a retired boot must never become the current command context again.
var retiredControlBoots=[];
function servoUiAngle(raw){return 180-Number(raw);}
function servoRawAngle(ui){return 180-Number(ui);}
var compassServoState=null;
var speedLoaded=false,speedSaving=false,lastSavedSpeed=30;
var predictionLoaded=false,predictionSaving=false,lastSavedPrediction=null,pendingPrediction=null;
var predictionContext=null;
// Keep a delayed poll/load from reverting a setting already confirmed by POST.
function showPredictionSetting(j,confirmed){
  var enabled=j.enabled;
  if(typeof enabled!=='boolean'||j.alpha!==(enabled?1:0))return false;
  if(!syncMotionContext(j))return false;
  if(predictionSaving&&!confirmed)return false;
  if(predictionContext&&j.control_boot_id===predictionContext.control_boot_id){
    var elapsed=(j.clock_ms-predictionContext.clock_ms)|0;
    if(elapsed<0)return false;
    if(j.control_epoch===predictionContext.control_epoch&&
       ((j.command_seq-predictionContext.command_seq)|0)<0)return false;
    if(elapsed===0&&((j.control_epoch-predictionContext.control_epoch)|0)<0)return false;
  }
  var changed=!predictionLoaded||enabled!==lastSavedPrediction;
  predictionContext={control_boot_id:j.control_boot_id,control_epoch:j.control_epoch,
    command_seq:j.command_seq>>>0,clock_ms:j.clock_ms>>>0};
  lastSavedPrediction=enabled;predictionLoaded=true;
  $('cfgPrediction').checked=enabled;$('cfgPrediction').indeterminate=false;
  $('cfgPrediction').disabled=controlChanging||predictionSaving;
  if(changed||confirmed)$('predictionSaveState').textContent=enabled?'已記住：開啟（α＝1）':'已記住：關閉（α＝0）';
  return true;
}
function syncMotionContext(j){
  if(retiredControlBoots.indexOf(j.control_boot_id)>=0)return false;
  if(j.control_epoch==null||j.clock_ms==null)return true;
  if(!motionContext||j.control_boot_id!==motionContext.control_boot_id){
    if(motionContext&&motionContext.control_boot_id!=null)retiredControlBoots.push(motionContext.control_boot_id);
    motionContext={control_boot_id:j.control_boot_id,control_epoch:j.control_epoch,
      command_seq:j.command_seq>>>0,clock_ms:j.clock_ms>>>0};return true;
  }
  if(((j.clock_ms-motionContext.clock_ms)|0)<0)return false;
  if(j.clock_ms===motionContext.clock_ms&&((j.control_epoch-motionContext.control_epoch)|0)<0)return false;
  if(j.control_epoch!==motionContext.control_epoch){
    motionContext.control_epoch=j.control_epoch;motionContext.command_seq=j.command_seq>>>0;
  }else if(((j.command_seq-motionContext.command_seq)|0)>0){
    motionContext.command_seq=j.command_seq>>>0;
  }
  motionContext.clock_ms=j.clock_ms>>>0;
  return true;
}
function motionUrl(url){
  if(!motionContext)throw new Error('等待控制狀態更新，請稍後再試');
  motionContext.command_seq=(motionContext.command_seq+1)>>>0;
  return url+(url.indexOf('?')<0?'?':'&')+'epoch='+motionContext.control_epoch+
    '&seq='+motionContext.command_seq+'&stamp='+motionContext.clock_ms;
}
function isMotionUrl(url){return /^\/api\/(servo(?:[/?]|$)|track\/(start|resume|pause|calibrate|prediction)(?:[/?]|$))/.test(url);}
// One transport slot covers fetch AND the complete response body. Background
// callers share a pending job by key; priority is reconsidered between reads.
var httpQueue=[],httpActive=null,httpByKey={},httpOrder=0,httpContextAt=-Infinity;
var HTTP_TIMEOUT_MS=2000;
// Capture when the user commits, before either the slider queue or HTTP queue.
function controlIntent(){return {deadline:performance.now()+HTTP_TIMEOUT_MS,
  context:motionContext?{boot:motionContext.control_boot_id,epoch:motionContext.control_epoch}:null};}
function httpError(message,name){var e=new Error(message);e.name=name||'Error';return e;}
function httpReject(job,error){if(!job.done){job.done=true;job.reject(error);}}
function httpReceiveContext(data){
  var context=data&&data.servo||data;
  if(context&&context.control_epoch!=null&&context.clock_ms!=null&&syncMotionContext(context))httpContextAt=performance.now();
}
function httpJsonBody(response){return response.json();}
function httpExchange(job,url,method,read){
  if(job.cancelled)return Promise.reject(httpError('指令或讀取已逾時，請重試','TimeoutError'));
  var ac=typeof AbortController!=='undefined'?new AbortController():null,expired=false;
  job.abort=ac;
  var timer=setTimeout(function(){
    expired=true;job.cancelled=true;if(ac)ac.abort();
    httpReject(job,httpError('讀取逾時；若未恢復請重新整理','TimeoutError'));
  },HTTP_TIMEOUT_MS);
  var options={cache:'no-store'};if(method)options.method=method;if(ac)options.signal=ac.signal;
  if(method==='POST'&&job.body!==undefined){options.headers={'Content-Type':job.form?'application/x-www-form-urlencoded;charset=UTF-8':'application/json'};options.body=job.body;}
  // Do not release httpActive on the timeout alone: unsupported/ineffective
  // abort must not let a second fetch overlap a still-running body read.
  return Promise.resolve().then(function(){
    if(job.cancelled)throw httpError('指令或讀取已逾時，請重試','TimeoutError');
    return fetch(url,options);
  }).then(function(response){
    return read(response).then(function(data){
      if(expired||job.cancelled)throw httpError('讀取已逾時，忽略延遲回覆','TimeoutError');
      if(!response.ok||data&&data.ok===false){
        var error=httpError(data&&data.error||'HTTP '+response.status);error.status=response.status;throw error;
      }
      httpReceiveContext(data);return data;
    });
  }).finally(function(){clearTimeout(timer);job.abort=null;});
}
function httpPump(){
  if(httpActive||!httpQueue.length)return;
  httpQueue.sort(function(a,b){return a.priority-b.priority||a.order-b.order;});
  var job=httpQueue.shift();httpActive=job;
  // Non-control requests have a queue timeout and a separate wire timeout.
  // A control's original 2 s intent deadline also covers a context refresh.
  if(!job.motion)clearTimeout(job.timer);
  var run=Promise.resolve();
  if(job.motion){
    run=run.then(function(){
      if(!motionContext||performance.now()-httpContextAt>1000)
        return httpExchange(job,'/api/track',null,httpJsonBody).then(function(state){
          var context=state&&state.servo;
          if(!context||context.control_boot_id==null||context.control_epoch==null||context.clock_ms==null||!syncMotionContext(context))
            throw httpError('控制狀態未更新，請重試');
        });
    }).then(function(){
      if(job.cancelled||performance.now()>=job.deadline)throw httpError('控制指令等待逾時，請重試','TimeoutError');
      if(job.context&&(!motionContext||job.context.boot!==motionContext.control_boot_id||job.context.epoch!==motionContext.control_epoch))
        throw httpError('設備或控制模式已更新，請重試');
      return httpExchange(job,motionUrl(job.url),job.method,job.read);
    });
  }else run=run.then(function(){return httpExchange(job,typeof job.url==='function'?job.url():job.url,job.method,job.read);});
  run.then(function(data){if(!job.done){job.done=true;job.resolve(data);}},function(error){httpReject(job,error);})
    .finally(function(){
      clearTimeout(job.timer);if(job.key)delete httpByKey[job.key];httpActive=null;
      Promise.resolve().then(httpPump);
    });
}
function httpRequest(url,priority,key,read,method,intent,body,form){
  if(key&&httpByKey[key])return httpByKey[key].promise;
  var motion=method==='POST'&&isMotionUrl(url);
  if(motion){intent=intent||controlIntent();if(performance.now()>=intent.deadline)
    return Promise.reject(httpError('控制指令等待逾時，請重試','TimeoutError'));}
  var job={url:url,priority:priority,key:key,read:read||httpJsonBody,method:method,
    motion:motion,order:++httpOrder,deadline:motion?intent.deadline:performance.now()+HTTP_TIMEOUT_MS,
    context:motion?intent.context:null,body:body,form:!!form};
  job.promise=new Promise(function(resolve,reject){job.resolve=resolve;job.reject=reject;});
  job.timer=setTimeout(function(){
    job.cancelled=true;if(job.abort)job.abort.abort();
    httpReject(job,httpError(job.motion?'控制指令等待逾時，請重試':'讀取排隊逾時','TimeoutError'));
    if(httpActive!==job){httpQueue=httpQueue.filter(function(x){return x!==job;});if(key)delete httpByKey[key];}
  },Math.max(0,job.deadline-performance.now()));
  if(key)httpByKey[key]=job;httpQueue.push(job);httpPump();return job.promise;
}
function post(url,intent){
  return httpRequest(url,0,null,null,'POST',intent).catch(function(e){toast(e.message);throw e;});
}

// ---- controls ----
// Drag previews locally; only a committed release submits a target.
// Keep one request in flight and only the newest committed pending target.
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
function postServoAngle(v,intent){
  return httpRequest('/api/servo?angle='+encodeURIComponent(v),0,null,null,'POST',intent)
    .catch(function(e){servoWarn(e.message||'角度指令送不出去，檢查 WiFi');});
}
function sendPendingServoAngle(){
  if(controlChanging||servoSending||servoPendingAngle===null)return;
  var v=servoPendingAngle,intent=servoPendingIntent;servoPendingAngle=null;servoPendingIntent=null;servoSending=true;
  servoRequest=postServoAngle(v,intent).then(function(){
    servoSending=false;
    if(servoPendingAngle!==null)sendPendingServoAngle();
    else if(servoDragEnded){servoDragEnded=false;dragging=false;}
    renderServoRing();
  });
}
function queueServoAngle(v,isFinal){
  if(controlChanging||!isFinal)return;
  servoPendingAngle=servoRawAngle(v);servoPendingIntent=controlIntent();servoDragEnded=true;
  sendPendingServoAngle();
}
$('sld').addEventListener('pointerdown',function(){servoGestureCancelled=false;});
$('sld').addEventListener('keydown',function(){servoGestureCancelled=false;});
$('sld').addEventListener('pointercancel',function(){servoGestureCancelled=true;dragging=false;servoDragEnded=false;});
$('sld').addEventListener('input',function(){
  dragging=true;servoDragEnded=false;$('angTxt').textContent=this.value;
  renderServoRing();
});
$('sld').addEventListener('change',function(){
  if(!servoGestureCancelled)queueServoAngle(this.value,true);
});
// Cancel unsent slider values and let the in-flight manual write finish first.
// Otherwise an old slider request can arrive after Auto and take control back.
function controlAction(url){
  if(controlChanging)return Promise.resolve();
  cancelRingGesture();
  var intent=controlIntent();
  controlChanging=true;
  clearTimeout(servoSendTimer);servoSendTimer=0;servoPendingAngle=null;servoPendingIntent=null;
  servoDragEnded=false;dragging=false;
  $('sld').disabled=true;
  renderServoRing();
  return servoRequest.then(function(){return post(url,intent);})
    .catch(function(){})
    .then(function(){controlChanging=false;return refresh();});
}
$('mGps').onclick=function(){controlAction('/api/servo/mode?mode=gps');};
$('mUart').onclick=function(){controlAction('/api/servo/mode?mode=uart');};
$('mManual').onclick=function(){controlAction('/api/servo/mode?mode=manual');};

$('vRadar').onclick=function(){setView('radar');};
$('vMap').onclick=function(){setView('map');};
function radarHeading(d){
  var angle=d&&d.servo.mount_offset_deg;
  return Number.isFinite(angle)?((angle%360)+360)%360:90;
}
function setView(m){
  if(m!==viewMode)cancelRingGesture();
  viewMode=m;
  $('vRadar').classList.toggle('on',m==='radar');
  $('vMap').classList.toggle('on',m==='map');
  if(last.track)drawRadar(last.track,geoDist(last.track.station,last.track.client));
}

// ---- tabs + responsive canvas (fills its container, no fixed px) ----
function showPage(p){
  if(p!==page)cancelRingGesture();
  page=p;
  $('pgRadar').classList.toggle('on',p==='radar');
  $('pgInfo').classList.toggle('on',p==='info');
  if(p==='info')document.documentElement.style.setProperty('--alert-list-height','40dvh');
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
  drawRadar(last.track,geoDist(last.track.station,last.track.client));
}
function syncViewportHeight(){
  var h=window.visualViewport?window.visualViewport.height:window.innerHeight;
  if(h>0)document.documentElement.style.setProperty('--app-height',Math.round(h)+'px');
  cancelRingGesture();redraw();
}
window.addEventListener('resize',syncViewportHeight);
if(window.visualViewport)window.visualViewport.addEventListener('resize',syncViewportHeight);
syncViewportHeight();

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
  cancelRingGesture();
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

// Same radius as the outer distance ring. A fixed screen-space control:
// left 0, top 90, right 180. UI degrees remain 180 - raw; the radar's
// geographic aim bearing is drawn separately using the calibration formula.
function radarGeometry(){
  var c=$('radar'),w=c.width,h=c.height,pad=24,bottom=12,top=64,lw=124,lh=114,gap=24;
  function candidate(cx,cy,r,lx,ly){
    var dx=Math.max(lx-cx,0,cx-lx-lw),dy=Math.max(ly-cy,0,cy-ly-lh);
    return {cx:cx,cy:cy,r:Math.min(r,Math.hypot(dx,dy)-gap),lightsX:lx,lightsY:ly};
  }
  var cy=(h+pad-bottom)/2,r=Math.min(w/2-pad,(h-pad-bottom)/2),midY=Math.max(top,(h-lh)/2);
  var reservedTop=top+lh+10;
  var choices=[candidate(w/2,cy,r,8,h>=w?top:midY),candidate(w/2,cy,r,8,top),
    candidate((w+lw+16)/2,cy,Math.min((w-lw-16)/2-pad,(h-pad-bottom)/2),8,midY),
    candidate(w/2,(h+reservedTop+pad-bottom)/2,Math.min(w/2-pad,(h-reservedTop-pad-bottom)/2),8,top)];
  // Near-square screens gain space by shifting the circle away from the lamps.
  var low=24,high=Math.max(low,r);
  for(var i=0;i<14;i++){
    var trial=(low+high)/2,corner=candidate(w-pad-trial,h-bottom-trial,trial,8,top);
    if(corner.r>=trial)low=trial;else high=trial;
  }
  choices.push(candidate(w-pad-low,h-bottom-low,low,8,top));
  var best=choices.reduce(function(a,b){return b.r>a.r?b:a;});
  best.r=Math.max(24,best.r);return best;
}
function layoutRadarOverlays(){
  var c=$('radar'),g=radarGeometry(),map=viewMode==='map';
  $('signalPanel').style.left=(map?8:g.lightsX)+'px';
  $('signalPanel').style.top=(map?Math.max(64,(c.height-114)/2):g.lightsY)+'px';
  // The alert drawer reaches toward the lower compass-letter circle, while
  // the canvas keeps the full viewport. Extra alerts scroll within the drawer.
  var space=map?c.height*.35:c.height-(g.cy+g.r-44+4);
  document.documentElement.style.setProperty('--alert-list-height',Math.max(54,Math.min(c.height*.45,space-49))+'px');
}
function servoRingGeometry(){
  var c=$('radar'),g=viewMode==='map'?{cx:c.width/2,cy:c.height/2,r:Math.max(24,Math.min(c.width,c.height)/2-26)}:radarGeometry();g.start=-90;
  return g;
}
function ringPoint(g,angle,radius){
  var a=(g.start+angle)*Math.PI/180,r=radius==null?g.r:radius;
  return {x:g.cx+Math.sin(a)*r,y:g.cy-Math.cos(a)*r};
}
function renderServoRing(){
  var ring=$('servoRing'),sv=compassServoState||(last.track&&last.track.servo);
  ring.style.display=sv?'':'none';
  $('servoRingTrack').style.display=sv&&sv.mode==='manual'?'':'none';
  $('servoRingGrip').style.display=sv&&sv.mode==='manual'?'':'none';
  var c=$('radar');if(!c.width||!c.height)return;
  if(page==='radar')layoutRadarOverlays();
  var g=servoRingGeometry(),enabled=!$('sld').disabled;
  ring.setAttribute('viewBox','0 0 '+c.width+' '+c.height);
  ring.classList.toggle('is-disabled',!enabled);ring.classList.toggle('is-dragging',!!ringGesture);
  var p=ringPoint(g,0),q=ringPoint(g,180);
  var path='M'+p.x+' '+p.y+' A'+g.r+' '+g.r+' 0 0 1 '+q.x+' '+q.y;
  ['servoRingRail','servoRingAccent','servoRingHit'].forEach(function(id){$(id).setAttribute('d',path);});
  var ticks='';
  for(var a=0;a<=180;a+=15){
    p=ringPoint(g,a,g.r-5);q=ringPoint(g,a,g.r+5);
    ticks+='<line x1="'+p.x+'" y1="'+p.y+'" x2="'+q.x+'" y2="'+q.y+'" stroke="#116238" stroke-width="2"/>';
    if(a%45===0){p=ringPoint(g,a,g.r-20);ticks+='<text x="'+p.x+'" y="'+(p.y+4)+'" text-anchor="middle">'+a+'°</text>';}
  }
  $('servoRingTicks').innerHTML=ticks;
  var angle=clamp(Number($('sld').value)||0,0,180);p=ringPoint(g,angle);
  $('servoRingHandle').setAttribute('transform','translate('+p.x+' '+p.y+')');
  var line=$('servoRingPreview');
  line.setAttribute('x1',g.cx);line.setAttribute('y1',g.cy);line.setAttribute('x2',p.x);line.setAttribute('y2',p.y);
  line.setAttribute('visibility',dragging&&enabled?'visible':'hidden');
}
function cancelRingGesture(){
  if(!ringGesture)return;
  ringGesture=null;servoGestureCancelled=true;dragging=false;servoDragEnded=false;
  var sv=compassServoState;
  if(sv){var angle=servoUiAngle(sv.mode==='manual'&&sv.target!=null?sv.target:sv.angle);
    $('sld').value=Math.round(angle);$('angTxt').textContent=Math.round(angle);}
  renderServoRing();
}
function updateRingPointer(ev){
  if(!ringGesture)return;
  var box=$('radar').getBoundingClientRect(),g=ringGesture.geometry;
  var dx=(ev.clientX-box.left)*$('radar').width/box.width-g.cx;
  var dy=(ev.clientY-box.top)*$('radar').height/box.height-g.cy;
  if(Math.hypot(dx,dy)<g.r*.35)return;
  var angle=((Math.atan2(dx,-dy)*180/Math.PI-g.start)%360+360)%360;
  if(angle>180)angle=angle<270?180:0;
  $('sld').value=Math.round(angle);$('angTxt').textContent=$('sld').value;
  dragging=true;servoDragEnded=false;renderServoRing();
}
$('servoRing').addEventListener('pointerdown',function(ev){
  if(ringGesture){cancelRingGesture();return;}
  if($('sld').disabled||controlChanging||Object.keys(activePointers).length||ev.button>0)return;
  ev.preventDefault();servoGestureCancelled=false;
  ringGesture={id:ev.pointerId,geometry:servoRingGeometry()};
  try{this.setPointerCapture(ev.pointerId);}catch(_e){}
  updateRingPointer(ev);
});
$('servoRing').addEventListener('pointermove',function(ev){
  if(ringGesture&&ringGesture.id===ev.pointerId){ev.preventDefault();updateRingPointer(ev);}
});
$('servoRing').addEventListener('pointerup',function(ev){
  if(!ringGesture||ringGesture.id!==ev.pointerId)return;
  ev.preventDefault();updateRingPointer(ev);ringGesture=null;
  if(!$('sld').disabled&&!controlChanging&&!servoGestureCancelled)queueServoAngle($('sld').value,true);
  renderServoRing();
});
$('servoRing').addEventListener('pointercancel',cancelRingGesture);
$('servoRing').addEventListener('lostpointercapture',cancelRingGesture);

function showSpeedSetting(j){
  if(!Number.isFinite(j.speed)||!syncMotionContext(j))return;
  lastSavedSpeed=j.speed;speedLoaded=true;
  $('cfgSpeed').value=j.speed;
  $('cfgSpeed').disabled=false;
  $('speedSaveState').textContent='已記住 '+j.speed+'°／秒';
}
function loadSpeedSetting(){
  return httpRequest('/api/servo/settings',1,'speed')
    .then(showSpeedSetting).catch(function(e){$('speedSaveState').textContent=e.message;});
}
function saveSpeedLimit(){
  if(controlChanging||speedSaving||!speedLoaded)return;
  var raw=$('cfgSpeed').value.trim(),speed=Number(raw);
  if(raw===''||!Number.isFinite(speed)||speed<1||speed>90){
    $('cfgSpeed').value=lastSavedSpeed;toast('最高速度請填 1–90°／秒');return;
  }
  var intent=controlIntent();
  speedSaving=true;controlChanging=true;$('cfgSpeed').disabled=true;
  $('speedSaveState').textContent='儲存中…';
  if(last.track)applyMode(last.track.servo);
  return servoRequest.then(function(){return post('/api/servo/settings?speed='+encodeURIComponent(speed),intent);})
    .then(showSpeedSetting).catch(function(e){
      $('cfgSpeed').value=lastSavedSpeed;
      $('speedSaveState').textContent='儲存未完成，請重試';
      toast(e.message||e.error||'速度未儲存');
    }).finally(function(){
      speedSaving=false;controlChanging=false;$('cfgSpeed').disabled=!speedLoaded;
      sendPendingServoAngle();return refresh();
    });
}
$('cfgSpeed').onchange=saveSpeedLimit;
$('cfgSpeed').onkeydown=function(e){if(e.key==='Enter'){e.preventDefault();this.blur();}};

function loadPredictionSetting(){
  return httpRequest('/api/track/prediction',1,'prediction').then(function(j){
    if(j.ok!==true||typeof j.enabled!=='boolean'||j.alpha!==(j.enabled?1:0))throw new Error('預測設定讀取失敗');
    showPredictionSetting(j,false);
  }).catch(function(e){
    if(!predictionLoaded){
      $('cfgPrediction').disabled=true;$('cfgPrediction').indeterminate=true;
      $('predictionSaveState').textContent=e.status===404?'目前韌體不支援預測開關':'預測設定讀取失敗：'+(e.message||'');
    }
  });
}
function savePredictionSetting(){
  if(controlChanging||predictionSaving||!predictionLoaded){
    $('cfgPrediction').checked=predictionSaving?pendingPrediction:lastSavedPrediction===true;
    $('cfgPrediction').indeterminate=!predictionLoaded;return;
  }
  var enabled=$('cfgPrediction').checked;
  if(enabled===lastSavedPrediction)return;
  var intent=controlIntent();
  pendingPrediction=enabled;predictionSaving=true;controlChanging=true;
  $('cfgPrediction').disabled=true;$('predictionSaveState').textContent='儲存中…';
  if(last.track)applyMode(last.track.servo);
  return servoRequest.then(function(){return post('/api/track/prediction?enabled='+(enabled?1:0),intent);})
    .then(function(j){
      if(j.ok!==true||typeof j.enabled!=='boolean'||j.alpha!==(j.enabled?1:0))throw new Error('預測設定回覆無效');
      if(!showPredictionSetting(j,true))throw new Error('設定狀態已更新，請重試');
    }).catch(function(e){
      $('cfgPrediction').checked=lastSavedPrediction;
      $('predictionSaveState').textContent='儲存未完成，請重試';
      toast(e.message||e.error||'預測設定未儲存');
    }).finally(function(){
      predictionSaving=false;pendingPrediction=null;controlChanging=false;
      $('cfgPrediction').disabled=!predictionLoaded;
      sendPendingServoAngle();return refresh();
    });
}
$('cfgPrediction').indeterminate=true;
$('cfgPrediction').onchange=savePredictionSetting;

function applyMode(sv){
  if(!syncMotionContext(sv))return;
  compassServoState=sv;
  var gps=sv.mode==='gps',uart=(sv.mode==='uart'||sv.mode==='jetson'),tracking=gps||uart;
  $('mGps').classList.toggle('on',gps);
  $('mUart').classList.toggle('on',uart);
  $('mManual').classList.toggle('on',!tracking);
  $('mGps').disabled=controlChanging||!sv.gps_available;
  $('mGps').title=sv.gps_available?'':'Station 與 Client 都需有持續更新的有效位置';
  $('mUart').disabled=$('mManual').disabled=controlChanging;
  $('sld').disabled=controlChanging||tracking;
  if($('sld').disabled)cancelRingGesture();
  if(!speedSaving && document.activeElement!==$('cfgSpeed') && Number.isFinite(sv.speed_limit_deg_s)) {
    lastSavedSpeed=sv.speed_limit_deg_s;speedLoaded=true;
    $('cfgSpeed').value=lastSavedSpeed;
  }
  $('cfgSpeed').disabled=controlChanging||speedSaving||!speedLoaded;
  showPredictionSetting({enabled:sv.prediction_enabled,alpha:sv.prediction_alpha,
    control_boot_id:sv.control_boot_id,control_epoch:sv.control_epoch,
    command_seq:sv.command_seq,clock_ms:sv.clock_ms},false);
  $('cfgPrediction').disabled=controlChanging||predictionSaving||!predictionLoaded;
  if(!dragging&&document.activeElement!==$('sld')){
    var shown=sv.mode==='manual'&&sv.target!=null?sv.target:sv.angle;
    $('sld').value=Math.round(servoUiAngle(shown));$('angTxt').textContent=Math.round(servoUiAngle(shown));
  }
  renderServoRing();
}

// HDOP is a dimensionless multiplier, which nobody on a beach can act on.
// Horizontal accuracy ~= HDOP x UERE, and 2.5 m is the usual 1-sigma UERE for a
// consumer single-band module like the ATGM336H, so HDOP 1.2 reads as +-3 m.
// The wire format still carries HDOP (protocol.h unchanged) — this is display
// only, so the number stays comparable with any other GPS tool.
var UERE_M=2.5;
function accM(hdop){return Number.isFinite(hdop)&&hdop>=0?hdop*UERE_M:null;}
// Whole metres only: UERE is a rule of thumb, so a decimal would claim accuracy
// the estimate does not have.
function fmtAcc(hdop){var a=accM(hdop);
  return a==null?'--':('\u00b1'+Math.round(a)+' m');}
// Satellite count as words for the same reason: 8 vs 11 changes no decision,
// "夠不夠" does. Thresholds match gpsGrade() below so the two never disagree.
function satWord(sats){
  if(!Number.isFinite(sats)||sats<0)return '--';
  if(sats>=8)return '好';
  if(sats>=6)return '普通';
  return '少';
}
function satelliteClassWord(cls){return cls===3?'≥8 顆':cls===2?'6–7 顆':cls===1?'≤5 顆':'未知';}
function clientSatelliteWord(client){
  if(Number.isFinite(client.satellite_class))return satelliteClassWord(client.satellite_class);
  return satWord(client.satellites);
}
function clientSatelliteGradeCount(client){
  // Class comes with each position; an exact telemetry count can be 30 s old.
  if(Number.isFinite(client.satellite_class))return [null,1,6,8][client.satellite_class];
  return client.satellites;
}
// GPS quality grade (same thresholds as the firmware, shared by both ends).
function gpsGrade(sats,hdop){
  if(!Number.isFinite(sats)||!Number.isFinite(hdop)||sats<0||hdop<0||sats<=0)return 'miss';
  if(sats<4)return 'bad';
  if(hdop<=1.5&&sats>=8)return 'good';
  if(hdop<=3&&sats>=6)return 'ok';
  return 'bad';
}
// Leader line + small info card placed next to a marker (px,py) on the canvas.
function drawGpsTag(x,px,py,W,H,title,sats,hdop,satLabel){
  var g=gpsGrade(sats,hdop);
  var col=g==='good'?'#086a35':g==='ok'?'#825200':g==='bad'?'#b2241d':'#405849';
  var l1=title;
  var l2='衛星 '+(satLabel||satWord(sats))+'   '+fmtAcc(hdop);
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
  x.fillStyle='rgba(255,255,255,.94)';x.fillRect(bx,by,bw,bh);
  x.strokeStyle=col;x.strokeRect(bx,by,bw,bh);
  x.textAlign='left';
  x.fillStyle='#11251a';x.font='bold 11px system-ui';x.fillText(l1,bx+8,by+15);
  x.fillStyle='#405849';x.font='11px system-ui';x.fillText(l2,bx+8,by+29);
  x.fillStyle=col;x.font='bold 11px system-ui';x.fillText(l3,bx+8,by+43);
}

// /api/track is now small (no history); the browser accumulates the path itself.
function fetchTrack(){return httpRequest('/api/track',1,'track');}

// Append the current sample to the local path (skip stale/duplicate points).
// Prune on append to the same five-minute window shown by radar and map.
function pushHist(d){
  if(!d.linked||!d.client.fix)return;
  var e={t:Date.now(),lat:d.client.lat,lon:d.client.lon,sfix:d.station.fix?1:0,
         slat:d.station.fix?d.station.lat:0,slon:d.station.fix?d.station.lon:0};
  var l=hist[hist.length-1];
  if(l&&l.lat===e.lat&&l.lon===e.lon&&l.slat===e.slat&&l.slon===e.slon)return;
  hist.push(e);
  var cutoff=Date.now()-HIST_RETAIN_MS;
  while(hist.length&&hist[0].t<cutoff)hist.shift();
}
// Slice of hist within the last windowMs, walked backwards so it stays O(window
// size) instead of scanning older points every redraw.
function recentHist(windowMs){
  var cutoff=Date.now()-windowMs,out=[];
  for(var i=hist.length-1;i>=0;i--){
    if(hist[i].t<cutoff)break;
    out.unshift(hist[i]);
  }
  return out;
}
var refreshing=false;
function rfAlive(d){return typeof d.rf_alive==='boolean'?d.rf_alive:!!d.linked;}
function rfStatus(d){return rfAlive(d)?(d.linked?'已連線':'收到遙測，等待 DATA'):'未連線';}
function renderSignalLights(){
  var fresh=trackReceivedMs!=null&&performance.now()-trackReceivedMs<=3000,d=fresh?last.track:null;
  [['signalStation',d&&d.station.fix,'有定位','無定位'],
   ['signalClient',d&&d.client.fix,'有定位','無定位'],
   ['signalLora',d&&rfAlive(d),d?rfStatus(d):'已連線','未連線']].forEach(function(item){
    var text=d?(item[1]?item[2]:item[3]):(trackReceivedMs==null?'等待資料':'資料過期');
    if(item[0]==='signalLora'){
      var status=text;
      if(d)text=Number.isFinite(d.lora_fps_10s)?d.lora_fps_10s.toFixed(1)+' FPS · 10s':'-- FPS · 10s';
      $(item[0]).setAttribute('aria-label','LoRa '+status+'，'+text);
      $(item[0]).title=status+'；最近 10 秒接受的 DATA 封包／秒';
    }
    $(item[0]).setAttribute('data-state',d?(item[1]?'ok':'bad'):'unknown');
    $(item[0]+'Text').textContent=text;
  });
}
function refresh(){
  renderSignalLights();
  if(refreshing)return Promise.resolve();
  refreshing=true;
  return fetchTrack().then(function(d){
    last.track=d;
    trackReceivedMs=performance.now();renderSignalLights();
    last.track_received_at=new Date().toISOString();
    pushHist(d);
    applyMode(d.servo);
    var dist=geoDist(d.station,d.client);
    // --- Station block ---
    $('sFix').textContent=d.station.fix?'有':'無';
    $('sSat').textContent=satWord(d.station.satellites);
    $('sAcc').textContent=fmtAcc(d.station.hdop);
    $('sBattV').textContent=(d.station.batt_pct>=0?d.station.batt_pct+'%':'--')+
      (d.station.charging?' \u26a1':'');
    $('sTemp').textContent=d.station.temp_c==null?'--':d.station.temp_c+'°C';
    $('sHum').textContent=d.station.humidity_pct==null?'--':d.station.humidity_pct+'%';
    $('mcOff').textContent=d.servo.calibrated?d.servo.mount_offset_deg.toFixed(1)+'°':'未校正';
    var stationAverage=d.station_average||{};
    $('stationSpread').textContent=stationAverage.samples&&Number.isFinite(stationAverage.rms_m)?
      stationAverage.rms_m.toFixed(1)+' m RMS'+(stationAverage.warning?'・偏大':''):'等待資料';
    $('stationSpread').style.color=stationAverage.warning?'var(--bad)':'';
    $('mcDeclination').textContent=d.servo.declination_deg==null?'等待有效 GPS 位置／日期':
      d.servo.declination_deg.toFixed(1)+'°（自動）';
    // --- Client block ---
    $('cFix').textContent=d.client.fix?'有':'無';
    $('cSat').textContent=clientSatelliteWord(d.client);
    $('cAcc').textContent=fmtAcc(d.client.hdop);
    $('cDist').textContent=(d.station.fix&&d.client.fix&&dist!=null)?dist.toFixed(0)+' m':'--';
    $('cBrg').textContent=d.bearing>=0?d.bearing.toFixed(0)+'°':'--';
    $('cLink').textContent=rfAlive(d)?(d.linked?'ONLINE':'收到遙測，等待 DATA'):'離線';
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
var alertsFold=true, alertsSig='';
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
  var head='<button type="button" class="ahead" aria-expanded="'+(!alertsFold)+'"><span>'+(nErr?'⛔':'⚠')+'</span><span>'+
    (nErr?nErr+' 項要立刻處理'+(list.length>nErr?('，另有 '+(list.length-nErr)+' 項提醒'):'')
        :list.length+' 項提醒')+
    '</span><span class="chev">'+(alertsFold?'▲ 展開提醒':'▼ 收合提醒')+'</span></button>';
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
  return httpRequest('/api/status',2,'status').then(function(s){
    last.status=s;
    last.status_received_at=new Date().toISOString();
    if(s.servo)showPredictionSetting({enabled:s.servo.prediction_enabled,alpha:s.servo.prediction_alpha,
      control_boot_id:s.servo.control_boot_id,control_epoch:s.servo.control_epoch,
      command_seq:s.servo.command_seq,clock_ms:s.servo.clock_ms},false);
    last.alerts=s.alerts||[];
    renderAlerts(last.alerts);
    $('sRssi').textContent=s.lora.rssi?s.lora.rssi.toFixed(0)+' dBm':'--';
    $('sSnr').textContent=s.lora.snr!=null?s.lora.snr.toFixed(1)+' dB':'--';
    $('sDrop').textContent=(s.lora.drop_rate*100).toFixed(1)+'%';
    $('sVersion').textContent=s.health.firmware_version?'v'+s.health.firmware_version:'未標版';
  }).catch(function(){});
}

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
  var geometry=radarGeometry(),cx=geometry.cx,cy=geometry.cy,R=geometry.r;
  renderServoRing();
  x.clearRect(0,0,W,H);
  x.fillStyle='#ffffff';x.fillRect(0,0,W,H);
  var heading=radarHeading(d),rotation=heading*Math.PI/180;
  var st=d.station.fix?d.station:d.client,maxR=50;
  var recent=recentHist(RADAR_WINDOW_MS);
  if(st.fix){
    recent.forEach(function(p){var en=toEN(st,p);
      maxR=Math.max(maxR,Math.hypot(en.e,en.n));});
    if(d.client.fix){var cur0=toEN(st,d.client);maxR=Math.max(maxR,Math.hypot(cur0.e,cur0.n));}
  }
  maxR*=1.1;
  var visR=maxR/radarZoom;
  // 圈圈（同心圓格線）與旁邊文字的亮度：調這兩個 alpha（0~1，越大越亮）即可。
  var RING_ALPHA=0.65, LABEL_ALPHA=1;
  // grid rings + scale labels
  x.strokeStyle='rgba(7,108,56,'+RING_ALPHA+')';x.lineWidth=1;
  x.fillStyle='rgba(7,108,56,'+LABEL_ALPHA+')';x.font='10px system-ui';x.textAlign='left';
  for(var k=1;k<=3;k++){var rr=R*k/3;x.beginPath();x.arc(cx,cy,rr,0,7);x.stroke();
    x.fillText((visR*k/3).toFixed(0)+'m',cx+4,cy-rr+34);}
  // cross + compass
  x.strokeStyle='rgba(7,108,56,'+RING_ALPHA+')';x.beginPath();
  x.moveTo(cx-R,cy);x.lineTo(cx+R,cy);x.moveTo(cx,cy-R);x.lineTo(cx,cy+R);x.stroke();
  x.textAlign='left';
  var sc=R/visR;
  function plot(e,n){
    var right=e*Math.cos(rotation)-n*Math.sin(rotation);
    var up=e*Math.sin(rotation)+n*Math.cos(rotation);
    return[cx+right*sc,cy-up*sc];
  }
  // surfer path (faded by age: oldest dim, newest bright)
  var h=recent;
  if(st.fix&&h.length>1){
    for(var i=1;i<h.length;i++){
      var a=toEN(st,h[i-1]),b=toEN(st,h[i]);
      var pa=plot(a.e,a.n),pb=plot(b.e,b.n);
      x.strokeStyle='rgba(7,108,56,'+(0.25+0.7*i/h.length).toFixed(2)+')';
      x.lineWidth=2;x.beginPath();x.moveTo(pa[0],pa[1]);x.lineTo(pb[0],pb[1]);x.stroke();
    }
  }
  // servo aim lines (angle increases CCW; compass bearing increases CW)
  var off=(d.servo.mount_offset_deg||0)+(d.servo.declination_deg||0);
  function aim(angle,col,w){
    var brg=((off-angle-heading)%360+360)%360,r=brg*Math.PI/180;
    x.strokeStyle=col;x.lineWidth=w;x.beginPath();x.moveTo(cx,cy);
    x.lineTo(cx+Math.sin(r)*R,cy-Math.cos(r)*R);x.stroke();
  }
  // Servo 目前：畫成一個 5 度扇形雷達波束（半徑方向漸層 + 發光邊緣），
  // 比單一細線更有「雷達掃描」的感覺；halfWidthDeg 可調整扇形寬度。
  function aimSector(angle,rgb,halfWidthDeg){
    var brg=((off-angle-heading)%360+360)%360;
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
  if(d.station.fix&&d.servo.calibrated&&d.servo.declination_deg!=null){
    aim(d.servo.target,'rgba(5,80,40,.95)',2);
    aimSector(d.servo.angle,'7,140,64',2.5);   // 5° 扇形 (±2.5°)
  }
  // current surfer marker + GPS status tag
  if(st.fix&&d.client.fix){
    var cur=toEN(st,d.client),p=plot(cur.e,cur.n);
    x.fillStyle='#076c38';x.beginPath();x.arc(p[0],p[1],6,0,7);x.fill();
    x.strokeStyle='#fff';x.lineWidth=1.5;x.stroke();
    drawGpsTag(x,p[0],p[1],W,H,
      'Surfer'+(dist!=null?(' '+dist.toFixed(0)+'m'):''),
      clientSatelliteGradeCount(d.client),d.client.hdop,clientSatelliteWord(d.client));
  }
  // A client-only view is centred on the client, not a fabricated station.
  if(d.station.fix){x.fillStyle='#123b25';x.beginPath();x.arc(cx,cy,5,0,7);x.fill();}
  // Keep compass letters legible above the aim glow and GPS tags.
  x.font='bold 12px system-ui';x.textAlign='center';
  [[0,'N'],[90,'E'],[180,'S'],[270,'W']].forEach(function(dir){
    var a=(dir[0]-heading)*Math.PI/180,px=cx+Math.sin(a)*(R-44),py=cy-Math.cos(a)*(R-44);
    x.fillStyle='#ffffff';x.fillRect(px-8,py-9,16,18);
    x.fillStyle='#123b25';x.fillText(dir[1],px,py+4);
  });
  x.textAlign='left';
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
      drawMap(last.track,geoDist(last.track.station,last.track.client));};
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
  renderServoRing();
  var c=$('radar'),x=c.getContext('2d'),W=c.width,H=c.height;
  x.clearRect(0,0,W,H);
  var h=recentHist(RADAR_WINDOW_MS),cli=[],srv=[];
  for(var i=0;i<h.length;i++){
    if(h[i].lat||h[i].lon)cli.push({lat:h[i].lat,lon:h[i].lon});
    if(h[i].sfix)srv.push({lat:h[i].slat,lon:h[i].slon});
  }
  if(d.client.fix)cli.push({lat:d.client.lat,lon:d.client.lon});
  if(d.station.fix)srv.push({lat:d.station.lat,lon:d.station.lon});
  var all=cli.concat(srv);
  x.fillStyle='#f1f6f2';x.fillRect(0,0,W,H);
  if(all.length===0){
    x.fillStyle='#f85149';x.font='13px system-ui';x.textAlign='center';
    x.fillText('尚無 GPS 軌跡資料',W/2,H/2);x.textAlign='left';return;}
  // Centre on the LIVE station/surfer midpoint, not on the track's bounding box.
  // Using the box made the view drift away as the trail grew, so the two things
  // you actually care about slid off-centre while old track pulled the camera.
  var minSpan=0.0009,midLat,midLon;
  if(d.station.fix&&d.client.fix){
    midLat=(d.station.lat+d.client.lat)/2;midLon=(d.station.lon+d.client.lon)/2;
  }else if(d.station.fix){midLat=d.station.lat;midLon=d.station.lon;}
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
  [d.station.fix?d.station:null,d.client.fix?d.client:null].forEach(function(p){
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
    x.fillStyle='#405849';x.font='12px system-ui';x.textAlign='center';
    x.fillText('地圖底圖載入中…（手機需開行動數據）',W/2,18);x.textAlign='left';
  }
  // tracks on top
  drawPath(x,srv,plot,'rgba(7,108,56,0.95)','#076c38');
  drawPath(x,cli,plot,'rgba(248,81,73,0.95)','#f85149');
  if(cli.length&&d.client.fix){var lp=plot(cli[cli.length-1]);
    drawGpsTag(x,lp[0],lp[1],W,H,
      'Surfer'+(d.station.fix&&dist!=null?(' '+dist.toFixed(0)+'m'):''),
      clientSatelliteGradeCount(d.client),d.client.hdop,clientSatelliteWord(d.client));}
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
  if(d.station.fix&&d.client.fix&&dist!=null){
    x.fillStyle='#fff';x.font='bold 12px system-ui';x.textAlign='left';
    x.fillText('站\u2194Surfer '+dist.toFixed(0)+' m',10,H-12);}
  x.fillStyle='#263f30';x.font='9px system-ui';x.textAlign='right';
  x.fillText('© OpenStreetMap',W-6,12);x.textAlign='left';
}

// ---- Station phone position + remote T096 Client controls -----------------
// The station itself is HTTP, so browser geolocation is deliberately delegated
// to the HTTPS helper.  It sends no location anywhere except this open station
// page via postMessage; the Station POST below remains on the local Wi-Fi link.
var PHONE_HELPER_ORIGIN='https://n8886919.github.io';
var PHONE_HELPER_URL=PHONE_HELPER_ORIGIN+'/Shore-Spotter/phone-location.html';
var phonePopup=null,stationPositionBusy=false,queuedPhonePosition=null;
var clientControlBusy=false;
function formEncode(values){
  return Object.keys(values).map(function(k){return encodeURIComponent(k)+'='+encodeURIComponent(values[k]);}).join('&');
}
function formPost(url,values,key){
  return httpRequest(url,0,key||null,null,'POST',null,formEncode(values),true);
}
function validStationReturnUrl(url){
  try{
    var u=new URL(url),h=u.hostname.toLowerCase();
    return u.protocol==='http:'&&(h==='localhost'||/^(?:10|127)(?:\.\d{1,3}){3}$/.test(h)||
      /^192\.168(?:\.\d{1,3}){2}$/.test(h)||/^172\.(?:1[6-9]|2\d|3[01])(?:\.\d{1,3}){2}$/.test(h)||/\.local$/.test(h));
  }catch(_e){return false;}
}
function validPhonePosition(p){
  return p&&p.type==='shore-spotter-phone-position-v1'&&Number.isFinite(p.lat)&&Number.isFinite(p.lon)&&
    Number.isFinite(p.accuracy)&&Number.isFinite(p.timestamp)&&p.lat>=-90&&p.lat<=90&&p.lon>=-180&&p.lon<=180&&
    p.accuracy>=0&&p.accuracy<=100000&&p.timestamp>=1577836800000&&p.timestamp<=Date.now()+300000;
}
function phoneLocationMessage(event,popup){
  if(event.origin!==PHONE_HELPER_ORIGIN||event.source!==popup||!validPhonePosition(event.data))return null;
  return {lat:event.data.lat,lon:event.data.lon,accuracy:event.data.accuracy,timestamp:Math.round(event.data.timestamp)};
}
function positionTime(ms){
  if(!Number.isFinite(ms)||ms<=0)return '--';
  var d=new Date(ms);return d.toLocaleTimeString([], {hour:'2-digit',minute:'2-digit',second:'2-digit'});
}
function renderStationPosition(j){
  j=j||{};var valid=!!j.valid,source=j.source==='phone'?'手機':'Station GNSS';
  $('stationPositionSource').textContent=valid?source:'無有效位置';
  $('stationPositionTime').textContent=valid?positionTime(j.updated_utc_ms):'--';
  $('stationPositionAccuracy').textContent=valid&&Number.isFinite(j.accuracy_m)?j.accuracy_m.toFixed(1)+' m':'--';
  if(j.helper_url&&/^https:\/\/n8886919\.github\.io\/Shore-Spotter\/phone-location\.html(?:$|[?#])/.test(j.helper_url))PHONE_HELPER_URL=j.helper_url;
}
function refreshStationPosition(){
  return httpRequest('/api/station/position',2,'station-position').then(renderStationPosition).catch(function(){
    $('stationPositionSource').textContent='讀取失敗';
  });
}
function submitStationPhonePosition(p,quiet){
  if(!validPhonePosition({type:'shore-spotter-phone-position-v1',lat:p.lat,lon:p.lon,accuracy:p.accuracy,timestamp:p.timestamp})){
    if(!quiet)toast('位置資料無效');return Promise.resolve();
  }
  if(stationPositionBusy){queuedPhonePosition=p;return Promise.resolve();}
  stationPositionBusy=true;$('stationPositionState').textContent='正在更新 Station 位置…';
  return formPost('/api/station/position',{action:'phone',lat:p.lat,lon:p.lon,accuracy:p.accuracy,timestamp:Math.round(p.timestamp)},'station-phone')
    .then(function(j){renderStationPosition(j);$('stationPositionState').textContent='已更新；手機定位頁背景化或關閉後，Station 會保留此最後位置到重新開機。';})
    .catch(function(e){$('stationPositionState').textContent='位置未確認：'+(e.message||'連線失敗');if(!quiet)toast(e.message||'位置未確認');})
    .finally(function(){stationPositionBusy=false;var next=queuedPhonePosition;queuedPhonePosition=null;if(next)submitStationPhonePosition(next,true);});
}
function openPhoneLocationHelper(){
  var returnUrl=window.location.href;
  if(!validStationReturnUrl(returnUrl)){toast('目前 Station 位址不是允許的私有 IP 或 .local');return;}
  phonePopup=window.open(PHONE_HELPER_URL+'#return='+encodeURIComponent(returnUrl),'shoreSpotterPhoneLocation','popup,width=420,height=620');
  if(!phonePopup)toast('瀏覽器封鎖手機定位頁，請允許 popup 後重試');
  else $('stationPositionState').textContent='已開啟 HTTPS 手機定位頁；在頁內明確按「開始持續定位」。';
}
window.addEventListener('message',function(event){
  var p=phoneLocationMessage(event,phonePopup);
  if(p)submitStationPhonePosition(p,true);
});
$('btnPhonePosition').onclick=openPhoneLocationHelper;
$('btnManualPhonePosition').onclick=function(){
  var latText=$('stationPhoneLat').value.trim(),lonText=$('stationPhoneLon').value.trim(),accuracyText=$('stationPhoneAccuracy').value.trim();
  if(!latText||!lonText||!accuracyText){toast('請完整填寫緯度、經度與精度');return;}
  var p={lat:Number(latText),lon:Number(lonText),accuracy:Number(accuracyText),timestamp:Date.now()};
  submitStationPhonePosition(p,false);
};
$('btnGnssPosition').onclick=function(){
  $('btnGnssPosition').disabled=true;$('stationPositionState').textContent='正在切回 Station GNSS…';
  formPost('/api/station/position',{action:'gnss'},'station-gnss').then(function(j){renderStationPosition(j);$('stationPositionState').textContent='已要求改用 Station GNSS。';})
    .catch(function(e){$('stationPositionState').textContent='切換未確認：'+(e.message||'連線失敗');toast(e.message||'切換未確認');})
    .finally(function(){$('btnGnssPosition').disabled=false;});
};
function clientStateText(state){return ({ready:'停止待命（網頁最遲約 30 秒可啟動）',tracking:'追蹤中',test:'RF 測試中',storage:'已回覆收納請求',unknown:'未知／未確認'})[state]||'未知／未確認';}
function clientCommandText(command){return ({idle:'無待命令',pending:'等待 Client 確認',confirmed:'Client 已確認',timeout:'等待逾時',error:'命令錯誤'})[command]||'--';}
function renderClientControl(j){
  j=j||{};var supported=j.supported===true,command=clientCommandText(j.command);
  $('clientRadioGroup').textContent=Number.isFinite(j.frequency_mhz)?(j.rf_group===0?'A':j.rf_group===1?'B':'?')+' / '+j.frequency_mhz.toFixed(1)+' MHz':'--';
  $('clientPairedId').textContent=j.client_id>0?Number(j.client_id).toString(16).toUpperCase().padStart(4,'0'):'未配對';
  $('clientControlState').textContent=supported?clientStateText(j.state):'未收到近期 STATE';
  $('clientControlAge').textContent=supported&&Number.isFinite(j.age_ms)?(j.age_ms/1000).toFixed(1)+' s':'--';
  $('clientControlCommand').textContent=(j.state==='storage'&&j.command==='confirmed'?'已回覆收納請求':command)+(j.command_id!=null?' #'+j.command_id:'');
  var test=Number.isFinite(j.test_received)?'RF：收 '+j.test_received+'／漏 '+(Number.isFinite(j.test_missing)?j.test_missing:'--')+
    '，'+(Number.isFinite(j.test_rssi)?j.test_rssi.toFixed(0)+' dBm':'--')+'，'+(Number.isFinite(j.test_snr)?j.test_snr.toFixed(1)+' dB':'--')+
    '，最大間隔 '+(Number.isFinite(j.test_max_gap_ms)?j.test_max_gap_ms+' ms':'--'):'RF：尚無測試結果';
  $('clientControlTest').textContent=test;
  $('clientControlHint').textContent=supported?(j.last_error?'最後錯誤：'+j.last_error:
    j.command==='pending'?'命令已入隊，等待 T096 STATE 確認。':
    j.charging===true?'USB 供電中：待命 12 小時自動深度休眠會抑制；請先移除 USB 才能請求深度休眠。':
    j.state==='storage'?'Client 已回覆收納請求；這不證明已進入深度休眠。深度休眠後須由外部喚醒，網頁不能喚醒。':
    '停止後為待命，網頁最遲約 30 秒可重新啟動；未接 USB 時待命 12 小時會自動請求深度休眠。開始／停止 Client 不會切換 Servo GPS 模式。'):
    '尚未收到近期 T096 STATE；離線不會被標示為待命或深度休眠。';
  ['btnClientStart','btnClientStop','btnClientTest'].forEach(function(id){$(id).disabled=!supported||clientControlBusy;});
  $('btnClientStore').disabled=!supported||clientControlBusy||j.charging===true;
}
function refreshClientControl(){return httpRequest('/api/client/control',2,'client-control').then(renderClientControl).catch(function(){renderClientControl({supported:false});});}
function sendClientControl(action){
  if(clientControlBusy)return;clientControlBusy=true;renderClientControl({supported:true,state:'unknown',command:'pending'});
  return formPost('/api/client/control',{action:action},'client-command').then(function(j){renderClientControl(j);})
    .catch(function(e){toast(e.message||'Client 命令未確認');$('clientControlHint').textContent='命令未確認：'+(e.message||'連線失敗');})
    .finally(function(){clientControlBusy=false;refreshClientControl();});
}
$('btnClientStart').onclick=function(){sendClientControl('start');};
$('btnClientStop').onclick=function(){sendClientControl('stop');};
$('btnClientTest').onclick=function(){sendClientControl('test');};
$('btnClientStore').onclick=function(){sendClientControl('store');};

showPage('radar');
refresh();refreshStatus();
// ---- camera compass calibration ----
var compassRequestId=0;
$('btnCompassCenter').onclick=function(){
  if(controlChanging){toast('控制操作處理中，請稍後再回到 90°');return;}
  cancelRingGesture();
  var requestId=++compassRequestId,intent=controlIntent();
  controlChanging=true;$('btnCompassCenter').disabled=true;
  clearTimeout(servoSendTimer);servoSendTimer=0;servoPendingAngle=null;servoPendingIntent=null;
  servoDragEnded=false;dragging=false;$('sld').disabled=true;
  renderServoRing();
  // Invalidate the cached centre state before sending the move.
  compassServoState=null;
  $('compassCalState').textContent='正在切為手動並回到 90°…';
  return servoRequest.then(function(){return post('/api/servo/center',intent);})
    .then(function(j){
      if(j.ok!==true||j.mode!=='manual'||j.target!==90)
        throw new Error('回中回覆不完整');
      if(requestId===compassRequestId)
        $('compassCalState').textContent='已送出回到 90°；等鏡頭停穩後再按校正';
    }).catch(function(e){
      if(requestId===compassRequestId)$('compassCalState').textContent='回中未確認：'+(e.message||'連線失敗')+'，可重試';
    }).then(function(){
      controlChanging=false;$('btnCompassCenter').disabled=false;return refresh();
    });
};
$('btnCompassCal').onclick=function(){
  var requestId=++compassRequestId;
  var text=$('compassBearing').value.trim();
  var bearing=Number(text);
  if(text.length>16||!/^(?:\d+\.?\d*|\.\d+)$/.test(text)||!Number.isFinite(bearing)||bearing<0||bearing>=360){
    $('compassCalState').textContent='請輸入 0 到未滿 360 度的十進位角度';
    toast($('compassCalState').textContent);return;
  }
  if(controlChanging||servoSending||servoPendingAngle!==null||dragging||
     !compassServoState||compassServoState.calibration_ready!==true){
    var hint='請先回到 90° 並等鏡頭停穩，再按校正';
    $('compassCalState').textContent=hint;toast(hint);return;
  }
  // The backend rechecks centre after queueing, so stale UI cannot calibrate
  // a servo that has moved since the last status poll.
  $('compassCalState').textContent='校正送出中…';
  return post('/api/track/calibrate?bearing='+encodeURIComponent(text),controlIntent())
    .then(function(j){
      if(requestId!==compassRequestId)return;
      if(j.ok!==true||j.method!=='compass'||!Number.isFinite(j.bearing)||
         !Number.isFinite(j.mount_offset_deg)||!Number.isFinite(j.servo_angle))
        throw new Error('校正回覆不完整，請重新校正');
      $('compassCalState').textContent='已校正 '+j.bearing+'°；方向不準可再校正';
      $('mcOff').textContent=j.mount_offset_deg.toFixed(1)+'°';
      return refresh();
    }).catch(function(e){
      if(requestId===compassRequestId)$('compassCalState').textContent='校正未確認：'+(e.message||'連線失敗')+'，可重試';
    });
};

refreshStatus();          // 提醒不要等到第一個 3 秒週期才出現
refreshStationPosition();
refreshClientControl();
setInterval(refresh,1000);
setInterval(refreshStatus,3000);
setInterval(refreshStationPosition,3000);
setInterval(refreshClientControl,3000);
loadSpeedSetting();
loadPredictionSetting();
</script>
</body>
</html>
)rawlit";
