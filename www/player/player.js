(() => {
  'use strict';
  const stage = document.getElementById('stage');
  const preview = new URLSearchParams(location.search).has('preview');
  if (preview) document.body.classList.add('preview');
  let playlist = [], current = null, transitioning = false, requested = false, timer = 0, pollTimer = 0, boundaryTimer = 0, clockTimer = 0;
  let lastPoll = 0, previousRevision = null, stopped = false;
  const images = new Map(), hints = new Map();
  const delay = (ms) => new Promise((resolve) => setTimeout(resolve,ms));
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
    // Browser cache warm-up does not create a second live hardware video sink.
    if (a.kind === 'video' && !hints.has(a.url)) {
      if(hints.size >= 4) { const key=hints.keys().next().value; hints.get(key).remove(); hints.delete(key); }
      const hint = document.createElement('link'); hint.rel = 'prefetch'; hint.href = a.url; hint.as = 'video'; document.head.appendChild(hint); hints.set(a.url,hint);
    }
  }
  function notify(entry) {
    const name = entry ? entry.asset.name : 'Waiting for scheduled content';
    document.getElementById('now-playing').textContent = name;
    window.parent.postMessage({type:'signage-status',empty:!entry,name},location.origin);
  }
  function remove(item) {
    if (!item) return;
    clearTimeout(item.watchdog);
    if (item.node.tagName === 'VIDEO') { item.node.onended = item.node.onerror = null; item.node.pause(); item.node.removeAttribute('src'); item.node.load(); }
    item.node.remove(); if (item.cover) item.cover.remove();
  }
  function handshake(frame,type,timeout) {
    return new Promise((resolve,reject) => {
      const handler = (event) => { if (event.source === frame.contentWindow && event.data?.type === type) { cleanup(); resolve(); } };
      const alarm = setTimeout(() => { cleanup(); reject(new Error('HTML readiness timed out')); },timeout);
      function cleanup() { clearTimeout(alarm); window.removeEventListener('message',handler); }
      window.addEventListener('message',handler);
    });
  }
  async function hold(item) {
    if (!item || item.held) return;
    item.held = true;
    if (item.node.tagName === 'VIDEO') {
      const url = item.entry.asset.end_poster || item.entry.asset.poster;
      // Predecoded last-frame still survives teardown of the hardware plane.
      if (url) { await imageReady(url); const cover = new Image(); cover.className = 'cover'; cover.src = url; stage.appendChild(cover); item.cover = cover; await paint(); }
      item.node.onended = item.node.onerror = null; item.node.pause(); item.node.removeAttribute('src'); item.node.load(); await paint();
    } else if (item.node.tagName === 'IFRAME') {
      const posters = {};
      for (const e of playlist) if (e.asset.end_poster) posters[new URL(e.asset.url,location.href).pathname] = e.asset.end_poster;
      const held = handshake(item.node,'signage-held',2000);
      item.node.contentWindow.postMessage({type:'signage-hold',posters},'*');
      await held;
    }
  }
  async function prepare(entry) {
    const a = entry.asset;
    let node;
    if (a.kind === 'video') {
      node = document.createElement('video'); node.className='surface video'; node.muted=true; node.defaultMuted=true; node.playsInline=true; node.preload='auto'; node.setAttribute('muted',''); node.setAttribute('playsinline','');
    } else if (a.kind === 'image') { node = new Image(); node.className='surface image'; }
    else { node=document.createElement('iframe'); node.className='surface frame'; node.title=a.name; node.setAttribute('sandbox','allow-scripts'); node.setAttribute('allow','autoplay'); }
    node.style.zIndex='1';
    const item = {entry,node,held:false,watchdog:0};
    try {
      let ready;
      if (a.kind === 'video') {
        ready = new Promise((resolve,reject) => {
          const alarm=setTimeout(() => reject(new Error('Video did not start')),15000);
          node.onerror=() => { clearTimeout(alarm); reject(new Error('Video unavailable')); };
          node.onplaying=() => {
            // A decoded/presented frame is a stronger signal than canplay.
            if (node.requestVideoFrameCallback) {
              // Holepunch sinks can expose the callback API without delivering callbacks.
              const background=setTimeout(() => { if(node.readyState >= 2 && node.currentTime > 0) { clearTimeout(alarm); resolve(); } },750);
              node.requestVideoFrameCallback(() => { clearTimeout(background); clearTimeout(alarm); resolve(); });
            }
            else { const check=() => { if (node.readyState>=2 && node.currentTime>0) { clearTimeout(alarm); resolve(); } else if (node.isConnected) setTimeout(check,30); }; check(); }
          };
        });
        stage.appendChild(node); node.src=a.url;
        node.play().catch(() => { if(node.onerror) node.onerror(); });
      } else if (a.kind === 'image') {
        await imageReady(a.url); node.src=a.url; stage.appendChild(node); ready=paint();
      } else {
        // Append about:blank first so the handshake can bind to its WindowProxy before navigation.
        stage.appendChild(node); ready=handshake(node,'signage-ready',15000); node.src=a.url;
      }
      await ready; await paint(); return item;
    } catch(error) { remove(item); throw error; }
  }
  function arm(item) {
    clearTimeout(timer);
    const duration=item.entry.duration || item.entry.asset.video_duration || 120;
    const finish=() => { if(current===item && !stopped) advance(); };
    if (item.node.tagName === 'VIDEO') {
      item.node.onended=finish; item.node.onerror=finish;
      // Full videos follow ended; the watchdog also advances broken or stalled media.
      timer=setTimeout(finish,((item.entry.duration || duration+5))*1000);
    } else timer=setTimeout(finish,duration*1000);
  }
  async function advance(forceId) {
    if (transitioning) { requested=true; return; }
    if (!playlist.length) { clearTimeout(timer); remove(current); current=null; notify(null); return; }
    transitioning=true; clearTimeout(timer);
    let index = forceId ? playlist.findIndex((e) => e.id===forceId) : current ? (playlist.findIndex((e) => e.id===current.entry.id)+1)%playlist.length : 0;
    index=Math.max(index,0);
    try {
      try { await hold(current); } catch (_) { /* Retain its visible surface while attempting the next item. */ }
      let next = null;
      const attempts=playlist.length;
      for (let i=0;i<attempts;i++) {
        const entry=playlist[(index+i)%playlist.length]; if (!entry) break;
        warm(entry); warm(playlist[(index+i+1)%playlist.length]);
        try { next=await prepare(entry); break; } catch(error) { console.warn('Skipping content:',entry.asset.name,error.message); }
      }
      if (next) {
        next.node.style.zIndex='2'; remove(current); current=next; notify(next.entry); arm(next);
        warm(playlist[(playlist.findIndex((e) => e.id===next.entry.id)+1)%playlist.length]);
      } else { document.getElementById('now-playing').textContent='Retrying content…'; timer=setTimeout(() => advance(),5000); }
    } finally {
      transitioning=false;
      if (requested) { requested=false; reconcile(); }
    }
  }
  function reconcile() {
    if(transitioning) { requested=true; return; }
    const fresh=current && playlist.find((e) => e.id===current.entry.id);
    if (!fresh) { advance(playlist[0]?.id); return; }
    if (JSON.stringify(fresh)!==JSON.stringify(current.entry)) { current.entry=fresh; arm(current); }
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
      reconcile();
    } catch (_) { document.getElementById('now-playing').textContent=previousRevision ? 'Playing saved loop · reconnecting…' : 'Connecting to your content…'; }
    if(!stopped) pollTimer=setTimeout(poll,2000);
  }
  let controlsTimer;
  const showControls=() => { if(preview) return; document.body.classList.add('controls'); clearTimeout(controlsTimer); controlsTimer=setTimeout(() => document.body.classList.remove('controls'),2500); };
  document.addEventListener('pointermove',showControls); document.addEventListener('keydown',showControls);
  document.getElementById('fullscreen').onclick=() => document.fullscreenElement ? document.exitFullscreen() : document.documentElement.requestFullscreen?.();
  window.addEventListener('pagehide',() => { stopped=true; clearTimeout(timer); clearTimeout(pollTimer); clearTimeout(boundaryTimer); clearTimeout(clockTimer); remove(current); });
  poll();
})();
