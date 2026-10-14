// The REAL player.js + video-engine.js (HTTP transport, FrameHost proxy and a real page-side
// FrameTransport controller) against a MOCK ENGINE SERVER that implements the engine's line
// protocol semantics (signage-video-engine.c handle_command / feed_locked / advance_locked):
//   - one owner, preempt on acquire, owner-gone (hide + pause, clip retained) when the owner's
//     event stream closes;
//   - ONE queued slot: a newer play supersedes the queued one; at:'boundary' waits for the current
//     clip to complete, at:'now' is a flushing switch; an idle engine starts immediately;
//   - a hidden/paused current clip does NOT advance, so a boundary request behind it never starts;
//   - the first presented frame produces 'started' (PRESENTED), clip end 'ended' reason complete.
// Virtual time: the 15 s readiness watchdog and every clip duration run on one fake clock.
//
// SIGNAGE_PLAYER_JS=<path> runs the same scenarios against another player build (red/green proof).
const assert = require('node:assert/strict');
const {readFileSync} = require('node:fs');
const vm = require('node:vm');
const test = require('node:test');

const PLAYER = process.env.SIGNAGE_PLAYER_JS || 'www/player/player.js';
const flush = async () => { for (let i = 0; i < 60; i++) await Promise.resolve(); };

class Clock {
  constructor() { this.now = 0; this.timers = []; this.seq = 0; }
  setTimeout(fn, ms) { const id = ++this.seq; this.timers.push({id, at: this.now + Math.max(0, ms || 0), fn}); return id; }
  clearTimeout(id) { this.timers = this.timers.filter((t) => t.id !== id); }
  async advance(ms) {
    const target = this.now + ms;
    for (;;) {
      await flush();
      const due = this.timers.filter((t) => t.at <= target).sort((a, b) => a.at - b.at || a.id - b.id)[0];
      if (!due) break;
      this.timers = this.timers.filter((t) => t !== due);
      this.now = Math.max(this.now, due.at);
      due.fn();
    }
    this.now = target; await flush();
  }
}

const CLIPS = {'promo-b': 20000, demo: 10000, 'promo-a': 10000, pipclip: 10000};

