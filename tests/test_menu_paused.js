const {test}=require('node:test'); const assert=require('node:assert/strict'); const fs=require('node:fs'); const vm=require('node:vm');
test('native PAUSED menu UI readiness uses metadata; native gate separately proves DMA-BUF preroll',async()=>{
 const source=fs.readFileSync('www/menuboard/menuboard.js','utf8'); const fn=source.slice(source.indexOf('  function loadVideo('),source.indexOf('  // Start playback;'));
 const listeners=new Map();let timeout;
 const video={readyState:0,addEventListener:(e,f)=>listeners.set(e,f),removeEventListener:e=>listeners.delete(e),load(){}};
 const context={Q:new URLSearchParams('native_stage=1'),CFG:{canplayTimeout:4000},setTimeout:(f,ms)=>{timeout=ms;return 1;},clearTimeout:()=>{}};vm.createContext(context);vm.runInContext(fn+';this.loadVideo=loadVideo;',context);
 const ready=context.loadVideo(video,'/clip.mp4'); const result=await ready; assert.equal(result.ok,true); assert.equal(result.why,'native-ui-only'); assert.equal(listeners.size,0,'UI-only completion removes DOM readiness listeners');
  });
test('native EOS poster SVG is decoded during preparation, never painted as vectors at EOS',()=>{
 const source=fs.readFileSync('www/menuboard/menuboard.js','utf8');
 assert.match(source,/await preparePosterBitmap\(\)/);
 assert.match(source,/await image\.decode\(\)/);
 assert.match(source,/poster\.replaceChildren\(image\)/);
});

test('native staging suspends hidden animations until actual activation',()=>{
 const source=fs.readFileSync('www/menuboard/menuboard.js','utf8');
 assert.match(source,/document\.getAnimations\(\)\.forEach\(a=>a\.pause\(\)\)/);
 assert.match(source,/document\.getAnimations\(\)\.forEach\(a=>a\.play\(\)\)/);
});

test('sheen owns a transform layer before its delayed motion starts',()=>{
 const css=fs.readFileSync('www/menuboard/menuboard.css','utf8');
 for(const selector of ['.pr::after,.banner::after','.deal .pbadge::after']) {
  const declaration=css.slice(css.indexOf(selector+'{')).split('}')[0];
  assert.match(declaration,/will-change:transform/);
  assert.match(declaration,/animation:sheen 6s ease-in-out infinite/);
 }
 assert.match(css,/@keyframes sheen\{0%,70%\{transform:translateX\(-160%\) skewX\(-18deg\)\}100%\{transform:translateX\(330%\) skewX\(-18deg\)\}\}/);
});

test('native menu keeps poster precomposited away from media plane',()=>{
 const css=fs.readFileSync('www/menuboard/menuboard.css','utf8');
 assert.match(css,/\.poster\{[^}]*will-change:opacity/);
});

test('native scene A is an explicit UI-only lifecycle: no video sink, ui_only declaration',()=>{
 const source=fs.readFileSync('www/menuboard/menuboard.js','utf8');
 const start=source.indexOf("if (only === 'A') {"); assert.ok(start>0);
 const block=source.slice(start,source.indexOf('return;\n      }',start));
 assert.doesNotMatch(block,/makeVideo|runScene|loadVideo/);
 assert.match(block,/document\.fonts\.ready/);
 assert.match(block,/send\(\{type:'ui-ready',ui_only:true\}\)/);
 assert.ok(block.indexOf('a.pause()')<block.indexOf("ui_only:true"));
});
