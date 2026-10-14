// Built-in welcome: explicit UI-only native lifecycle (fonts + decoded images + paused animations).
const {test}=require('node:test'); const assert=require('node:assert/strict'); const fs=require('node:fs'); const vm=require('node:vm');
const html=fs.readFileSync('www/welcome/index.html','utf8');
const script=html.slice(html.lastIndexOf('<script>')+8,html.lastIndexOf('</script>'));
function run(search,{media=false,complete=true}={}) {
  const sent=[],listeners={},animations=[{state:'running',pause(){this.state='paused';},play(){this.state='running';}}];
  let decoded=0;
  const document={readyState:complete?'complete':'loading',fonts:{ready:Promise.resolve()},
    images:[{decode:async()=>{decoded++;}}],querySelector:()=>media?{}:null,getAnimations:()=>animations};
  const window={signageNative:{send:m=>sent.push(m)}};
  const context={URLSearchParams,location:{search},document,window,addEventListener:(t,f)=>(listeners[t]=f)};
  vm.runInNewContext(script,context);
  return {sent,listeners,animations,decoded:()=>decoded};
}
const settle=async()=>{for(let i=0;i<20;i++) await Promise.resolve();};
test('welcome loads the bridge and keeps native 4K CSS (no zoom/scale hacks)',()=>{
  assert.match(html,/<script src="\/player\/bridge.js"><\/script>/);
  assert.doesNotMatch(html,/zoom|initial-scale=0|scale\(0\.5\)/);
});
test('native_stage welcome declares UI-only ready only after fonts and decoded images, animations paused',async()=>{
  const r=run('?native_stage=1&signage_standby=5'); await settle();
  assert.equal(JSON.stringify(r.sent),JSON.stringify([{type:'ui-ready',ui_only:true}]));
  assert.equal(r.decoded(),1); assert.equal(r.animations[0].state,'paused');
  r.listeners['signage-activated'](); assert.equal(r.animations[0].state,'running');
});
test('welcome is not ready while loading and never declares UI-only if media exists',async()=>{
  const loading=run('?native_stage=1',{complete:false}); await settle(); assert.equal(loading.sent.length,0);
  loading.listeners.load(); await settle(); assert.equal(loading.sent.length,1);
  const media=run('?native_stage=1',{media:true}); await settle(); assert.equal(media.sent.length,0);
});
test('ordinary welcome (no native_stage) sends nothing to the native bridge',async()=>{
  const r=run(''); await settle(); assert.equal(r.sent.length,0); assert.equal(r.animations[0].state,'running');
});