class MockEngine {
  constructor(clock) {
    this.clock = clock; this.owner = null; this.cur = null; this.queued = null; this.idle = true; this.visible = false; this.intent = false;
    this.streams = new Map(); this.log = []; this.presented = []; this.commands = []; this.holdLoading = new Set();
  }
  emit(event, only) {
    const e = {...event, mono_us: this.clock.now * 1000};
    this.log.push(e);
    for (const [client, list] of this.streams) if (!only || client === only) for (const s of list) s.push(e);
  }
  assetOf(asset) { const m = /\/media\/([^/.]+)\.mp4/.exec(asset?.url || ''); return m && CLIPS[m[1]] ? {id: m[1], ms: CLIPS[m[1]]} : null; }
  subscribe(client) { const list = this.streams.get(client) || []; const s = {push: null}; list.push(s); this.streams.set(client, list); return s; }
  unsubscribe(client, s) {
    const list = (this.streams.get(client) || []).filter((x) => x !== s);
    if (list.length) { this.streams.set(client, list); return; }
    this.streams.delete(client);
    if (this.owner === client) { this.owner = null; this.visible = false; this.intent = false; this.apply(); this.emit({event: 'owner-gone', code: 'client-gone'}); }
  }
  playing() { return this.cur && !this.idle && this.visible && this.intent; }
  apply() { // (re)arm the end-of-clip timer according to visibility/intent
    if (this.endTimer) { this.clock.clearTimeout(this.endTimer); this.endTimer = null; }
    if (this.cur && this.cur.since != null) { this.cur.remaining -= this.clock.now - this.cur.since; this.cur.since = null; }
    if (this.playing()) {
      this.cur.since = this.clock.now;
      this.endTimer = this.clock.setTimeout(() => this.clipEnded(), Math.max(0, this.cur.remaining));
    }
  }
  begin(asset, playId, client) {
    this.cur = {asset: asset.id, playId, client, remaining: asset.ms, since: null, presented: false};
    this.idle = false; this.apply();
    this.clock.setTimeout(() => this.firstFrame(this.cur), 16);
  }
  firstFrame(inst) {
    if (this.cur !== inst || inst.presented) return;
    if (!this.visible) { this.pendingFrame = inst; return; }
    inst.presented = true; this.presented.push({asset: inst.asset, playId: inst.playId, t: this.clock.now});
    this.emit({event: 'started', asset: inst.asset, play_id: inst.playId, presented: 1});
  }
  clipEnded() {
    const done = this.cur; this.endTimer = null;
    if (this.queued) {
      const q = this.queued; this.queued = null;
      this.emit({event: 'ended', asset: done.asset, play_id: done.playId, reason: 'handoff'});
      this.emit({event: 'activated', asset: q.asset.id, play_id: q.playId});
      this.begin(q.asset, q.playId, q.client);
    } else {
      this.idle = true; this.cur.remaining = 0;
      this.emit({event: 'ended', asset: done.asset, play_id: done.playId, reason: 'complete'});
    }
  }
  command(body) {
    const {cmd, client} = body;
    this.commands.push({t: this.clock.now, ...body});
    const ack = (fields) => ({event: 'ack', cmd, client, ...fields});
    const needsOwner = ['play', 'pause', 'resume', 'seek', 'show', 'hide', 'rect', 'stop'].includes(cmd);
    if (needsOwner && this.owner && this.owner !== client) return ack({ok: false, code: 'not-owner'});
    if (cmd === 'acquire') {
      if (this.owner && this.owner !== client) this.emit({event: 'preempted', client: this.owner, by: client});
      this.owner = client; return ack({ok: true});
    }
    if (cmd === 'rect') return ack({ok: true});
    if (cmd === 'hide' || cmd === 'show') { this.visible = cmd === 'show'; this.apply(); if (this.visible && this.pendingFrame) this.firstFrame(this.pendingFrame); return ack({ok: true}); }
    if (cmd === 'pause' || cmd === 'resume') { this.intent = cmd === 'resume'; this.apply(); return ack({ok: true}); }
    if (cmd === 'release') return ack({ok: true});
    if (cmd === 'cancel') {
      const a = this.assetOf(body.asset);
      const found = !!(a && this.queued && this.queued.asset.id === a.id);
      if (found) { this.queued = null; this.emit({event: 'cancelled', asset: a.id}); }
      return ack({ok: true, message: found ? 'cancelled' : 'not queued'});
    }
    if (cmd === 'prepare') { const a = this.assetOf(body.asset); return a ? ack({ok: true, message: 'ready'}) : ack({ok: false, code: 'no-prepared-variant'}); }
    if (cmd === 'play') {
      const asset = this.assetOf(body.asset);
      if (!asset) return ack({ok: false, code: 'no-prepared-variant'});
      if (!this.owner) this.owner = client;
      const playId = body.play_id, at = body.at || 'boundary';
      let message;
      if (!this.cur || this.idle) { this.queued = null; this.begin(asset, playId, client); message = 'started'; }
      else {
        if (this.queued) this.emit({event: 'superseded', asset: this.queued.asset.id, play_id: this.queued.playId, client: this.queued.client,
          by_play_id: playId, by_asset: asset.id, by_client: client});
        this.queued = {asset, playId, client, at};
        if (at === 'now') { const q = this.queued; this.queued = null; this.emit({event: 'activated', asset: asset.id, play_id: playId}); this.begin(asset, q.playId, q.client); message = 'switching'; }
        else message = 'queued';
      }
      this.intent = true; if (body.show !== false) this.visible = true;
      this.apply(); if (this.visible && this.pendingFrame) this.firstFrame(this.pendingFrame);
      return ack({ok: true, play_id: playId, message});
    }
    return ack({ok: false, code: 'invalid'});
  }
}

const entry = (id, kind, duration = 0, extra = {}) => ({id, duration, enabled: true,
  asset: {id, kind, name: id, url: kind === 'video' ? `/media/${id}.mp4` : `/content/${id}/index.html`, ...extra}});

