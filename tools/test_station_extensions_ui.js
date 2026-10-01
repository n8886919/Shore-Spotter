// Focused UI contract test: popup-origin validation and Client pending/confirmed display.
const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict');
const source=fs.readFileSync(process.env.SHORE_WEB_UI_TEST_SOURCE||'include/web_ui.h','utf8');
const html=source.match(/R"(\w+)\(([\s\S]*?)\)\1"/)[2];
const script=html.match(/<script>([\s\S]*?)<\/script>/)[1];
const ids=[...html.matchAll(/\bid="([^"]+)"/g)].map(m=>m[1]);
assert.equal(ids.length,new Set(ids).size,'duplicate HTML id');
const elements={};
const canvas=new Proxy({measureText:t=>({width:String(t).length*6}),createRadialGradient:()=>({addColorStop(){}}),createLinearGradient:()=>({addColorStop(){}})}, {get:(o,k)=>k in o?o[k]:(()=>{})});
for(const id of ids)elements[id]={value:'',textContent:'',disabled:false,checked:false,indeterminate:false,style:{},clientWidth:500,clientHeight:500,
  classList:{toggle(){},add(){},remove(){}},setAttribute(){},getAttribute(){},getContext:()=>canvas,getBoundingClientRect(){return {left:0,top:0,width:500,height:500};},addEventListener(){},setPointerCapture(){}};
const context={console,Promise,Date,Math,Number,URL,encodeURIComponent,performance:{now:()=>0},AbortController,
  setTimeout:()=>1,clearTimeout(){},setInterval:()=>1,clearInterval(){},
  document:{getElementById:id=>elements[id],activeElement:null,documentElement:{style:{setProperty(){}}}},
  window:{location:{href:'http://192.168.1.55/'},addEventListener(){},open(){return null;},visualViewport:null},
  fetch:()=>Promise.resolve({ok:true,status:200,json:()=>Promise.resolve({})})};
vm.createContext(context);vm.runInContext(script,context);
const popup={};
const good={type:'shore-spotter-phone-position-v1',lat:25.031,lon:121.565,accuracy:7.5,timestamp:Date.now()};
assert.deepEqual({...context.phoneLocationMessage({origin:'https://n8886919.github.io',source:popup,data:good},popup)},
  {lat:good.lat,lon:good.lon,accuracy:good.accuracy,timestamp:good.timestamp},'trusted popup accepted');
assert.equal(context.phoneLocationMessage({origin:'https://evil.example',source:popup,data:good},popup),null,'untrusted origin rejected');
assert.equal(context.phoneLocationMessage({origin:'https://n8886919.github.io',source:{},data:good},popup),null,'wrong popup rejected');
assert.equal(context.phoneLocationMessage({origin:'https://n8886919.github.io',source:popup,data:{...good,lat:999}},popup),null,'malformed coordinates rejected');
assert(context.validStationReturnUrl('http://192.168.1.8:80/'));
assert(context.validStationReturnUrl('http://station.local/'));
assert(!context.validStationReturnUrl('https://public.example/'));
context.renderClientControl({supported:true,state:'ready',age_ms:230,command:'pending',command_id:12,test_received:2,test_missing:1,test_rssi:-101,test_snr:4.5,test_max_gap_ms:900});
assert.match(elements.clientControlCommand.textContent,/等待 Client 確認 #12/);
assert.equal(elements.btnClientStart.disabled,false);
context.renderClientControl({supported:true,state:'tracking',age_ms:300,command:'confirmed',command_id:12});
assert.match(elements.clientControlCommand.textContent,/Client 已確認 #12/);
context.renderClientControl({supported:true,state:'ready',age_ms:300,command:'idle',charging:true});
assert.match(elements.clientControlState.textContent,/停止待命/);
assert.equal(elements.btnClientStore.disabled,true,'USB power disables deep-sleep request');
assert.match(elements.clientControlHint.textContent,/先移除 USB/);
context.renderClientControl({supported:true,state:'storage',age_ms:300,command:'confirmed',charging:false});
assert.match(elements.clientControlCommand.textContent,/已回覆收納請求/);
assert.match(elements.clientControlHint.textContent,/不證明已進入深度休眠/);
context.renderClientControl({supported:false});
assert.equal(elements.btnClientTest.disabled,true);
assert.match(elements.clientControlHint.textContent,/不會被標示為待命或深度休眠/);
console.log('PASS station extensions UI: trusted popup only, malformed/untrusted rejected, Client pending/confirmed, USB sleep guard and offline state');
