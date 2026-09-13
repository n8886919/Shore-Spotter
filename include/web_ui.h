#pragma once
// Embedded single-page control UI served by the shore station.
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
// Manual stops tracking and allows direct slider adjustment.
// Layout: full-height, three top tabs — 雷達 (radar/map canvas with the
//   雷達/地圖 + 手動/GPS/UART controls and the slider overlaid at the bottom) and
//   資訊 (shared speed limit, GPS prediction, compass calibration and telemetry).
//   除錯 records read-only browser snapshots and exports a diagnostic JSON file.
//   The canvas auto-sizes to its container, and the
//   surfer marker carries a GPS-status tag (衛星 少/普通/好, 預期精度 ±N m,
//   Good/OK/Bad).
// GPS is reported in operator terms, not receiver terms: the satellite count
//   becomes 少/普通/好 and HDOP becomes 預期精度 in metres (see accM() below).
// UART is the boot default; GPS is selectable only with fresh Good/OK signals.
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
#pgInfo,#pgDebug{overflow:auto}
#debugNote{width:100%;min-height:70px;resize:vertical;background:var(--bg);color:var(--fg);border:1px solid var(--line);border-radius:6px;padding:8px;font:inherit}
#debugSummary,#debugLog{white-space:pre-wrap;overflow-wrap:anywhere;font:12px/1.5 ui-monospace,monospace;margin:8px 0 0}
#debugLog{max-height:45vh;overflow:auto}
#pgDebug .card{flex:none}#debugRecordState{font-size:13px}
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
.sliderTrack{flex:1;min-width:0}.rangeEnds{display:flex;justify-content:space-between;font-size:10px;color:var(--mut)}
.sliderOverlay input[type=range]{width:100%;direction:ltr;accent-color:var(--acc)}
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
.seg:last-child{border-radius:0 6px 6px 0}
.seg+.seg{border-left:0}
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
#cfgSpeed{width:130px;padding:8px;border:1px solid var(--line);border-radius:6px;background:#0d1117;color:var(--fg)}
#cfgPrediction{width:20px;height:20px;accent-color:var(--acc)}
#radarWrap{min-height:180px}#pgRadar{overflow:auto}
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
  <button id="tabDebug" type="button">除錯</button>
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
        <button id="mGps" class="seg" type="button" disabled>GPS</button>
        <button id="mUart" class="seg on" type="button">UART</button>
        <button id="mManual" class="seg" type="button">手動</button>
      </span>
      <span id="controlState" class="hint2">UART・等待指令</span>
    </div>
    <div id="radarWrap">
      <canvas id="radar"></canvas>
      <div class="sliderOverlay">
        <div class="sliderTrack"><input id="sld" aria-label="Servo 目標角度，放開生效" type="range" min="0" max="180" step="1" value="90" disabled><div class="rangeEnds"><span>0°</span><span>180°</span></div></div>
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
      <span id="predictionHelp" class="hint3">僅影響 GPS 模式。開啟（α＝1）：用速度與方向推估目前位置。關閉（α＝0）：追蹤最近收到的位置。切換可能改變追蹤目標，共用最高速度維持不變。修改後自動儲存，重開機保留。</span>
      <span id="predictionSaveState" class="hint3" role="status" aria-live="polite">讀取預測設定中</span>
    </div>
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
        <div class="r"><span>韌體版本</span><b id="sVersion">--</b></div>
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
      <h3>鏡頭指南針校正</h3>
      <div class="calRow">
        <input id="compassBearing" type="number" inputmode="decimal" min="0" max="359.9" step="0.1" placeholder="指南針角度 0–359.9°" aria-label="鏡頭指南針角度">
        <button id="btnCompassCal" type="button">校正</button>
      </div>
      <span class="hint3">切到手動、等鏡頭停穩，輸入鏡頭上磁針指南針的讀數。北 0°、東 90°、南 180°、西 270°。
        校正不需要 GPS；台灣磁偏角會依攝影站 GPS 位置與日期自動換算。完成後選「GPS」。
        腳架轉動、重開機、重新架設或移動指南針後請重新校正。UART 模式不需要此校正。</span>
      <div class="r"><span>校正偏移</span><b id="mcOff">--</b></div>
      <div class="r"><span>磁偏角補償</span><b id="mcDeclination">--</b></div>

    </div>
    <div class="card" style="flex:none;display:flex;align-items:center;gap:10px;flex-wrap:wrap">
      <button id="btnClear" type="button">清除軌跡</button>
      <button id="btnExport" type="button">匯出 GPX（可上傳 Strava）</button>
      <span id="exportHint" class="hint2" style="margin-left:0">網頁最多保留過去 2 小時軌跡，雷達／地圖仍只顯示最近 5 分鐘；匯出後會自動清空。GPX 檔請自行到 Strava 網頁上傳</span>
    </div>
  </section>
  <section id="pgDebug" class="page">
    <div class="card">
      <h3>測試紀錄</h3>
      <div class="calRow"><button id="btnDebugStart" type="button">開始新紀錄</button><button id="btnDebugStop" type="button" disabled>停止紀錄</button><button id="btnDebugExport" type="button">匯出 JSON</button></div>
      <p id="debugRecordState" role="status" aria-live="polite">尚未錄製</p>
      <label for="debugNote">測試備註</label><textarea id="debugNote" maxlength="2000" placeholder="例如：距離、直線／折返、鏡頭落後或抖動的時間"></textarea>
      <span class="hint3">每秒最多取樣一次，最長 10 分鐘、1200 筆或 5 MiB，達上限自動停止。切換分頁仍繼續錄製；手機鎖屏可能漏採，重整網頁會遺失。匯出包含定位、設定與紀錄，方便交給 AI 分析。</span>
      <span class="hint3">發送排程不代表 GPS 有同樣頻率的新定位。角度與速度是軟體命令，未量測機構實際動作。</span>
      <span class="hint3">Client DIAG 約每 30 秒回報一次，只代表回報當時的狀態；過期資料仍保留供比對。</span>
    </div>
    <div class="card"><h3>即時狀態</h3><span id="debugPollState" class="hint3" role="status">進入此頁後開始讀取</span><pre id="debugSummary">尚無資料</pre></div>
    <div class="card"><h3>Server 文字紀錄</h3><span class="hint3">僅讀取，匯出不會清除 Server 或瀏覽器紀錄。畫面最多保留最近 64 KiB 文字。</span><pre id="debugLog">尚無紀錄</pre></div>
  </section>
