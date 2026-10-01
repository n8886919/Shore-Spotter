// Axiom configuration remains API-only after removal of the debug page.
const assert=require('node:assert/strict');
const fs=require('node:fs');
const html=fs.readFileSync('include/web_ui.h','utf8');
const backend=fs.readFileSync('src/main.cpp','utf8');
assert.doesNotMatch(html,/axiomToken|axiomSettings|pollAxiom|saveAxiom/);
assert.match(backend,/httpServer.on\("\/api\/axiom", HTTP_GET/);
assert.match(backend,/httpServer.on\("\/api\/axiom", HTTP_POST, handleAxiomSettings/);
console.log('PASS Axiom UI removed; existing GET/POST configuration endpoints remain reachable');