async function harness(entries) {
  const clock = new Clock(), engine = new MockEngine(clock), nodes = [], listeners = {}, warnings = [], logs = [];
  let signalHooks = [];
  class Element {
    constructor(tag) {
      this.tagName = tag.toUpperCase(); this.style = {}; this.children = []; this.dataset = {}; this.classList = {add() {}, remove() {}, toggle() {}}; nodes.push(this);
      this.contentWindow = {postMessage: (message) => frameInbox(this, message)};
    }
    setAttribute() {} getAttribute() { return this._src || null; } appendChild(n) { n.isConnected = true; return n; }
    remove() { this.removed = true; this.isConnected = false; } removeAttribute() {} load() {} pause() {} addEventListener() {} removeEventListener() {}
    getBoundingClientRect() { return {left: 0, top: 0, right: 3840, bottom: 2160}; }
    set src(v) { this._src = v; if (this.tagName === 'IFRAME') queueMicrotask(() => frameBoot(this, v)); }
    get src() { return this._src; }
  }
  const frames = () => nodes.filter((n) => n.tagName === 'IFRAME' && !n.removed);
  const dispatchMessage = (event) => (listeners.message || []).forEach((fn) => fn(event));
  // ---- iframe page: handshake + (for menuboard scene C) a real SignageVideo controller over FrameTransport
  const pages = new Map();
  function frameInbox(frame, message) {
    if (message?.type === 'signage-hold') queueMicrotask(() => dispatchMessage({source: frame.contentWindow, data: {type: 'signage-held'}}));
    else if (message?.type === 'signage-video-host') pages.get(frame)?.deliver(message);
  }
  function frameBoot(frame, url) {
    queueMicrotask(() => dispatchMessage({source: frame.contentWindow, data: {type: 'signage-ready'}}));
    if (!/menuboard\/index\.html\?scene=C/.test(url)) return;
    const SV = context.window.SignageVideo;
    const parent = {postMessage: (message) => dispatchMessage({source: frame.contentWindow, data: message})};
    const pageListeners = [];
    const pageWindow = {addEventListener: (t, fn) => pageListeners.push(fn), removeEventListener() {}, devicePixelRatio: 1};
    const env = {window: pageWindow, parent, setTimeout: clock.setTimeout.bind(clock), clearTimeout: clock.clearTimeout.bind(clock)};
    pages.set(frame, {deliver: (data) => pageListeners.forEach((fn) => fn({source: parent, data}))});
    const client = 'iframe-' + frame.getAttribute('src').replace(/\W+/g, '').slice(-12) + '-' + nodes.indexOf(frame);
    const transport = new SV.FrameTransport(env, client);
    const box = new Element('div');
    const video = new SV.SignageVideo(box, env, {transport, client});
    frame.pageVideo = video; frame.pageClient = client;
    video.ready.then(() => video.play('/media/pipclip.mp4', {at: 'now'})).catch(() => {});
  }
  // ---- network: fetch bridge into the mock engine
  const fetch = async (url, options = {}) => {
    const path = new URL(url, 'http://127.0.0.1:8080').pathname;
    const json = (value) => ({ok: true, status: 200, json: async () => value});
    if (path === '/api/playlist') return json({entries, revision: 1, now: clock.now / 1000, next_change: null});
    if (path === '/api/preload') return json({ok: true});
    if (path === '/api/video/events') {
      const client = new URL(url, 'http://x').searchParams.get('client');
      const sub = engine.subscribe(client); const queue = []; let wake = null;
      const encoder = new TextEncoder();
      sub.push = (e) => { queue.push(encoder.encode('data: ' + JSON.stringify(e) + '\n\n')); wake?.(); wake = null; };
      engine.owner === null || true;
      options.signal?.addEventListener('abort', () => { engine.unsubscribe(client, sub); queue.push(null); wake?.(); });
      queueMicrotask(() => sub.push({event: 'engine-ready', warm: true}));
      return {ok: true, status: 200, body: {getReader: () => ({read: async () => {
        for (;;) { if (queue.length) { const v = queue.shift(); return v ? {value: v, done: false} : {done: true}; } await new Promise((r) => { wake = r; }); }
      }})}};
    }
    if (path === '/api/video/command') return json(engine.command(JSON.parse(options.body)));
    return {ok: false, status: 404, json: async () => ({})};
  };
  class AbortController { constructor() { const l = []; this.signal = {addEventListener: (t, fn) => l.push(fn)}; this.abort = () => l.forEach((fn) => fn()); } }
  const stage = new Element('div'), status = new Element('div');
  const window = {addEventListener: (t, fn) => (listeners[t] ??= []).push(fn), removeEventListener() {}, devicePixelRatio: 1,
    postMessage() {}, fetch, AbortController, TextDecoder, setTimeout: clock.setTimeout.bind(clock), clearTimeout: clock.clearTimeout.bind(clock)};
  window.parent = window;
  const document = {body: new Element('body'), documentElement: {classList: {toggle() {}}}, createElement: (t) => new Element(t),
    getElementById: (id) => (id === 'stage' ? stage : status), addEventListener() {}, querySelectorAll: (sel) => (sel === 'iframe' ? frames() : [])};
  window.document = document;
  const DateClass = class extends Date { static now() { return 1_000_000 + clock.now; } };
  const context = {console: {...console, log: (...a) => logs.push(a.join(' ')), warn: (...a) => warnings.push(a.join(' '))}, URL, URLSearchParams, Promise, Date: DateClass, Map, Set,
    JSON, Math, TextEncoder, TextDecoder, AbortController, Image: class extends Element { constructor() { super('img'); } }, window, document,
    location: {origin: 'http://s', href: 'http://127.0.0.1:8080/player/', search: ''}, queueMicrotask,
    setTimeout: clock.setTimeout.bind(clock), clearTimeout: clock.clearTimeout.bind(clock), requestAnimationFrame: (fn) => queueMicrotask(fn), fetch};
  vm.runInNewContext(readFileSync('www/player/video-engine.js', 'utf8'), context);
  vm.runInNewContext(readFileSync(PLAYER, 'utf8'), context);
  await clock.advance(50);
  return {clock, engine, nodes, warnings, logs, frames, window, advance: (ms) => clock.advance(ms),
    ledger: () => window.signageEngineLedger || [], current: () => status.textContent};
}

