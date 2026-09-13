// Run with: node tools/test_servo_slider.js
// Exercise the real page handlers with a virtual clock and a fake HTTP transport.
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const test = require('node:test');
const assert = require('node:assert/strict');

const source = fs.readFileSync(process.env.SHORE_WEB_UI_TEST_SOURCE ||
  path.join(__dirname, '../include/web_ui.h'), 'utf8');
const globalsStart = source.indexOf('var dragging=false;');
const globalsEnd = source.indexOf("var viewMode='radar';", globalsStart);
const controlsStart = source.indexOf('function sendPendingServoAngle(){');
const controlsEnd = source.indexOf("$('mGps').onclick=", controlsStart);
assert(globalsStart >= 0 && globalsEnd > globalsStart);
assert(controlsStart >= 0 && controlsEnd > controlsStart);
const helpersStart=source.indexOf('var motionContext=null;');
const helpersEnd=source.indexOf('function post(',helpersStart);
assert(helpersStart>=0&&helpersEnd>helpersStart);
const script = source.slice(helpersStart,helpersEnd)+'\n'+source.slice(globalsStart, globalsEnd) + '\n' +
  source.slice(controlsStart, controlsEnd);
const modeStart = source.indexOf('function applyMode(sv){');
const modeEnd = source.indexOf('\n}\n', modeStart) + 2;
assert(modeStart >= 0 && modeEnd > modeStart);
const modeScript = source.slice(modeStart, modeEnd);

function page(responseMs = 10) {
  let now = 0, timerId = 0, inFlight = 0, maxInFlight = 0;
  const timers = new Map(), elements = {}, posts = [];
  function setTimer(fn, delay) {
    const id = ++timerId;
    timers.set(id, { at: now + delay, fn });
    return id;
  }
  function element(id) {
    if (!elements[id]) elements[id] = {
      value: '90', disabled: false, handlers: {}, style: {},
      classList: { toggle() {} },
      addEventListener(name, fn) { this.handlers[name] = fn; }
    };
    return elements[id];
  }
  const context = {
    $: element, Promise, performance: { now: () => now },
    document: { activeElement: null },
    setTimeout: setTimer, clearTimeout: id => timers.delete(id),
    postServoAngle(value) {
      posts.push({ kind: 'angle', at: now, angle: Number(value) });
      maxInFlight = Math.max(maxInFlight, ++inFlight);
      return new Promise(resolve => setTimer(() => { --inFlight; resolve(); }, responseMs));
    },
    post(url) { posts.push({ kind: 'mode', at: now, url }); return Promise.resolve(); },
    refresh: () => Promise.resolve()
  };
  vm.createContext(context);
  vm.runInContext(script, context);
  vm.runInContext(modeScript, context);
  async function settle() {
    for (let i = 0; i < 8; ++i) await Promise.resolve();
  }
  async function advance(to) {
    assert(to >= now);
    await settle();
    while (true) {
      const next = [...timers.entries()].filter(([, t]) => t.at <= to)
        .sort((a, b) => a[1].at - b[1].at || a[0] - b[0])[0];
      if (!next) break;
      timers.delete(next[0]); now = next[1].at;
      next[1].fn(); await settle();
    }
    now = to; await settle();
  }
  function event(name, angle) {
    const slider = element('sld'); slider.value = String(angle);
    slider.handlers[name].call(slider);
  }
  return { context, posts, advance, input: v => event('input', v),
    release: v => event('change', v), maxInFlight: () => maxInFlight };
}

test('manual ramp keeps the slider target and blocks compass calibration until settled', () => {
  const p = page();
  p.context.applyMode({ mode: 'manual', angle: 95, target: 120, moving: true });
  assert.equal(p.context.$('sld').value, 60);
  assert.equal(p.context.$('sld').disabled, false);
  assert.equal(p.context.$('btnCompassCal').disabled, true);
  assert.match(p.context.$('controlState').textContent, /85° → 60°/);
  p.context.applyMode({ mode: 'manual', angle: 120, target: 120, moving: false });
  assert.equal(p.context.$('btnCompassCal').disabled, false);
  assert.equal(p.context.$('controlState').textContent, '手動');
});

