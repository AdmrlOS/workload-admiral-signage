// Exercise player handoffs without a GPU: the old surface must survive until readiness.
const assert = require('node:assert/strict');
const {readFileSync} = require('node:fs');
const vm = require('node:vm');
const test = require('node:test');

async function harness(entries, options={}) {
  const events=[], timers=new Map(), listeners={}, nodes=[], logs=[];
  let serial=0, revision=1;
  const timeout=(fn,ms) => { const id=++serial; timers.set(id,{fn,ms}); return id; };
  class Element {
    constructor(tag) { this.tagName=tag.toUpperCase(); this.style={}; this.children=[]; this.isConnected=false; this.classList={add(){},remove(){}}; this.readyState=options.readyState ?? 3; this._t=options.currentTime ?? 1; this._playing=false; this.contentWindow={postMessage:() => queueMicrotask(() => (listeners.message||[]).forEach((fn) => fn({source:this.contentWindow,data:{type:'signage-held'}})))}; nodes.push(this); }
    setAttribute() {}
    appendChild(node) { node.isConnected=true; this.children.push(node); events.push('append:'+node.tagName); }
    remove() { this.isConnected=false; events.push('remove:'+this.tagName); }
    removeAttribute(key) { if(key==='src') { this._src=''; events.push('release:'+this.tagName); } }
    addEventListener(type,fn) { (this.listeners??={})[type]=fn; }
    removeEventListener(type) { delete this.listeners?.[type]; }
    load() { if(options.preflight && this.src) queueMicrotask(()=>this.listeners?.loadeddata?.()); }
    pause() {}
    set src(value) { this._src=value; if(this.tagName==='IMG') queueMicrotask(() => this.onload?.()); if(this.tagName==='IFRAME' && !options.manualHtml) queueMicrotask(() => (listeners.message||[]).forEach((fn) => fn({source:this.contentWindow,data:{type:'signage-ready'}}))); }
    get src() { return this._src; }
    // Media time advances on each read while playing unless options.advance is false (a stalled decoder).
    get currentTime() { return this._t; }
    set currentTime(v) { this._t = v; events.push('seek'); }
    get duration() { return options.duration ?? NaN; }
    getVideoPlaybackQuality() { return {totalVideoFrames: options.frames ?? 0, droppedVideoFrames: options.dropped ?? 0}; }
    play() { events.push('play:'+this.src); this._playing=true; queueMicrotask(() => this.onplaying?.()); return Promise.resolve(); }
    requestVideoFrameCallback(fn) {
      this.callbacks ??= new Map(); const id=++serial; this.callbacks.set(id,fn);
      if(options.frameCallbacks!==false && !options.manualCallbacks && !this._rvfc) {
        this._rvfc=true; queueMicrotask(() => { this.callbacks.delete(id); events.push('video-ready'); fn(0,{presentedFrames:1}); });
      }
      return id;
    }
    cancelVideoFrameCallback(id) { this.callbacks?.delete(id); }
    get ended() { return this._ended || false; }
  }
  const stage=new Element('div'), body=new Element('body'), head=new Element('head'), status=new Element('div');
  const window={parent:{postMessage(){}},addEventListener:(type,fn) => (listeners[type]??=[]).push(fn),removeEventListener:(type,fn) => { listeners[type]=(listeners[type]||[]).filter((f)=>f!==fn); }};
    if(options.native) window.signageNative={preflight:!!options.preflight,send:message=>events.push(message)};
    let fakeNow=1_700_000_000_000; class FakeDate extends Date { constructor(...a){ super(...(a.length?a:[fakeNow])); } static now(){ return fakeNow; } }
    const context={console:{...console,log:(...a)=>{logs.push(a.map(String).join(' '));console.log(...a);}}, URL,URLSearchParams,Promise,Date:FakeDate,Map,Image:class extends Element { constructor(){super('img');} },
    window,document:{body,head,createElement:(tag)=>new Element(tag),getElementById:(id)=>id==='stage'?stage:status,addEventListener(){}},
    location:{origin:'http://signage.test',href:'http://127.0.0.1:8080/player/',search:options.search||''},setTimeout:timeout,clearTimeout:(id)=>timers.delete(id),requestAnimationFrame:(fn)=>queueMicrotask(fn),
    fetch:async(url)=>{ if(options.networkFailure?.() && url==='/api/playlist') throw new Error('offline'); return {ok:true,json:async()=>({entries,revision,now:Date.now()/1000,next_change:null})}; }};
  vm.runInNewContext(readFileSync('www/player/player.js','utf8'),context);
  const settle=async()=>{ for(let i=0;i<80;i++) await Promise.resolve(); };
  await settle();
  return {events,nodes,timers,logs,settle,native:data=>{(listeners['signage-native']||[]).forEach(fn=>fn({detail:data}));},message:(node,type)=>{(listeners.message||[]).forEach(fn=>fn({source:node.contentWindow,data:{type}}));},hide:()=>{(listeners.pagehide||[]).forEach(fn=>fn());},setPlaylist:(next)=>{entries=next;revision++;},run:async(ms)=>{ const pair=Array.from(timers).find(([,t])=>t.ms===ms); assert.ok(pair,`Missing timer ${ms}`); timers.delete(pair[0]); fakeNow+=ms; for (const n of nodes) if (n._playing && options.advance !== false) n._t += ms/1000; pair[1].fn(); await settle(); }};
}
test('native startup prepares next context before first media clock starts',async()=>{
 const h=await harness([entry('a','video'),entry('b','video')],{native:true,preflight:true});
 const reservation=h.events.find(e=>e.type==='native-stage'); assert.ok(reservation,'next context must initialize before timed playback');
 assert.equal(h.events.some(e=>typeof e==='string' && e.startsWith('play:')),false);
 h.native({event:'prepared',request:reservation.request,generation:'2'}); await h.settle();
 assert.ok(h.events.includes('play:/a'));
});

