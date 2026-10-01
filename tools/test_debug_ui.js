// Removed diagnostic page must not retain controls, pollers or DOM references.
const assert=require('node:assert/strict');
const {createPage,settle,html,script}=require('./test_motion_ui.js');
(async()=>{
 assert.doesNotMatch(html,/tabDebug|pgDebug|btnDebug|debugNote|btnClear|btnExport|Strava|GPX|id="sUp"/);
 assert.doesNotMatch(script,/pollDebug|pollAxiom|buildDebugBundle|exportTrackGpx|fmtUptime/);
 const p=createPage();await settle();
 const calls=[],original=p.c.fetch;
 p.c.fetch=(url,...args)=>{calls.push(url);return original(url,...args);};
 p.elements.tabInfo.onclick();await p.c.refresh();await p.c.refreshStatus();await settle();
 assert.equal(p.c.page,'info');
 assert.equal(calls.filter(u=>/\/api\/(debug|log|axiom)/.test(u)).length,0);
 assert(html.indexOf('id="mcOff"')>html.indexOf('id="pgInfo"'));
 console.log('PASS removed debug/export UI: two pages, no hidden diagnostic polling, calibration remains in Info');
})().catch(e=>{console.error(e);process.exitCode=1;});