</main>

<div id="toast"></div>

<script>
var $=function(id){return document.getElementById(id);};
var last={track:null,status:null,alerts:[]};
var dragging=false;
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
var hist=[];               // client-accumulated path (server no longer stores it)
var HIST_RETAIN_MS=2*60*60*1000; // prune older points on append (for GPX export)
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
function httpRequest(url,priority,key,read,method,intent){
  if(key&&httpByKey[key])return httpByKey[key].promise;
  var motion=method==='POST'&&isMotionUrl(url);
  if(motion){intent=intent||controlIntent();if(performance.now()>=intent.deadline)
    return Promise.reject(httpError('控制指令等待逾時，請重試','TimeoutError'));}
  var job={url:url,priority:priority,key:key,read:read||httpJsonBody,method:method,
    motion:motion,order:++httpOrder,deadline:motion?intent.deadline:performance.now()+HTTP_TIMEOUT_MS,
    context:motion?intent.context:null};
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
});
$('sld').addEventListener('change',function(){
  if(!servoGestureCancelled)queueServoAngle(this.value,true);
});
// Cancel unsent slider values and let the in-flight manual write finish first.
// Otherwise an old slider request can arrive after Auto and take control back.
function controlAction(url){
  if(controlChanging)return Promise.resolve();
  var intent=controlIntent();
  controlChanging=true;
  clearTimeout(servoSendTimer);servoSendTimer=0;servoPendingAngle=null;servoPendingIntent=null;
  servoDragEnded=false;dragging=false;
  $('sld').disabled=true;
  return servoRequest.then(function(){return post(url,intent);})
    .catch(function(){})
    .then(function(){controlChanging=false;return refresh();});
}
$('mGps').onclick=function(){controlAction('/api/servo/mode?mode=gps');};
$('mUart').onclick=function(){controlAction('/api/servo/mode?mode=uart');};
$('mManual').onclick=function(){controlAction('/api/servo/mode?mode=manual');};

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
  $('tabDebug').classList.toggle('on',p==='debug');
  $('pgRadar').classList.toggle('on',p==='radar');
  $('pgInfo').classList.toggle('on',p==='info');
  $('pgDebug').classList.toggle('on',p==='debug');
  if(p==='radar')redraw();
  if(p==='debug')pollDebug();
}
$('tabRadar').onclick=function(){showPage('radar');};
$('tabInfo').onclick=function(){showPage('info');};
$('tabDebug').onclick=function(){showPage('debug');};
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
  var gps=sv.mode==='gps',uart=(sv.mode==='uart'||sv.mode==='jetson'),tracking=gps||uart;
  $('mGps').classList.toggle('on',gps);
  $('mUart').classList.toggle('on',uart);
  $('mManual').classList.toggle('on',!tracking);
  $('mGps').disabled=controlChanging||!sv.gps_available;
  $('mGps').title=sv.gps_available?'':'Server 與 Client 的 GPS 都需為 Good 或 OK';
  $('mUart').disabled=$('mManual').disabled=controlChanging;
  var moving=!!sv.moving;
  $('sld').disabled=controlChanging||tracking;
  $('btnCompassCal').disabled=controlChanging||tracking||moving||!!sv.motion_fault;
  if(!speedSaving && document.activeElement!==$('cfgSpeed') && Number.isFinite(sv.speed_limit_deg_s)) {
    lastSavedSpeed=sv.speed_limit_deg_s;speedLoaded=true;
    $('cfgSpeed').value=lastSavedSpeed;
  }
  $('cfgSpeed').disabled=controlChanging||speedSaving||!speedLoaded;
  showPredictionSetting({enabled:sv.prediction_enabled,alpha:sv.prediction_alpha,
    control_boot_id:sv.control_boot_id,control_epoch:sv.control_epoch,
    command_seq:sv.command_seq,clock_ms:sv.clock_ms},false);
  $('cfgPrediction').disabled=controlChanging||predictionSaving||!predictionLoaded;
  var label=sv.mode==='paused'?'已暫停':gps?'GPS':uart?'UART':'手動';
  if(gps){
    label+='・'+(sv.source==='gps'?'追蹤中':
      !sv.calibrated?'保持・待指南針校正':
      sv.declination_deg==null?'保持・待 GPS 位置／日期':
      sv.gps_usable?'保持・GPS 穩定中':'保持・等待有效 GPS');
  }else if(uart){
    label+='・'+(sv.source==='uart'?'追蹤中':'保持・等待 SET');
  }else if(moving){
    label+='・移動中 '+Math.round(servoUiAngle(sv.angle))+'° → '+Math.round(servoUiAngle(sv.target))+'°';
  }
  if(sv.motion_fault)label='運動控制異常・已保持，請重開機';
  $('controlState').textContent=label;
  $('controlState').style.color=tracking&&sv.source==='hold'?'var(--bad)':'var(--ok)';
  if(!dragging&&document.activeElement!==$('sld')){
    var shown=sv.mode==='manual'&&sv.target!=null?sv.target:sv.angle;
    $('sld').value=Math.round(servoUiAngle(shown));$('angTxt').textContent=Math.round(servoUiAngle(shown));
  }
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
  var col=g==='good'?'#3fb950':g==='ok'?'#d29922':g==='bad'?'#f85149':'#8b949e';
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
  x.fillStyle='rgba(13,17,23,.85)';x.fillRect(bx,by,bw,bh);
  x.strokeStyle=col;x.strokeRect(bx,by,bw,bh);
  x.textAlign='left';
  x.fillStyle='#e6edf3';x.font='bold 11px system-ui';x.fillText(l1,bx+8,by+15);
  x.fillStyle='#8b949e';x.font='11px system-ui';x.fillText(l2,bx+8,by+29);
  x.fillStyle=col;x.font='bold 11px system-ui';x.fillText(l3,bx+8,by+43);
}

