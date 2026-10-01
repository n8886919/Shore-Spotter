// Real outer-ring pointer handlers and HTTP queue; no hardware I/O.
const assert=require('node:assert/strict');
const {createPage,settle}=require('./test_motion_ui.js');
async function setup(){
 const p=createPage();await settle();p.c.getTile=()=>({_ok:false});
 Object.assign(p.track.station,{fix:true,lat:24,lon:121});
 Object.assign(p.track.client,{fix:true,lat:24.0001,lon:121.001});
 Object.assign(p.track.servo,{mode:'manual',angle:90,target:90,mount_offset_deg:90,declination_deg:0});
 await p.c.refresh();p.c.setView('radar');return p;
}
function event(p,name,angle,id=1){
 const point=p.c.ringPoint(p.c.servoRingGeometry(),angle);
 p.elements.servoRing.handlers[name].call(p.elements.servoRing,{clientX:point.x,clientY:point.y,pointerId:id,button:0,preventDefault(){}});
}
(async()=>{
 const p=await setup(),g=p.c.servoRingGeometry();
 assert.equal(g.r,p.c.radarGeometry().r);
 const centre=p.c.ringPoint(g,90);assert(centre.y<g.cy&&Math.abs(centre.x-g.cx)<1e-7);
 assert.equal(p.elements.servoRing.style.display,'');
 // UI control is always the upper semicircle, independent of GPS/calibration/view.
 for(const view of ['radar','map'])for(const offset of [0,90,230])for(const decl of [-5,3]){
  Object.assign(p.track.servo,{mount_offset_deg:offset,declination_deg:decl});
  await p.c.refresh();p.c.setView(view);
  const fixed=p.c.servoRingGeometry();
  for(const [angle,x,y] of [[0,fixed.cx-fixed.r,fixed.cy],[90,fixed.cx,fixed.cy-fixed.r],[180,fixed.cx+fixed.r,fixed.cy]]){
   const point=p.c.ringPoint(fixed,angle);assert(Math.abs(point.x-x)<1e-7&&Math.abs(point.y-y)<1e-7);
  }
 }
 p.c.setView('radar');
 event(p,'pointerdown',90);event(p,'pointermove',120);await settle();
 assert.equal(p.posts.length,0);assert.equal(p.elements.sld.value,'120');
 assert.equal(p.elements.angTxt.textContent,'120');
 // Polls cannot overwrite the active preview. The release uses the existing inversion.
 await p.c.refresh();assert.equal(p.elements.sld.value,'120');
 event(p,'pointerup',120);await settle();
 assert.equal(p.posts.length,1);assert.equal(p.posts[0].pathname,'/api/servo');assert.equal(p.posts[0].searchParams.get('angle'),'60');
 for(const cancel of ['pointercancel','lostpointercapture']){
  event(p,'pointerdown',130);event(p,cancel,130);event(p,'pointerup',130);await settle();assert.equal(p.posts.length,1);
 }
 // A second touch cancels, instead of leaving a stale release to move the camera.
 event(p,'pointerdown',20);event(p,'pointerdown',80,2);event(p,'pointerup',20);await settle();assert.equal(p.posts.length,1);
 event(p,'pointerdown',40);await p.c.controlAction('/api/servo/mode?mode=uart');event(p,'pointerup',40);await settle();
 assert.equal(p.elements.servoRing.style.display,'');
 assert.equal(p.elements.servoRingTrack.style.display,'none');
 assert.equal(p.posts.filter(u=>u.pathname==='/api/servo').length,1);
 event(p,'pointerdown',70);event(p,'pointerup',70);await settle();assert.equal(p.posts.length,2);
 p.track.servo.gps_available=true;await p.c.refresh();
 await p.c.controlAction('/api/servo/mode?mode=gps');
 assert.equal(p.elements.servoRing.style.display,'');assert(p.elements.sld.disabled);
 assert.equal(p.elements.servoRingTrack.style.display,'none');
 p.track.servo.angle=55;p.track.servo.target=10;await p.c.refresh();
 assert.equal(p.elements.angTxt.textContent,125);
 const count=p.posts.length;event(p,'pointerdown',90);event(p,'pointerup',90);await settle();assert.equal(p.posts.length,count);
 await p.c.controlAction('/api/servo/mode?mode=manual');
 assert.equal(p.elements.servoRing.style.display,'');assert(!p.elements.sld.disabled);
 assert.equal(p.elements.servoRingTrack.style.display,'');
 console.log('PASS ring: fixed upper semicircle, UART/GPS keep noninteractive angle handle but hide track, Manual restores track, local preview and gesture isolation');
 const map=await setup();map.c.setView('map');
 for(const angle of [0,180]){event(map,'pointerdown',angle);event(map,'pointerup',angle);await settle();}
 assert.deepEqual(map.posts.map(u=>Number(u.searchParams.get('angle'))),[180,0]);
 map.c.setView('radar');event(map,'pointerdown',35);map.track.station.fix=map.track.client.fix=false;
 await map.c.refresh();event(map,'pointerup',35);await settle();
 assert.equal(map.posts.length,3);assert.equal(map.posts[2].searchParams.get('angle'),'145');
 assert.equal(map.c.viewMode,'radar');assert.equal(map.elements.mGps.disabled,true);
 event(map,'pointerdown',50);map.c.setView('map');event(map,'pointerup',50);await settle();
 assert.equal(map.posts.length,3);
 console.log('PASS ring: angle label follows preview, map endpoints, manual control without GPS, explicit view switch cancels gesture');
 const resize=[],viewport={height:620,addEventListener(k,fn){resize.push(fn);}};
 const mobile=createPage({visualViewport:viewport});await settle();
 assert.equal(mobile.viewportWrites['--app-height'],'620px');
 viewport.height=500;resize[0]();assert.equal(mobile.viewportWrites['--app-height'],'500px');
 console.log('PASS mobile: body follows the visible viewport when browser chrome changes');
})().catch(e=>{console.error(e);process.exitCode=1;});
