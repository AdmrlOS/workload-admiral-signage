(() => {
  'use strict';
  const stage = document.getElementById('stage');
  const nativeParams=new URLSearchParams(location.search);
  // A separate native WPE export, not a hidden DOM video in the outgoing view.
  // PAUSED preroll retains its first DMA-BUF while outgoing motion continues.
  if(nativeParams.has('native_surface')) {
    const video=document.createElement('video'); video.className='surface video';
    video.muted=true; video.defaultMuted=true; video.playsInline=true; video.preload='auto';
    const uri=new URL(nativeParams.get('native_surface'),location.href);
    uri.searchParams.set('signage_standby',nativeParams.get('signage_standby'));
    stage.appendChild(video); video.src=uri.href; video.load();
    video.addEventListener('loadeddata',async()=>{
      if(document.fonts) await document.fonts.ready;
      window.signageNative?.send({type:'ui-ready'});
    },{once:true});
    window.addEventListener('signage-native',event=>{
      if(event.detail?.event==='play') video.play().catch(()=>{});
    });
    video.onended=()=>window.signageNative?.send({type:'ended'});
    return;
  }
  let nativeStage=null, nativeRequest=0, nativeActiveGeneration=null;
  // Native standby admission is fail-closed (capacity/TEST_ONLY/deadline). A refusal must never
  // park playback: remember it briefly so lookahead does not re-stage every poll, and let advance()
  // take the explicit, non-gapless serial path instead of retrying the same refusal forever.
  // A refusal can be transient (the previous generation is still retiring right after an activation)
  // or permanent (capacity). Back off 1,2,4..30 s per entry instead of re-staging on every poll.
  const NATIVE_REFUSAL_MIN_MS=1000, NATIVE_REFUSAL_MAX_MS=30000, NATIVE_PREROLL_BOUND_MS=20000;
  let nativeRefused={key:null,until:0,count:0};
  const nativeRefusedFor=key=>nativeRefused.key===key && Date.now()<nativeRefused.until;
  function noteNativeRefusal(key) {
    const count=nativeRefused.key===key ? nativeRefused.count+1 : 1;
    nativeRefused={key,count,until:Date.now()+Math.min(NATIVE_REFUSAL_MAX_MS,NATIVE_REFUSAL_MIN_MS*2**(count-1))};
  }
  function nativeSend(message) { window.signageNative?.send(message); }
  // Persistent single-decoder engine: one native pipeline for every engine-prepared video.
  // Engine videos never create a <video>/WebKit pipeline; handoffs between them are queued
  // on the engine timeline (at:'boundary') and confirmed by the first PRESENTED frame.
  let engine=null, engineBox=null, engineState='off', engineNext=null, engineHost=null;
  const engineRecent=[], engineWaiters=new Map();
  // Every engine play request, whoever issues it, is accounted from request to first PRESENTED:
  // serial (per page), owner token (engine client id), playlist entry, engine disposition
  // (started/switching/queued/loading), activated, started (first PRESENTED frame) and outcome.
  // Validation reads window.signageEngineLedger instead of inferring from skips in the log.
  const engineLedger=[]; let engineSerial=0, enginePreparing=0;
  window.signageEngineLedger=engineLedger;
  function ledgerOpen(entry,via,at) {
    const rec={serial:++engineSerial,via,entry:entry?.id ?? null,asset:entry?.asset?.id ?? null,owner:engine?.client ?? null,at,
      requested:Date.now(),playId:null,disposition:null,code:null,acked:null,activated:null,started:null,superseded:null,
      retries:0,outcome:'pending'};
    engineLedger.push(rec); if(engineLedger.length>400) engineLedger.shift();
    return rec;
  }
  function ledgerClose(rec,outcome,detail) {
    if(!rec || (rec.outcome!=='pending' && rec.outcome!=='queued')) return;
    rec.outcome=outcome; rec.closed=Date.now(); if(detail) rec.detail=detail;
    try { console.log('[engine-ledger] '+JSON.stringify(rec)); } catch(_) { /* never let accounting break playback */ }
  }
  const ledgerFor=playId=>{ for(let i=engineLedger.length-1;i>=0;i--) if(engineLedger[i].playId===playId) return engineLedger[i]; return null; };
  const engineCapable=entry=>!!(entry && entry.asset.kind==='video' && entry.asset.engine && engineState!=='failed');
  // The built-in menuboard creates its videos through the engine adapter, so it needs the engine
  // controller even when no playlist item is itself an engine video.
  const engineBuiltinPage=entry=>!!(entry && entry.asset.kind==='html' && entry.asset.builtin &&
    new URL(entry.asset.url,location.href).pathname==='/content/menuboard/index.html');
  function engineSetup() {
    if(engine || !window.SignageVideo || !playlist.some(entry=>engineCapable(entry)||engineBuiltinPage(entry))) return engine;
    engineBox=document.createElement('div'); engineBox.className='surface engine'; engineBox.style.zIndex='0';
    stage.appendChild(engineBox);
    engine=window.SignageVideo.attach(engineBox); engineState='starting';
    engine.ready.then(()=>{ engineState='ready'; dbg('engine ready'); })
      .catch(error=>{ engineState='failed'; console.warn('Video engine unavailable; legacy <video> path:',error.message); });
    engine.addEventListener('engine-disconnected',()=>{ if(engineState==='ready') engineState='starting'; });
    engine.addEventListener('engine-ready',()=>{ if(engineState!=='failed') engineState='ready'; });
    engine.addEventListener('started',event=>engineStarted(event.detail));
    engine.addEventListener('activated',event=>{ const rec=ledgerFor(event.detail.play_id); if(rec) rec.activated=Date.now(); dbg('engine activated',{play_id:event.detail.play_id,asset:event.detail.asset}); });
    engine.addEventListener('superseded',event=>engineSuperseded(event.detail));
    engine.addEventListener('ended',event=>engineEnded(event.detail));
    engine.addEventListener('waiting',event=>dbg('engine waiting',{asset:event.detail.asset}));
    return engine;
  }
  async function engineReady() {
    engineSetup();
    if(!engine) return false;
    try { await engine.ready; } catch(_) { return false; }
    if(engineState==='released') {
      const back=new Promise(resolve=>{ const fn=()=>{ engine.removeEventListener('engine-ready',fn); resolve(); }; engine.addEventListener('engine-ready',fn); });
      await engine.acquire(); await back; engineState='ready';
    }
    return engineState==='ready';
  }
  async function engineRelease() {
    // Legacy asset without a prepared variant: free the only decoder explicitly (non-gapless).
    if(!engine || engineState!=='ready') return;
    engineNext=null; await engine.release().catch(()=>{}); engineState='released';
  }
  function engineSuperseded(event) {
    const rec=ledgerFor(event.play_id);
    if(rec) { rec.superseded={by_play_id:event.by_play_id ?? null,by_client:event.by_client ?? null,by_asset:event.by_asset ?? null,t:Date.now()}; }
    dbg('engine superseded',event);
    // A request of OURS replaced before it ever started can never start: fail it now with the
    // cause instead of letting the watchdog report a generic "did not start" 15 s later.
    engineWaiters.get(event.play_id)?.reject(new Error('Engine request superseded by '+(event.by_asset||'?')+' ('+(event.by_client||'?')+')'));
  }
  function engineStarted(event) {
    const rec=ledgerFor(event.play_id);
    if(rec && !rec.started) rec.started=Date.now();
    engineRecent.push(event.play_id); if(engineRecent.length>16) engineRecent.shift();
    engineWaiters.get(event.play_id)?.resolve(); engineWaiters.delete(event.play_id);
    if(rec && rec.via==='gapless' && rec.outcome==='queued') ledgerClose(rec,'started');
    if(engineNext && event.play_id===engineNext.playId && current?.engine) {
      // Gapless handoff already happened on the engine timeline; follow it.
      const entry=engineNext.entry; engineNext=null;
      current.entry=entry; current.playId=event.play_id; current.held=false;
      notify(entry); arm(current); lookahead();
    }
  }
  function engineEnded(event) {
    if(!current?.engine || current.playId!==event.play_id) return;
    if(event.reason==='complete' && !engineNext) current.finish?.('ended');
  }
  window.addEventListener('signage-native',event=>{
    const data=event.detail;
    if(data.event==='ended' && current?.native && data.generation===nativeActiveGeneration) { advance(); return; }
    if(!nativeStage || data.request!==nativeStage.request) return;
    if(data.event==='prepared') { nativeRefused={key:null,until:0,count:0}; nativeStage.generation=data.generation; nativeStage.ready=true; nativeStage.prepared?.(true); }
    else if(data.event==='not-ready' || data.event==='cancelled' || data.event==='failed') {
      nativeStage.error=data.event; noteNativeRefusal(nativeStage.key);
      console.warn('Native standby refused:',data.event); nativeStage.resolve?.(false); nativeStage.prepared?.(false);
    } else if(data.event==='activated' && data.generation===nativeStage.generation) nativeStage.resolve?.(true);

  });
  const preview = new URLSearchParams(location.search).has('preview');
  if (preview) document.body.classList.add('preview');
  let playlist = [], current = null, transitioning = false, requested = false, timer = 0, pollTimer = 0, boundaryTimer = 0, clockTimer = 0;
  let lastPoll = 0, previousRevision = null, stopped = false;
  let staged = null, stageGeneration = 0, cacheWindow = '', preparingInitial=false;
  const images = new Map();
  const delay = (ms) => new Promise((resolve) => setTimeout(resolve,ms));
  // Hand-off timeline marks for on-device timing: ?debug=1 or localStorage.signageDebug='1'. No-op otherwise.
  const debugOn = (() => { try { return /[?&]debug=1(&|$)/.test(location.search) || localStorage.getItem('signageDebug') === '1'; } catch (_) { return false; } })();
  const dbg = (ev, extra) => { if (debugOn) console.log('[player] ' + JSON.stringify({t:(typeof performance !== 'undefined' ? performance.now() : Date.now()).toFixed(1), ev, ...extra})); };
  const paint = () => new Promise((resolve) => { const alarm=setTimeout(resolve,250); requestAnimationFrame(() => requestAnimationFrame(() => { clearTimeout(alarm); resolve(); })); });
  function imageReady(url) {
    while (!images.has(url) && images.size >= 8) images.delete(images.keys().next().value);
    if (!images.has(url)) images.set(url,new Promise((resolve,reject) => {
      const image = new Image(); image.onload = () => resolve(image); image.onerror = () => { images.delete(url); reject(new Error('Image unavailable')); }; image.src = url;
    }));
    return images.get(url);
  }
  function warm(entry) {
    if (!entry) return;
    const a = entry.asset;
    for (const url of [a.poster,a.end_poster,a.kind === 'image' ? a.url : null].filter(Boolean)) imageReady(url).catch(() => {});
    // Compressed media is warmed by the server's bounded anonymous-RAM cache.
    // Never create another live video sink merely to warm a URL.
  }
  const signature = (entry) => JSON.stringify(entry);
  function cancelStage() {
    if(nativeStage) { nativeSend({type:'native-cancel'}); nativeStage.resolve?.(false); nativeStage.prepared?.(false); nativeStage=null; }
    stageGeneration++;
    if (staged) { remove(staged.item); staged=null; }
  }
  function lookahead() {
    const index=current ? playlist.findIndex(e=>e.id===current.entry.id) : -1;
    const next=playlist.length ? playlist[(index+1)%playlist.length] : null;
    const urls=[current?.entry,next].filter(Boolean).flatMap(e=>[e.asset.url,e.asset.poster,e.asset.end_poster].filter(Boolean));
    const key=JSON.stringify(urls);
    if(key!==cacheWindow) {
      cacheWindow=key;
      fetch('/api/preload',{method:'POST',headers:{'Content-Type':'application/json','X-Signage-Request':'player'},body:JSON.stringify({urls})}).then(response=>{if(!response.ok) cacheWindow='';}).catch(()=>{ cacheWindow=''; });
    }
    warm(next);
    if(current?.engine) {
      if(engineNext && engineNext.key!==signature(next)) { engine.cancel(engineNext.entry.asset.url); engineNext=null; }
      if(!engineNext && !enginePreparing && engineCapable(next) && engineState==='ready') {
        const slot={key:signature(next),entry:next,playId:null,rec:ledgerOpen(next,'gapless','boundary')}; engineNext=slot;
        engine.play(next.asset.url,{at:'boundary'}).then(ack=>{
          slot.rec.acked=Date.now(); slot.rec.disposition=ack.message || (ack.ok ? 'accepted' : 'refused'); slot.rec.code=ack.code ?? null;
          if(ack.ok) { slot.rec.playId=ack.play_id; slot.rec.outcome='queued'; }
          else ledgerClose(slot.rec,'refused',ack.code);
          if(engineNext!==slot) { if(ack.ok) slot.rec.detail='replaced before acknowledgement'; return; }
          if(ack.ok) slot.playId=ack.play_id; else { engineNext=null; console.warn('Engine refused next item:',ack.code); }
        });
      }
      if(engineCapable(next)) return;
    }
    if((staged && staged.key===signature(next)) || (nativeStage && !nativeStage.error && nativeStage.key===signature(next))) return;
    cancelStage();
    if(window.signageNative && !engine && current && next && !nativeRefusedFor(signature(next)) &&
        (next.asset.kind==='video' || next.asset.kind==='html') &&
        (current.entry.asset.kind==='video' || current.native) &&
        (next.asset.url!==current.entry.asset.url || current.native)) {
      const uri=next.asset.kind==='video' ? new URL('/player/',location.href) : new URL(next.asset.url,location.href);
      if(next.asset.kind==='video') uri.searchParams.set('native_surface',new URL(next.asset.url,location.href).href);
      else {
        // Built-in seeds persist in /data and WebKit caches JS for an hour.
        // Stage the matching image's lifecycle without mutating published data.
        if (next.asset.builtin && uri.pathname==='/content/menuboard/index.html')
          uri.pathname='/native-builtin/menuboard/index.html';
        uri.searchParams.set('native_stage','1');
      }
      const slot={key:signature(next),request:++nativeRequest,ready:false,generation:null};
      nativeStage=slot;
      nativeSend({type:'native-stage',request:slot.request,uri:uri.href});
      return;
    }
    // Static opt-in packages are enforced media-free by response CSP. Arbitrary
    // HTML (which can create video dynamically) gets byte warming only.
    if(!current || !next || next.id===current.entry.id || next.asset.kind!=='html' || !next.asset.preload_static) return;
    const generation=stageGeneration;
    const slot={key:signature(next),item:null,ready:false,promise:null}; staged=slot;
    slot.promise=prepare(next,true,item=>{slot.item=item;}).then(item=>{
      if(stopped || generation!==stageGeneration) { remove(item); return null; }
      slot.ready=true; return item;
    }).catch(()=>{ if(staged===slot) staged=null; return null; });
  }
  /* The engine's video plane lies BELOW the page: while it is the current surface every page layer
   * in front of it (html/body background, the standby panel, an iframe element that hosts an
   * engine component) must be transparent, or the page paints over the video. A plain <video>
   * punched its own hole; the engine box cannot. Opaque again for every other entry. */
  function engineSurface() {
    const on = !!(current?.engine || (current?.node?.tagName === 'IFRAME' && current.node.dataset?.engineFrame === '1'));
    document.documentElement?.classList?.toggle('engine-live', on);
  }
  function notify(entry) {
    engineSurface();
    const name = entry ? entry.asset.name : 'Waiting for scheduled content';
    document.getElementById('now-playing').textContent = name;
    window.parent.postMessage({type:'signage-status',empty:!entry,name},location.origin);
  }
  function cancelProgress(item) {
    item.generation = (item.generation || 0) + 1;
    clearTimeout(item.poll); clearTimeout(item.qualityTimer);
    if (item.frameCallback != null) item.node.cancelVideoFrameCallback?.(item.frameCallback);
    item.frameCallback = null;
    item.cancelReady?.();
  }
  function remove(item) {
    if (!item) return;
    if (item.engine) {
      cancelProgress(item); clearTimeout(item.watchdog);
      if(!item.keepEngine) engine?.hide();
      return;
    }
    if (item.node.tagName === 'IFRAME') engineHost?.dropFrame(item.node);
    cancelProgress(item);
    clearTimeout(item.watchdog);
    if (item.node.tagName === 'VIDEO' && !item.released) { item.released = true; item.node.onended = item.node.onerror = item.node.ontimeupdate = null; item.node.pause(); item.node.removeAttribute('src'); item.node.load(); }
    item.node.cancelHandshake?.();
    item.node.remove(); if (item.cover) { item.cover.remove(); dbg('cover removed'); }
  }
  function handshake(frame,type,timeout) {
    return new Promise((resolve,reject) => {
      const handler = (event) => {
        if(event.source!==frame.contentWindow) return;
        if(event.data?.type===type) { cleanup(); resolve(); }
        else if(event.data?.type==='signage-stage-error') { cleanup(); reject(new Error('HTML package is not media-free')); }
      };
      const alarm = setTimeout(() => { cleanup(); reject(new Error('HTML readiness timed out')); },timeout);
      const cancel=() => { cleanup(); reject(new Error('HTML staging cancelled')); };
      function cleanup() { clearTimeout(alarm); window.removeEventListener('message',handler); if(frame.cancelHandshake===cancel) frame.cancelHandshake=null; }
      frame.cancelHandshake=cancel;
      window.addEventListener('message',handler);
    });
  }
  async function hold(item, nextPoster) {
    if (!item || item.held || item.engine) return;
    item.held = true;
    if (item.node.tagName === 'VIDEO') {
      dbg('hold start', {id:item.entry.asset.id});
      // The cover is the next clip's first frame when it has a poster (so the board shows the
      // incoming still), else this clip's last frame. A failed still never blocks the teardown.
      for (const url of [nextPoster, item.entry.asset.end_poster, item.entry.asset.poster].filter(Boolean)) {
        try {
          await imageReady(url);
          const cover = new Image(); cover.className = 'cover'; cover.src = url;
          if (cover.decode) await cover.decode().catch(() => {});
          stage.appendChild(cover); item.cover = cover; dbg('cover shown', {url}); break;
        } catch (_) { /* try the next candidate */ }
      }
      // Predecoded still survives teardown of the hardware plane. Exactly one paint, before teardown.
      await paint();
      item.node.onended = item.node.onerror = item.node.ontimeupdate = null; item.node.pause(); item.node.removeAttribute('src'); item.node.load(); item.released = true;
      dbg('old video released', {id:item.entry.asset.id});
    } else if (item.node.tagName === 'IFRAME') {
      const posters = {};
      for (const e of playlist) if (e.asset.end_poster) posters[new URL(e.asset.url,location.href).pathname] = e.asset.end_poster;
      const held = handshake(item.node,'signage-held',2000);
      item.node.contentWindow.postMessage({type:'signage-hold',posters},'*');
      await held;
    }
  }
  // Serial (non-gapless) start of an engine entry: the player has decided to switch to THIS clip now.
  // It is requested at:'now' (a flushing switch inside the same pipeline, same decoder), never
  // queued at a boundary: the engine's current clip may be a stale, hidden or paused remnant of an
  // item the player already left (menuboard iframe, scheduled cut, watchdog), which would never
  // reach a boundary and would starve the request until the readiness watchdog skipped the entry.
  async function prepareEngine(entry) {
    const item={entry,node:engineBox,engine:true,held:false,watchdog:0,poll:0,fired:false,playId:null};
    const rec=ledgerOpen(entry,'serial','now');
    enginePreparing++;
    // The serial path owns the engine now; a gapless request queued for a clip we are leaving
    // would race this one (its own play_id would then never start).
    engineNext=null;
    let fail=null;
    const started=new Promise((resolve,reject)=>{
      item.started=resolve; fail=reject;
      item.watchdog=setTimeout(()=>{
        const stage=rec.started ? 'started' : rec.activated ? 'activated' : rec.disposition || (rec.acked ? 'acknowledged' : 'unacknowledged');
        reject(new Error('Engine video did not start ('+stage+')'));
      },15000);
    });
    started.catch(()=>{});
    try {
      let ack;
      // A late command of the page we are leaving (menuboard iframe) can take ownership between our
      // acquire and play: that surfaces as not-owner. Re-acquire and re-send, bounded, never silently.
      for(let attempt=0;attempt<3;attempt++) {
        const owned=await engine.acquire();
        if(owned && owned.ok===false) { rec.retries=attempt+1; rec.code=owned.code; if(attempt<2) continue; throw new Error('Engine refused ownership: '+owned.code); }
        ack=await engine.play(entry.asset.url,{at:'now'});
        if(ack.ok || ack.code!=='not-owner' || attempt===2) break;
        rec.retries=attempt+1;
      }
      rec.acked=Date.now(); rec.disposition=ack.message || (ack.ok ? 'accepted' : 'refused'); rec.code=ack.code ?? rec.code;
      if(!ack.ok) throw new Error('Engine refused video: '+ack.code);
      item.playId=ack.play_id; rec.playId=ack.play_id;
      if(engineRecent.includes(item.playId)) { rec.started=rec.started || Date.now(); item.started(); }
      else engineWaiters.set(item.playId,{resolve:item.started,reject:fail});
      await started;
      ledgerClose(rec,'started');
    } catch(error) {
      // Leave nothing of this request behind: a queued remnant would start later, over whatever
      // the player moved on to, or supersede the next entry's own request.
      if(rec.playId!==null && !rec.started) Promise.resolve(engine.cancel(entry.asset.url)).catch(()=>{});
      ledgerClose(rec,'failed',error.message);
      throw error;
    } finally {
      enginePreparing--; clearTimeout(item.watchdog); if(item.playId!==null) engineWaiters.delete(item.playId);
    }
    if(current?.engine) current.keepEngine=true; // the box now shows the new instance
    return item;
  }
  async function prepare(entry, hidden=false, created=()=>{}) {
    const a = entry.asset;
    if (!hidden && a.kind==='video') {
      if (engineCapable(entry) && await engineReady()) return prepareEngine(entry);
      await engineRelease();
    }
    let node;
    if (a.kind === 'video') {
      node = document.createElement('video'); node.className='surface video'; node.muted=true; node.defaultMuted=true; node.playsInline=true; node.preload='auto'; node.setAttribute('muted',''); node.setAttribute('playsinline','');
    } else if (a.kind === 'image') { node = new Image(); node.className='surface image'; }
    else { node=document.createElement('iframe'); node.className='surface frame'; node.title=a.name; node.setAttribute('sandbox','allow-scripts'); node.setAttribute('allow','autoplay'); }
    node.style.zIndex='1';
    if(hidden) node.style.visibility='hidden';
    dbg('next element created', {id:a.id, kind:a.kind});
    const item = {entry,node,held:false,watchdog:0,poll:0,fired:false};
    created(item);
    try {
      let ready;
      if (a.kind === 'video') {
        ready = new Promise((resolve,reject) => {
          let done=false, checkTimer=0, callback=null;
          const cleanup=() => { clearTimeout(alarm); clearTimeout(checkTimer); if(callback!=null) node.cancelVideoFrameCallback?.(callback); callback=null; node.onplaying=null; };
          item.cancelReady=() => { done=true; cleanup(); };
          const alarm=setTimeout(() => { done=true; cleanup(); reject(new Error('Video did not start')); },15000);
          node.onerror=() => { done=true; cleanup(); reject(new Error('Video unavailable')); };
          node.onplaying=() => {
            if(done || checkTimer || callback!=null) return;
            dbg('playing', {id:a.id});
            // Holepunch can expose rVFC without ever delivering it. Media-clock progress
            // is readiness only, never evidence that a frame was actually presented.
            const start=node.currentTime;
            const finish=() => { if(done) return; done=true; cleanup(); dbg('first progress', {id:a.id}); resolve(); };
            if (node.requestVideoFrameCallback) callback=node.requestVideoFrameCallback(finish);
            const check=() => { if(done) return; if(node.readyState>=2 && node.currentTime>start) finish(); else if(node.isConnected) checkTimer=setTimeout(check,30); };
            check();
          };
        });
        stage.appendChild(node); node.src=a.url; dbg('src set', {id:a.id});
        if (!current && !engine && window.signageNative?.preflight && playlist.length>1) {
          // Establish the outgoing PAUSED pipeline and broker generation before
          // staging. No media clock runs until actual next-context PREPARED.
          const preroll=new Promise((resolve,reject)=>{
            const loaded=()=>{node.removeEventListener('loadeddata',loaded);resolve();};
            node.addEventListener('loadeddata',loaded,{once:true});
            node.onerror=()=>reject(new Error('Initial video preroll failed'));
          });
          node.load(); await preroll;
          preparingInitial=true;
          current=item; lookahead(); current=null;
          nativeSend({type:'native-warm-pool'});
          const slot=nativeStage;
          if (slot && !slot.ready && !slot.error) {
            const bound=setTimeout(()=>slot.prepared?.(false),NATIVE_PREROLL_BOUND_MS);
            await new Promise(resolve=>{slot.prepared=resolve;}); clearTimeout(bound);
          }
          if (stopped) throw new Error('Startup preparation cancelled');
          if (!slot || slot.error || !slot.ready || nativeStage!==slot) {
            // Standby refused or unready: start the first video without a staged successor
            // (later transitions use the explicit serial path) instead of never starting.
            console.warn('Startup standby unavailable: starting without gapless successor',slot?.error || 'not ready');
            if (nativeStage) cancelStage();
          } else {
            // Pool and next native page exist; from here WebView creation is a violation.
            nativeSend({type:'native-playback',running:true});
          }
        }
        node.play().catch(() => { if(node.onerror) node.onerror(); });
      } else if (a.kind === 'image') {
        await imageReady(a.url); node.src=a.url; stage.appendChild(node); ready=paint();
      } else {
        // Append about:blank first so the handshake can bind to its WindowProxy before navigation.
        stage.appendChild(node); ready=handshake(node,'signage-ready',15000);
        // Built-in pages always come from the image so their video lifecycle matches this
        // engine/protocol version, never from a stale persisted /data seed copy.
        let url=a.url;
        if (engine && a.builtin && new URL(url,location.href).pathname==='/content/menuboard/index.html')
          url=url.replace('/content/menuboard/','/native-builtin/menuboard/');
        node.src=hidden ? url+(url.includes('?')?'&':'?')+'signage_stage=1' : url;
      }
      await ready; if (a.kind !== 'video') await paint(); return item;
    } catch(error) { remove(item); throw error; }
  }
  function arm(item) {
    clearTimeout(timer); cancelProgress(item); item.fired = false;
    const generation = item.generation;
    const duration=item.entry.duration || item.entry.asset.video_duration || 120;
    const active=() => item.generation===generation && current===item && !stopped && !item.fired;
    const finish=(reason='duration') => {
      if(!active()) return;
      item.fired=true;
      if(debugOn) {
        const q=item.node.getVideoPlaybackQuality?.();
        dbg('end transition', {id:item.entry.asset.id, generation, reason, ct:item.node.currentTime,
          callbacks:item.callbackCount || 0, presented:item.presentedFrames ?? null,
          created:q?.totalVideoFrames ?? null, dropped:q?.droppedVideoFrames ?? null});
      }
      cancelProgress(item); advance();
    };
    if (item.engine) {
      item.finish=finish;
      const length=item.entry.asset.engine?.duration_ms ? item.entry.asset.engine.duration_ms/1000 : duration;
      const short=item.entry.duration && item.entry.duration < length;
      timer=setTimeout(() => finish(short ? 'scheduled-cut' : 'watchdog'),(short ? item.entry.duration : length+5)*1000);
    } else if (item.node.tagName === 'VIDEO') {
      const v=item.node;
      item.callbackCount=0; item.presentedFrames=null;
      // Only EOS ends a complete clip: currentTime nearing duration is NOT final-frame proof.
      // Clock polling remains an EOS fallback for sinks whose rVFC never fires.
      const progress=() => { if(active() && v.ended && !v.seeking) finish('ended'); };
      const frame=(_now,metadata) => {
        if(!active()) return;
        item.frameCallback=null; item.callbackCount++;
        item.presentedFrames=metadata?.presentedFrames ?? null;
        progress();
        if(active()) item.frameCallback=v.requestVideoFrameCallback(frame);
      };
      v.onended=() => { if(v.ended && !v.seeking) finish('ended'); };
      v.onerror=() => finish('error'); v.ontimeupdate=progress;
      if(v.requestVideoFrameCallback) item.frameCallback=v.requestVideoFrameCallback(frame);
      const poll=() => { if(!active()) return; progress(); if(active()) item.poll=setTimeout(poll,50); };
      item.poll=setTimeout(poll,50);
      if (debugOn && v.getVideoPlaybackQuality) {
        let qn=0;
        const quality=() => {
          if (item.generation!==generation || current!==item || stopped || qn>=120) return;
          qn++;
          const q=v.getVideoPlaybackQuality();
          dbg('quality', {id:item.entry.asset.id, ct:+v.currentTime.toFixed(2), dur:+(v.duration||0).toFixed(2), created:q.totalVideoFrames, dropped:q.droppedVideoFrames});
          item.qualityTimer=setTimeout(quality,1000);
        };
        item.qualityTimer=setTimeout(quality,1000);
      }
      // Explicitly shortened entries may cut playback; full-length entries wait for EOS.
      const fullDuration=Number.isFinite(v.duration) && v.duration>0 ? v.duration : item.entry.asset.video_duration;
      const short=item.entry.duration && fullDuration && item.entry.duration < fullDuration;
      timer=setTimeout(() => finish(short ? 'scheduled-cut' : 'watchdog'),(short ? item.entry.duration : duration+5)*1000);
    } else timer=setTimeout(finish,duration*1000);
  }
  async function advance(forceId) {
    if (transitioning) { requested=true; return; }
    if (!playlist.length) { if(current?.native) nativeSend({type:'native-reset'}); clearTimeout(timer); cancelStage(); remove(current); current=null; lookahead(); notify(null); return; }
    transitioning=true; clearTimeout(timer);
    let index = forceId ? playlist.findIndex((e) => e.id===forceId) : current ? (playlist.findIndex((e) => e.id===current.entry.id)+1)%playlist.length : 0;
    index=Math.max(index,0);
    try {
      const candidate = playlist[index];
      // Same clip back to back (including a one-entry loop): restart in place. No teardown, no cover,
      // still exactly one decoder. Invalidate handlers before seek/play can dispatch media events.
      if(current) {
        cancelProgress(current);
        current.node.onended = current.node.onerror = current.node.ontimeupdate = null;
      }
      if (current && current.node.tagName === 'VIDEO' && current.node.isConnected && candidate && candidate.asset.kind === 'video' && candidate.asset.url === current.entry.asset.url) {
        dbg('restart in place', {id:candidate.asset.id});
        current.entry = candidate; current.held = false;
        const v = current.node; v.currentTime = 0; v.play().catch(() => {});
        notify(candidate); arm(current); return;
      }
      if(nativeStage && nativeStage.key===signature(candidate)) {
        const slot=nativeStage;
        slot.waitSince ??= Date.now();
        const waited=Date.now()-slot.waitSince;
        if(!slot.error && !slot.ready && waited<NATIVE_PREROLL_BOUND_MS) {
          // Native reports not-ready itself at its own 15 s deadline; this bound only guards a lost notice.
          timer=setTimeout(()=>advance(forceId),100); return;
        }
        if(slot.error || !slot.ready) {
          // Bounded: outgoing surface was retained while waiting; now degrade to the serial (non-gapless) path.
          console.warn('Native next-not-ready: serial fallback',slot.error || 'preroll timeout');
          if(!slot.error) noteNativeRefusal(slot.key);
          cancelStage();
          // fall through to the serial path below
        } else {
        const activated=new Promise(resolve=>{slot.resolve=resolve;});
        nativeSend({type:'native-activate',generation:slot.generation});
        if(await activated) {
          const old=current;
          // Native matching flip already switched UI+video. Teardown cannot precede it.
          remove(old);
          const node=document.createElement('div'); node.className='surface'; stage.appendChild(node);
          current={entry:candidate,node,native:true,generation:0}; nativeActiveGeneration=slot.generation; nativeStage=null;
          notify(candidate);
          if(candidate.asset.kind!=='video') timer=setTimeout(()=>advance(),(candidate.duration || 120)*1000);
          lookahead(); return;
        }
        // Activation refused: never park on the outgoing surface. Degrade explicitly (non-gapless).
        console.warn('Native activation failed; serial fallback');
        noteNativeRefusal(slot.key);
        if(nativeStage===slot) cancelStage();
        }
      }
      // A staged media-free page is ready before the outgoing sink is released.
      // Reveal and removal share one browser paint task; rAF is not KMS proof.
      if(staged && staged.key===signature(candidate)) {
        const slot=staged;
        const next=await slot.promise;
        if(next && !stopped && playlist.some(e=>signature(e)===slot.key)) {
          staged=null;
          await new Promise(resolve=>requestAnimationFrame(()=>{
            if(stopped || !playlist.some(e=>signature(e)===slot.key)) { remove(next); resolve(); return; }
            next.node.style.visibility='visible'; next.node.style.zIndex='2';
            next.node.contentWindow.postMessage({type:'signage-activate'},'*');
            remove(current); current=next; notify(next.entry); arm(next); resolve();
          }));
          lookahead(); return;
        }
      }
      if(stopped) return;
      if(current?.native) nativeSend({type:'native-reset'});
      // Serial video fallback is explicitly non-gapless: no spare decoder is
      // supported by the broker. Never disguise a frozen poster as motion.
      const nextPoster = candidate && candidate.asset.kind === 'video' ? candidate.asset.poster : null;
      // With the persistent engine a page owns no decoder, so freezing it (pausing every animation)
      // before the next item is loaded only shows a stuck frame for the whole load. Keep it live,
      // build the next item underneath, and swap in one step.
      const keepLive = !!engine && current?.node?.tagName === 'IFRAME';
      if (!keepLive) { try { await hold(current, nextPoster); } catch (_) { /* Retain its visible surface while attempting the next item. */ } }
      let next = null;
      const attempts=playlist.length;
      for (let i=0;i<attempts;i++) {
        const entry=playlist[(index+i)%playlist.length]; if (!entry) break;
        warm(entry); warm(playlist[(index+i+1)%playlist.length]);
        try { next=await prepare(entry); break; } catch(error) { console.warn('Skipping content:',entry.asset.name,error.message); }
      }
      if (next) {
        next.node.style.zIndex='2'; remove(current); current=next; notify(next.entry); arm(next);
        lookahead();
      } else { document.getElementById('now-playing').textContent='Retrying content…'; timer=setTimeout(() => advance(),5000); }
    } finally {
      preparingInitial=false;
      transitioning=false;
      if (requested) { requested=false; reconcile(); }
    }
  }
  function reconcile() {
    if(transitioning) { requested=true; return; }
    const fresh=current && playlist.find((e) => e.id===current.entry.id);
    if (!fresh) { advance(playlist[0]?.id); return; }
    if (JSON.stringify(fresh)!==JSON.stringify(current.entry)) { current.entry=fresh; arm(current); }
    lookahead();
  }
  async function poll() {
    clearTimeout(pollTimer);
    try {
      const response=await fetch('/api/playlist',{cache:'no-store'}); if(!response.ok) throw new Error('Playlist unavailable');
      const data=await response.json(); playlist=data.entries; previousRevision=data.revision; lastPoll=Date.now();
      engineSetup();
      if(!engineHost && window.SignageVideo && engine) {
        engineHost=window.SignageVideo.host();
        engineHost.hooks={
          // Page-hosted controllers (menuboard iframes) command the same engine through this proxy:
          // account their play requests with the owner token they used, next to the player's own.
          command:info=>{
            if(info.cmd!=='play' && info.cmd!=='acquire' && info.cmd!=='release') return;
            const rec=ledgerOpen(info.entry||{id:info.frameSrc||null,asset:{id:info.fields?.asset?.url||null}},'frame:'+info.cmd,info.fields?.at||null);
            rec.owner=info.client; rec.playId=info.ack?.play_id ?? null; rec.acked=info.acked; rec.disposition=info.ack?.message || (info.ack?.ok ? 'accepted' : 'refused'); rec.code=info.ack?.code ?? null;
            if(info.cmd==='play' && info.ack?.ok) rec.outcome='queued'; else ledgerClose(rec,info.ack?.ok ? 'ok' : 'refused',info.ack?.code);
          },
          open:frame=>{ if(frame.dataset) frame.dataset.engineFrame='1'; engineSurface(); },
          close:frame=>{ if(frame.dataset) delete frame.dataset.engineFrame; engineSurface(); }
        };
      }
      clearTimeout(boundaryTimer);
      if(data.next_change) boundaryTimer=setTimeout(poll,Math.max(20,Math.min(2147483647,(data.next_change-data.now)*1000+25)));
      // Expired items stop locally even if a subsequent network request fails.
      clearTimeout(clockTimer);
      if(data.next_change) clockTimer=setTimeout(() => { const now=data.now+(Date.now()-lastPoll)/1000; playlist=playlist.filter((e) => !e.end || new Date(e.end).getTime()/1000>now); reconcile(); },Math.max(25,Math.min(2147483647,(data.next_change-data.now)*1000+30)));
      warm(playlist[0]);
      if(!current && !preparingInitial) lookahead();
      if((staged && !playlist.some(e=>signature(e)===staged.key)) ||
          (nativeStage && !playlist.some(e=>signature(e)===nativeStage.key))) cancelStage();
      reconcile();
    } catch (_) { document.getElementById('now-playing').textContent=previousRevision ? 'Playing saved loop · reconnecting…' : 'Connecting to your content…'; }
    if(!stopped) pollTimer=setTimeout(poll,2000);
  }
  let controlsTimer;
  const showControls=() => { if(preview) return; document.body.classList.add('controls'); clearTimeout(controlsTimer); controlsTimer=setTimeout(() => document.body.classList.remove('controls'),2500); };
  document.addEventListener('pointermove',showControls); document.addEventListener('keydown',showControls);
  document.getElementById('fullscreen').onclick=() => document.fullscreenElement ? document.exitFullscreen() : document.documentElement.requestFullscreen?.();
  window.addEventListener('pagehide',() => { engine?.destroy();
    nativeSend({type:'native-playback',running:false}); stopped=true; clearTimeout(timer); clearTimeout(pollTimer); clearTimeout(boundaryTimer); clearTimeout(clockTimer); cancelStage(); remove(current); });
  poll();
})();
