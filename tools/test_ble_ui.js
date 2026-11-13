// Production UI simulation: guarded binding edits, BLE ownership, channel workflow.
const assert=require('node:assert/strict');
const {createPage,settle,html}=require('./test_motion_ui.js');
const event={preventDefault(){}};
(async()=>{
 const {c,elements:e}=createPage();await settle();
 assert.match(html,/id="btnClientStart"[^>]*>開啟位置回報<\/button>/);
 assert.match(html,/id="btnClientStop"[^>]*>關閉位置回報<\/button>/);
 assert(!html.includes('cancelClientRadio'));
 let state={api_version:2,client_id:64295,supported:true,state:'ready',age_ms:100,
   command:'idle',can_control:true,ble:{state:'connected',connected:true,claimed:true}};
 let radio={selected:1,active:1,saved:1,verified:false,scan_state:'idle',scan_id:0,
   can_scan:true,can_select:true,candidates:[
    {id:0,label:'A',frequency_mhz:923.2,samples:0,busy:0,error:0},
    {id:1,label:'B',frequency_mhz:923.8,samples:0,busy:0,error:0}]};
 c.httpRequest=path=>Promise.resolve(path==='/api/client/control'?state:radio);
 const writes=[];let release;
 c.formPost=(path,body)=>{writes.push({path,body});return new Promise(r=>release=r);};
 c.renderClientControl(state);c.renderClientRadio(radio);
 assert.match(html,/<button id="clientPairedId"[^>]+linkButton/);
 e.clientPairedId.onclick();assert(e.clientBindingDialog.open);assert.equal(writes.length,0);
 e.clientBindingInput.value='F035';e.cancelClientBinding.onclick();assert(!e.clientBindingDialog.open);assert.equal(writes.length,0);
 e.clientPairedId.onclick();
 for(const invalid of ['','0000','0010','FFFF','XYZ','12345','0xAB']){
  e.clientBindingInput.value=invalid;await e.clientBindingForm.onsubmit(event);
  assert.equal(writes.length,0);assert.match(e.clientBindingError.textContent,/有效/);
 }
 e.clientBindingInput.value='f035';const save=e.clientBindingForm.onsubmit(event);
 assert.equal(writes[0].path,'/api/whitelist');assert.equal(writes[0].body.id,'F035');
 assert.equal(e.clientPairedId.textContent,'FB27');assert(e.saveClientBinding.disabled);
 await e.clientBindingForm.onsubmit(event);assert.equal(writes.length,1);
 state={...state,client_id:61493,supported:false,can_control:false,ble:{state:'searching'}};
 release({whitelist:['F035']});await save;await settle();
 assert.equal(e.clientPairedId.textContent,'F035');assert(!e.clientBindingDialog.open);assert(e.btnClientStart.disabled);
 // A failed update preserves the confirmed identity and leaves the editor open.
 e.clientPairedId.onclick();e.clientBindingInput.value='FB27';
 c.formPost=()=>Promise.reject(new Error('save failed'));
 await e.clientBindingForm.onsubmit(event);await settle();
 assert.equal(e.clientPairedId.textContent,'F035');assert(e.clientBindingDialog.open);assert.match(e.clientBindingError.textContent,/未確認/);
 e.cancelClientBinding.onclick();
 // An earlier status read may not overwrite the result of a later ID update.
 let oldReply;c.httpRequest=()=>new Promise(r=>oldReply=r);
 const old=c.refreshClientControl();++c.clientBindingEpoch;
 oldReply({...state,client_id:64295});await old;assert.equal(e.clientPairedId.textContent,'F035');
 c.httpRequest=path=>Promise.resolve(path==='/api/client/control'?state:radio);
 state={...state,supported:true,can_control:false,ble:{state:'owned_elsewhere',owner_station_id:1234}};
 c.renderClientControl(state);assert(e.btnClientStart.disabled);assert.match(e.clientControlHint.textContent,/重開 Client/);
 assert.equal(e.clientPairedId.textContent,'F035');
 state={...state,can_control:true,ble:{state:'connected',claimed:true}};
 c.renderClientControl(state);assert(!e.btnClientStart.disabled);assert(e.btnClientStop.disabled);
 c.renderClientControl({...state,command:'pending'});assert(e.btnClientStart.disabled);assert(e.clientPairedId.disabled);
 c.renderClientControl(state);
 await e.clientChannelValue.onclick();assert(e.clientRadioDialog.open);assert(e.clientRadioB.checked);
 const dialog=e.clientRadioDialog;
 function tap(target,x,y){const ev={target,clientX:x,clientY:y};dialog.handlers.pointerdown(ev);dialog.handlers.click(ev);}
 const beforeDismiss=writes.length;
 e.clientRadioA.checked=true;e.clientRadioB.checked=false;
 tap(e.clientRadioA,10,10);assert(dialog.open); // child content
 tap(dialog,20,20);assert(dialog.open); // dialog padding
 dialog.handlers.pointerdown({target:e.clientRadioA,clientX:20,clientY:20});
 dialog.handlers.click({target:dialog,clientX:-1,clientY:-1});assert(dialog.open); // drag out
 tap(dialog,-1,-1);assert(!dialog.open);assert.equal(writes.length,beforeDismiss);
 await e.clientChannelValue.onclick();assert(e.clientRadioB.checked&&!e.clientRadioA.checked);
 c.clientRadioBusy=true;tap(dialog,-1,-1);assert(dialog.open);c.clientRadioBusy=false;
 c.formPost=(path,body)=>{writes.push({path,body});return Promise.resolve(radio);};
 radio={...radio,scan_state:'scanning',can_scan:false,can_select:false};
 await e.scanClientRadio.onclick();await settle();assert(e.selectClientRadio.disabled);assert(e.scanClientRadio.disabled);
 assert.match(e.clientRadioState.textContent,/掃描中/);
 radio={...radio,scan_state:'complete',can_scan:true,can_select:true,
   candidates:radio.candidates.map(x=>({...x,samples:5,busy:x.id,error:0}))};
 await c.refreshClientRadio();assert.match(e.clientRadioAResult.textContent,/0／5/);assert(!e.selectClientRadio.disabled);
 e.clientRadioA.checked=true;e.clientRadioB.checked=false;
 c.formPost=(path,body)=>{writes.push({path,body});radio={...radio,selected:body.channel};return Promise.resolve(radio);};
 await e.clientRadioForm.onsubmit(event);await settle();
 assert.equal(writes.at(-1).body.action,'select');assert.equal(writes.at(-1).body.channel,0);
 assert(!e.clientRadioDialog.open);assert.match(e.clientChannelValue.textContent,/待驗證/);
 radio={...radio,can_scan:false,can_select:false};await c.refreshClientRadio();assert(e.scanClientRadio.disabled);assert(e.selectClientRadio.disabled);
 assert(writes.every(x=>!['release','takeover'].includes(x.body.action)));
 console.log('PASS BLE UI: explicit ID confirmation, validation, failed/stale replies, immutable owner, pending gates, scan/select and no premature saved state');
})().catch(err=>{console.error(err);process.exitCode=1;});