test('media clock is declared running only after the pool and next native page are prepared',async()=>{
 const h=await harness([entry('a','video'),entry('b','video')],{native:true,preflight:true});
 const reservation=h.events.find(e=>e.type==='native-stage');
 assert.equal(h.events.some(e=>e.type==='native-playback'),false);
 h.native({event:'prepared',request:reservation.request,generation:'2'}); await h.settle();
 const order=h.events.map(e=>e.type==='native-playback' ? 'guard:'+e.running : e);
 assert.ok(order.indexOf('guard:true')>=0 && order.indexOf('guard:true')<order.indexOf('play:/a'));
});

test('startup playlist polls preserve the pending preparation barrier',async()=>{
 const h=await harness([entry('a','video'),entry('b','video')],{native:true,preflight:true});
 const reservation=h.events.find(e=>e.type==='native-stage');
 await h.run(2000);
 assert.equal(h.events.some(e=>e.type==='native-cancel'),false);
 h.native({event:'prepared',request:reservation.request,generation:'2'});await h.settle();
 assert.ok(h.events.includes('play:/a'));
});

test('native two-client handoff never creates an incoming DOM sink or releases old before matching flip',async()=>{
 const h=await harness([entry('a','video'),entry('b','video')],{native:true});
 const reservation=h.events.find(e=>e.type==='native-stage'); assert.ok(reservation);
 assert.equal(h.nodes.filter(n=>n.tagName==='VIDEO').length,1);
 h.native({event:'prepared',request:reservation.request,generation:'2'});
 const old=h.nodes.find(n=>n.tagName==='VIDEO'); old._ended=true; old.onended(); await h.settle();
 assert.ok(h.events.find(e=>e.type==='native-activate' && e.generation==='2'));
 assert.equal(h.events.includes('release:VIDEO'),false);
 h.native({event:'activated',request:reservation.request,generation:'1'}); await h.settle();
 assert.equal(h.events.includes('release:VIDEO'),false,'stale generation cannot authorize teardown');
 h.native({event:'activated',request:reservation.request,generation:'2'}); await h.settle();
 assert.equal(h.events.includes('release:VIDEO'),true);
});

