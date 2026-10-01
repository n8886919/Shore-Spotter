// Exercise actual map drawing with one/two live fixes and distant old history.
const assert=require('node:assert/strict');
const {createPage,settle}=require('./test_motion_ui.js');
(async()=>{
 const p=createPage();await settle();
 p.c.getTile=()=>({_ok:false});p.c.drawGpsTag=()=>{};
 p.c.hist=[{t:Date.now(),lat:20,lon:118,sfix:1,slat:21,slon:119}];
 for(const [stationFix,clientFix,lat,lon] of [[true,false,24,121],[false,true,24.2,121.4],[true,true,24.1,121.2]]){
  const data={...p.track,station:{...p.track.station,fix:stationFix,lat:24,lon:121},client:{...p.track.client,fix:clientFix,lat:24.2,lon:121.4}};
  let checks=0;
  p.c.drawPath=(context,points,plot)=>{
   const [x,y]=plot({lat,lon});
   assert(Math.abs(x-p.elements.radar.width/2)<1e-7);
   assert(Math.abs(y-p.elements.radar.height/2)<1e-7);checks++;
  };
  p.c.drawMap(data,null);assert.equal(checks,2);
 }
 console.log('PASS actual map: Station-only, Surfer-only and live midpoint; historical path does not move centre');
})().catch(e=>{console.error(e);process.exitCode=1;});
(async()=>{
 const p=createPage();await settle();p.c.getTile=()=>({_ok:false});p.c.drawGpsTag=()=>{};p.c.hist=[];
 const canvas=p.elements.radar.getContext('2d'),labels={},markers=[];
 canvas.fillText=(text,x,y)=>{if(['N','E','S','W'].includes(text))labels[text]={x,y};};
 canvas.arc=(x,y,r)=>{if(r===6)markers.push({x,y});};
 Object.assign(p.track.station,{fix:true,lat:24,lon:121});
 Object.assign(p.track.client,{fix:true,lat:24,lon:121.001});
 Object.assign(p.track.servo,{mount_offset_deg:90,declination_deg:0,calibrated:true});
 p.c.last.track=p.track;p.c.viewMode='radar';p.c.drawRadar(p.track,100);
 const {cx,cy}=p.c.radarGeometry();
 assert(Math.abs(labels.E.x-cx)<1e-8&&labels.E.y<cy);
 assert(labels.N.x<cx&&Math.abs(labels.N.y-cy)<5);
 assert(Math.abs(markers.at(-1).x-cx)<1e-6&&markers.at(-1).y<cy);
 for(const [s,c] of [[true,false],[false,true]]){
  p.track.station.fix=s;p.track.client.fix=c;p.c.viewMode='radar';
  await p.c.refresh();assert.equal(p.c.viewMode,'radar');assert(!p.elements.vRadar.disabled);
  markers.length=0;p.c.drawRadar(p.track,null);
  if(c)assert(Math.abs(markers.at(-1).x-cx)<1e-6&&Math.abs(markers.at(-1).y-cy)<1e-6);
 }
 for(const [s,c] of [[false,false]]){
  p.track.station.fix=s;p.track.client.fix=c;p.c.viewMode='radar';
  await p.c.refresh();assert.equal(p.c.viewMode,'radar');assert(!p.elements.vRadar.disabled);
  markers.length=0;p.c.drawRadar(p.track,null);assert.equal(markers.length,0);
  p.c.setView('map');assert.equal(p.c.viewMode,'map');
  p.c.setView('radar');assert.equal(p.c.viewMode,'radar');
 }
 p.track.station.fix=p.track.client.fix=true;await p.c.refresh();
 assert(!p.elements.vRadar.disabled);assert.equal(p.c.viewMode,'radar');
 console.log('PASS radar: East up/North left; single fix centred radar; no fixes keep radar without fabricated positions; map/radar freely selectable');
})().catch(e=>{console.error(e);process.exitCode=1;});