// promo-b is cut by its entry duration (a scheduled cut) after 4 s of its 20 s clip, leaving the engine
// holding a hidden, paused, unfinished clip while the menu page is on screen. The HEVC demo that
// follows must start on its own request: first frame within a second, never behind that remnant.
test('HEVC entry after a cut clip and an HTML page starts on its own request (no stale boundary queue)', async () => {
  const h = await harness([entry('promo-b', 'video', 4, {engine: {id: 'promo-b', frames: 1200, duration_ms: 20000}}),
    entry('main', 'html', 5),
    entry('demo', 'video', 0, {engine: {id: 'demo', frames: 600, duration_ms: 10000}}),
    entry('pip', 'html', 6, {builtin: true, url: '/content/menuboard/index.html?scene=C'})]);
  // t = 0..4 s promo-b, 4..9 s menu page, demo requested at ~9 s
  await h.advance(8900); 
  assert.equal(h.engine.presented.at(-1)?.asset, 'promo-b', 'only promo-b has been presented before the demo is requested');
  const before = h.engine.presented.length;
  await h.advance(1000);
  const shown = h.engine.presented.slice(before);
  assert.equal(shown[0]?.asset, 'demo', 'the first frame presented after the request is the demo\'s: ' + JSON.stringify(shown));
  assert.ok(shown[0].t - 9000 <= 1000, 'demo first frame within a second of its request, not after a 15 s watchdog');
  await h.advance(20000);
  assert.equal(h.warnings.filter((w) => /Skipping content/.test(w)).length, 0, 'no entry skipped: ' + h.warnings.join(' | '));
});