const entry=(id,kind,duration=1)=>({id,asset_id:id,duration,enabled:true,start:null,end:null,asset:{id,kind,name:id,url:'/'+id,poster:'/first.jpg',end_poster:'/last.jpg',video_duration:1}});

test('static HTML is prepared while the outgoing video still plays, with one sink',async()=>{
  const page=entry('page','html'); page.asset.preload_static=true;
  const h=await harness([entry('a','video'),page]);
  const staged=h.nodes.find(n=>n.tagName==='IFRAME');
  assert.ok(staged,'Next static page must already be loaded');
  assert.equal(staged.style.visibility,'hidden');
  assert.equal(h.events.includes('release:VIDEO'),false);
  assert.equal(h.nodes.filter(n=>n.tagName==='VIDEO').length,1);
  const v=h.nodes.find(n=>n.tagName==='VIDEO'); v._ended=true; v.onended(); await h.settle();
  assert.equal(staged.style.visibility,'visible');
  assert.equal(h.nodes.filter(n=>n.tagName==='IFRAME').length,1,'Reveal the prepared page, do not navigate again');
});

test('not-ready staged HTML never releases the outgoing video at EOS',async()=>{
  const page=entry('page','html'); page.asset.preload_static=true;
  const h=await harness([entry('a','video'),page],{manualHtml:true});
  const frame=h.nodes.find(n=>n.tagName==='IFRAME'), v=h.nodes.find(n=>n.tagName==='VIDEO');
  v._ended=true; v.onended(); await h.settle();
  assert.equal(h.events.includes('release:VIDEO'),false);
  h.message(frame,'signage-ready'); await h.settle();
  assert.equal(frame.style.visibility,'visible');
  assert.equal(h.events.includes('release:VIDEO'),true);
});

test('playlist edits cancel hidden prepared pages and stale readiness',async()=>{
  const page=entry('page','html'); page.asset.preload_static=true;
  const h=await harness([entry('a','video'),page],{manualHtml:true});
  const frame=h.nodes.find(n=>n.tagName==='IFRAME');
  h.setPlaylist([entry('a','video'),entry('b','video')]); await h.run(2000);
  assert.equal(frame.isConnected,false);
  h.message(frame,'signage-ready'); await h.settle();
  assert.notEqual(frame.style.visibility,'visible');
  assert.equal(h.nodes.filter(n=>n.tagName==='VIDEO').length,1);
  h.hide(); await h.settle();
});

test('untrusted mixed HTML never creates a hidden hardware media sink',async()=>{
  const h=await harness([entry('a','video'),entry('mixed','html')]);
  assert.equal(h.nodes.filter(n=>n.tagName==='IFRAME').length,0);
});

test('playlist network failure preserves the already prepared next surface',async()=>{
  let offline=false;
  const page=entry('page','html'); page.asset.preload_static=true;
  const h=await harness([entry('a','video'),page],{networkFailure:()=>offline});
  const frame=h.nodes.find(n=>n.tagName==='IFRAME');
  offline=true; await h.run(2000);
  const v=h.nodes.find(n=>n.tagName==='VIDEO'); v._ended=true; v.onended(); await h.settle();
  assert.equal(frame.style.visibility,'visible');
  assert.equal(h.nodes.filter(n=>n.tagName==='IFRAME').length,1);
});