test('mode polling preserves active drag and still disables manual controls for GPS and UART', () => {
  const p = page();
  p.context.dragging = true;
  p.context.$('sld').value = 130;
  p.context.applyMode({ mode: 'manual', angle: 95, target: 120, moving: true });
  assert.equal(p.context.$('sld').value, 130);
  for (const mode of ['gps', 'uart']) {
    p.context.applyMode({ mode, source: mode, angle: 95, target: 120 });
    assert.equal(p.context.$('sld').disabled, true);
    assert.equal(p.context.$('btnCompassCal').disabled, true);
  }
});

test('dragging only previews; releasing sends one inverted target',async()=>{
 const p=page();for(let t=0;t<1000;t+=16){await p.advance(t);p.input(t%181);}
 await p.advance(1000);assert.equal(p.posts.length,0);
 p.release(180);await p.advance(1100);
 assert.deepEqual(p.posts,[{kind:'angle',at:1000,angle:0}]);assert.equal(p.context.dragging,false);
});
test('slow replies keep only the latest committed release',async()=>{
 const p=page(500);p.input(10);p.release(10);
 await p.advance(20);p.input(30);p.release(30);
 await p.advance(40);p.input(99);p.release(99);
 await p.advance(1000);
 assert.deepEqual(p.posts,[{kind:'angle',at:0,angle:170},{kind:'angle',at:500,angle:81}]);
 assert.equal(p.maxInFlight(),1);
});
test('new drag preview never leaks into a pending committed request',async()=>{
 const p=page(100);p.release(20);await p.advance(20);p.input(160);await p.advance(200);
 assert.equal(p.posts.length,1);assert.equal(p.posts[0].angle,160);assert.equal(p.context.dragging,true);
 p.release(160);await p.advance(400);assert.equal(p.posts[1].angle,20);
});
test('pointer cancel does not commit a preview',async()=>{
 const p=page();p.input(10);p.context.$('sld').handlers.pointercancel();p.release(10);
 await p.advance(100);assert.equal(p.posts.length,0);
 p.context.$('sld').handlers.pointerdown();p.input(0);p.release(0);await p.advance(200);
 assert.equal(p.posts[0].angle,180);
});
test('mode switch cancels committed pending targets and waits for in-flight request',async()=>{
 const p=page(100);p.release(10);await p.advance(20);p.release(30);
 const changed=p.context.controlAction('/api/servo/mode?mode=gps');await p.advance(300);await changed;
 assert.deepEqual(p.posts,[{kind:'angle',at:0,angle:170},{kind:'mode',at:100,url:'/api/servo/mode?mode=gps'}]);
 assert.equal(p.context.servoPendingAngle,null);
});
test('mode switch during preview sends no manual target',async()=>{
 const p=page();p.input(10);const changed=p.context.controlAction('/api/servo/mode?mode=uart');
 await p.advance(100);await changed;assert.deepEqual(p.posts.map(x=>x.kind),['mode']);
});
test('settled small target displays inverted angle and allows calibration',()=>{
 const p=page();p.context.applyMode({mode:'manual',angle:90,target:91,moving:false,deadband_hold:true});
 assert.equal(p.context.$('sld').value,89);assert.equal(p.context.$('btnCompassCal').disabled,false);
});

test('motion request context preserves sequence and rejects stale status rollback',()=>{
  const p=page(),c=p.context;
  assert.throws(()=>c.motionUrl('/api/servo?angle=90'));
  c.syncMotionContext({control_boot_id:1,control_epoch:10,command_seq:2,clock_ms:100});
  assert.equal(c.motionUrl('/api/servo?angle=90'),'/api/servo?angle=90&epoch=10&seq=3&stamp=100');
  c.syncMotionContext({control_boot_id:1,control_epoch:10,command_seq:2,clock_ms:100});
  assert.equal(c.motionContext.command_seq,3);
  c.syncMotionContext({control_boot_id:1,control_epoch:11,command_seq:0,clock_ms:200});
  c.syncMotionContext({control_boot_id:1,control_epoch:10,command_seq:2,clock_ms:150});
  assert.equal(c.motionContext.control_epoch,11);
  c.syncMotionContext({control_boot_id:2,control_epoch:20,command_seq:0,clock_ms:10});
  assert.equal(c.motionContext.control_epoch,20);
  assert.equal(c.motionContext.command_seq,0);
});
