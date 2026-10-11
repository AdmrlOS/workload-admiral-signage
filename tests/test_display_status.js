const assert = require('node:assert/strict');
const test = require('node:test');
const displayStatus = require('../www/studio/display-status.js');
const config = {browser_url:'http://127.0.0.1:8080/player/',player_url:'http://127.0.0.1:8080/player/'};
test('native standalone menu triggers a warning even when configured for the player',()=>{
  assert.equal(displayStatus({browser:{uri:'http://127.0.0.1:8080/menuboard/'}},config).help,true);
});
test('native local player accepts query strings and loopback aliases',()=>{
  for (const uri of ['http://127.0.0.1:8080/player/','http://localhost:8080/player/?preview=1']) {
    assert.equal(displayStatus({browser:{uri}},config).tone,'');
  }
});
test('another server or port cannot confirm this scheduled loop',()=>{
  for (const uri of ['http://other.test:8080/player/','http://127.0.0.1:8099/player/','invalid']) {
    assert.equal(displayStatus({browser:{uri}},config).help,true);
  }
});
test('server-only preview never claims the physical display is playing',()=>{
  assert.equal(displayStatus({},config).title,'Browser preview only');
});

test('diagnostic failures are not presented as a server-only preview',()=>{
  assert.equal(displayStatus({error:'status unavailable'},config).title,'Device status unavailable');
});
