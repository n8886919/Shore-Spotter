// Full embedded UI simulation: speed/prediction auto-save and the GPS selection gate.
const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict');
const source=fs.readFileSync(process.env.SHORE_WEB_UI_TEST_SOURCE||'include/web_ui.h','utf8');
const html=source.includes('R"')?source.match(/R"(\w+)\(([\s\S]*?)\)\1"/)[2]:source;
const script=html.match(/<script>([\s\S]*?)<\/script>/)[1];
const examples=[...fs.readFileSync('docs/interface.md','utf8').matchAll(/```json\s*\n([\s\S]*?)\n```/g)].map(m=>JSON.parse(m[1]));
const trackExample=examples.find(x=>x.server&&x.client&&x.servo);
function createPage(options={}){
const track=structuredClone(trackExample);
Object.assign(track.servo,{mode:'uart',source:'hold',angle:90,target:90,moving:false,speed_limit_deg_s:30,
  gps_available:false,gps_usable:false,motion_fault:false,control_boot_id:1,control_epoch:10,command_seq:0,clock_ms:1000,
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
 textContent:'',clientWidth:500,clientHeight:500,getContext:()=>canvas,classList:{toggle(){},add(){},remove(){}},addEventListener(){}};}
const posts=[],gates=[];
const api={failSave:false,failPrediction:0,predictionNetworkError:false,invalidPredictionPost:false,
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
const c={document:{getElementById:id=>elements[id],activeElement:null},window:{addEventListener(){}},console,Promise,Date,Math,Number,AbortController,performance:{now:()=>1000},
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
  else if(u.pathname==='/api/status')payload={servo:structuredClone(track.servo),alerts:[],
    lora:{rssi:-100,snr:5,drop_rate:0},health:{uptime_s:1,firmware_version:'test'}};
  const response={ok,status:ok?200:status===200?409:status,json:()=>Promise.resolve(payload)};
  const index=gates.findIndex(g=>g.path===u.pathname&&g.method===(options.method||'GET'));
  if(index>=0)return gates.splice(index,1)[0].promise.then(()=>response);
  return Promise.resolve(response);
 }};
Object.assign(c,options.context||{});
vm.createContext(c);vm.runInContext(script,c);
return {c,track,settings,prediction,elements,posts,api,holdNext,setPrediction};
}
async function settle(){for(let i=0;i<150;i++)await Promise.resolve();}
let motionTestsDone=false;
if(require.main===module)process.on('beforeExit',()=>{if(!motionTestsDone){console.error('FAIL motion UI: unfinished async test');process.exitCode=1;}});
if(require.main===module)(async()=>{
 const {c,track,settings,elements,posts,api}=createPage();
 await settle();c.applyMode(track.servo);
 assert.equal(elements.cfgSpeed.value,'30');assert.equal(elements.cfgSpeed.disabled,false);
 assert.equal(elements.sld.disabled,true);assert.equal(elements.mGps.disabled,true);assert.equal(posts.length,0);
 assert(html.indexOf('id="servoSpeedSettings"')>html.indexOf('id="pgInfo"'));
 for(const id of ['servoSettingsPanel','cfgAcceleration','cfgJerk','cfgDeadband','enableSpeed','btnMotionSave','btnMotionApply','btnOledRefresh','btnTimingReset','motionTiming','btnLog'])assert(!elements[id],id+' should be removed');
 assert(!/20 Hz|333 Hz|Jerk|死區|OLED／|調試/.test(html));
 elements.cfgSpeed.value='20';elements.cfgSpeed.onchange();await settle();
 assert.equal(settings.speed,20);assert.equal(track.servo.mode,'uart');assert.equal(posts.length,1);assert.match(elements.speedSaveState.textContent,/已記住 20/);
 elements.cfgSpeed.value='90';elements.cfgSpeed.onchange();await settle();assert.equal(settings.speed,90);
 const count=posts.length;elements.cfgSpeed.value='91';elements.cfgSpeed.onchange();await settle();assert.equal(posts.length,count);assert.equal(elements.cfgSpeed.value,'90');
 elements.cfgSpeed.value='';elements.cfgSpeed.onchange();await settle();assert.equal(posts.length,count);
 api.failSave=true;elements.cfgSpeed.value='25';elements.cfgSpeed.onchange();await settle();assert.equal(settings.speed,90);assert.equal(elements.cfgSpeed.value,'90');assert.match(elements.speedSaveState.textContent,/未完成/);
 api.failSave=false;elements.mManual.onclick();await settle();assert.equal(track.servo.mode,'manual');
 track.servo.moving=true;elements.cfgSpeed.value='30';elements.cfgSpeed.onchange();await settle();assert.equal(settings.speed,30);assert.equal(track.servo.moving,true);
 c.document.activeElement=elements.cfgSpeed;elements.cfgSpeed.value='45';c.applyMode(track.servo);assert.equal(elements.cfgSpeed.value,'45');c.document.activeElement=null;
 track.servo.gps_available=true;c.applyMode(track.servo);assert.equal(elements.mGps.disabled,false);
 // GPS can deteriorate after the button was rendered; a rejected mode request must leave Manual active.
 track.servo.gps_available=false;elements.mGps.onclick();await settle();assert.equal(track.servo.mode,'manual');assert.equal(elements.mGps.disabled,true);
 track.servo.gps_available=true;track.servo.moving=false;c.applyMode(track.servo);elements.mGps.onclick();await settle();assert.equal(track.servo.mode,'gps');assert.equal(elements.sld.disabled,true);
 elements.cfgSpeed.value='29.5';elements.cfgSpeed.onchange();await settle();assert.equal(settings.speed,29.5);assert.equal(track.servo.mode,'gps');
 await c.loadSpeedSetting();assert.equal(elements.cfgSpeed.value,'29.5');
 console.log('PASS full UI: Info shared speed, automatic save in all modes, persistence reload, validation/failure, active edits and GPS availability race');
 // v4 carries a current satellite class; a lower bound must never look like an exact satellite count.
 for(const [cls,label,grade] of [[0,'未知','miss'],[1,'≤5 顆','bad'],[2,'6–7 顆','ok'],[3,'≥8 顆','good']]){
  Object.assign(track.client,{satellite_class:cls,satellites:null,hdop:1});await c.refresh();
  assert.equal(elements.cSat.textContent,label);assert.equal(c.gpsGrade(c.clientSatelliteGradeCount(track.client),track.client.hdop),grade);
 }
 Object.assign(track.client,{satellite_class:1,satellites:12,hdop:1});await c.refresh();
 assert.equal(elements.cSat.textContent,'≤5 顆');assert.equal(c.gpsGrade(c.clientSatelliteGradeCount(track.client),1),'bad');
 assert.equal(c.gpsGrade(null,null),'miss');assert.equal(c.fmtAcc(null),'--');
 // Prediction is a real binary, accessible setting and starts unknown until read.
 assert(html.indexOf('id="gpsPredictionSettings"')>html.indexOf('id="pgInfo"'));
 assert.match(html,/<label[^>]+for="cfgPrediction"/);
 assert.match(html,/id="cfgPrediction"[^>]+type="checkbox"[^>]+disabled/);
 assert.match(html,/id="predictionSaveState"[^>]+aria-live="polite"/);
 const p=createPage();
 assert.equal(p.elements.cfgPrediction.disabled,true);
 assert.equal(p.elements.cfgPrediction.indeterminate,true);
 await settle();
 assert.equal(p.elements.cfgPrediction.disabled,false);assert.equal(p.elements.cfgPrediction.checked,true);
 assert.equal(p.elements.cfgPrediction.indeterminate,false);assert.equal(p.posts.length,0);
 for(const mode of ['uart','manual','gps']){
  p.track.servo.mode=mode;p.track.servo.gps_available=true;p.track.servo.calibrated=true;p.c.applyMode(p.track.servo);
  for(const enabled of [false,true]){
   p.elements.cfgPrediction.checked=enabled;await p.elements.cfgPrediction.onchange();await settle();
   assert.equal(p.prediction.enabled,enabled);assert.equal(p.prediction.alpha,enabled?1:0);
   assert.equal(p.elements.cfgPrediction.checked,enabled);assert.equal(p.track.servo.mode,mode);
   assert.equal(p.track.servo.calibrated,true);assert.equal(p.settings.speed,30);
   assert.match(p.elements.predictionSaveState.textContent,/已記住/);
  }
 }
 const posted=p.posts.length;p.elements.cfgPrediction.onchange();await settle();assert.equal(p.posts.length,posted);
 p.elements.cfgPrediction.checked=false;await p.elements.cfgPrediction.onchange();await settle();
 const reload=createPage({enabled:p.prediction.enabled});await settle();
 assert.equal(reload.elements.cfgPrediction.checked,false);assert.equal(reload.elements.cfgPrediction.indeterminate,false);
 assert.equal(reload.posts.length,0);
 // API rejections and transport/malformed failures roll back, unlock, and preserve mode/speed.
 for(const failure of [400,409,503,'network','malformed']){
  p.api.failPrediction=typeof failure==='number'?failure:0;
  p.api.predictionNetworkError=failure==='network';p.api.invalidPredictionPost=failure==='malformed';
  p.elements.cfgPrediction.checked=true;await p.elements.cfgPrediction.onchange();await settle();
  assert.equal(p.prediction.enabled,false);assert.equal(p.elements.cfgPrediction.checked,false);
  assert.equal(p.elements.cfgPrediction.disabled,false);assert.equal(p.c.controlChanging,false);
  assert.equal(p.track.servo.mode,'gps');assert.equal(p.settings.speed,30);
  assert.match(p.elements.predictionSaveState.textContent,/未完成/);
 }
 p.api.failPrediction=0;p.api.predictionNetworkError=false;p.api.invalidPredictionPost=false;
 // A slow save blocks duplicate toggles and mode/slider actions; polling cannot erase its pending choice.
 const delayed=p.holdNext('/api/track/prediction','POST');
 const oldState=structuredClone(p.track.servo),before=p.posts.length;
 p.elements.cfgPrediction.checked=true;const saving=p.elements.cfgPrediction.onchange();await settle();
 assert.equal(p.elements.cfgPrediction.disabled,true);assert.equal(p.c.controlChanging,true);
 p.elements.cfgPrediction.checked=false;p.elements.cfgPrediction.onchange();
 p.elements.mManual.onclick();p.c.queueServoAngle(60,true);p.c.applyMode(oldState);await settle();
 assert.equal(p.posts.length,before+1);assert.equal(p.elements.cfgPrediction.checked,true);
 assert.equal(p.track.servo.mode,'gps');assert.match(p.elements.predictionSaveState.textContent,/儲存中/);
 delayed.release();await saving;await settle();
 assert.equal(p.elements.cfgPrediction.checked,true);assert.equal(p.elements.cfgPrediction.disabled,false);
 // A stale status/GET from before POST must not undo a confirmed setting (same-ms seq matters too).
 p.c.applyMode(oldState);assert.equal(p.elements.cfgPrediction.checked,true);
 const oldGet=p.holdNext('/api/track/prediction');const loading=p.c.loadPredictionSetting();await settle();
 const waitingCount=p.posts.length;p.elements.cfgPrediction.checked=false;const afterRead=p.elements.cfgPrediction.onchange();await settle();
 assert.equal(p.posts.length,waitingCount,'control must wait for the active read body');
 oldGet.release();await loading;await afterRead;await settle();assert.equal(p.elements.cfgPrediction.checked,false);
 // External writes become visible through track and status polling, without issuing a write back.
 const beforeExternal=p.posts.length;
 p.setPrediction(true);++p.track.servo.command_seq;p.track.servo.clock_ms+=10;p.c.applyMode(p.track.servo);
 assert.equal(p.elements.cfgPrediction.checked,true);assert.match(p.elements.predictionSaveState.textContent,/開啟/);
 p.setPrediction(false);++p.track.servo.command_seq;p.track.servo.clock_ms+=10;p.c.refreshStatus();await settle();
 assert.equal(p.elements.cfgPrediction.checked,false);assert.equal(p.posts.length,beforeExternal);
 // Busy speed/mode requests cannot submit or leave an unsaved checkbox value visible.
 const busy=p.holdNext('/api/servo/settings','POST');p.elements.cfgSpeed.value='20';const speedSave=p.elements.cfgSpeed.onchange();await settle();
 const busyCount=p.posts.length;p.elements.cfgPrediction.checked=true;p.elements.cfgPrediction.onchange();await settle();
 assert.equal(p.posts.length,busyCount);assert.equal(p.elements.cfgPrediction.checked,false);
 busy.release();await speedSave;await settle();
 // In-flight manual commands settle first, so setting writes share the existing control order.
 p.track.servo.mode='manual';p.c.applyMode(p.track.servo);
 const angleGate=p.holdNext('/api/servo','POST');p.c.queueServoAngle(60,true);await settle();
 const manualCount=p.posts.length;p.elements.cfgPrediction.checked=true;const afterAngle=p.elements.cfgPrediction.onchange();await settle();
 assert.equal(p.posts.length,manualCount);angleGate.release();await afterAngle;await settle();
 assert.equal(p.posts.at(-1).pathname,'/api/track/prediction');assert.equal(p.track.servo.mode,'manual');
 // Cached new UI on old firmware never suggests a supported, enabled default.
 const old=createPage({unsupported:true});await settle();
 assert.equal(old.elements.cfgPrediction.disabled,true);assert.equal(old.elements.cfgPrediction.indeterminate,true);
 assert.match(old.elements.predictionSaveState.textContent,/不支援/);
 old.elements.cfgPrediction.checked=true;old.elements.cfgPrediction.onchange();await settle();assert.equal(old.posts.length,0);
 // An unavailable GET is also visibly unknown when no valid status value exists; later valid status recovers it.
 const unavailable=createPage({unsupported:true,failPredictionGet:503});unavailable.api.failPredictionGet=503;await settle();
 await unavailable.c.loadPredictionSetting();assert.equal(unavailable.elements.cfgPrediction.disabled,true);
 assert.match(unavailable.elements.predictionSaveState.textContent,/讀取失敗/);
 unavailable.setPrediction(false);unavailable.c.applyMode(unavailable.track.servo);
 assert.equal(unavailable.elements.cfgPrediction.disabled,false);assert.equal(unavailable.elements.cfgPrediction.checked,false);
 // A reboot resets clock/epoch. Old GET, track, status and speed replies must not revive its retired boot.
 const rebooted=createPage();await settle();
 const boot1=structuredClone(rebooted.track.servo);
 const oldPredictionGet=rebooted.holdNext('/api/track/prediction'),predictionRead=rebooted.c.loadPredictionSetting();
 const oldTrack=rebooted.holdNext('/api/track'),trackRead=rebooted.c.refresh();
 const oldStatus=rebooted.holdNext('/api/status');rebooted.c.refreshStatus();
 const oldSpeed=rebooted.holdNext('/api/servo/settings'),speedRead=rebooted.c.loadSpeedSetting();await settle();
 rebooted.setPrediction(false);Object.assign(rebooted.track.servo,
  {control_boot_id:2,control_epoch:20,command_seq:0,clock_ms:100,speed_limit_deg_s:20});
 rebooted.settings.speed=20;
 rebooted.c.applyMode(rebooted.track.servo);
 assert.equal(rebooted.elements.cfgPrediction.checked,false);assert.equal(rebooted.c.motionContext.control_boot_id,2);
 oldPredictionGet.release();oldTrack.release();oldStatus.release();oldSpeed.release();
 await Promise.all([predictionRead,trackRead,speedRead]);await settle();
 assert.equal(rebooted.elements.cfgPrediction.checked,false);assert.equal(rebooted.elements.cfgSpeed.value,'20');
 assert.equal(rebooted.c.motionContext.control_boot_id,2);assert.equal(rebooted.c.motionContext.control_epoch,20);
 assert.equal(rebooted.c.motionContext.command_seq,0);assert.equal(rebooted.c.motionContext.clock_ms,100);
 const boot2=structuredClone(rebooted.track.servo);
 rebooted.setPrediction(true);Object.assign(rebooted.track.servo,{control_boot_id:3,control_epoch:30,clock_ms:20});
 rebooted.c.applyMode(rebooted.track.servo);rebooted.c.applyMode(boot1);rebooted.c.applyMode(boot2);
 assert.equal(rebooted.c.motionContext.control_boot_id,3);assert.equal(rebooted.elements.cfgPrediction.checked,true);
 // post() also sees retired replies before the setting handler: a delayed success cannot roll the context back.
 const postReboot=createPage({enabled:false});await settle();
 const oldPost=postReboot.holdNext('/api/track/prediction','POST');
 postReboot.elements.cfgPrediction.checked=true;const rebootSave=postReboot.elements.cfgPrediction.onchange();await settle();
 postReboot.setPrediction(false);Object.assign(postReboot.track.servo,{control_boot_id:2,control_epoch:20,command_seq:0,clock_ms:100});
 postReboot.c.applyMode(postReboot.track.servo);assert.equal(postReboot.c.motionContext.control_boot_id,2);
 oldPost.release();await rebootSave;await settle();
 assert.equal(postReboot.elements.cfgPrediction.checked,false);assert.equal(postReboot.elements.cfgPrediction.disabled,false);
 assert.equal(postReboot.c.motionContext.control_boot_id,2);assert.equal(postReboot.c.motionContext.command_seq,0);
 assert.equal(postReboot.track.servo.mode,'uart');assert.match(postReboot.elements.predictionSaveState.textContent,/未完成/);
 console.log('PASS full UI: binary prediction toggle, all modes, persistence reload, 400/409/503/network/malformed failures, duplicate/busy serialization, stale polls and external changes, unsupported/load recovery, retired-boot GET/status/POST races');
 motionTestsDone=true;
})().catch(e=>{console.error(e);process.exitCode=1;});
module.exports={createPage,settle,html,script};