test('pagehide while staged readiness is pending cannot create replacement media',async()=>{
  const page=entry('page','html'); page.asset.preload_static=true;
  const h=await harness([entry('a','video'),page],{manualHtml:true});
  const frame=h.nodes.find(n=>n.tagName==='IFRAME');
  const v=h.nodes.find(n=>n.tagName==='VIDEO'); v._ended=true; v.onended(); await h.settle();
  h.hide(); await h.settle();
  h.message(frame,'signage-ready'); await h.settle();
  assert.equal(frame.isConnected,false);
  assert.equal(h.nodes.filter(n=>n.tagName==='VIDEO').length,1);
});

test('video handoff keeps an opaque still until the next video makes progress',async()=>{
  const h=await harness([entry('a','video'),entry('b','video')]);
  const v=h.nodes.find((n)=>n.tagName==='VIDEO'); v._ended=true; v.onended(); await h.settle();
  const secondPlay=h.events.indexOf('play:/b');
  assert.ok(secondPlay>h.events.indexOf('release:VIDEO'));
  const cover=h.nodes.find((n)=>n.className==='cover');
  assert.ok(cover);
  assert.equal(cover.isConnected,false);
  // Cover is removed on the incoming clip's first progress (same task), after its play() call.
  assert.ok(h.events.lastIndexOf('remove:IMG')>secondPlay);
});

test('HTML is retained until the next surface has loaded',async()=>{
  const h=await harness([entry('page','html'),entry('clip','video')]);
  await h.run(1000);
  assert.ok(h.events.indexOf('remove:IFRAME')>h.events.indexOf('video-ready'));
});

test('unchanged playlist polls do not restart playback or reset item duration',async()=>{
  const h=await harness([entry('page','html',5)]);
  const durationTimer=Array.from(h.timers).find(([,t])=>t.ms===5000)[0];
  await h.run(2000);
  assert.ok(h.timers.has(durationTimer));
  assert.equal(h.events.filter((e)=>e==='append:IFRAME').length,1);
});

test('empty schedule shows standby with no decoder or iframe',async()=>{
  const h=await harness([]);
  assert.equal(h.events.filter((e)=>e.startsWith('play:')).length,0);
  assert.equal(h.events.filter((e)=>e==='append:IFRAME').length,0);
});

test('publishing a replacement loop removes the old menu on the next poll',async()=>{
  const h=await harness([entry('sandwich','html',80)]);
  h.setPlaylist([entry('welcome','html',12)]);
  await h.run(2000);
  const frames=h.nodes.filter((n)=>n.tagName==='IFRAME');
  assert.equal(frames[0].isConnected,false);
  assert.equal(frames[1].isConnected,true);
  assert.equal(frames[1].src,'/welcome');
});

test('publishing an empty loop releases the active video',async()=>{
  const h=await harness([entry('clip','video')]);
  h.setPlaylist([]);
  await h.run(2000);
  assert.ok(h.events.includes('release:VIDEO'));
  assert.equal(h.nodes.find((n)=>n.tagName==='VIDEO').isConnected,false);
});

test('published reordering is used at the next handoff without restarting current content',async()=>{
  const a=entry('a','html'), b=entry('b','html'), c=entry('c','html');
  const h=await harness([a,b,c]);
  h.setPlaylist([a,c,b]);
  await h.run(2000);
  assert.equal(h.events.filter((e)=>e==='append:IFRAME').length,1);
  await h.run(1000);
  assert.equal(h.nodes.filter((n)=>n.tagName==='IFRAME').at(-1).src,'/c');
});

test('production video progresses when the frame callback API never delivers',async()=>{
  const h=await harness([entry('page','html'),entry('clip','video')],{frameCallbacks:false});
  await h.run(1000);
  const outgoing=h.nodes.find((n)=>n.tagName==='IFRAME');
  assert.equal(outgoing.isConnected,true);
  await h.run(30);
  assert.equal(outgoing.isConnected,false,'Progressing media must hand off on the first polled currentTime advance, with no fixed dwell');
  assert.equal(h.nodes.find((n)=>n.tagName==='VIDEO').isConnected,true);
});

