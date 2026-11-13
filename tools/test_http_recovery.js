// Exercise the real embedded page, including body reads and lifecycle events.
// No live Station requests or actuator commands are used.
const test=require('node:test'),assert=require('node:assert/strict');
const {createPage,settle}=require('./test_motion_ui.js');

function deferred(){let resolve,reject;const promise=new Promise((a,b)=>{resolve=a;reject=b;});return {promise,resolve,reject};}
function response(data){return {ok:true,status:200,json:()=>Promise.resolve(data)};}
async function page(context={}){
  let now=1000,nextId=0;
  const timers=new Map();
  const p=createPage({context:{performance:{now:()=>now},
    setTimeout(fn,delay=0){const id=++nextId;timers.set(id,{at:now+delay,fn});return id;},
    clearTimeout(id){timers.delete(id);},...context}});
  await settle();
  p.advance=async ms=>{
    const until=now+ms;await settle();
    for(;;){
      const next=[...timers.entries()].filter(([,t])=>t.at<=until).sort((a,b)=>a[1].at-b[1].at||a[0]-b[0])[0];
      if(!next)break;
      timers.delete(next[0]);now=next[1].at;next[1].fn();await settle();
    }
    now=until;await settle();
  };
  p.jump=ms=>{now+=ms;}; // Background throttling: time passes without timers.
  p.hide=()=>{p.c.document.visibilityState='hidden';p.documentEvent('visibilitychange');};
  p.show=()=>{p.c.document.visibilityState='visible';p.documentEvent('visibilitychange');};
  return p;
}

for(const abort of [true,false])test('hung fetch releases the queue '+(abort?'despite ignored abort':'without AbortController'),async()=>{
  const p=await page(abort?{}:{AbortController:undefined}),c=p.c;
  const original=c.fetch,gate=deferred();let calls=0;
  c.fetch=(url,options)=>{++calls;return calls===1?gate.promise:original(url,options);};
  const first=c.refresh();await settle();
  assert.equal(calls,1);
  await p.advance(2000);await first;
  assert.equal(c.refreshing,false);
  assert.equal(c.httpActive,null,'a fetch that ignores abort must not retain the slot');
  assert.equal(c.httpFailures.at(-1).name,'TimeoutError');
  const recovered=c.refresh();await settle();
  assert.equal(calls,1,'timeout recovery has a one-second wire cooldown');
  await p.advance(1000);await recovered;
  assert.equal(calls,2);assert.equal(c.trackReceivedMs,4000);
  gate.reject(new Error('late network failure'));await settle();
  assert.equal(c.trackReceivedMs,4000);assert.equal(c.httpActive,null);
});

for(const stage of ['fetch','body'])test('late '+stage+' cannot overwrite a reboot or release a newer job',async()=>{
  const p=await page(),c=p.c,original=c.fetch;
  const oldState=structuredClone(p.track),oldGate=deferred(),newGate=deferred();
  let reads=0,lateBodyReads=0;
  c.fetch=(url,options)=>{
    if(new URL(url,'http://test').pathname!=='/api/track')return original(url,options);
    ++reads;
    if(reads===1)return stage==='fetch'?oldGate.promise:Promise.resolve({ok:true,status:200,json:()=>oldGate.promise});
    if(reads===2)return Promise.resolve({ok:true,status:200,json:()=>newGate.promise});
    return original(url,options);
  };
  const first=c.refresh();await settle();await p.advance(2000);await first;
  Object.assign(p.track.servo,{control_boot_id:2,control_epoch:1,command_seq:0,clock_ms:20});
  p.track.station.fix=p.track.client.fix=p.track.rf_alive=true;
  const fresh=c.refresh();await p.advance(1000);
  const active=c.httpActive;
  assert(active&&c.httpByKey.track===active);
  oldGate.resolve(stage==='body'?oldState:{ok:true,status:200,json(){++lateBodyReads;return Promise.resolve(oldState);}});
  await settle();
  assert.equal(c.httpActive,active);assert.equal(c.httpByKey.track,active);
  assert.equal(c.trackReceivedMs,1000,'late data must not refresh the lights');
  assert.equal(lateBodyReads,0,'do not even read the body of an expired fetch');
  newGate.resolve(structuredClone(p.track));await fresh;await settle();
  assert.equal(c.motionContext.control_boot_id,2);assert.equal(c.motionContext.clock_ms,20);
  assert.equal(c.last.track.servo.control_boot_id,2);
  assert.equal(p.elements.signalStation.attrs['data-state'],'ok');
  assert.equal(p.elements.signalClient.attrs['data-state'],'ok');
  assert.equal(p.elements.signalLora.attrs['data-state'],'ok');
});

