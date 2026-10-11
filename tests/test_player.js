// Exercise player handoffs without a GPU: the old surface must survive until readiness.
const assert = require('node:assert/strict');
const {readFileSync} = require('node:fs');
const vm = require('node:vm');
const test = require('node:test');

async function harness(entries, options={}) {
  const events=[], timers=new Map(), listeners={}, nodes=[];
  let serial=0, revision=1;
  const timeout=(fn,ms) => { const id=++serial; timers.set(id,{fn,ms}); return id; };
  class Element {
    constructor(tag) { this.tagName=tag.toUpperCase(); this.style={}; this.children=[]; this.isConnected=false; this.classList={add(){},remove(){}}; this.readyState=options.readyState ?? 3; this.currentTime=options.currentTime ?? 1; this.contentWindow={postMessage:() => queueMicrotask(() => (listeners.message||[]).forEach((fn) => fn({source:this.contentWindow,data:{type:'signage-held'}})))}; nodes.push(this); }
    setAttribute() {}
    appendChild(node) { node.isConnected=true; this.children.push(node); events.push('append:'+node.tagName); }
    remove() { this.isConnected=false; events.push('remove:'+this.tagName); }
    removeAttribute(key) { if(key==='src') { this._src=''; events.push('release:'+this.tagName); } }
    load() {}
    pause() {}
    set src(value) { this._src=value; if(this.tagName==='IMG') queueMicrotask(() => this.onload?.()); if(this.tagName==='IFRAME') queueMicrotask(() => (listeners.message||[]).forEach((fn) => fn({source:this.contentWindow,data:{type:'signage-ready'}}))); }
    get src() { return this._src; }
    play() { events.push('play:'+this.src); queueMicrotask(() => this.onplaying?.()); return Promise.resolve(); }
    requestVideoFrameCallback(fn) { if(options.frameCallbacks===false) return; queueMicrotask(() => { events.push('video-ready'); fn(); }); }
  }
  const stage=new Element('div'), body=new Element('body'), head=new Element('head'), status=new Element('div');
  const window={parent:{postMessage(){}},addEventListener:(type,fn) => (listeners[type]??=[]).push(fn),removeEventListener:(type,fn) => { listeners[type]=(listeners[type]||[]).filter((f)=>f!==fn); }};
  const context={console, URL,URLSearchParams,Promise,Date,Map,Image:class extends Element { constructor(){super('img');} },
    window,document:{body,head,createElement:(tag)=>new Element(tag),getElementById:(id)=>id==='stage'?stage:status,addEventListener(){}},
    location:{origin:'http://signage.test',search:''},setTimeout:timeout,clearTimeout:(id)=>timers.delete(id),requestAnimationFrame:(fn)=>queueMicrotask(fn),
    fetch:async()=>({ok:true,json:async()=>({entries,revision,now:Date.now()/1000,next_change:null})})};
  vm.runInNewContext(readFileSync('www/player/player.js','utf8'),context);
  const settle=async()=>{ for(let i=0;i<80;i++) await Promise.resolve(); };
  await settle();
  return {events,nodes,timers,settle,setPlaylist:(next)=>{entries=next;revision++;},run:async(ms)=>{ const pair=Array.from(timers).find(([,t])=>t.ms===ms); assert.ok(pair,`Missing timer ${ms}`); timers.delete(pair[0]); pair[1].fn(); await settle(); }};
}
const entry=(id,kind,duration=1)=>({id,asset_id:id,duration,enabled:true,start:null,end:null,asset:{id,kind,name:id,url:'/'+id,poster:'/first.jpg',end_poster:'/last.jpg',video_duration:1}});

test('video handoff keeps an opaque still until the next decoded frame is ready',async()=>{
  const h=await harness([entry('a','video'),entry('b','video')]);
  await h.run(1000);
  const secondPlay=h.events.indexOf('play:/b');
  assert.ok(secondPlay>h.events.indexOf('release:VIDEO'));
  const cover=h.nodes.find((n)=>n.className==='cover');
  assert.ok(cover);
  assert.equal(cover.isConnected,false);
  const lastReady=h.events.lastIndexOf('video-ready');
  assert.ok(h.events.lastIndexOf('remove:IMG')>lastReady);
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
  await h.run(750);
  assert.equal(outgoing.isConnected,false,'Playing media must use the bounded fallback instead of retaining the old surface until the start timeout');
  assert.equal(h.nodes.find((n)=>n.tagName==='VIDEO').isConnected,true);
});

test('production video fallback retains the outgoing surface when media has not progressed',async()=>{
  const h=await harness([entry('page','html'),entry('clip','video')],{frameCallbacks:false,currentTime:0});
  await h.run(1000);
  const outgoing=h.nodes.find((n)=>n.tagName==='IFRAME');
  await h.run(750);
  assert.equal(outgoing.isConnected,true);
  await h.run(15000);
  assert.equal(h.nodes.find((n)=>n.tagName==='VIDEO').isConnected,false);
});