test('production video fallback retains the outgoing surface when media has not progressed',async()=>{
  const h=await harness([entry('page','html'),entry('clip','video')],{frameCallbacks:false,currentTime:0,advance:false});
  await h.run(1000);
  const outgoing=h.nodes.find((n)=>n.tagName==='IFRAME');
  await h.run(30);
  assert.equal(outgoing.isConnected,true);
  await h.run(15000);
  assert.equal(h.nodes.find((n)=>n.tagName==='VIDEO').isConnected,false);
});

test('poll and timeupdate cannot multiply the single pending frame callback',async()=>{
  const clip=entry('a','video',10);
  const h=await harness([clip],{duration:10,currentTime:1,frameCallbacks:false});
  await h.run(30); // readiness fallback, leaving rVFC undelivered
  const v=h.nodes.find((n)=>n.tagName==='VIDEO');
  for(let i=0;i<5;i++) { v.ontimeupdate(); await h.run(50); }
  assert.equal(v.callbacks.size,1,'Exactly one callback, including cancelled readiness callback');
});

test('full clip keeps its final frames until EOS, not a 50ms early handoff',async()=>{
  const clip=entry('a','video',10); clip.asset.video_duration=10;
  const h=await harness([clip,entry('b','video')],{duration:10,currentTime:9.92});
  await h.run(50);
  assert.equal(h.events.includes('play:/b'),false,'Frames 598-600 must not be cut off');
  const v=h.nodes.find((n)=>n.tagName==='VIDEO'); v._ended=true; v.onended(); await h.settle();
  assert.ok(h.events.includes('play:/b'));
});

test('EOS does not double-advance when an old ended handler fires',async()=>{
  const clip=entry('a','video',10);
  const h=await harness([clip,entry('b','video'),entry('c','video')],{duration:10,currentTime:9.92});
  const first=h.nodes.find((n)=>n.tagName==='VIDEO');
  await h.settle();
  const lateEnded=first.onended; // Captured while 'a' is live, fired after the hand-off.
  first._ended=true; first.onended(); await h.settle();
  const plays=h.events.filter((e)=>e.startsWith('play:')).length;
  assert.ok(lateEnded,'The first clip must still have its ended handler armed');
  lateEnded();
  await h.settle();
  assert.equal(h.events.filter((e)=>e.startsWith('play:')).length,plays,'A second end signal for the same item must not advance again');
});

test('outgoing video holds on the next clip poster as its cover',async()=>{
  const first=entry('a','video'); const second=entry('b','video'); second.asset.poster='/b-poster.jpg';
  const h=await harness([first,second]);
  const v=h.nodes.find((n)=>n.tagName==='VIDEO'); v._ended=true; v.onended(); await h.settle();
  const covers=h.nodes.filter((n)=>n.className==='cover');
  assert.equal(covers.length,1);
  assert.equal(covers[0]._src,'/b-poster.jpg','The cover must be the incoming clip first frame, not the outgoing end poster');
});

test('same clip back to back restarts in place without a new video element',async()=>{
  const h=await harness([entry('a','video')]);
  const v=h.nodes.find((n)=>n.tagName==='VIDEO'); v._ended=true; v.onended(); await h.settle();
  assert.equal(h.nodes.filter((n)=>n.tagName==='VIDEO').length,1,'No second decoder may be created');
  assert.equal(h.events.filter((e)=>e==='release:VIDEO').length,0,'The clip must not be released');
  assert.equal(h.nodes.filter((n)=>n.className==='cover').length,0,'No cover for an in-place restart');
  assert.ok(h.events.filter((e)=>e==='play:/a').length>=2,'The clip must play again');
});

