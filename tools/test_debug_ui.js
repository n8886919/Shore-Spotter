// Exercise the real embedded page, diagnostic transport, recording limits and JSON download.
const assert=require('node:assert/strict');
const {createPage,settle,html}=require('./test_motion_ui.js');

function page(){
 let now=0,nextTimer=0,inFlight=0,maxInFlight=0,held=null,failDebug=0,failLog=0,missingHeaders=false;
 const timers=new Map(),requests=[],allRequests=[],downloads=[],revoked=[];
 const debug={schema_version:2,firmware_version:'0.5-test',protocol_version:4,boot_id:1,clock_ms:1000,
  config:{rf_frequency_mhz:923.2,bw_khz:125,sf:9,cr:5,data_bytes:17,ack_bytes:11,telemetry_bytes:11,diagnostic_bytes:17,send_interval_ms:500,ack_every_n:8,gnss_baud:9600,bound_client_id:0},
  gps:{scope:'server_local',last_epoch_interval_ms:500,source_age_ms:80,age_basis:'nmea_epoch_aligned_arrival',measurement_clock_synchronized:false},
  client_diagnostic:{received:true,fresh:true,rx_age_ms:5000,epoch_interval_ms:1000,backlog_drops:3,nmea_errors:4,tx_errors:2,skipped_slots:5,status_bits:63,counter_encoding:'uint16_saturating_since_client_boot'},
  counters:{rx_data:2,rejected_format:0,inferred_source_interval_ms:750},events:{capacity:64,total:2,overwritten:0,items:[{id:1,ms:500,kind:'data',client_id:0xE91C,seq:1}]}};
 const log={next:14,boot:1,dropped:false,text:'[BOOT] ready\n'};
 class FakeDate extends Date{constructor(...args){super(...(args.length?args:[1700000000000+now]));}static now(){return 1700000000000+now;}}
 const context={Date:FakeDate,Blob,performance:{now:()=>now},
  setTimeout(fn,ms){const id=++nextTimer;timers.set(id,{fn,at:now+ms});return id;},clearTimeout(id){timers.delete(id);},
  setInterval(){return 1;},clearInterval(){},
  URL:{createObjectURL(blob){downloads.push(blob);return 'blob:test';},revokeObjectURL(url){revoked.push(url);}},
  debugFetch(url,options){
   const u=new URL(url,'http://test');requests.push({path:u.pathname,from:u.searchParams.get('from'),since:u.searchParams.get('since'),boot:u.searchParams.get('boot_id'),limit:u.searchParams.get('limit'),method:options.method||'GET',at:now});
   assert.equal(options.method,undefined,'diagnostic reads must never POST');
   const snapshot=structuredClone(debug),logSnapshot=structuredClone(log),status=u.pathname==='/api/debug'?failDebug:failLog;
   if(u.pathname==='/api/debug'){
    assert.equal(u.searchParams.get('limit'),'8');
    const reset=u.searchParams.get('boot_id')!==String(snapshot.boot_id),since=Number(u.searchParams.get('since'));
    const available=reset?snapshot.events.items:snapshot.events.items.filter(e=>((e.id-since)|0)>0);
    const items=available.slice(0,8);
    snapshot.events={...snapshot.events,items,next_id:items.length?items.at(-1).id:reset?snapshot.events.total:since,
     more:available.length>8,reset,dropped:!reset&&snapshot.events.items.length>0&&((snapshot.events.items[0].id-since)|0)>1};
   }
   const response={ok:status===0,status:status||200,json:()=>Promise.resolve(snapshot),text:()=>Promise.resolve(logSnapshot.text),
    headers:{get(name){if(missingHeaders)return null;return name==='X-Log-Next'?String(logSnapshot.next):name==='X-Log-Boot'?String(logSnapshot.boot):name==='X-Log-Dropped'?(logSnapshot.dropped?'1':'0'):null;}}};
   if(held&&held.path===u.pathname){
    const gate=held;held=null;
    const delayed=()=>new Promise((resolve,reject)=>{
     let done=false;
     gate.release=()=>{if(!done){done=true;resolve(gate.phase==='body'?(u.pathname==='/api/debug'?snapshot:logSnapshot.text):response);}};
     if(options.signal&&!gate.ignoreAbort)options.signal.addEventListener('abort',()=>{if(!done){done=true;reject(new Error('aborted'));}},{once:true});
    });
    if(gate.phase==='body'){if(u.pathname==='/api/debug')response.json=delayed;else response.text=delayed;return Promise.resolve(response);}
    return delayed();
   }
   return Promise.resolve(response);
  }};
 const p=createPage({context});
 const originalFetch=p.c.fetch;
 p.c.fetch=function(url,options){
  allRequests.push({url,method:options&&options.method||'GET',at:now});maxInFlight=Math.max(maxInFlight,++inFlight);let finished=false;
  const finish=()=>{if(!finished){finished=true;--inFlight;}};
  return originalFetch(url,options).then(response=>{
   const wrapped={...response};for(const kind of ['json','text'])if(response[kind])wrapped[kind]=()=>response[kind]().finally(finish);
   return wrapped;
  },error=>{finish();throw error;});
 };
 p.c.document.hidden=false;
 p.c.document.body={appendChild(){},removeChild(){}};
 p.c.document.createElement=()=>({click(){},href:'',download:''});
 p.elements.debugNote.value='';
 Object.assign(p.track.client,{satellite_class:3,satellites:null,source_age_ms:100,rx_age_ms:50,sample_age_ms:315,age_basis:'nmea_epoch_aligned_arrival'});
 async function advance(ms){
  now+=ms;
  for(const [id,timer] of [...timers])if(timer.at<=now){timers.delete(id);timer.fn();await settle();}
  await settle();
 }
 function hold(path,phase='headers',ignoreAbort=false){held={path,phase,ignoreAbort};return held;}
 return {...p,debug,log,requests,allRequests,downloads,revoked,advance,hold,maxInFlight:()=>maxInFlight,
  failDebug(value){failDebug=value;},failLog(value){failLog=value;},missingHeaders(value){missingHeaders=value;}};
}

