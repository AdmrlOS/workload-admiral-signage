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
    load() {}
    pause() {}
    set src(value) { this._src=value; if(this.tagName==='IMG') queueMicrotask(() => this.onload?.()); if(this.tagName==='IFRAME') queueMicrotask(() => (listeners.message||[]).forEach((fn) => fn({source:this.contentWindow,data:{type:'signage-ready'}}))); }
    get src() { return this._src; }
    // Media time advances on each read while playing unless options.advance is false (a stalled decoder).
    get currentTime() { return this._t; }
    set currentTime(v) { this._t = v; events.push('seek'); }
    get duration() { return options.duration ?? NaN; }
    getVideoPlaybackQuality() { return {totalVideoFrames: options.frames ?? 0, droppedVideoFrames: options.dropped ?? 0}; }
    play() { events.push('play:'+this.src); this._playing=true; queueMicrotask(() => this.onplaying?.()); return Promise.resolve(); }
    requestVideoFrameCallback(fn) { if(options.frameCallbacks===false || this._rvfc) return; this._rvfc=true; queueMicrotask(() => { events.push('video-ready'); fn(); }); }
  }
  const stage=new Element('div'), body=new Element('body'), head=new Element('head'), status=new Element('div');
  const window={parent:{postMessage(){}},addEventListener:(type,fn) => (listeners[type]??=[]).push(fn),removeEventListener:(type,fn) => { listeners[type]=(listeners[type]||[]).filter((f)=>f!==fn); }};
  const context={console:{...console,log:(...a)=>{logs.push(a.map(String).join(' '));console.log(...a);}}, URL,URLSearchParams,Promise,Date,Map,Image:class extends Element { constructor(){super('img');} },
    window,document:{body,head,createElement:(tag)=>new Element(tag),getElementById:(id)=>id==='stage'?stage:status,addEventListener(){}},
    location:{origin:'http://signage.test',search:options.search||''},setTimeout:timeout,clearTimeout:(id)=>timers.delete(id),requestAnimationFrame:(fn)=>queueMicrotask(fn),
    fetch:async()=>({ok:true,json:async()=>({entries,revision,now:Date.now()/1000,next_change:null})})};
  vm.runInNewContext(readFileSync('www/player/player.js','utf8'),context);
  const settle=async()=>{ for(let i=0;i<80;i++) await Promise.resolve(); };
  await settle();
  return {events,nodes,timers,logs,settle,setPlaylist:(next)=>{entries=next;revision++;},run:async(ms)=>{ const pair=Array.from(timers).find(([,t])=>t.ms===ms); assert.ok(pair,`Missing timer ${ms}`); timers.delete(pair[0]); for (const n of nodes) if (n._playing && options.advance !== false) n._t += ms/1000; pair[1].fn(); await settle(); }};
}
const entry=(id,kind,duration=1)=>({id,asset_id:id,duration,enabled:true,start:null,end:null,asset:{id,kind,name:id,url:'/'+id,poster:'/first.jpg',end_poster:'/last.jpg',video_duration:1}});

test('video handoff keeps an opaque still until the next video makes progress',async()=>{
  const h=await harness([entry('a','video'),entry('b','video')]);
  await h.run(1000);
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

test('early hand-off advances before ended when media time nears the end',async()=>{
  const clip=entry('a','video',10); clip.asset.video_duration=10;
  const h=await harness([clip,entry('b','video')],{duration:10,currentTime:9.92});
  await h.run(50);
  assert.ok(h.events.includes('play:/b'),'The next item must start before the first clip reports ended');
  assert.equal(h.nodes.filter((n)=>n.tagName==='VIDEO'&&n.isConnected).length,1,'Still one live decoder');
});

test('early hand-off does not double-advance when ended and the poll both fire',async()=>{
  const clip=entry('a','video',10);
  const h=await harness([clip,entry('b','video'),entry('c','video')],{duration:10,currentTime:9.92});
  const first=h.nodes.find((n)=>n.tagName==='VIDEO');
  await h.settle();
  const lateEnded=first.onended; // Captured while 'a' is live, fired after the hand-off.
  await h.run(50);
  const plays=h.events.filter((e)=>e.startsWith('play:')).length;
  assert.ok(lateEnded,'The first clip must still have its ended handler armed');
  lateEnded();
  await h.settle();
  assert.equal(h.events.filter((e)=>e.startsWith('play:')).length,plays,'A second end signal for the same item must not advance again');
});

test('outgoing video holds on the next clip poster as its cover',async()=>{
  const first=entry('a','video'); const second=entry('b','video'); second.asset.poster='/b-poster.jpg';
  const h=await harness([first,second]);
  await h.run(1000);
  const covers=h.nodes.filter((n)=>n.className==='cover');
  assert.equal(covers.length,1);
  assert.equal(covers[0]._src,'/b-poster.jpg','The cover must be the incoming clip first frame, not the outgoing end poster');
});

test('same clip back to back restarts in place without a new video element',async()=>{
  const h=await harness([entry('a','video')]);
  await h.run(1000);
  assert.equal(h.nodes.filter((n)=>n.tagName==='VIDEO').length,1,'No second decoder may be created');
  assert.equal(h.events.filter((e)=>e==='release:VIDEO').length,0,'The clip must not be released');
  assert.equal(h.nodes.filter((n)=>n.className==='cover').length,0,'No cover for an in-place restart');
  assert.ok(h.events.filter((e)=>e==='play:/a').length>=2,'The clip must play again');
});

test('a looping clip restarts in place exactly once per loop',async()=>{
  const clip=entry('a','video',10); clip.asset.video_duration=10;
  const h=await harness([clip],{duration:10,currentTime:9.92,search:'?debug=1'});
  await h.run(50);
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

test('hand-off has no fixed dwell timer',async()=>{
  const h=await harness([entry('page','html'),entry('clip','video')]);
  await h.run(1000);
  assert.equal(Array.from(h.timers.values()).some((t)=>t.ms===750),false);
});