// /api/track is now small (no history); the browser accumulates the path itself.
function fetchTrack(){return httpRequest('/api/track',1,'track');}

// Append the current sample to the local path (skip stale/duplicate points).
// Full buffer prunes on append at HIST_RETAIN_MS (2h) for GPX export; drawing only
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
    last.track_received_at=new Date().toISOString();
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
    $('mcOff').textContent=d.servo.calibrated?d.servo.mount_offset_deg.toFixed(1)+'°':'未校正';
    $('mcDeclination').textContent=d.servo.declination_deg==null?'等待有效 GPS 位置／日期':
      d.servo.declination_deg.toFixed(1)+'°（自動）';
    // --- Client block ---
    $('cFix').textContent=d.client.fix?'有':'無';
    $('cSat').textContent=clientSatelliteWord(d.client);
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
    $('sUp').textContent=fmtUptime(s.health.uptime_s);
    $('sVersion').textContent=s.health.firmware_version?'v'+s.health.firmware_version:'未標版';
  }).catch(function(){});
}

// Read-only diagnostic capture. Each bounded poll uses the shared HTTP queue.
var DEBUG_INTERVAL_MS=1000,DEBUG_MAX_MS=600000;
var DEBUG_MAX_SAMPLES=1200,DEBUG_MAX_BYTES=5*1024*1024,DEBUG_LOG_CHARS=32768;
var debugBusy=false,debugLastPoll=-Infinity,debugLatest=null,debugLatestAt=null;
var debugLogCursor=0,debugLogBoot=null,debugLogText='',debugLogTrimmed=0;
var debugRecording=false,debugStartedAt=null,debugStartedMono=0,debugStoppedAt=null,debugStopReason='';
var debugSamples=[],debugBytes=0,debugGapCount=0,debugErrorCount=0,debugLogDrops=0,debugReboots=0;
var debugPreviousSample=null,debugLastErrors=[],debugLastWarnings=[],debugRecordingId=0;
var debugEventBoot=null,debugEventNext=null,debugEventCurrent=[],debugEventRetired=[];
function debugWanted(){return debugRecording||(page==='debug'&&!document.hidden);}
function debugClone(value){return value==null?null:JSON.parse(JSON.stringify(value));}
function debugNumber(value,suffix){return Number.isFinite(value)?value+(suffix||''):'未知';}
function debugInterval(value){return Number.isFinite(value)&&value>0?value+' ms':'未知';}
function debugClientId(value){return Number.isInteger(value)&&value>0&&value<65535?'0x'+('0000'+value.toString(16).toUpperCase()).slice(-4):'未知';}
function renderDebug(){
  var d=debugLatest||{},cfg=d.config||{},g=d.gps||{},counts=d.counters||{},diag=d.client_diagnostic||{};
  var client=last.track&&last.track.client||{},servo=last.track&&last.track.servo||{};
  var http=last.status&&last.status.timing&&last.status.timing.http_detail,slow=http&&http.slowest;
  var diagReceived=diag.received===true,diagState=!diagReceived?'尚未收到':diag.fresh===true?'仍在有效期，非即時':
    diag.fresh===false?'已過期，僅供歷史參考':'有效期未知';
  var rejected=debugEventCurrent.filter(function(e){return e.kind==='binding';}).slice(-1)[0];
  var diagFlags=Number.isInteger(diag.status_bits)?diag.status_bits:null;
  var lines=[
    '韌體 '+(d.firmware_version||'未知')+' ／ LoRa 協定 '+debugNumber(d.protocol_version),
    '發送排程 '+(cfg.send_interval_ms?Number(1000/cfg.send_interval_ms).toFixed(2)+' Hz':'未知'),
    'Server 本機 GNSS 新定位間隔 '+debugInterval(g.last_epoch_interval_ms)+
      '；來源年齡 '+debugNumber(g.source_age_ms,' ms'),
    'Client GNSS 低頻 DIAG：'+diagState+'；接收後 '+debugNumber(diagReceived?diag.rx_age_ms:null,' ms')+
      '；該次新定位間隔 '+debugInterval(diagReceived?diag.epoch_interval_ms:null),
    'Server 由封包推估 Client 更新間隔 '+debugInterval(counts.inferred_source_interval_ms)+'（推估，非 Client 實測）',
    'LoRa '+debugNumber(cfg.rf_frequency_mhz,' MHz')+' ／ SF '+debugNumber(cfg.sf)+' ／ 頻寬 '+debugNumber(cfg.bw_khz,' kHz'),
    '綁定 Client：'+(cfg.bound_client_id===0?'未綁定（不接受 Client 定位）':debugClientId(cfg.bound_client_id))+
      (rejected?'；最近因綁定不符拒收 '+debugClientId(rejected.client_id):''),
    'Client 衛星 '+clientSatelliteWord(client)+'；遙測精確數 '+debugNumber(client.satellites,' 顆')+'（低頻更新）',
    '位置年齡：來源 '+debugNumber(client.source_age_ms,' ms')+' ／ 接收後 '+debugNumber(client.rx_age_ms,' ms')+
      ' ／ 合計估計 '+debugNumber(client.sample_age_ms,' ms'),
    '年齡依據 '+(client.age_basis||g.age_basis||'未知')+'；時鐘同步 '+(g.measurement_clock_synchronized===true?'是':'未證實'),
    'Servo '+(servo.mode||'未知')+' ／ '+(servo.source||'未知')+'；預測 '+
      (typeof servo.prediction_enabled==='boolean'?(servo.prediction_enabled?'開':'關'):'未知'),
    'Client 該次 DIAG 計數：UART 積壓丟棄 '+debugNumber(diagReceived?diag.backlog_drops:null)+
      ' ／ NMEA 錯誤 '+debugNumber(diagReceived?diag.nmea_errors:null)+' ／ 發送錯誤 '+debugNumber(diagReceived?diag.tx_errors:null)+
      ' ／ 跳過排程 '+debugNumber(diagReceived?diag.skipped_slots:null)+'（自 Client 開機累計，最大 65535）',
    'Client 該次 DIAG 狀態：'+(!diagReceived||diagFlags===null?'未知':
      '樣本 '+((diagFlags&1)?'有':'無')+' ／ 定位 '+((diagFlags&2)?'有效':'無效')+' ／ 速度 '+((diagFlags&4)?'有效':'無效')+
      ' ／ GGA '+((diagFlags&8)?'有':'無')+' ／ RMC '+((diagFlags&16)?'有':'無')),
    '事件環形紀錄覆寫 '+debugNumber(d.events&&d.events.overwritten)+'；文字遺失警示 '+debugLogDrops+'；重開機 '+debugReboots,
    '最慢 HTTP：'+(slow?(slow.route||'未知')+' ／ 全程 '+debugNumber(slow.total_ms,' ms')+
      ' ／ 解析／派送 '+debugNumber(slow.pre_handler_ms,' ms')+' ／ 建立回覆 '+debugNumber(slow.build_ms,' ms')+
      ' ／ 同步寫入 '+debugNumber(slow.write_ms,' ms')+' ／ 其他 '+debugNumber(slow.other_ms,' ms')+'（寫入時間非網路 RTT）':'尚無資料'),
    '計數器 '+JSON.stringify(counts)
  ];
  $('debugSummary').textContent=lines.join('\n');
  $('debugLog').textContent=debugLogText||'尚無紀錄';
  var elapsed=debugStartedAt?Math.round((debugRecording?performance.now()-debugStartedMono:
    new Date(debugStoppedAt).getTime()-new Date(debugStartedAt).getTime())/1000):0;
  $('debugRecordState').textContent=(debugRecording?'錄製中':debugStartedAt?'已停止'+(debugStopReason?'（'+debugStopReason+'）':''):'尚未錄製')+
    ' · '+debugSamples.length+' 筆 · '+Math.max(0,elapsed)+' 秒 · '+(debugBytes/1024).toFixed(0)+' KiB'+
    ' · 漏採／中斷 '+debugGapCount+' 次 · 讀取失敗 '+debugErrorCount+' 次'+
    (debugLogTrimmed?' · 畫面文字已截短':'');
  $('btnDebugStart').disabled=debugRecording;$('btnDebugStop').disabled=!debugRecording;
  var warningNames={event_ring_gap:'事件紀錄有缺口',event_boot_reset:'事件紀錄已換至新開機',event_backlog:'歷史事件分批讀取中',server_reboot:'Server 已重開機',log_boot_unknown:'文字紀錄缺少開機識別',
    server_log_dropped:'Server 文字紀錄有遺失',snapshot_boot_mismatch:'狀態與文字來自不同次開機'};
  $('debugPollState').textContent=debugBusy?'讀取中…':
    (debugLastErrors.length?'讀取未完成：'+debugLastErrors.join('；'):debugLatestAt?'最近讀取 '+debugLatestAt:'尚無資料')+
    (debugLastWarnings.length?'；'+debugLastWarnings.map(function(w){return warningNames[w]||w;}).join('；'):'');
}
function stopDebugRecording(reason){
  if(!debugRecording)return;
  debugRecording=false;debugStoppedAt=new Date().toISOString();debugStopReason=reason||'手動停止';renderDebug();
}
function startDebugRecording(){
  if(debugRecording)return;
  debugSamples=[];debugBytes=0;debugGapCount=0;debugErrorCount=0;debugPreviousSample=null;
  ++debugRecordingId;
  debugStartedAt=new Date().toISOString();debugStartedMono=performance.now();debugStoppedAt=null;debugStopReason='';
  debugRecording=true;renderDebug();return pollDebug();
}
function debugRead(url,read){
  return httpRequest(url,3,typeof url==='string'&&url.indexOf('/api/log')===0?'log':'debug',read);
}
function debugEventUrl(){
  return '/api/debug?limit=8'+(debugEventBoot===null?'':'&boot_id='+debugEventBoot+'&since='+debugEventNext);
}
function acceptDebugEvents(d,warnings){
  var e=d&&d.events;
  if(!d||d.schema_version!==2||!Number.isInteger(d.boot_id)||!e||!Array.isArray(e.items)||e.items.length>8||
      !Number.isInteger(e.next_id)||e.next_id<0||e.next_id>4294967295||
      typeof e.more!=='boolean'||typeof e.dropped!=='boolean'||typeof e.reset!=='boolean')throw new Error('不支援或無效的增量除錯格式');
  if(debugEventRetired.indexOf(d.boot_id)>=0)throw new Error('忽略舊開機的事件回覆');
  var switched=debugEventBoot!==null&&debugEventBoot!==d.boot_id;
  var previous=switched?null:debugEventNext,current=switched?[]:debugEventCurrent,delta=[];
  if(previous!==null&&((e.next_id-previous)|0)<0)throw new Error('忽略倒退的事件游標');
  e.items.forEach(function(item){
    if(!item||!Number.isInteger(item.id)||item.id<0||item.id>4294967295)throw new Error('無效事件識別碼');
    if(previous!==null&&((item.id-previous)|0)<=0)return;
    if(current.some(function(old){return old.id===item.id;})||delta.some(function(old){return old.id===item.id;}))return;
    if(((e.next_id-item.id)|0)<0)throw new Error('事件超出回覆游標');
    delta.push(item);
  });
  if(switched){debugEventRetired.push(debugEventBoot);warnings.push('event_boot_reset');}
  if(e.dropped)warnings.push('event_ring_gap');
  if(e.more)warnings.push('event_backlog');
  debugEventBoot=d.boot_id;debugEventNext=e.next_id;
  debugEventCurrent=current.concat(delta).slice(-64);
  return {boot_id:d.boot_id,next_id:e.next_id,more:e.more,dropped:e.dropped,reset:e.reset,items:delta};
}
function appendDebugSample(sample){
  if(!debugRecording||sample.recording_id!==debugRecordingId)return;
  if(debugPreviousSample!==null&&sample.client_monotonic_ms-debugPreviousSample>2500){
    sample.gap_since_previous_ms=sample.client_monotonic_ms-debugPreviousSample;++debugGapCount;
  }
  debugPreviousSample=sample.client_monotonic_ms;
  debugErrorCount+=sample.errors.length;
  debugGapCount+=sample.warnings.filter(function(w){return w!=='event_backlog';}).length;
  var saved=JSON.stringify(sample),bytes=new Blob([saved]).size;
  if(debugSamples.length>=DEBUG_MAX_SAMPLES||debugBytes+bytes>DEBUG_MAX_BYTES){
    stopDebugRecording('容量上限，最後一次取樣未保存');return;
  }
  debugSamples.push(JSON.parse(saved));debugBytes+=bytes;
  if(debugSamples.length>=DEBUG_MAX_SAMPLES)stopDebugRecording('已達取樣上限');
  if(performance.now()-debugStartedMono>=DEBUG_MAX_MS)stopDebugRecording('已達 10 分鐘');
}
function pollDebug(){
  var now=performance.now();
  if(!debugWanted()||debugBusy||controlChanging||now-debugLastPoll<DEBUG_INTERVAL_MS)return Promise.resolve();
  if(debugRecording&&now-debugStartedMono>=DEBUG_MAX_MS){stopDebugRecording('已達 10 分鐘');if(!debugWanted())return Promise.resolve();}
  debugBusy=true;debugLastPoll=now;
  var sample={observed_at:new Date().toISOString(),client_time_ms:Date.now(),client_monotonic_ms:now,
    recording_id:debugRecordingId,errors:[],warnings:[],debug:null,events:null,log:null};
  renderDebug();
  return debugRead(debugEventUrl,function(r){return r.json();}).then(function(d){
    sample.events=acceptDebugEvents(d,sample.warnings);
    // State snapshots contain ring metadata; raw events are recorded only as
    // deltas. The current display owns a separate bounded, deduplicated ring.
    var state=debugClone(d);delete state.events.items;
    debugLatest=state;debugLatestAt=new Date().toISOString();sample.debug=state;sample.debug_received_at=debugLatestAt;
  }).catch(function(e){sample.errors.push('狀態 '+(e.message||String(e)));}).then(function(){
    var from=debugLogCursor;
    return debugRead('/api/log?from='+from,function(r){
      var next=r.headers.get('X-Log-Next'),boot=r.headers.get('X-Log-Boot'),dropped=r.headers.get('X-Log-Dropped')==='1';
      return r.text().then(function(text){return {from:from,next:next===null?null:Number(next),boot_id:boot===null?null:Number(boot),dropped:dropped,text:text};});
    }).then(function(log){
      sample.log=log;sample.log_received_at=new Date().toISOString();
      if(log.boot_id!==null&&debugLogBoot!==null&&log.boot_id!==debugLogBoot){
        ++debugReboots;sample.warnings.push('server_reboot');debugLogText+='\n--- Server 重開機 ---\n';
      }
      if(log.boot_id===null)sample.warnings.push('log_boot_unknown');
      else debugLogBoot=log.boot_id;
      if(log.dropped){++debugLogDrops;sample.warnings.push('server_log_dropped');debugLogText+='\n--- Server 文字紀錄有遺失 ---\n';}
      if(log.boot_id!==null&&sample.debug&&log.boot_id!==sample.debug.boot_id)sample.warnings.push('snapshot_boot_mismatch');
      if(Number.isInteger(log.next)&&log.next>=0)debugLogCursor=log.next;
      else sample.errors.push('文字紀錄游標缺失');
      debugLogText+=log.text;
      if(debugLogText.length>DEBUG_LOG_CHARS){debugLogTrimmed+=debugLogText.length-DEBUG_LOG_CHARS;debugLogText=debugLogText.slice(-DEBUG_LOG_CHARS);}
    }).catch(function(e){sample.errors.push('文字 '+(e.message||String(e)));});
  }).then(function(){
    sample.track=debugClone(last.track);sample.status=debugClone(last.status);
    sample.track_received_at=last.track_received_at||null;sample.status_received_at=last.status_received_at||null;
    sample.completed_at=new Date().toISOString();
    debugLastErrors=sample.errors;debugLastWarnings=sample.warnings;appendDebugSample(sample);
  }).finally(function(){debugBusy=false;renderDebug();});
}
function buildDebugBundle(){
  var d=debugLatest||{};
  return {schema_version:2,kind:'shore_spotter_diagnostics',exported_at:new Date().toISOString(),
    event_identity:['boot_id','id'],
    firmware_version:d.firmware_version||null,protocol_version:d.protocol_version||null,
    notes:$('debugNote').value.slice(0,2000),config:debugClone(d.config),
    evidence_limits:['Browser sampling may have gaps, especially in background or with a locked screen.',
      'Radio send cadence is not proof of new GNSS epochs at the same rate.',
      'Servo angles and speed are commands; physical mechanism motion was not measured.',
      'GNSS source ages are estimates; receiver and server measurement clocks are not synchronized.'],
    recording:{active:debugRecording,started_at:debugStartedAt,stopped_at:debugStoppedAt,stop_reason:debugStopReason||null,
      sample_count:debugSamples.length,approx_bytes:debugBytes,gap_count:debugGapCount,error_count:debugErrorCount,
      max_duration_ms:DEBUG_MAX_MS,max_samples:DEBUG_MAX_SAMPLES,max_bytes:DEBUG_MAX_BYTES,
      log_counters_scope:'page_lifetime',server_log_drop_notices:debugLogDrops,server_reboots_observed:debugReboots,display_log_trimmed_chars:debugLogTrimmed},
    current:{track:debugClone(last.track),track_received_at:last.track_received_at||null,
      status:debugClone(last.status),status_received_at:last.status_received_at||null,
      debug:debugClone(debugLatest),debug_received_at:debugLatestAt,text_log:debugLogText,
      events:{boot_id:debugEventBoot,next_id:debugEventNext,items:debugClone(debugEventCurrent)},
      log_next:debugLogCursor,log_boot_id:debugLogBoot,errors:debugLastErrors.slice(),warnings:debugLastWarnings.slice()},samples:debugClone(debugSamples)};
}
function exportDebugBundle(){
  try{
    var blob=new Blob([JSON.stringify(buildDebugBundle(),null,2)],{type:'application/json'}),url=URL.createObjectURL(blob);
    var a=document.createElement('a');a.href=url;a.download='shorespotter_debug_'+new Date().toISOString().replace(/[:.]/g,'-')+'.json';
    document.body.appendChild(a);a.click();document.body.removeChild(a);
    setTimeout(function(){URL.revokeObjectURL(url);},1000);toast('已產生 JSON，請確認下載完成');
  }catch(e){toast('匯出失敗：'+(e.message||String(e)));}
}
$('btnDebugStart').onclick=startDebugRecording;
$('btnDebugStop').onclick=function(){stopDebugRecording('手動停止');};
$('btnDebugExport').onclick=exportDebugBundle;
setInterval(pollDebug,DEBUG_INTERVAL_MS);

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
  var off=(d.servo.mount_offset_deg||0)+(d.servo.declination_deg||0);
  function aim(angle,col,w){
    var brg=((off-angle)%360+360)%360,r=brg*Math.PI/180;
    x.strokeStyle=col;x.lineWidth=w;x.beginPath();x.moveTo(cx,cy);
    x.lineTo(cx+Math.sin(r)*R,cy-Math.cos(r)*R);x.stroke();
  }
  // Servo 目前：畫成一個 5 度扇形雷達波束（半徑方向漸層 + 發光邊緣），
  // 比單一細線更有「雷達掃描」的感覺；halfWidthDeg 可調整扇形寬度。
  function aimSector(angle,rgb,halfWidthDeg){
    var brg=((off-angle)%360+360)%360;
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
  if(d.servo.calibrated&&d.servo.declination_deg!=null){
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
      clientSatelliteGradeCount(d.client),d.client.hdop,clientSatelliteWord(d.client));
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
  if(d.server.fix&&d.client.fix&&dist!=null){
    x.fillStyle='#fff';x.font='bold 12px system-ui';x.textAlign='left';
    x.fillText('站\u2194Surfer '+dist.toFixed(0)+' m',10,H-12);}
  x.fillStyle='rgba(230,237,243,.7)';x.font='9px system-ui';x.textAlign='right';
  x.fillText('© OpenStreetMap',W-6,12);x.textAlign='left';
}

showPage('radar');
refresh();refreshStatus();
// ---- camera compass calibration ----
$('btnCompassCal').onclick=function(){
  var text=$('compassBearing').value.trim();
  var bearing=Number(text);
  if(!text||!Number.isFinite(bearing)||bearing<0||bearing>=360){
    toast('請輸入 0 到未滿 360 度，北 0°、東 90°');return;
  }
  controlAction('/api/track/calibrate?bearing='+encodeURIComponent(text));
};

refreshStatus();          // 提醒不要等到第一個 3 秒週期才出現
setInterval(refresh,1000);
setInterval(refreshStatus,3000);
loadSpeedSetting();
loadPredictionSetting();
</script>
</body>
</html>
)rawlit";
