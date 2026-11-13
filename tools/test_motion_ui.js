// Full embedded UI simulation: speed/prediction auto-save and the GPS selection gate.
const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict');
const source=fs.readFileSync(process.env.SHORE_WEB_UI_TEST_SOURCE||'include/web_ui.h','utf8');
const html=source.includes('R"')?source.match(/R"(\w+)\(([\s\S]*?)\)\1"/)[2]:source;
const script=html.match(/<script>([\s\S]*?)<\/script>/)[1];
const examples=[...fs.readFileSync('docs/interface.md','utf8').matchAll(/```json\s*\n([\s\S]*?)\n```/g)].map(m=>JSON.parse(m[1]));
const trackExample=examples.find(x=>x.station&&x.client&&x.servo);
function createPage(options={}){
const track=structuredClone(trackExample);
Object.assign(track.servo,{mode:'manual',source:'manual',angle:179.5,target:179.5,moving:false,speed_limit_deg_s:30,
  output_ready:true,profile:'youmi-359-uart-v1',controller:{channel:0,baud:9600,tx_gpio:43,pulse_us:1500,last_frame:'#00P1500!',tx_commands:1,tx_deferred:0,tx_errors:0},gps_available:false,gps_usable:false,calibration_ready:true,motion_fault:false,control_boot_id:1,control_epoch:10,command_seq:0,clock_ms:1000,
  prediction_enabled:options.enabled!==false,prediction_alpha:options.enabled===false?0:1});
if(options.unsupported){delete track.servo.prediction_enabled;delete track.servo.prediction_alpha;}
const settings={speed:30,min_speed:1,max_speed:90,default_speed:30};
const prediction={enabled:options.enabled!==false,alpha:options.enabled===false?0:1,default_enabled:true};
const ids=[...html.matchAll(/\bid="([^"]+)"/g)].map(m=>m[1]);assert.equal(ids.length,new Set(ids).size);
const canvas=new Proxy({measureText:text=>({width:String(text).length*6}),createRadialGradient:()=>({addColorStop(){}}),createLinearGradient:()=>({addColorStop(){}})},
 {get:(x,k)=>k in x?x[k]:(()=>{})});
const elements={};
for(const id of ids){let value='30';elements[id]={get value(){return value;},set value(v){value=String(v);},style:{},
 disabled:new RegExp('<[^>]+id="'+id+'"[^>]*\\bdisabled').test(html),checked:false,indeterminate:false,
 textContent:'',open:false,showModal(){this.open=true;},close(){this.open=false;},focus(){},select(){},clientWidth:500,clientHeight:500,getContext:()=>canvas,classList:{toggle(){},add(){},remove(){}},
 attrs:{},handlers:{},setAttribute(k,v){this.attrs[k]=String(v);},getBoundingClientRect(){return {left:0,top:0,width:this.clientWidth,height:this.clientHeight};},
 setPointerCapture(){},addEventListener(k,fn){this.handlers[k]=fn;}};}
const posts=[],gates=[];
const api={failSave:false,failPrediction:0,predictionNetworkError:false,invalidPredictionPost:false,
  failCalibration:0,calibrationNetworkError:false,invalidCalibrationPost:false,failCenter:0,invalidCenter:false,
  failPredictionGet:options.unsupported?404:options.failPredictionGet||0};
function holdNext(path,method='GET'){
 let release;const promise=new Promise(resolve=>{release=resolve;});
 const gate={path,method,promise,release};gates.push(gate);return gate;
}
function setPrediction(enabled){
 prediction.enabled=enabled;prediction.alpha=enabled?1:0;
 track.servo.prediction_enabled=enabled;track.servo.prediction_alpha=prediction.alpha;
}
function contextFields(){return {control_boot_id:track.servo.control_boot_id,control_epoch:track.servo.control_epoch,command_seq:track.servo.command_seq,clock_ms:track.servo.clock_ms};}
const documentEvents={},windowEvents={};
const c={document:{getElementById:id=>elements[id],activeElement:null,visibilityState:'visible',
 addEventListener(name,fn){(documentEvents[name]||= []).push(fn);}},
 window:{addEventListener(name,fn){(windowEvents[name]||= []).push(fn);}},console,Promise,Date,Math,Number,AbortController,performance:{now:()=>1000},
 setTimeout:()=>1,clearTimeout(){},setInterval:()=>1,clearInterval(){},
 fetch(url,options={}){
  const u=new URL(url,'http://test');let payload={},ok=true,status=200;
  if(typeof c.debugFetch==='function'&&(u.pathname==='/api/debug'||u.pathname==='/api/log'))return c.debugFetch(url,options);
  if(options.method==='POST'){
   posts.push(u);assert.equal(Number(u.searchParams.get('epoch')),track.servo.control_epoch);
   assert(Number(u.searchParams.get('seq'))>track.servo.command_seq);assert.equal(Number(u.searchParams.get('stamp')),track.servo.clock_ms);
   track.servo.command_seq=Number(u.searchParams.get('seq'));
   if(u.pathname==='/api/servo/settings'){
    assert.deepEqual([...u.searchParams.keys()].sort(),['epoch','seq','speed','stamp']);
    if(api.failSave){ok=false;payload={ok:false,error:'save failed'};}
    else {settings.speed=Number(u.searchParams.get('speed'));track.servo.speed_limit_deg_s=settings.speed;payload={ok:true,...settings,...contextFields()};}
   }else if(u.pathname==='/api/track/prediction'){
    assert.deepEqual([...u.searchParams.keys()].sort(),['enabled','epoch','seq','stamp']);
    assert(['0','1'].includes(u.searchParams.get('enabled')));
    if(api.predictionNetworkError)return Promise.reject(new Error('network failure'));
    if(api.failPrediction){ok=false;status=api.failPrediction;payload={ok:false,error:'prediction rejected'};}
    else if(api.invalidPredictionPost)payload={ok:true,enabled:'true'};
    else {setPrediction(u.searchParams.get('enabled')==='1');payload={ok:true,...prediction,...contextFields()};}
   }else if(u.pathname==='/api/track/calibrate'){
    assert.deepEqual([...u.searchParams.keys()].sort(),['bearing','epoch','physical_center','seq','stamp']);
    if(api.calibrationNetworkError)return Promise.reject(new Error('network failure'));
    if(api.failCalibration){ok=false;status=api.failCalibration;payload={ok:false,error:'calibration rejected'};}
    else if(api.invalidCalibrationPost)payload={ok:true};
    else {
     assert.equal(u.searchParams.get('physical_center'),'1');
     track.servo.angle=track.servo.target=179.5;
     ++track.servo.control_epoch;track.servo.command_seq=0;
     const bearing=Number(u.searchParams.get('bearing'));track.servo.calibrated=true;
     track.servo.mount_offset_deg=(bearing+track.servo.angle)%360;
     if(track.servo.mount_offset_deg>180)track.servo.mount_offset_deg-=360;
     payload={ok:true,method:'compass',bearing,servo_angle:track.servo.angle,mount_offset_deg:track.servo.mount_offset_deg};
    }
   }else if(u.pathname==='/api/servo/center'){
    assert.deepEqual([...u.searchParams.keys()].sort(),['epoch','seq','stamp']);
    if(api.failCenter){ok=false;status=api.failCenter;payload={ok:false,error:'centre rejected'};}
    else if(api.invalidCenter)payload={ok:true};
    else {track.servo.mode='manual';track.servo.source='manual';track.servo.target=179.5;
     track.servo.moving=track.servo.angle!==179.5;track.servo.calibration_ready=!track.servo.moving;
     ++track.servo.control_epoch;track.servo.command_seq=0;
     payload={ok:true,mode:'manual',angle:track.servo.angle,target:179.5,...contextFields()};}
   }else if(u.pathname==='/api/servo/mode'){
    const mode=u.searchParams.get('mode');
    if(mode==='gps'&&!track.servo.gps_available){ok=false;payload={ok:false,error:'GPS unavailable'};}
    else {track.servo.mode=mode;++track.servo.control_epoch;track.servo.command_seq=0;payload={ok:true,mode,...contextFields()};}
   }else payload={ok:true,...contextFields()};
  }else if(u.pathname==='/api/servo/settings')payload={ok:true,...settings,...contextFields()};
  else if(u.pathname==='/api/track/prediction'){
   if(api.failPredictionGet){ok=false;status=api.failPredictionGet;payload={ok:false};}
   else payload={ok:true,...prediction,...contextFields()};
  }
  else if(u.pathname==='/api/track')payload=structuredClone(track);
  else if(u.pathname==='/api/station/position')payload={source:'gnss',valid:true,accuracy_m:null};
  else if(u.pathname==='/api/client/control')payload={supported:true,client_id:61493,boot_id:123,state:'ready',age_ms:1200,battery_mv:4000,command:'idle',ready_poll_ms:30000};
  else if(u.pathname==='/api/sd/status')payload={state:'recording'};
  else if(u.pathname==='/api/debug')payload={gps:{observed_hz:2,rmc_hz:2,gga_hz:2,checksum_errors:0,stream:{missing_or_invalid_time:0}},client_gnss:{received:false}};
  else if(u.pathname==='/api/status')payload={servo:structuredClone(track.servo),alerts:[],
    lora:{rssi:-100,snr:5,drop_rate:0,rx_data:0},health:{uptime_s:1,firmware_version:'test'}};
  const response={ok,status:ok?200:status===200?409:status,json:()=>Promise.resolve(payload)};
  const index=gates.findIndex(g=>g.path===u.pathname&&g.method===(options.method||'GET'));
  if(index>=0)return gates.splice(index,1)[0].promise.then(()=>response);
  return Promise.resolve(response);
 }};
const viewportWrites={};
c.document.documentElement={style:{setProperty(k,v){viewportWrites[k]=v;}}};
if(options.visualViewport)c.window.visualViewport=options.visualViewport;
Object.assign(c,options.context||{});
vm.createContext(c);vm.runInContext(script,c);
function documentEvent(name,event={}){for(const fn of documentEvents[name]||[])fn(event);}
function windowEvent(name,event={}){for(const fn of windowEvents[name]||[])fn(event);}
return {c,track,settings,prediction,elements,posts,api,holdNext,setPrediction,viewportWrites,documentEvent,windowEvent};
}
async function settle(){for(let i=0;i<150;i++)await Promise.resolve();}