let testsDone=false;process.on('beforeExit',()=>{if(!testsDone){console.error('FAIL debug UI: unfinished async test');process.exitCode=1;}});
(async()=>{
 assert.match(html,/id="tabDebug"[^>]*>除錯/);assert.match(html,/id="debugNote"[^>]*maxlength="2000"/);
 const p=page();await settle();await p.c.refresh();assert.equal(p.requests.length,0,'normal pages must not poll diagnostic endpoints');
 p.elements.tabDebug.onclick();await settle();
 assert.deepEqual(p.requests.map(r=>r.path),['/api/debug','/api/log']);
 assert.match(p.elements.debugSummary.textContent,/發送排程 2\.00 Hz/);
 assert.match(p.elements.debugSummary.textContent,/Server 本機 GNSS 新定位間隔 500 ms/);
 assert.match(p.elements.debugSummary.textContent,/Client GNSS 低頻 DIAG：仍在有效期，非即時.*接收後 5000 ms.*該次新定位間隔 1000 ms/);
 assert.match(p.elements.debugSummary.textContent,/Server 由封包推估 Client 更新間隔 750 ms（推估，非 Client 實測）/);
 assert.match(p.elements.debugSummary.textContent,/綁定 Client：未綁定/);
 assert.match(p.elements.debugSummary.textContent,/Client 該次 DIAG 計數：UART 積壓丟棄 3.*NMEA 錯誤 4.*發送錯誤 2.*跳過排程 5/);
 assert.match(p.elements.debugSummary.textContent,/Client 該次 DIAG 狀態：樣本 有.*定位 有效.*速度 有效.*GGA 有.*RMC 有/);
 assert.match(p.elements.debugSummary.textContent,/來源 100 ms.*接收後 50 ms.*合計估計 315 ms/);
 assert.equal(p.elements.debugLog.textContent,'[BOOT] ready\n');assert.equal(p.c.debugSamples.length,0);
 await p.c.pollDebug();assert.equal(p.requests.length,2,'a second click within one second must not repoll');
 p.elements.tabInfo.onclick();await p.advance(1000);await p.c.pollDebug();assert.equal(p.requests.length,2);
 p.c.document.hidden=true;p.elements.tabDebug.onclick();await settle();assert.equal(p.requests.length,2);
 p.c.document.hidden=false;p.elements.tabDebug.onclick();await settle();assert.equal(p.requests.length,4);
 assert.equal(p.requests[3].from,'14');
 // Diagnostic reads are serialized; timeout aborts the current request and releases the poll.
 await p.advance(1000);const slow=p.hold('/api/debug');const polling=p.c.pollDebug();await settle();
 await p.c.pollDebug();await p.advance(1000);await p.c.pollDebug();assert.equal(p.requests.length,5);
 await p.advance(1000);await polling;assert.equal(p.requests.length,6);assert.equal(p.maxInFlight(),1);
 assert.equal(p.c.debugBusy,false);assert.match(p.elements.debugPollState.textContent,/讀取未完成.*逾時/);
 slow.release();await settle();assert.equal(p.requests.length,6);
 // Recording continues on Radar and in a hidden tab, with gaps recorded rather than claiming continuity.
 await p.advance(1000);await p.elements.btnDebugStart.onclick();await settle();
 assert.equal(p.c.debugRecording,true);assert.equal(p.c.debugSamples.length,1);
 p.elements.tabRadar.onclick();p.c.document.hidden=true;await p.advance(4500);await p.c.pollDebug();
 assert.equal(p.c.debugSamples.length,2);assert.equal(p.c.debugSamples[1].gap_since_previous_ms,4500);
 assert.equal(p.c.debugGapCount,1);assert.match(p.elements.debugRecordState.textContent,/漏採／中斷 1 次/);
 assert.equal(p.posts.length,0);assert.equal(p.track.servo.mode,'uart');
 // Snapshot clones preserve old state, complete event data, and log text, even when current data changes.
 const savedRx=p.c.debugSamples[0].debug.counters.rx_data;p.debug.counters.rx_data=99;
 assert.equal(p.c.debugSamples[0].debug.counters.rx_data,savedRx);
 p.log.boot=2;p.log.next=4;p.log.dropped=true;p.log.text='<script>alert(1)</script>\n';
 p.debug.boot_id=2;p.debug.events.total=1;await p.advance(1000);await p.c.pollDebug();
 assert(p.c.debugSamples.at(-1).warnings.includes('server_reboot'));assert(p.c.debugSamples.at(-1).warnings.includes('server_log_dropped'));
 assert.match(p.elements.debugLog.textContent,/<script>alert\(1\)<\/script>/,'log must remain literal text');
 assert.match(p.elements.debugPollState.textContent,/重開機.*遺失/);assert.equal(p.c.debugLogCursor,4);
 p.debug.events.total=100;p.debug.events.overwritten=36;p.debug.events.items=[{id:100,kind:'data',seq:100}];p.log.dropped=false;await p.advance(1000);await p.c.pollDebug();
 assert(p.c.debugSamples.at(-1).warnings.includes('event_ring_gap'));assert.match(p.elements.debugPollState.textContent,/事件紀錄有缺口/);
 // Old firmware / failed endpoints and missing cursor headers remain explicit, without affecting motion controls.
 p.failDebug(404);p.failLog(503);await p.advance(1000);await p.c.pollDebug();
 assert.equal(p.c.debugSamples.at(-1).errors.length,2);assert.match(p.elements.debugPollState.textContent,/HTTP 404.*HTTP 503/);
 p.failDebug(0);p.failLog(0);p.missingHeaders(true);await p.advance(1000);await p.c.pollDebug();
 assert(p.c.debugSamples.at(-1).warnings.includes('log_boot_unknown'));assert.match(p.elements.debugPollState.textContent,/游標缺失/);
 p.missingHeaders(false);
 // Export is a real JSON Blob with timing/config/evidence metadata; it clears neither browser nor server logs.
 p.elements.debugNote.value='站在沙灘，折返時鏡頭落後';const before=p.c.debugSamples.length,logBefore=p.c.debugLogText,requestsBefore=p.requests.length;
 p.elements.btnDebugExport.onclick();await settle();assert.equal(p.downloads.length,1);
 const bundle=JSON.parse(await p.downloads[0].text());
 assert.equal(bundle.schema_version,2);assert.equal(bundle.protocol_version,4);assert.equal(bundle.config.data_bytes,17);
 assert.equal(bundle.config.diagnostic_bytes,17);assert.equal(bundle.samples[0].debug.gps.scope,'server_local');
 assert.equal(bundle.samples[0].debug.client_diagnostic.epoch_interval_ms,1000);
 assert.equal(bundle.notes,'站在沙灘，折返時鏡頭落後');assert.equal(bundle.recording.sample_count,before);
 assert(bundle.samples[0].observed_at);assert(bundle.samples[0].track_received_at);assert(bundle.samples[0].status_received_at);
 assert(Array.isArray(bundle.samples[0].events.items));assert(!('items' in bundle.samples[0].debug.events));assert.equal(bundle.samples[0].log.text,'[BOOT] ready\n');
 assert.equal(bundle.current.track.client.satellites,null);assert.equal(bundle.current.track.client.satellite_class,3);
 assert.equal(bundle.evidence_limits.length,4);assert.equal(p.c.debugSamples.length,before);assert.equal(p.c.debugLogText,logBefore);
 assert.equal(p.requests.length,requestsBefore);assert.equal(p.c.debugRecording,true);await p.advance(1000);assert.deepEqual(p.revoked,['blob:test']);
 p.elements.btnDebugStop.onclick();await p.advance(1000);await p.c.pollDebug();assert.equal(p.c.debugRecording,false);
 assert.equal(p.requests.length,requestsBefore);assert.equal(p.c.debugSamples.length,before);
 // A poll started before a new recording cannot leak into that recording after delayed completion.
 const q=page();await settle();q.c.page='debug';const stale=q.hold('/api/debug');const stalePoll=q.c.pollDebug();await settle();
 await q.c.startDebugRecording();stale.release();await stalePoll;assert.equal(q.c.debugSamples.length,0);
 await q.advance(1000);await q.c.pollDebug();assert.equal(q.c.debugSamples.length,1);
 // Caps stop recording visibly, do not silently drop old samples or allow unlimited text growth.
 q.c.DEBUG_MAX_SAMPLES=2;await q.advance(1000);await q.c.pollDebug();assert.equal(q.c.debugSamples.length,2);
 assert.equal(q.c.debugRecording,false);assert.match(q.elements.debugRecordState.textContent,/取樣上限/);
 q.c.DEBUG_MAX_SAMPLES=1200;q.c.DEBUG_MAX_BYTES=10;await q.advance(1000);await q.c.startDebugRecording();
 assert.equal(q.c.debugSamples.length,0);assert.equal(q.c.debugRecording,false);assert.match(q.elements.debugRecordState.textContent,/容量上限/);
 q.c.DEBUG_MAX_BYTES=5*1024*1024;await q.advance(1000);await q.c.startDebugRecording();q.elements.tabRadar.onclick();
 await q.advance(600000);await q.c.pollDebug();assert.equal(q.c.debugRecording,false);assert.match(q.elements.debugRecordState.textContent,/10 分鐘/);
 q.log.text='x'.repeat(40000);q.c.page='debug';await q.advance(1000);await q.c.pollDebug();
 assert.equal(q.c.debugLogText.length,q.c.DEBUG_LOG_CHARS);assert(q.c.debugLogTrimmed>0);assert.match(q.elements.debugRecordState.textContent,/文字已截短/);
 assert.equal(q.posts.length,0);assert.equal(q.maxInFlight(),1);
 // Real Client diagnostic values stay explicitly low-rate/stale; server GNSS and inferred intervals cannot impersonate them.
 const schema=page();await settle();schema.c.page='debug';
 schema.debug.client_diagnostic.fresh=false;schema.debug.client_diagnostic.rx_age_ms=95000;
 schema.debug.config.bound_client_id=0xE91C;
 schema.debug.events.items.push({id:2,ms:750,kind:'binding',client_id:0x9999,seq:2,raw_hex:'53a4'});
 await schema.c.pollDebug();
 assert.match(schema.elements.debugSummary.textContent,/Client GNSS 低頻 DIAG：已過期，僅供歷史參考.*接收後 95000 ms.*1000 ms/);
 assert.doesNotMatch(schema.elements.debugSummary.textContent,/仍在有效期/);
 assert.match(schema.elements.debugSummary.textContent,/綁定 Client：0xE91C；最近因綁定不符拒收 0x9999/);
 assert.equal(schema.c.buildDebugBundle().current.events.items.at(-1).raw_hex,'53a4');
 schema.debug.client_diagnostic.received=false;schema.debug.gps.last_epoch_interval_ms=0;
 schema.debug.counters.inferred_source_interval_ms=0;await schema.advance(1000);await schema.c.pollDebug();
 assert.match(schema.elements.debugSummary.textContent,/Server 本機 GNSS 新定位間隔 未知/);
 assert.match(schema.elements.debugSummary.textContent,/Client GNSS 低頻 DIAG：尚未收到.*接收後 未知.*該次新定位間隔 未知/);
 assert.match(schema.elements.debugSummary.textContent,/Client 該次 DIAG 計數：UART 積壓丟棄 未知/);
 assert.match(schema.elements.debugSummary.textContent,/Client 該次 DIAG 狀態：未知/);
 assert.doesNotMatch(schema.elements.debugSummary.textContent,/新定位間隔 0 ms/);
 assert.equal(schema.posts.length,0,'binding diagnostics remain read-only');
 // The transport slot includes delayed response bodies, and control priority is
 // considered before queued track/settings/status/log work. Same-key background
 // calls share one result rather than accumulating duplicate network requests.
 const priority=page();await settle();priority.c.page='debug';const priorityStart=priority.allRequests.length;
 const body=priority.hold('/api/debug','body');const bodyPoll=priority.c.pollDebug();await settle();
 const status1=priority.c.httpRequest('/api/status',2,'status'),status2=priority.c.httpRequest('/api/status',2,'status');
 assert.equal(status1,status2);
 const queuedTrack=priority.c.httpRequest('/api/track',1,'track');
 const queuedSettings=priority.c.httpRequest('/api/servo/settings',1,'speed');
 const control=priority.c.post('/api/servo?angle=45');await settle();
 assert.equal(priority.allRequests.length,priorityStart+1,'headers must not release the HTTP slot');
 assert.equal(priority.c.motionContext.command_seq,0,'queued commands must not consume a sequence');
 priority.track.servo.clock_ms=1500;priority.c.syncMotionContext(priority.track.servo);
 body.release();await Promise.all([bodyPoll,status1,queuedTrack,queuedSettings,control]);await settle();
 assert.deepEqual(priority.allRequests.slice(priorityStart).map(r=>new URL(r.url,'http://test').pathname),
  ['/api/debug','/api/servo','/api/track','/api/servo/settings','/api/status','/api/log']);
 assert.equal(priority.posts[0].searchParams.get('stamp'),'1500','motion URL uses context at dispatch');
 assert.equal(priority.maxInFlight(),1);
 // An original control intent expires while waiting for a read. Even ignored
 // aborts cannot release the slot or allow a late diagnostic body to update state.
 const expired=page();await settle();expired.c.page='debug';const stuck=expired.hold('/api/debug','body',true);
 const stuckPoll=expired.c.pollDebug();await settle();
 const expiredControl=expired.c.post('/api/servo?angle=46').then(()=>null,e=>e);await settle();
 await expired.advance(2000);assert.match((await expiredControl).message,/等待逾時/);
 assert.equal(expired.posts.length,0);assert.equal(expired.c.motionContext.command_seq,0);
 assert.equal(expired.requests.length,1,'an ignored abort retains the transport slot');
 stuck.release();await stuckPoll;await settle();
 assert.equal(expired.c.debugLatest,null);assert.equal(expired.c.debugEventNext,null);
 assert.equal(expired.posts.length,0);assert.equal(expired.maxInFlight(),1);
 // Missing AbortController follows the same rule; eventual transport completion
 // releases the slot, while the timed-out response stays rejected.
 const fallback=page();await settle();fallback.c.AbortController=undefined;fallback.c.page='debug';
 const unsupported=fallback.hold('/api/debug','body');const unsupportedPoll=fallback.c.pollDebug();await settle();
 await fallback.advance(2000);assert.equal(fallback.requests.length,1);assert.equal(fallback.c.debugLatest,null);
 const fallbackTrack=fallback.c.httpRequest('/api/track',1,'track');await settle();
 assert.equal(fallback.allRequests.at(-1).url,'/api/debug?limit=8');
 unsupported.release();await Promise.all([unsupportedPoll,fallbackTrack]);
 assert.equal(fallback.c.debugLatest,null);assert.equal(fallback.maxInFlight(),1);
 // Context refresh occurs inside the same slot, retains the original intent
 // deadline, and refuses to repackage a command across a boot or mode epoch.
 for(const field of ['control_boot_id','control_epoch']){
  const changed=page();await settle();await changed.advance(1001);++changed.track.servo[field];
  const mark=changed.allRequests.length;const error=await changed.c.post('/api/servo?angle=47').then(()=>null,e=>e);
  assert.match(error.message,/已更新/);assert.equal(changed.posts.length,0);
  assert.deepEqual(changed.allRequests.slice(mark).map(r=>r.url),['/api/track']);assert.equal(changed.maxInFlight(),1);
 }
 const preflight=page();await settle();await preflight.advance(1001);
 const delayedContext=preflight.holdNext('/api/track');
 const preflightControl=preflight.c.post('/api/servo?angle=48').then(()=>null,e=>e);await settle();
 await preflight.advance(2000);assert.match((await preflightControl).message,/等待逾時/);
 delayedContext.release();await settle();assert.equal(preflight.posts.length,0);assert.equal(preflight.maxInFlight(),1);
 for(const staleKind of ['retired_boot','old_clock']){
  const staleContext=page();await settle();
  if(staleKind==='retired_boot')staleContext.c.syncMotionContext({...staleContext.track.servo,control_boot_id:2});
  else staleContext.c.syncMotionContext({...staleContext.track.servo,clock_ms:2000});
  await staleContext.advance(1001);
  const error=await staleContext.c.post('/api/servo?angle=49').then(()=>null,e=>e);
  assert.match(error.message,/狀態未更新/);assert.equal(staleContext.posts.length,0);
  assert.equal(staleContext.c.motionContext.command_seq,0);
 }
 // Intent begins at the UI commit, including time spent outside the HTTP queue
 // waiting for servoRequest or as the slider's latest committed pending target.
 for(const actionName of ['mode','pending_angle','speed','prediction'])for(const invalidation of ['deadline','boot','epoch']){
  const outer=page();await settle();outer.c.page='debug';const outerBody=outer.hold('/api/debug','body',true);
  const outerPoll=outer.c.pollDebug();await settle();outer.c.queueServoAngle(30,true);await settle();
  let action;
  if(actionName==='mode')action=outer.c.controlAction('/api/servo/mode?mode=manual');
  if(actionName==='pending_angle')outer.c.queueServoAngle(40,true);
  if(actionName==='speed'){outer.elements.cfgSpeed.value='20';action=outer.elements.cfgSpeed.onchange();}
  if(actionName==='prediction'){outer.elements.cfgPrediction.checked=false;action=outer.elements.cfgPrediction.onchange();}
  await settle();
  if(invalidation==='deadline')await outer.advance(2100);
  else {
   if(invalidation==='boot')Object.assign(outer.track.servo,{control_boot_id:2,control_epoch:20,command_seq:0,clock_ms:100});
   else {++outer.track.servo.control_epoch;outer.track.servo.command_seq=0;}
   outer.c.applyMode(outer.track.servo);await outer.advance(100);
  }
  outerBody.release();await Promise.all([outerPoll,action]);await settle();
  assert.equal(outer.posts.length,0,actionName+' must retain its original '+invalidation+' boundary');
  assert.equal(outer.settings.speed,30);assert.equal(outer.prediction.enabled,true);
  assert.equal(outer.track.servo.mode,'uart');assert.equal(outer.maxInFlight(),1);
 }
 const newer=page();await settle();newer.c.page='debug';const newerBody=newer.hold('/api/debug','body',true);
 const newerPoll=newer.c.pollDebug();await settle();newer.c.queueServoAngle(30,true);await settle();
 await newer.advance(1500);newer.c.queueServoAngle(40,true);await settle();await newer.advance(600);
 newerBody.release();await newerPoll;await settle();
 assert.equal(newer.posts.length,1,'a later commit owns a later deadline');assert.equal(newer.posts[0].searchParams.get('angle'),'140');
 assert.equal(newer.maxInFlight(),1);
 // First-load backlog drains at one bounded batch per second. State metadata is
 // separate from recorded raw deltas; total is never used as the next cursor.
 const batches=page();await settle();batches.debug.events.total=64;
 batches.debug.events.items=Array.from({length:64},(_,i)=>({id:i+1,ms:i*500,kind:'data',seq:i}));
 await batches.c.startDebugRecording();assert.equal(batches.c.debugEventNext,8);
 assert.equal(batches.c.debugEventCurrent.length,8);assert.equal(batches.c.debugGapCount,0);
 assert.equal(batches.c.debugSamples[0].events.items.length,8);assert(!('items' in batches.c.debugSamples[0].debug.events));
 await batches.c.pollDebug();assert.equal(batches.requests.filter(r=>r.path==='/api/debug').length,1);
 for(let i=0;i<7;i++){await batches.advance(1000);await batches.c.pollDebug();}
 assert.equal(batches.c.debugEventNext,64);assert.equal(batches.c.debugEventCurrent.length,64);
 assert.deepEqual(batches.requests.filter(r=>r.path==='/api/debug').map(r=>r.since),[null,'8','16','24','32','40','48','56']);
 await batches.advance(1000);await batches.c.pollDebug();assert.equal(batches.c.debugSamples.at(-1).events.items.length,0);
 const recordedIds=batches.c.buildDebugBundle().samples.flatMap(s=>s.events.items.map(e=>e.id));
 assert.equal(recordedIds.length,64);assert.equal(new Set(recordedIds).size,64);
 batches.debug.events.items.push({id:65,kind:'data'});batches.debug.events.items.shift();batches.debug.events.total=65;
 await batches.advance(1000);await batches.c.pollDebug();assert.equal(batches.c.debugEventCurrent.length,64);
 assert.equal(batches.c.debugEventCurrent[0].id,2);assert.equal(batches.c.debugSamples.at(-1).events.items.length,1);
 const oldBoot=batches.c.debugEventBoot;batches.debug.boot_id=2;batches.debug.events={capacity:64,total:1,overwritten:0,items:[{id:1,kind:'data'}]};
 batches.log.boot=2;await batches.advance(1000);await batches.c.pollDebug();
 assert.equal(batches.c.debugEventCurrent.length,1);assert.equal(batches.c.debugEventBoot,2);
 assert.equal(batches.c.debugSamples[0].events.boot_id,oldBoot);assert.equal(batches.c.debugSamples.at(-1).events.boot_id,2);
 // Cursor validation, wrap, retired-boot rejection and empty first-load semantics.
 const cursors=page();await settle();
 function eventResponse(boot,next,ids,flags={}){return {schema_version:2,boot_id:boot,events:{capacity:64,total:999,overwritten:0,
  next_id:next,more:false,dropped:false,reset:false,...flags,items:ids.map(id=>({id,kind:'data'}))}};}
 assert.equal(cursors.c.acceptDebugEvents(eventResponse(1,0,[],{reset:true}),[]).items.length,0);
 assert.equal(cursors.c.debugEventNext,0);assert.match(cursors.c.debugEventUrl(),/since=0$/);
 assert.equal(cursors.c.acceptDebugEvents(eventResponse(1,1,[1]),[]).items.length,1);
 assert.equal(cursors.c.acceptDebugEvents(eventResponse(1,1,[1]),[]).items.length,0);
 for(const invalid of [eventResponse(1,0,[]),eventResponse(1,2,[3]),eventResponse(1,2,Array(9).fill(2)),
   {...eventResponse(1,2,[2]),schema_version:1},eventResponse(1,-1,[])]){
  assert.throws(()=>cursors.c.acceptDebugEvents(invalid,[]));assert.equal(cursors.c.debugEventNext,1);
 }
 cursors.c.acceptDebugEvents(eventResponse(2,0xfffffffe,[0xfffffffd,0xfffffffe],{reset:true}),[]);
 const wrap=cursors.c.acceptDebugEvents(eventResponse(2,1,[0xffffffff,0,1]),[]);
 assert.deepEqual(Array.from(wrap.items,e=>e.id),[0xffffffff,0,1]);assert.equal(cursors.c.debugEventNext,1);
 assert.throws(()=>cursors.c.acceptDebugEvents(eventResponse(1,100,[100]),[]),/舊開機/);
 assert.equal(cursors.c.debugEventBoot,2);assert.equal(cursors.c.debugEventNext,1);
 const gap=[];cursors.c.acceptDebugEvents(eventResponse(2,20,[20],{dropped:true,more:true}),gap);
 assert.deepEqual(gap,['event_ring_gap','event_backlog']);
 // Null timing details are honest; the write phase is explicitly distinct from RTT.
 cursors.c.last.status.timing={http_detail:{slowest:null}};cursors.c.renderDebug();
 assert.match(cursors.elements.debugSummary.textContent,/最慢 HTTP：尚無資料/);
 cursors.c.last.status.timing.http_detail.slowest={route:'/api/debug',total_ms:211,pre_handler_ms:3,build_ms:4,write_ms:200,other_ms:4};
 cursors.c.renderDebug();assert.match(cursors.elements.debugSummary.textContent,/最慢 HTTP：\/api\/debug.*全程 211 ms.*解析／派送 3 ms.*建立回覆 4 ms.*同步寫入 200 ms.*非網路 RTT/);
 console.log('PASS diagnostic full UI: whole-page body serialization, control priority/deadlines/context refresh, timeout/abort fallback, incremental bounded cursors/wrap/boots, recording gaps/limits and schema 2 JSON export');
 testsDone=true;
})().catch(e=>{console.error(e);process.exitCode=1;});