test('every request is accounted: serial, owner token, entry, disposition, activated, first PRESENTED', async function () {
  const h = await harness([entry('promo-b', 'video', 4, {engine: {id: 'promo-b', frames: 1200, duration_ms: 20000}}),
    entry('main', 'html', 5),
    entry('demo', 'video', 0, {engine: {id: 'demo', frames: 600, duration_ms: 10000}}),
    entry('pip', 'html', 6, {builtin: true, url: '/content/menuboard/index.html?scene=C'})]);
  await h.advance(60000);
  const ledger = h.ledger();
  assert.ok(ledger.length >= 4, 'ledger exists: ' + ledger.length);
  const serial = ledger.filter((r) => r.via === 'serial');
  const byEntry = (id) => serial.filter((r) => r.entry === id);
  for (const id of ['promo-b', 'demo']) {
    assert.ok(byEntry(id).length >= 1, id + ' was requested');
    for (const r of byEntry(id)) {
      assert.equal(r.outcome, 'started', id + ' request ' + r.serial + ' started: ' + JSON.stringify(r));
      assert.ok(r.owner && r.playId && r.disposition && r.started >= r.requested, 'complete record ' + JSON.stringify(r));
    }
  }
  assert.ok(ledger.some((r) => r.via === 'frame:play' && r.owner.startsWith('iframe-')), 'the menuboard iframe controller\'s play is accounted with its own owner token');
  assert.equal(new Set(ledger.map((r) => r.serial)).size, ledger.length, 'serials are unique');
});

test('a request replaced by another controller fails with the cause at once, not after 15 s', async () => {
  const h = await harness([entry('main', 'html', 2),
    entry('promo-b', 'video', 0, {engine: {id: 'promo-b', frames: 1200, duration_ms: 20000}})]);
  await h.advance(500);
  const real = h.engine.command.bind(h.engine);
  h.engine.command = (body) => {
    if (body.cmd !== 'play' || body.client.startsWith('iframe')) return real(body);
    // the engine accepts the player's request, then a late controller of a page being left replaces it
    setTimeout_(() => h.engine.emit({event: 'superseded', asset: 'x', play_id: body.play_id, client: body.client, by_play_id: 4242, by_asset: 'promo-a', by_client: 'intruder'}));
    return {event: 'ack', ok: true, cmd: 'play', client: body.client, play_id: body.play_id, message: 'queued'};
  };
  const setTimeout_ = (fn) => h.clock.setTimeout(fn, 5);
  await h.advance(25000);
  const lost = h.ledger().filter((r) => r.via === 'serial' && r.outcome === 'failed');
  assert.ok(lost.length >= 1, 'a superseded request is recorded as failed');
  assert.match(lost[0].detail, /superseded by promo-a \(intruder\)/);
  assert.ok(lost[0].closed - lost[0].requested < 1000, 'failed immediately with the cause');
  assert.ok(lost[0].superseded.by_client === 'intruder');
});

test('a request that cannot start leaves no queued remnant behind and names its stage', async () => {
  const h = await harness([entry('main', 'html', 2),
    entry('promo-b', 'video', 0, {engine: {id: 'promo-b', frames: 1200, duration_ms: 20000}})]);
  const real = h.engine.command.bind(h.engine);
  h.engine.command = (body) => (body.cmd === 'play' && !body.client.startsWith('iframe')
    ? {event: 'ack', ok: true, cmd: 'play', client: body.client, play_id: body.play_id, message: 'queued'} : real(body));
  await h.advance(20000);
  const failed = h.ledger().filter((r) => r.via === 'serial' && r.outcome === 'failed');
  assert.ok(failed.length >= 1, 'timed-out request recorded');
  assert.match(failed[0].detail, /did not start \(queued\)/);
  assert.ok(h.engine.commands.some((c) => c.cmd === 'cancel'), 'the timed-out request was cancelled in the engine');
});

test('while a serial request is being prepared no gapless request is queued behind a clip being left', async () => {
  const h = await harness([entry('promo-b', 'video', 4, {engine: {id: 'promo-b', frames: 1200, duration_ms: 20000}}),
    entry('demo', 'video', 0, {engine: {id: 'demo', frames: 600, duration_ms: 10000}})]);
  await h.advance(15000);
  const plays = h.engine.commands.filter((c) => c.cmd === 'play');
  const superseded = h.engine.log.filter((e) => e.event === 'superseded');
  assert.equal(superseded.filter((e) => e.by_client === e.client).length === superseded.length, true, 'only the player itself replaced requests: ' + JSON.stringify(superseded));
  assert.ok(plays.length >= 2);
});
