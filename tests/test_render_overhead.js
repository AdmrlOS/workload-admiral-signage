const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
test('unchanged labels do not invalidate page rendering', () => {
  const source = fs.readFileSync('www/app.js', 'utf8');
  const fn = source.match(/function setText\(id, text\) \{[^\n]+\}/)[0];
  let value='12:00', writes=0;
  const node={get textContent(){return value}, set textContent(text){writes++;value=text}};
  const context={byId:()=>node}; vm.createContext(context); vm.runInContext(fn,context);
  for(let i=0;i<10000;i++) context.setText('clock','12:00');
  assert.equal(writes,0);
  context.setText('clock','12:01'); assert.equal(writes,1);
});