if(require.main===module)(async()=>{
 const p=createPage();await settle();const {c,elements:e,track}=p;await c.refresh();await settle();
 assert(!e.mUart&&!e.cfgPrediction&&!e.sFix&&!e.cFix);
 assert.equal(e.sld.disabled,false);
 assert(!e.stationSd&&!e.sVersion&&!e.sTemp&&!e.sHum&&!e.controllerSettings&&!e.btnCompassCenter);
 assert.equal(e.speedSaveState.textContent,'');
 assert.equal(e.clientControlState.textContent,'關閉');assert.match(e.cBattV.textContent,/4\.000 V/);
 assert.equal(c.satWord(0),'無');assert.equal(c.satWord(5),'少');assert.equal(c.satWord(6),'中');assert.equal(c.satWord(12),'多');
 assert.equal(c.fmtAcc(1.2),'HDOP 1.2');assert.equal(c.mapLink({fix:false,lat:24,lon:121}),'--');
 assert.match(c.mapLink({fix:true,lat:24,lon:121}),/query=24%2C121/);assert(!c.mapLink({fix:true,lat:24,lon:121}).includes('>24'));
 e.speedValue.onclick();assert(e.speedDialog.open);e.cfgSpeed.value='99';e.speedForm.onsubmit({preventDefault(){}});assert(e.speedDialog.open);assert.match(e.speedError.textContent,/整數/);
 e.cfgSpeed.value='35.5';e.speedForm.onsubmit({preventDefault(){}});assert(e.speedDialog.open);
 e.cfgSpeed.value='35';await e.speedForm.onsubmit({preventDefault(){}});await settle();assert(!e.speedDialog.open);assert.equal(p.settings.speed,35);
 e.speedValue.onclick();e.cfgSpeed.value='40';e.cancelSpeed.onclick();assert.equal(p.settings.speed,35);
 Object.assign(track.servo,{mode:'gps',gps_available:true});await c.refresh();assert(e.sld.disabled);
 Object.assign(track.servo,{mode:'manual',angle:250,target:250,calibration_ready:false});await c.refresh();
 e.compassBearing.value='90';await e.btnCompassCal.onclick();await settle();
 assert.equal(track.servo.target,179.5);assert.match(e.compassCalState.textContent,/已校正/);
 assert(!p.posts.some(u=>u.pathname==='/api/servo/center'));
 Object.assign(track.servo,{mode:'gps',gps_available:true});await c.refresh();
 const before=p.posts.length;await e.btnCompassCal.onclick();assert.equal(p.posts.length,before);
 Object.assign(track.servo,{mode:'manual',moving:true});await c.refresh();
 await e.btnCompassCal.onclick();assert.equal(p.posts.length,before);
 Object.assign(track.servo,{moving:false});await c.refresh();
 e.compassBearing.value='90.5';e.btnCompassCal.onclick();assert.match(e.compassCalState.textContent,/整數/);
 e.compassBearing.value='360';e.btnCompassCal.onclick();assert.match(e.compassCalState.textContent,/整數/);
 c.renderClientControl({supported:true,state:'tracking',age_ms:100});assert.equal(e.clientControlState.textContent,'開啟');
 c.renderClientControl({supported:false,state:'tracking',age_ms:100000});assert.equal(e.clientControlState.textContent,'Client 尚未回報');
 assert(p.posts.every(u=>!u.pathname.includes('prediction')&&!u.pathname.includes('controller')));
 // RF quality is independent of GPS DATA; each metric keeps its own grade.
 function rf(rssi,snr){c.last.status={lora:{rssi,snr}};c.statusReceivedMs=1000;c.renderRfQuality();}
 rf(-53,11);assert.equal(e.sRssiQuality.textContent,'Good');assert.equal(e.sSnrQuality.textContent,'Good');
 assert.equal(e.sRssi.textContent,'-53 dBm');assert.equal(e.sSnr.textContent,'11.0 dB');
 rf(-105,0);assert.equal(e.sRssiQuality.textContent,'Good');assert.equal(e.sSnrQuality.textContent,'OK');
 rf(-115,7);assert.equal(e.sRssiQuality.textContent,'OK');assert.equal(e.sSnrQuality.textContent,'Good');
 rf(-115.1,-0.1);assert.equal(e.sRssiQuality.textContent,'Bad');assert.equal(e.sSnrQuality.textContent,'Bad');
 rf(0,-5);assert.equal(e.sRssiQuality.textContent,'Good');assert.equal(e.sSnrQuality.textContent,'Bad');
 rf(null,null);assert.equal(e.sRssiQuality.textContent,'Off');assert.equal(e.sSnrQuality.textContent,'Off');
 assert.match(e.rfQualityHint.textContent,/不代表 Client 關機/);
 rf(undefined,NaN);assert.equal(e.sRssiQuality.textContent,'--');assert.equal(e.sSnrQuality.textContent,'--');
 rf(-53,11);c.performance.now=()=>7001;c.renderSignalLights();
 assert.equal(e.sRssiQuality.textContent,'--');assert.equal(e.sRssi.textContent,'--');
 c.performance.now=()=>1000;await c.refreshStatus();assert.equal(e.sRssiQuality.textContent,'Good');
 const request=c.httpRequest;c.httpRequest=()=>Promise.reject(new Error('offline'));await c.refreshStatus();
 assert.equal(e.sSnrQuality.textContent,'--');assert.equal(e.sSnr.textContent,'--');
 c.httpRequest=request;await c.refreshStatus();assert.equal(e.sSnrQuality.textContent,'OK');
 console.log('PASS independent RF grades, boundaries, missing samples, browser stale/failure/recovery and raw values');
 console.log('PASS real UI: map links, satellite words, HDOP, GPS states/freshness, removed clutter, speed modal save/cancel/validation, physical-centre calibration and mode isolation');
})().catch(e=>{console.error(e);process.exitCode=1;});
module.exports={createPage,settle,html,script};
