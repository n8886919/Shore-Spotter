'use strict';
(() => {
  const $ = id => document.getElementById(id);
  const esc = text => String(text).replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
  const badge = (text, level = 'neutral') => `<span class="badge ${level}">${esc(text)}</span>`;
  const small = text => `<small>${esc(text)}</small>`;
  const row = (label, value) => `<div class="row"><span class="label">${esc(label)}</span><div class="value">${value}</div></div>`;
  const comparison = (label, station, client) => `<tr><th scope="row">${esc(label)}</th><td>${station}</td><td>${client}</td></tr>`;
  let scenario = 'ready', source = 'gnss', speed = 30, angle = 179.5, bearing = 90, pending = null, lastCommand = '尚無待確認命令', mode = 'manual';
  let drag = null;
  let manual = null, toastTimer;
  const pulse = deg => Math.round(500 + deg * 2000 / 359);
  const boardCommand = deg => '#00P' + String(pulse(deg)).padStart(4, '0') + '!';
  function toast(message) {
    $('toast').textContent = message; $('toast').classList.add('show');
    clearTimeout(toastTimer); toastTimer = setTimeout(() => $('toast').classList.remove('show'), 3200);
  }
  function fixture() {
    const test = scenario === 'test', ready = scenario === 'ready' || test, offline = scenario === 'offline', stale = scenario === 'stale';
    const noFix = scenario === 'no_fix';
    return {ready, test, offline, stale, noFix, sdError:scenario === 'sd_error', state:test?'Test':ready?'Ready':'Tracking',
      fix:!ready && !offline && !stale && !noFix,
      stateAge:offline?'121 s':'12 s', dataAge:ready?'--（本次待機未送 DATA）':offline?'130 s':stale?'6.4 s':noFix?'--（尚無有效 DATA）':'0.4 s',
      diagAge:ready?'5.5 小時前':offline?'125 s 前':stale?'65 s 前':'4 s 前',
      fps:ready||offline||stale||noFix?'0.0':'1.8'};
  }
  function render() {
    const s = fixture(), phone = source !== 'gnss';
    const stationCoord = manual && source === 'manual' ? `${manual.lat.toFixed(5)}, ${manual.lon.toFixed(5)}` : phone?'24.00004, 121.00002':'24.00000, 121.00000';
    const stationState = phone?'位置快照有效':'定位有效';
    const clientState = s.test?'RF 測試 · GPS 已關閉':s.ready?'待機 · GPS 已關閉':s.offline?'資料過期 · RF 失聯':s.stale?'位置資料過期':s.noFix?'無有效定位':'定位有效';
    const clientLevel = s.ready?'neutral':s.fix?'ok':s.offline?'bad':'warn';
    $('positionRows').innerHTML = [
      comparison('衛星／狀態', (phone?'--（手機來源）':'10 顆')+small('')+badge(stationState,'ok'), (s.ready?'--':s.stale?'≥8 顆 · 舊':s.offline?'--':s.noFix?'4 顆':'≥8 顆')+small('')+badge(clientState,clientLevel)),
      comparison('HDOP', phone?'--（手機來源）':'1.2', s.ready||s.offline?'--':s.stale?'--（舊值 1.4）':s.noFix?'3.8':'1.4'),
      comparison('座標', esc(stationCoord), s.fix?'24.00110, 121.00165':'--'),
      comparison('位置更新', phone?'20 s 前'+small('手機位置快照'):'0.5 s 前'+small('GNSS 更新'), s.ready?'GPS 已關閉':esc(s.dataAge)+small('Station 接受 DATA 後經過時間')),
    ].join('');
    $('sourceNote').textContent = 'Demo 切換顯示；不要求手機定位權限';
    $('stationSource').setAttribute('aria-pressed',String(!phone)); $('phoneSource').setAttribute('aria-pressed',String(phone));
    $('positionMeta').innerHTML = [
      row('手機回報精度', phone?`${source==='manual'?manual.accuracy.toFixed(1):'8'} m${small('估計誤差半徑')}`:'--'),
      row('岸端 30 秒散布', phone?'--（手機位置來源）':`1.8 m RMS${small('GNSS 30 秒座標散布')}`),
      row('磁偏角補償', `−4.3°${small('WMM2025 · 位置／日期有效')}`),
    ].join('');
    $('gnssDetails').innerHTML = [
      row('Station GNSS 串流', `epoch 2.0 Hz · RMC 2.0 · GGA 2.0${small('接收端實際觀察值')}`),
      row('Client GNSS 診斷時間', esc(s.diagAge)),
      row('Client 串流狀態', s.ready?`GPS 已關閉${small('前次 GNSS 快照不代表目前狀態')}`:s.offline||s.stale?'快照過期':s.noFix?'NMEA 有更新，沒有 fix':'有效 epoch 更新'),
      row('Client epoch 間隔', s.ready||s.stale||s.offline?'--（診斷過期）':'500 ms'),
      row('Client checksum／積壓', `累計 169／0${small('歷史快照；需看新增量與診斷時間')}`),
    ].join('');
    $('stationRows').innerHTML = [
      row('溫度／濕度', '31°C／60%'), row('韌體版本', '0.6-dev'), row('運行時間', '8 小時 38 分'),
      row('SD 記錄', s.sdError?badge('寫入異常','bad')+small('寫入錯誤 3 · 快照遺失 2'):badge('正在記錄','ok')),
      row('Servo 控制來源', mode==='manual'?'手動':'GPS'), row('指令角度 → 目標', `${angle.toFixed(1)}° → ${angle.toFixed(1)}°${small('指令值；無實際角度回授')}`),
      row('控制狀態', mode==='manual'?'手動控制':s.fix?badge('定位條件符合（Demo）','ok'):badge('保持：Client 位置無效','warn')),
      row('控制板輸出', `串口 · 9600 baud${small('Youmi 35 kg · 通道／實機整合待驗')}`),
    ].join('');
    $('speedValue').innerHTML = `${speed}°／秒 <span>修改</span>`;
    $('stationDetails').innerHTML = [
      row('boot ID／重啟原因','DEMO-S001／上電'), row('控制板規格','50 Hz · 500–2500 μs'),
      row('示意串口命令',`<code>${esc(boardCommand(angle))}</code>${small('Demo 計算，未送出；M0 僅為示意')}`),
      row('控制板回覆','未知（文件未提供 ACK）'), row('行程與中位',`0–359°／179.5°${small('線性對應為待驗假設')}`),
      row('指南針校正讀數',`${bearing}°${small('一般磁針讀數，與 Servo 角度不同')}`),
      row('SD 寫入錯誤／快照遺失', s.sdError?'3／2':'0／0'),
    ].join('');
    $('surferRows').innerHTML = [
      row('電池',`4.08 V · 約 93%${small('電壓估算百分比，非校準 SOC')}`),
      row('USB 供電','未接 USB'+small('無線充電狀態無法由 USB VBUS 判定')),
      row('裝置狀態',badge(s.state,s.offline?'neutral':'ok')+small(s.test?'無 GPS 的 RF probe 診斷':s.ready?'多數時間睡眠 · 約每 30 秒交換指令':'GNSS 已開啟')),
      row('距上次 STATE',esc(s.stateAge)+(s.offline?small('沒有近期狀態回報'):'')),
      row('距上次 DATA',esc(s.dataAge)),
      row('DATA 接收 FPS',`${s.fps} FPS · 10 s`+(s.ready?small('待機不送定位 DATA'):'')),
      row('RSSI／SNR',s.offline?'--（RF 資料過期）':'−26 dBm／11.5 dB'+small('Station 最近接受的 Client RF 封包')),
      row('漏失 DATA',s.ready||s.noFix?'--（尚無 DATA 樣本）':s.offline?'2（歷史序號缺口）':'0（DATA 序號缺口）'),
      row('距離／方位',s.fix?'205 m／54°':'--（需要兩端有效位置）'),
      row('命令', pending?badge('等待 Client STATE 確認','warn'):esc(lastCommand)),
    ].join('');
    $('surferDetails').innerHTML = [
      row('boot ID／運行時間','DEMO-C001／4 小時 12 分'), row('RF 組別','B · 923.8 MHz · SF9'),
      row('最後 GNSS／DIAG 快照',esc(s.diagAge)),row('TX 錯誤／延後時槽','0／153（歷史快照）'),
      row('速度資料',s.fix?'有效（模擬）':'--'),row('LoRa RF 測試',scenario==='test'?'模擬 probe 觀察':'尚未開始'),
    ].join('');
    $('commandNote').textContent = pending?'已提出「'+pending.label+'」。Demo 請按上方「模擬收到 STATE」確認；實機 Ready 通常需等約一輪 30 秒窗口。':'啟動／回待機只改 Client；不會切換 Station Servo 模式。';
    ['clientStart','clientStop','clientTest'].forEach(id=>$(id).disabled=s.offline||!!pending);
    $('receiveState').disabled=!pending;
    renderRadar();
  }
  const integer = (text,min,max) => /^[0-9]+$/.test(text) && Number(text)>=min && Number(text)<=max;
  ['compassBearing','speedInput'].forEach(id=>{
    $(id).addEventListener('beforeinput',event=>{if(event.data && !/^[0-9]+$/.test(event.data)){event.preventDefault();toast('請輸入整數數字');}});
    $(id).addEventListener('paste',event=>{const text=event.clipboardData.getData('text');if(!/^[0-9]+$/.test(text)){event.preventDefault();toast('只接受整數數字');}});
  });
  $('scenario').addEventListener('change',()=>{cancelDrag();scenario=$('scenario').value;pending=null;lastCommand='尚無待確認命令';render();});
  $('stationSource').addEventListener('click',()=>{source='gnss';render();toast('Demo：使用 Station GPS');});
  $('phoneSource').addEventListener('click',()=>{source='phone';render();toast('Demo：使用手機位置快照，未要求權限');});
  $('manualPosition').addEventListener('click',()=>{
    const raw=[$('latitude').value,$('longitude').value,$('accuracy').value];
    const values=raw.map(Number),[lat,lon,accuracy]=values;
    if(raw.some(v=>!v.trim())||values.some(v=>!Number.isFinite(v))||Math.abs(lat)>90||Math.abs(lon)>180||accuracy<0){$('manualError').textContent='請輸入有效座標與非負誤差半徑。';return;}
    manual={lat,lon,accuracy};source='manual';$('manualError').textContent='';render();toast('Demo：已套用手動座標');
  });
  $('speedValue').addEventListener('click',()=>{$('speedInput').value=String(speed);$('speedError').textContent='';$('speedDialog').showModal();$('speedInput').focus();$('speedInput').select();});
  $('cancelSpeed').addEventListener('click',()=>$('speedDialog').close());
  $('speedForm').addEventListener('submit',event=>{event.preventDefault();const value=$('speedInput').value;if(!integer(value,1,90)){$('speedError').textContent='請輸入 1–90 的整數。';return;}speed=Number(value);$('speedDialog').close();render();toast('Demo：已儲存 '+speed+'°／秒（本頁有效）');});
  $('center').addEventListener('click',()=>{cancelDrag();mode='manual';angle=179.5;render();toast('Demo：指令已置中 179.5°；實機中位待驗');});
  $('compassForm').addEventListener('submit',event=>{event.preventDefault();const value=$('compassBearing').value;if(!integer(value,0,359)){$('compassError').textContent='請輸入 0–359 的整數。';return;}bearing=Number(value);$('compassError').textContent='';render();toast('Demo：指南針校正讀數 '+bearing+'°');});
  function command(kind,label){pending={kind,label};render();toast('Demo：等待 Client STATE 確認');}
  $('clientStart').addEventListener('click',()=>command('start','啟動 GPS'));
  $('clientStop').addEventListener('click',()=>command('stop','回待機'));
  $('clientTest').addEventListener('click',()=>command('test','LoRa RF 測試'));
  $('receiveState').addEventListener('click',()=>{
    const kind=pending.kind;lastCommand='Client 已確認：'+pending.label;pending=null;
    scenario=kind==='start'?'no_fix':kind==='stop'?'ready':'test';
    $('scenario').value=scenario;
    render();toast('Demo：已收到匹配 STATE');
  });
  const point = (deg,r=250) => ({x:300-Math.sin(deg*Math.PI/180)*r,y:300+Math.cos(deg*Math.PI/180)*r});
  $('circleTicks').innerHTML = Array.from({length:12},(_,i)=>{
    const deg=i*30,a=point(deg,238),b=point(deg,245),t=point(deg,275);
    return `<path d="M${a.x} ${a.y}L${b.x} ${b.y}"/><text x="${t.x}" y="${t.y+4}">${deg===0?'0° / 359°':deg+'°'}</text>`;
  }).join('');
  function renderRadar(preview=angle) {
    const s=fixture(),p=point(preview);
    const ctm=$('radarSvg').getScreenCTM(),scale=ctm?Math.hypot(ctm.a,ctm.b):1;
    if(scale>0){
      $('circleHandle').querySelector('circle').setAttribute('r',String(22/scale));
      $('circleValue').style.fontSize=(13/scale)+'px';
      $('circleHandle').querySelector('path').setAttribute('d',`M${-7/scale} ${10/scale}H${7/scale}M${-7/scale} ${13/scale}H${7/scale}`);
      $('ringHit').setAttribute('stroke-width',String(44/scale));
      $('circleTicks').querySelectorAll('text').forEach(el=>el.style.fontSize=(11/scale)+'px');
      $('radarSvg').querySelectorAll('.directions text').forEach(el=>el.style.fontSize=(14/scale)+'px');
    }
    $('ringHit').style.pointerEvents=mode==='manual'?'stroke':'none';
    $('circleHandle').setAttribute('transform',`translate(${p.x} ${p.y})`);
    $('circleValue').textContent=Number.isInteger(preview)?preview+'°':preview.toFixed(1)+'°';
    $('previewLine').setAttribute('d',`M300 300L${p.x} ${p.y}`);
    $('cameraBeam').setAttribute('transform',`rotate(${-(angle-179.5)} 300 300)`);
    $('radarDemo').classList.toggle('automatic',mode!=='manual');
    $('surferPoint').setAttribute('visibility',s.fix?'visible':'hidden');
    $('circleKeyboard').disabled=mode!=='manual';$('circleKeyboard').value=String(Math.round(preview));
    $('radarAngleNote').textContent=drag?`預覽 ${preview.toFixed(0)}° · 原指令 ${angle.toFixed(1)}°`:`指令角度 ${angle.toFixed(1)}°`;
    ['manual','gps'].forEach(m=>$('mode'+m[0].toUpperCase()+m.slice(1)).setAttribute('aria-pressed',String(mode===m)));
    $('modeGps').disabled=!s.fix;
    $('radarSignals').innerHTML=[['Station GPS','位置有效','ok'],['Client GPS',s.ready?'GPS 已關閉':s.fix?'位置有效':'無新鮮位置',s.ready?'neutral':s.fix?'ok':'bad'],['LoRa',`${s.fps} FPS · 10 s`,s.offline?'bad':'ok']].map(([name,text,level])=>`<div><i class="signal-dot ${level}"></i><span>${name}<small>${text}</small></span></div>`).join('');
  }
  function cancelDrag(){drag=null;renderRadar();}
  function cursorAngle(event){
    const svg=$('radarSvg'),p=new DOMPoint(event.clientX,event.clientY).matrixTransform(svg.getScreenCTM().inverse());
    return (Math.atan2(300-p.x,p.y-300)*180/Math.PI+360)%360;
  }
  function beginDrag(event){
    if(mode!=='manual'||event.button!==0||drag)return;
    event.preventDefault();const target=event.currentTarget,deg=cursorAngle(event);
    drag={id:event.pointerId,element:target,last:deg,preview:target.id==='circleHandle'?angle:Math.min(359,Math.round(deg))};
    target.setPointerCapture(event.pointerId);renderRadar(drag.preview);
  }
  function moveDrag(event){
    if(!drag||event.pointerId!==drag.id)return;
    const next=cursorAngle(event),delta=((next-drag.last+540)%360)-180;
    drag.preview=Math.max(0,Math.min(359,drag.preview+delta));drag.last=next;renderRadar(Math.round(drag.preview));
  }
  function endDrag(event){if(!drag||event.pointerId!==drag.id)return;angle=Math.round(drag.preview);drag=null;render();toast('Demo：套用指令 '+angle+'°，未送實機');}
  ['ringHit','circleHandle'].forEach(id=>{const el=$(id);el.addEventListener('pointerdown',beginDrag);el.addEventListener('pointermove',moveDrag);el.addEventListener('pointerup',endDrag);el.addEventListener('pointercancel',cancelDrag);el.addEventListener('lostpointercapture',()=>{if(drag)cancelDrag();});});
  $('circleKeyboard').addEventListener('input',()=>renderRadar(Number($('circleKeyboard').value)));
  $('circleKeyboard').addEventListener('change',()=>{angle=Number($('circleKeyboard').value);render();});
  $('returnRadar').addEventListener('click',()=>{cancelDrag();$('infoPage').hidden=true;$('radarDemo').hidden=false;document.body.classList.add('radar-view');renderRadar();});
  $('showInfo').addEventListener('click',()=>{cancelDrag();$('radarDemo').hidden=true;$('infoPage').hidden=false;document.body.classList.remove('radar-view');render();});
  ['manual','gps'].forEach(m=>$('mode'+m[0].toUpperCase()+m.slice(1)).addEventListener('click',()=>{cancelDrag();mode=m;render();}));
  window.addEventListener('resize',()=>{cancelDrag();});
  $('radarSvg').addEventListener('pointerdown',event=>{if(drag&&event.pointerId!==drag.id)cancelDrag();});
  render();
})();