test('a looping clip restarts in place exactly once per loop',async()=>{
  const clip=entry('a','video',10); clip.asset.video_duration=10;
  const h=await harness([clip],{duration:10,currentTime:9.92,search:'?debug=1'});
  const v=h.nodes.find((n)=>n.tagName==='VIDEO'); v._ended=true; v.onended(); await h.settle();
  assert.equal(h.events.filter((e)=>e==='seek').length,1,'ontimeupdate and the poll must not both restart the clip');
  assert.equal(h.logs.filter((l)=>l.includes('restart in place')).length,1);
});

test('the debug quality line is one sample per second, not one per frame callback',async()=>{
  const clip=entry('a','video',10); clip.asset.video_duration=10;
  const h=await harness([clip],{duration:10,currentTime:1,frames:60,dropped:0,search:'?debug=1'});
  await h.settle();
  await h.run(1000);
  await h.run(1000);
  const samples=h.logs.filter((l)=>l.includes('"ev":"quality"'));
  assert.equal(samples.length,2,'Exactly one quality sample per elapsed second');
  assert.ok(samples[1].includes('"ct":3'),'The sample must read the live currentTime, not a stale closure');
});

test('full-length duration timer is a watchdog, never an EOS substitute',async()=>{
  const clip=entry('a','video',10);
  const h=await harness([clip],{duration:10,currentTime:1});
  assert.equal([...h.timers.values()].some(t=>t.ms===10000),false);
  assert.ok([...h.timers.values()].some(t=>t.ms===15000));
});

test('cancelled frame callback cannot schedule or restart the new loop generation',async()=>{
  const h=await harness([entry('a','video',10)],{duration:10,currentTime:1,frameCallbacks:false});
  await h.run(30);
  const v=h.nodes.find(n=>n.tagName==='VIDEO');
  const oldFrame=[...v.callbacks.values()][0], oldEnd=v.onended;
  v._ended=true; oldEnd(); await h.settle();
  v._ended=false;
  oldFrame(0,{presentedFrames:600}); oldEnd(); await h.settle();
  assert.equal(h.events.filter(e=>e==='seek').length,1);
  assert.equal(v.callbacks.size,1);
  const id=[...v.callbacks.keys()][0], callback=v.callbacks.get(id);
  v.callbacks.delete(id); callback(0,{presentedFrames:1});
  assert.equal(v.callbacks.size,1,'A live callback replaces itself with exactly one successor');
});

test('page teardown cancels a never-delivered callback and stale polls',async()=>{
  const h=await harness([entry('a','video',10)],{duration:10,currentTime:1,frameCallbacks:false});
  await h.run(30);
  const v=h.nodes.find(n=>n.tagName==='VIDEO');
  h.setPlaylist([]); await h.run(2000);
  assert.equal(v.callbacks.size,0);
  assert.equal([...h.timers.values()].some(t=>t.ms===50),false);
});

test('600-frame loops retain every callback and produce one EOS restart per loop',async()=>{
  const clip=entry('a','video',10);
  const h=await harness([clip],{duration:10,currentTime:0,frameCallbacks:false,search:'?debug=1'});
  await h.run(30);
  const v=h.nodes.find(n=>n.tagName==='VIDEO');
  for(let loop=0;loop<3;loop++) {
    v._ended=false;
    for(let n=1;n<=600;n++) {
      v._t=n/60;
      const id=[...v.callbacks.keys()][0], callback=v.callbacks.get(id);
      v.callbacks.delete(id); callback(0,{presentedFrames:loop*600+n});
      v.ontimeupdate();
      assert.equal(v.callbacks.size,1);
      assert.equal(h.events.filter(e=>e==='seek').length,loop,'No restart before frame 600 and EOS');
    }
    const oldEnd=v.onended; v._ended=true; oldEnd(); await h.settle();
    v._ended=false; oldEnd(); await h.settle();
    assert.equal(h.events.filter(e=>e==='seek').length,loop+1);
  }
  const ends=h.logs.filter(l=>l.includes('"ev":"end transition"')).map(l=>JSON.parse(l.slice(9)));
  assert.equal(ends.length,3);
  assert.ok(ends.every(e=>e.callbacks===600 && e.reason==='ended'));
  assert.deepEqual(ends.map(e=>e.presented),[600,1200,1800]);
});