test('a late response expires even when its timeout callback was throttled',async()=>{
  const p=await page(),c=p.c,gate=deferred(),before=c.last.track;
  c.fetch=()=>Promise.resolve({ok:true,status:200,json:()=>gate.promise});
  const read=c.refresh();await settle();p.jump(2500);
  gate.resolve(structuredClone(p.track));await read;await settle();
  assert.equal(c.trackReceivedMs,1000);assert.equal(c.last.track,before);
  assert.equal(c.httpActive,null);assert.equal(c.httpFailures.at(-1).name,'TimeoutError');
});

test('healthy requests serialize through the full body and preserve priority/deduplication',async()=>{
  const p=await page(),c=p.c,original=c.fetch,body=deferred(),wire=[];
  c.fetch=(url,options={})=>{
    const path=new URL(url,'http://test').pathname;wire.push(path);
    return path==='/slow'?Promise.resolve({ok:true,status:200,json:()=>body.promise}):original(url,options);
  };
  const slow=c.httpRequest('/slow',2,'slow');await settle();
  const status=c.httpRequest('/api/status',2,'status');
  assert.equal(c.httpRequest('/api/status',2,'status'),status);
  const post=c.httpRequest('/api/servo?angle=180',0,null,null,'POST',c.controlIntent());
  await settle();assert.deepEqual(wire,['/slow']);
  body.resolve({ok:true});await Promise.all([slow,status,post]);await settle();
  assert.deepEqual(wire,['/slow','/api/servo','/api/status']);
  assert.equal(p.posts.length,1);assert.equal(c.httpActive,null);
});

test('an expired queued command cannot reach the wire even if timers have not fired',async()=>{
  const p=await page(),c=p.c,original=c.fetch,gate=deferred();
  c.fetch=(url,options)=>url==='/slow'?gate.promise:original(url,options);
  const slow=c.httpRequest('/slow',2,'slow').catch(e=>e);await settle();
  const write=c.httpRequest('/api/servo?angle=180',0,null,null,'POST',c.controlIntent()).catch(e=>e);
  p.jump(2100);gate.reject(new Error('disconnected'));await slow;await settle();
  assert.equal((await write).name,'TimeoutError');assert.equal(p.posts.length,0);
  assert.equal(c.httpActive,null);assert.equal(c.httpQueue.length,0);
});

for(const path of ['/api/servo?angle=180','/api/client/control'])test('timed-out POST is never replayed: '+path,async()=>{
  const p=await page(),c=p.c,original=c.fetch,body=deferred();let writes=0;
  c.fetch=(url,options={})=>{
    if(options.method==='POST'){
      ++writes;
      return Promise.resolve({ok:true,status:200,json:()=>body.promise});
    }
    return original(url,options);
  };
  const write=c.httpRequest(path,0,null,null,'POST',c.controlIntent()).catch(e=>e);
  await settle();await p.advance(2000);
  assert.equal((await write).name,'TimeoutError');assert.equal(writes,1);
  await p.advance(1000);await c.refresh();await settle();
  p.hide();await settle();p.show();await p.advance(0);
  body.resolve({ok:true});await settle();
  assert.equal(writes,1);assert.equal(c.httpActive,null);
});

test('background/foreground cancels queued writes and deferred gestures, then reads the new boot',async()=>{
  const p=await page(),c=p.c,original=c.fetch,gate=deferred();let writes=0,reads=0;
  c.fetch=(url,options={})=>{
    if(options.method==='POST')++writes;else ++reads;
    return url==='/slow'?gate.promise:original(url,options);
  };
  const oldIntent=c.controlIntent();
  const slow=c.httpRequest('/slow',2,'slow').catch(e=>e);await settle();
  c.queueServoAngle(150,true);c.queueServoAngle(160,true);
  const client=c.httpRequest('/api/client/control',0,null,null,'POST',null,'action=start',true).catch(e=>e);
  const mode=c.controlAction('/api/servo/mode?mode=gps');
  p.hide();await settle();await Promise.all([slow,client,mode]);
  assert.equal(writes,0);assert.equal(c.httpQueue.length,0);assert.equal(c.httpActive,null);
  assert.equal(c.servoPendingAngle,null);assert.equal(c.servoSending,false);assert.equal(c.controlChanging,false);
  assert.equal(c.trackReceivedMs,null);assert.equal(p.elements.signalLora.attrs['data-state'],'unknown');
  await c.refresh();assert.equal(reads,1,'hidden polling must not issue a fetch');
  Object.assign(p.track.servo,{control_boot_id:2,control_epoch:1,command_seq:0,clock_ms:20});
  const generation=c.httpGeneration;
  p.show();p.windowEvent('pageshow',{persisted:true});p.windowEvent('online');
  assert.equal(c.httpGeneration,generation+1,'coalesce foreground signals');
  await p.advance(0);
  assert.equal(c.motionContext.control_boot_id,2);assert.equal(c.motionContext.clock_ms,20);
  assert.equal(c.trackReceivedMs,1000);assert.equal(c.refreshing,false);
  const stale=await c.httpRequest('/api/servo?angle=180',0,null,null,'POST',oldIntent).catch(e=>e);
  assert.equal(stale.name,'AbortError');
  p.elements.sld.handlers.change.call(p.elements.sld);await settle();
  assert.equal(writes,0,'late slider change cannot submit an old gesture');
  gate.resolve(response({ok:true}));await settle();
  // An explicit new user action, after fresh context, still works normally.
  p.elements.sld.handlers.pointerdown();p.elements.sld.value='180';
  p.elements.sld.handlers.change.call(p.elements.sld);await settle();
  assert.equal(writes,1);assert.equal(p.posts.length,1);
});

