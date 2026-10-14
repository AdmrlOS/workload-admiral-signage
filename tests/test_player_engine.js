// Player on the persistent engine: no <video> element for engine assets, gapless queueing,
// explicit release for legacy assets, hide (not teardown) on HTML pages.
const assert = require('node:assert/strict');
const {readFileSync} = require('node:fs');
const vm = require('node:vm');
const test = require('node:test');

const entry = (id, kind, engine = kind === 'video') => ({id, duration: kind === 'video' ? 0 : 5, enabled: true,
  asset: {id, kind, name: id, url: '/media/' + id + '.mp4', ...(engine ? {engine: {id, frames: 600, duration_ms: 10000}} : {})}});

async function harness(entries) {
  const events = [], timers = new Map(), listeners = {}, nodes = [];
  let serial = 0, playSeq = 100;
  class Element {
    constructor(tag) { this.tagName = tag.toUpperCase(); this.style = {}; this.children = []; this.classList = {add() {}, remove() {}};
      this.contentWindow = {postMessage: () => queueMicrotask(() => (listeners.message || []).forEach((fn) => fn({source: this.contentWindow, data: {type: 'signage-held'}})))}; nodes.push(this); }
    setAttribute() {} appendChild(n) { n.isConnected = true; events.push('append:' + n.tagName); }
    remove() { events.push('remove:' + this.tagName); } removeAttribute() {} load() {} pause() {}
    addEventListener() {} removeEventListener() {}
    set src(v) { this._src = v; if (this.tagName === 'IFRAME') queueMicrotask(() => (listeners.message || []).forEach((fn) => fn({source: this.contentWindow, data: {type: 'signage-ready'}}))); }
    get src() { return this._src; }
    get readyState() { return 3; } get currentTime() { return (this._t = (this._t || 0) + 0.1); } set currentTime(v) { this._t = v; }
    play() { events.push('video-play:' + this._src); queueMicrotask(() => this.onplaying?.()); return Promise.resolve(); }
  }
  const engineListeners = {};
  const fire = (type, detail) => (engineListeners[type] || []).forEach((fn) => fn({type, detail}));
  let resolveReady;
  const fake = {
    ready: new Promise((r) => { resolveReady = r; }),
    addEventListener: (t, fn) => (engineListeners[t] ??= []).push(fn), removeEventListener() {},
    transport: {command: async (cmd) => { events.push('engine:' + cmd); return {ok: true}; }},
    play: async (url, o = {}) => { const id = ++playSeq; events.push(`engine:play:${url}:${o.at}:${id}`); return {ok: true, play_id: id}; },
    cancel: async (url) => { events.push('engine:cancel:' + url); return {ok: true}; },
    hide: async () => { events.push('engine:hide'); return {ok: true}; },
    release: async () => { events.push('engine:release'); return {ok: true}; },
    destroy: () => events.push('engine:destroy'),
  };
  const window = {parent: {postMessage() {}}, addEventListener: (t, fn) => (listeners[t] ??= []).push(fn), removeEventListener() {},
    SignageVideo: {attach: () => { events.push('engine:attach'); return fake; }, host: () => ({dropFrame: () => events.push('engine:drop-frame')})}};
  const stage = new Element('div'), status = new Element('div');
  const context = {console: {...console, log() {}}, URL, URLSearchParams, Promise, Date, Map, Image: class extends Element { constructor() { super('img'); } },
    window, document: {body: new Element('body'), documentElement: {classList: {toggle: (c, on) => events.push('root:' + c + ':' + on)}}, createElement: (t) => new Element(t), getElementById: (id) => id === 'stage' ? stage : status, addEventListener() {}},
    location: {origin: 'http://s', href: 'http://127.0.0.1:8080/player/', search: ''},
    setTimeout: (fn, ms) => { const id = ++serial; timers.set(id, {fn, ms}); return id; }, clearTimeout: (id) => timers.delete(id),
    requestAnimationFrame: (fn) => queueMicrotask(fn),
    fetch: async () => ({ok: true, json: async () => ({entries, revision: 1, now: Date.now() / 1000, next_change: null})})};
  vm.runInNewContext(readFileSync('www/player/player.js', 'utf8'), context);
  const settle = async () => { for (let i = 0; i < 100; i++) await Promise.resolve(); };
  await settle();
  const lastPlay = () => events.filter((e) => e.startsWith('engine:play:')).at(-1);
  const playId = (e) => Number(e.split(':').at(-1));
  return {events, nodes, timers, settle, fire, ready: async () => { resolveReady({warm: true}); await settle(); }, lastPlay, playId,
    setPlaylist: (next) => { entries = next; }, hide: () => (listeners.pagehide || []).forEach((fn) => fn())};
}

