const test=require('node:test');
const assert=require('node:assert/strict');
const vm=require('node:vm');
const {readFileSync}=require('node:fs');

async function bridge(options={}) {
  const handlers={},messages=[]; let finishImage, finishFonts;
  const image={complete:false,decode:()=>new Promise(resolve=>{finishImage=resolve;})};
  const fonts=new Promise(resolve=>{finishFonts=resolve;});
  const parent={postMessage:m=>messages.push(m)};
  class Media { play(){return Promise.resolve();} }
  const window={parent,__signage_stage:!!options.staged,setTimeout, setInterval, requestAnimationFrame:fn=>queueMicrotask(fn),addEventListener:(key,fn)=>{handlers[key]=fn;}};
  const document={fonts:{ready:fonts},images:[image],querySelector:()=>null,querySelectorAll:()=>options.media || [],body:{style:{}},getAnimations:()=>[]};
  vm.runInNewContext(readFileSync('www/player/bridge.js','utf8'),{window,document,HTMLMediaElement:Media,Image:class {},setTimeout,clearTimeout,clearInterval,cancelAnimationFrame(){},location:{href:'http://test/content/page'},URL,Promise});
  handlers.load();
  const settle=async()=>{for(let i=0;i<30;i++)await Promise.resolve();};
  await settle();
  return {messages,finishFonts:async()=>{finishFonts();await settle();},finishImage:async()=>{image.complete=true;finishImage?.();await settle();}};
}

test('package readiness waits for fonts and image decode, not just load',async()=>{
  const h=await bridge();
  assert.equal(h.messages.length,0);
  await h.finishFonts();
  assert.equal(h.messages.length,0,'Fonts alone must not expose undecoded images');
  await h.finishImage();
  assert.equal(h.messages[0]?.type,'signage-ready');
});
