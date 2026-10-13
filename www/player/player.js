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
    if((staged && staged.key===signature(next)) || (nativeStage && !nativeStage.error && nativeStage.key===signature(next))) return;
    cancelStage();
    if(window.signageNative && current && next && !nativeRefusedFor(signature(next)) &&
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
  function notify(entry) {
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
    if (!item || item.held) return;
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
  async function prepare(entry, hidden=false, created=()=>{}) {
    const a = entry.asset;
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
        if (!current && window.signageNative?.preflight && playlist.length>1) {
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
        node.src=hidden ? a.url+(a.url.includes('?')?'&':'?')+'signage_stage=1' : a.url;
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
    if (item.node.tagName === 'VIDEO') {
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
      try { await hold(current, nextPoster); } catch (_) { /* Retain its visible surface while attempting the next item. */ }
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
  window.addEventListener('pagehide',() => { nativeSend({type:'native-playback',running:false}); stopped=true; clearTimeout(timer); clearTimeout(pollTimer); clearTimeout(boundaryTimer); clearTimeout(clockTimer); cancelStage(); remove(current); });
  poll();
})();