test('page-cache restore and online event recover a stalled read without a visibility event',async()=>{
  const p=await page(),c=p.c,original=c.fetch,gate=deferred();
  c.fetch=()=>gate.promise;
  const pending=c.refresh();await settle();
  p.windowEvent('pagehide');await pending;await settle();
  assert.equal(c.httpSuspended,true);assert.equal(c.httpActive,null);
  c.fetch=original;p.windowEvent('pageshow',{persisted:true});await p.advance(0);
  assert.equal(c.httpSuspended,false);assert.equal(c.trackReceivedMs,1000);
  c.fetch=()=>gate.promise;
  const next=c.refresh();await settle();p.windowEvent('online');await next;await settle();
  c.fetch=original;await p.advance(0);
  assert.equal(c.httpActive,null);assert.equal(c.refreshing,false);assert.equal(c.trackReceivedMs,1000);
  assert.equal(p.posts.length,0);
});

test('the foreground HTTPS helper can submit new phone samples to its hidden opener without replaying old samples',async()=>{
  const p=await page(),c=p.c,original=c.fetch,gate=deferred(),positions=[];
  c.fetch=(url,options={})=>{
    if(url==='/slow')return gate.promise;
    if(url==='/api/station/position'&&options.method==='POST'){
      const form=new URLSearchParams(options.body);positions.push(Number(form.get('timestamp')));
      return Promise.resolve(response({source:'phone',valid:true,accuracy_m:5,updated_utc_ms:Number(form.get('timestamp'))}));
    }
    return original(url,options);
  };
  const sample={type:'shore-spotter-phone-position-v1',lat:25,lon:121,accuracy:5,timestamp:Date.now()};
  const slow=c.httpRequest('/slow',2,'slow').catch(e=>e);await settle();
  const old=c.submitStationPhonePosition(sample,true);
  c.submitStationPhonePosition({...sample,timestamp:sample.timestamp+1},true);
  p.hide();await Promise.all([slow,old]);await settle();
  assert.equal(c.queuedPhonePosition,null);assert.equal(c.stationPositionBusy,false);
  assert.deepEqual(positions,[],'discard both old queued samples');
  const popup={};c.phonePopup=popup;
  p.windowEvent('message',{origin:c.PHONE_HELPER_ORIGIN,source:popup,data:{...sample,timestamp:sample.timestamp+2}});
  await settle();assert.deepEqual(positions,[sample.timestamp+2]);
  p.windowEvent('message',{origin:'https://untrusted.example',source:popup,data:sample});
  p.windowEvent('message',{origin:c.PHONE_HELPER_ORIGIN,source:{},data:sample});
  await settle();assert.equal(positions.length,1,'retain origin and popup identity checks');
  p.show();await p.advance(0);gate.resolve(response({ok:true}));await settle();
  assert.equal(positions.length,1,'resuming does not resend a position');
});

test('repeated hung requests have bounded retry rate and failure history',async()=>{
  const p=await page(),c=p.c;let wires=0;
  c.fetch=()=>{++wires;return new Promise(()=>{});};
  for(let i=0;i<30;i++){c.refresh();await p.advance(1000);}
  assert(wires<=11,'at most one wire attempt per timeout plus cooldown');
  c.renderSignalLights();
  for(const id of ['signalStation','signalClient','signalLora'])assert.equal(p.elements[id].attrs['data-state'],'unknown');
  c.fetch=()=>Promise.reject(new Error('offline'));
  await p.advance(3000);
  for(let i=0;i<24;i++)await c.httpRequest('/api/status?private=value',2,'status').catch(()=>{});
  assert.equal(c.httpFailures.length,16);
  assert(c.httpFailures.every(x=>x.path==='/api/status'&&!JSON.stringify(x).includes('private=value')));
  assert.equal(p.posts.length,0);
});