test('engine assets play through one engine and are queued gaplessly, without any <video>', async () => {
  const h = await harness([entry('a', 'video'), entry('b', 'video')]);
  assert.ok(h.events.includes('engine:attach'));
  assert.equal(h.events.some((e) => e.startsWith('engine:play')), false, 'no timed playback before engine preflight');
  await h.ready();
  const first = h.lastPlay();
  assert.match(first, /^engine:play:\/media\/a\.mp4:boundary:/);
  h.fire('started', {play_id: h.playId(first), asset: 'a'}); await h.settle();
  const queued = h.lastPlay();
  assert.match(queued, /^engine:play:\/media\/b\.mp4:boundary:/, 'next item is queued on the engine timeline while A plays');
  h.fire('ended', {play_id: h.playId(first), reason: 'handoff'});
  h.fire('started', {play_id: h.playId(queued), asset: 'b'}); await h.settle();
  assert.match(h.lastPlay(), /^engine:play:\/media\/a\.mp4:boundary:/, 'following item is queued after the handoff');
  assert.equal(h.nodes.some((n) => n.tagName === 'VIDEO'), false, 'engine videos never create a WebKit media pipeline');
  assert.equal(h.events.includes('engine:hide'), false);
  assert.equal(h.events.filter((e) => e === 'engine:release').length, 0);
});

test('HTML after an engine video hides the engine (decoder retained); legacy video releases it explicitly', async () => {
  const h = await harness([entry('a', 'video'), entry('page', 'html'), entry('legacy', 'video', false)]);
  await h.ready();
  const first = h.lastPlay();
  h.fire('started', {play_id: h.playId(first), asset: 'a'}); await h.settle();
  assert.equal(h.events.filter((e) => e.startsWith('engine:play')).length, 1, 'HTML is not queued on the engine');
  h.fire('ended', {play_id: h.playId(first), reason: 'complete'}); await h.settle();
  assert.ok(h.events.includes('append:IFRAME'));
  assert.ok(h.events.includes('engine:hide'));
  assert.equal(h.events.includes('engine:release'), false);
  const htmlTimer = [...h.timers].find(([, t]) => t.ms === 5000);
  h.timers.delete(htmlTimer[0]); htmlTimer[1].fn(); await h.settle();
  const release = h.events.indexOf('engine:release'), video = h.events.indexOf('append:VIDEO');
  assert.ok(release >= 0 && video > release, 'legacy <video> only after the engine released the decoder');
  assert.ok(h.events.includes('engine:drop-frame'), 'removed iframe releases any engine component it hosted');
});

test('playlist edit replaces the queued next engine item', async () => {
  const h = await harness([entry('a', 'video'), entry('b', 'video')]);
  await h.ready();
  h.fire('started', {play_id: h.playId(h.lastPlay()), asset: 'a'}); await h.settle();
  assert.match(h.lastPlay(), /b\.mp4/);
  h.setPlaylist([entry('a', 'video'), entry('c', 'video')]);
  const poll = [...h.timers].find(([, t]) => t.ms === 2000);
  h.timers.delete(poll[0]); poll[1].fn(); await h.settle();
  assert.ok(h.events.includes('engine:cancel:/media/b.mp4'));
  assert.match(h.lastPlay(), /c\.mp4:boundary/);
});

test('pagehide destroys the controller (stream closes -> engine hides and pauses)', async () => {
  const h = await harness([entry('a', 'video')]);
  await h.ready();
  h.hide();
  assert.ok(h.events.includes('engine:destroy'));
});

test('the page is transparent only while the engine video is the current surface (video plane lies below the page)', async () => {
  const h = await harness([entry('a', 'video'), entry('page', 'html')]);
  await h.ready();
  const first = h.lastPlay();
  h.fire('started', {play_id: h.playId(first), asset: 'a'}); await h.settle();
  assert.equal(h.events.filter((e) => e.startsWith('root:engine-live')).at(-1), 'root:engine-live:true',
    'html/body/standby must not paint over the video plane');
  h.fire('ended', {play_id: h.playId(first), reason: 'complete'}); await h.settle();
  assert.equal(h.events.filter((e) => e.startsWith('root:engine-live')).at(-1), 'root:engine-live:false',
    'an HTML entry gets the normal opaque page again');
});
