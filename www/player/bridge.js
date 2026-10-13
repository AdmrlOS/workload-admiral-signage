/* Package lifecycle: paint before reveal; retain the page while releasing its decoder. */
(() => {
  'use strict';
  if(window.__signageLifecycleInstalled) return;
  window.__signageLifecycleInstalled=true;
  const nativeStage=new URLSearchParams(location.search).has('native_stage');
  if (nativeStage) {
    const token=new URLSearchParams(location.search).get('signage_standby');
    const nativePlay=HTMLMediaElement.prototype.play;
    const source=Object.getOwnPropertyDescriptor(HTMLMediaElement.prototype,'src');
    Object.defineProperty(HTMLMediaElement.prototype,'src',{...source,set(value){
      const uri=new URL(value,location.href); uri.searchParams.set('signage_standby',token);
      source.set.call(this,uri.href);
    }});
    let activated=false;
    HTMLMediaElement.prototype.play=function(){ return activated ? nativePlay.call(this):Promise.resolve(); };
    window.addEventListener('signage-native',event=>{
      if(event.detail?.event!=='play') return;
      activated=true;
      window.dispatchEvent(new CustomEvent('signage-activated'));
    });
    window.addEventListener('load',fitCanvas);
    window.addEventListener('resize',fitCanvas);
    // Content opts into its own paused preparation lifecycle. No generic hidden
    // autoplay or curtain timeout can assert readiness on behalf of a package.
    return;
  }
  if (window.parent === window) return;
  let held = false;
  const timers = new Set(), intervals = new Set(), frames = new Set();
  const nativeTimeout = window.setTimeout.bind(window), nativeInterval = window.setInterval.bind(window), nativeFrame = window.requestAnimationFrame.bind(window);
  window.setTimeout = (fn, delay, ...args) => { if (held) return 0; const id = nativeTimeout(typeof fn === 'function' ? () => { timers.delete(id); fn(...args); } : fn,delay); timers.add(id); return id; };
  window.setInterval = (fn, delay, ...args) => { if (held) return 0; const id = nativeInterval(fn,delay,...args); intervals.add(id); return id; };
  window.requestAnimationFrame = (fn) => { if (held) return 0; const id = nativeFrame((time) => { frames.delete(id); fn(time); }); frames.add(id); return id; };
  const afterPaint = () => new Promise((resolve) => { const alarm=nativeTimeout(resolve,250); nativeFrame(() => nativeFrame(() => { clearTimeout(alarm); resolve(); })); });
  const nativePlay = HTMLMediaElement.prototype.play;
  HTMLMediaElement.prototype.play = function () { return held ? new Promise(() => {}) : nativePlay.call(this); };
  async function release(video, posters) {
    if (video.dataset.signageHeld) return;
    video.dataset.signageHeld = '1';
    const bounds = video.getBoundingClientRect();
    let still;
    const known = posters[new URL(video.currentSrc || video.src || location.href,location.href).pathname];
    if (known) { still = new Image(); await new Promise((resolve) => { still.onload = still.onerror = resolve; still.src = known; nativeTimeout(resolve,1000); }); }
    else {
      still = document.createElement('canvas'); still.width = video.videoWidth || 1; still.height = video.videoHeight || 1;
      try { still.getContext('2d').drawImage(video,0,0); } catch (_) { /* The page underneath remains visible. */ }
    }
    Object.assign(still.style,{position:'fixed',left:bounds.left+'px',top:bounds.top+'px',width:bounds.width+'px',height:bounds.height+'px',objectFit:getComputedStyle(video).objectFit,zIndex:'2147483646',pointerEvents:'none'});
    document.body.appendChild(still);
    await afterPaint();
    video.pause(); video.removeAttribute('src'); video.querySelectorAll('source').forEach((source) => source.remove()); video.load();
  }
  window.addEventListener('message',async (event) => {
    if (event.source !== window.parent || event.data?.type !== 'signage-hold' || held) return;
    held = true;
    timers.forEach(clearTimeout); intervals.forEach(clearInterval); frames.forEach(cancelAnimationFrame);
    if (document.getAnimations) document.getAnimations().forEach((animation) => animation.pause());
    await Promise.all(Array.from(document.querySelectorAll('video,audio'), (video) => release(video,event.data.posters || {})));
    await afterPaint(); window.parent.postMessage({type:'signage-held'},'*');
  });
  function fitCanvas() {
    const canvas = document.querySelector('meta[name="signage-canvas"]');
    if (!canvas) return;
    const [width,height] = canvas.content.split('x').map(Number);
    if (!(width > 0 && height > 0)) return;
    const zoom = Math.min(innerWidth / width, innerHeight / height);
    document.body.style.zoom = zoom;
    document.body.style.marginLeft = ((innerWidth / zoom - width) / 2) + 'px';
    document.body.style.marginTop = ((innerHeight / zoom - height) / 2) + 'px';
  }
  window.addEventListener('resize',fitCanvas);
  window.addEventListener('load',async () => {
    fitCanvas();
    if (document.fonts) await document.fonts.ready;
    await Promise.all(Array.from(document.images || [], image=>image.decode ? image.decode() : Promise.resolve()));
    // A static staging declaration is not permission to open a second sink.
    // CSP blocks media in staged responses; reject a dishonest declaration too.
    if(window.__signage_stage && document.querySelectorAll('video,audio,iframe,object,embed').length) {
      window.parent.postMessage({type:'signage-stage-error'},'*'); return;
    }
    await afterPaint(); window.parent.postMessage({type:'signage-ready'},'*');
  });
})();