test('holepunch clock progress never fabricates presented-frame counts',async()=>{
  const h=await harness([entry('a','video',10)],{duration:10,currentTime:0,frameCallbacks:false,search:'?debug=1'});
  await h.run(30);
  const v=h.nodes.find(n=>n.tagName==='VIDEO'); v._ended=true; v.onended(); await h.settle();
  const end=JSON.parse(h.logs.find(l=>l.includes('"ev":"end transition"')).slice(9));
  assert.equal(end.presented,null); assert.equal(end.callbacks,0);
});

test('hand-off has no fixed dwell timer',async()=>{
  const h=await harness([entry('page','html'),entry('clip','video')]);
  await h.run(1000);
  assert.equal(Array.from(h.timers.values()).some((t)=>t.ms===750),false);
});

test('refused native standby at startup still starts the first video (no 0-commit deadlock)',async()=>{
  const h=await harness([entry('a','video'),entry('b','video')],{native:true,preflight:true});
  const reservation=h.events.find(e=>e.type==='native-stage'); assert.ok(reservation);
  assert.equal(h.events.includes('play:/a'),false);
  h.native({event:'not-ready',request:reservation.request,generation:'0'}); await h.settle();
  assert.ok(h.events.includes('play:/a'),'first video must start after a capacity refusal');
  assert.equal(h.events.some(e=>e.type==='native-playback' && e.running),false,'no guard claimed without a prepared successor');
});

test('refused native standby at EOS falls back to the serial path, never retries forever',async()=>{
  const h=await harness([entry('a','video'),entry('b','video')],{native:true});
  const reservation=h.events.find(e=>e.type==='native-stage'); assert.ok(reservation);
  h.native({event:'not-ready',request:reservation.request,generation:'0'});
  const old=h.nodes.find(n=>n.tagName==='VIDEO'); old._ended=true; old.onended(); await h.settle();
  assert.equal(h.events.some(e=>e.type==='native-activate'),false);
  assert.ok(h.events.includes('release:VIDEO'),'outgoing is released by the serial hand-off');
  assert.ok(h.events.includes('play:/b'),'next clip plays via the serial path');
});

test('transient refusal is retried with backoff, permanent refusal does not thrash every poll',async()=>{
  const h=await harness([entry('a','video'),entry('b','video')],{native:true});
  const stages=()=>h.events.filter(e=>e.type==='native-stage');
  let r=stages()[0]; assert.ok(r);
  h.native({event:'not-ready',request:r.request,generation:'0'}); await h.settle();
  const n0=stages().length;
  await h.run(2000);                        // poll after >=1 s backoff: staged again
  assert.equal(stages().length,n0+1,'first refusal is retried on the next poll');
  r=stages()[stages().length-1];
  h.native({event:'not-ready',request:r.request,generation:'0'}); await h.settle();
  const n1=stages().length;
  await h.run(2000);                        // 2nd refusal backs off 2 s: still allowed at +2 s
  const n2=stages().length;
  r=stages()[stages().length-1];
  h.native({event:'not-ready',request:r.request,generation:'0'}); await h.settle();
  await h.run(2000);                        // 3rd refusal backs off 4 s: no restage at +2 s
  assert.equal(stages().length,n2,'backoff grows instead of re-staging on every poll');
  assert.ok(n1>=n0+1);
});

test('lost native readiness notice is bounded: waits then serial fallback',async()=>{
  const h=await harness([entry('a','video'),entry('b','video')],{native:true});
  const old=h.nodes.find(n=>n.tagName==='VIDEO'); old._ended=true; old.onended(); await h.settle();
  assert.equal(h.events.includes('play:/b'),false,'still waiting inside the bound');
  assert.ok(h.timers.size>0);
});
