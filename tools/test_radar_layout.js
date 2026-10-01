// Actual UI layout and status logic, without hardware I/O.
const assert=require('node:assert/strict');
const {createPage,settle}=require('./test_motion_ui.js');
(async()=>{
 let now=1000;
 const p=createPage({context:{performance:{now:()=>now}}});await settle();
 p.c.getTile=()=>({_ok:false});
 for(const [w,h] of [[320,640],[390,844],[844,390],[731,698],[500,500],[320,320]]){
  Object.assign(p.elements.radar,{clientWidth:w,clientHeight:h});p.c.fitCanvas();
  const g=p.c.radarGeometry(),dx=Math.max(g.lightsX-g.cx,0,g.cx-g.lightsX-124),dy=Math.max(g.lightsY-g.cy,0,g.cy-g.lightsY-114);
  assert(Math.hypot(dx,dy)>=g.r+23.9,`${w}x${h}: lamps clear ring and handle`);
  assert(g.cx-g.r>=23.9&&g.cy-g.r>=23.9&&g.cx+g.r<=w-23.9&&g.cy+g.r<=h-11.9);
  assert(g.lightsX>=0&&g.lightsX+124<=w&&g.lightsY>=64&&g.lightsY+114<=h-49);
  p.c.setView('map');assert.equal(p.elements.signalPanel.style.left,'8px');
  p.c.setView('radar');
 }
 Object.assign(p.elements.radar,{clientWidth:731,clientHeight:698});p.c.fitCanvas();
 assert(p.c.radarGeometry().r>(698-49)/2-30,'full canvas yields a larger circle than the former footer layout');
 const lower=p.c.radarGeometry();
 assert.equal(Math.round(698-49-(lower.cy+lower.r-44+4)),3,'lower compass label baseline sits just above collapsed alerts');
 for(const [s,c,l] of [[true,false,true],[false,true,true],[false,false,false],[true,true,false]]){
  p.track.station.fix=s;p.track.client.fix=c;p.track.linked=l;await p.c.refresh();
  assert.equal(p.elements.signalStation.attrs['data-state'],s?'ok':'bad');
  assert.equal(p.elements.signalClient.attrs['data-state'],c?'ok':'bad');
  assert.equal(p.elements.signalLora.attrs['data-state'],l?'ok':'bad');
 }
 now+=3001;p.c.renderSignalLights();
 for(const id of ['signalStation','signalClient','signalLora']){
  assert.equal(p.elements[id].attrs['data-state'],'unknown');assert.equal(p.elements[id+'Text'].textContent,'資料過期');
 }
 p.track.station.fix=p.track.client.fix=p.track.linked=true;await p.c.refresh();
 assert.equal(p.elements.signalLora.attrs['data-state'],'ok');
 p.track.lora_fps_10s=1.7;await p.c.refresh();assert.equal(p.elements.signalLoraText.textContent,'1.7 FPS · 10s');
 p.track.linked=false;p.track.lora_fps_10s=0.4;await p.c.refresh();
 assert.equal(p.elements.signalLora.attrs['data-state'],'bad');assert.equal(p.elements.signalLoraText.textContent,'0.4 FPS · 10s');
 // RF telemetry may arrive before the first GPS DATA; the radio light must not
 // label this as a missing link or make the GPS/track path usable.
 p.track.rf_alive=true;p.track.client.fix=false;p.track.lora_fps_10s=0;await p.c.refresh();
 assert.equal(p.elements.signalLora.attrs['data-state'],'ok');
 assert.equal(p.elements.signalClient.attrs['data-state'],'bad');
 assert.match(p.elements.signalLora.title,/收到遙測，等待 DATA/);
 assert.match(p.elements.signalLora.attrs['aria-label'],/收到遙測，等待 DATA/);
 assert.equal(p.elements.signalLoraText.textContent,'0.0 FPS · 10s');
 assert.equal(p.elements.cLink.textContent,'收到遙測，等待 DATA');
 p.track.rf_alive=false;p.track.linked=true;await p.c.refresh();
 assert.equal(p.elements.signalLora.attrs['data-state'],'bad');
 delete p.track.rf_alive;await p.c.refresh();
 assert.equal(p.elements.signalLora.attrs['data-state'],'ok','older APIs fall back to DATA linked');
 now+=3001;p.c.renderSignalLights();assert.equal(p.elements.signalLoraText.textContent,'資料過期');
 p.c.showPage('info');await p.c.refresh();
 assert.equal(p.viewportWrites['--alert-list-height'],'40dvh','background tracking polls preserve Info drawer height');
 p.c.showPage('radar');assert.notEqual(p.viewportWrites['--alert-list-height'],'40dvh');
 console.log('PASS radar layout: portrait/landscape/square lamps avoid ring and handle, enlarged circle, map lamps fixed left; independent GPS/LoRa and stale-data recovery');
})().catch(e=>{console.error(e);process.exitCode=1;});
