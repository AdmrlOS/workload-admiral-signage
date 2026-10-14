// Engine component: transports, rect tracking, iframe proxy and <video> adapter (no GPU).
const assert = require('node:assert/strict');
const test = require('node:test');
const api = require('../www/player/video-engine.js');

function fakeEnv({dpr = 2, events = []} = {}) {
  const posted = [], timers = [];
  let push;
  const stream = new ReadableStream({start(c) { push = c; }});
  const env = {
    window: {devicePixelRatio: dpr, addEventListener() {}, removeEventListener() {}},
    parent: null, posted, timers,
    AbortController, TextDecoder,
    setTimeout: (fn) => { timers.push(fn); return timers.length; }, clearTimeout() {},
    fetch: async (url, init = {}) => {
      if (url.startsWith('/api/video/events')) {
        posted.push({url, headers: init.headers});
        return {ok: true, body: stream};
      }
      const body = JSON.parse(init.body);
      posted.push({url, body, headers: init.headers});
      return {json: async () => ({ok: true, seq: posted.length, play_id: body.play_id})};
    },
    send: (event) => push.enqueue(new TextEncoder().encode('data: ' + JSON.stringify(event) + '\n\n')),
    close: () => push.close(),
  };
  env.window.parent = env.window; env.parent = env.window;
  return env;
}
const box = (left, top, width, height) => ({style: {background: 'red'}, getBoundingClientRect: () => ({left, top, right: left + width, bottom: top + height})});
const settle = () => new Promise((r) => setTimeout(r, 10));

test('top-level component acquires, waits for preflight and sends physical rect', async () => {
  const env = fakeEnv({dpr: 2});
  const video = api.SignageVideo && new api.SignageVideo(box(10.25, 20, 960, 540), env, {client: 'c1'});
  await settle();
  assert.equal(env.posted[0].url, '/api/video/events?client=c1');
  assert.equal(env.posted[0].headers['X-Signage-Request'], 'player');
  assert.equal(env.posted[1].body.cmd, 'acquire');
  env.send({event: 'engine-ready', warm: true});
  const ready = await video.ready;
  assert.equal(ready.warm, true);
  const rect = env.posted.find((p) => p.body?.cmd === 'rect').body;
  assert.deepEqual({x: rect.x, y: rect.y, w: rect.w, h: rect.h}, {x: 21, y: 40, w: 1920, h: 1080});
  assert.equal(video.element.style.background, 'transparent');
  const sent = env.posted.length;
  video.trackNow();
  assert.equal(env.posted.length, sent, 'unchanged rect is not resent');
  video.element.getBoundingClientRect = () => ({left: 0, top: 0, right: 1920, bottom: 1080});
  video.trackNow();
  assert.deepEqual(env.posted.at(-1).body, {x: 0, y: 0, w: 3840, h: 2160, cmd: 'rect', client: 'c1'});
  video.destroy();
});

test('play/pause/seek map to engine commands; events and own-client preemption are surfaced', async () => {
  const env = fakeEnv();
  const video = new api.SignageVideo(box(0, 0, 100, 100), env, {client: 'me'});
  await settle();
  const seen = [];
  for (const type of ['started', 'ended', 'waiting', 'preempted']) video.addEventListener(type, (e) => seen.push([type, e.detail.asset || e.detail.client]));
  const ack = await video.play('/media/promo-b.mp4', {at: 'gop', loop: true});
  const play = env.posted.find((p) => p.body?.cmd === 'play').body;
  assert.deepEqual([play.asset.url, play.at, play.loop, play.show], ['/media/promo-b.mp4', 'gop', true, true]);
  assert.equal(ack.play_id, play.play_id);
  await video.seek(1500.4); await video.pause(); await video.hide();
  assert.deepEqual(env.posted.slice(-3).map((p) => [p.body.cmd, p.body.position_ms]), [['seek', 1500], ['pause', undefined], ['hide', undefined]]);
  env.send({event: 'started', asset: 'promo-b'});
  env.send({event: 'preempted', client: 'someone-else'});
  env.send({event: 'preempted', client: 'me'});
  env.send({event: 'waiting', asset: 'x'});
  await settle();
  assert.deepEqual(seen, [['started', 'promo-b'], ['preempted', 'me'], ['waiting', 'x']]);
  video.destroy();
  assert.equal(env.posted.at(-1).body.cmd, 'hide', 'destroy hides the video');
});

test('iframe component is proxied by the parent with the frame offset; removing the frame closes its stream', async () => {
  const listeners = [];
  const parentWindow = {devicePixelRatio: 2, addEventListener: (t, fn) => listeners.push(fn), removeEventListener() {}};
  const frameMessages = [];
  const frameWindow = {postMessage: (m) => frameMessages.push(m)};
  const frame = {contentWindow: frameWindow, getBoundingClientRect: () => ({left: 100, top: 50, right: 1060, bottom: 590})};
  const opened = [], commands = [], closed = [];
  const host = new api.FrameHost({window: parentWindow, frames: () => [frame]}, (client) => ({
    addEventListener() {}, open: async () => opened.push(client),
    command: async (cmd, fields) => { commands.push([cmd, fields]); return {ok: true}; }, close: () => closed.push(client)}));
  const deliver = (data, source = frameWindow) => Promise.all(listeners.map((fn) => fn({data, source})));
  await deliver({type: 'signage-video', op: 'open', client: 'f1'});
  await deliver({type: 'signage-video', op: 'command', id: 1, client: 'f1', cmd: 'rect', fields: {x: 10, y: 20, w: 400, h: 300}});
  await deliver({type: 'signage-video', op: 'command', id: 2, client: 'f1', cmd: 'play', fields: {asset: {url: '/media/a.mp4'}}}, {});
  assert.deepEqual(opened, ['f1']);
  assert.deepEqual(commands, [['rect', {x: 210, y: 120, w: 400, h: 300}]], 'unknown sources are ignored');
  assert.deepEqual(frameMessages.at(-1), {type: 'signage-video-host', reply: 1, ack: {ok: true}});
  host.dropFrame(frame);
  assert.deepEqual(closed, ['f1']);
});

test('opt-in <video data-signage-engine> adapter replaces the element without creating a media sink', async () => {
  const created = [];
  const video = {className: 'pip', style: {cssText: 'left:10px'}, attrs: {src: '/media/promo-a.mp4', loop: ''},
    getAttribute(k) { return this.attrs[k]; }, hasAttribute(k) { return k in this.attrs; },
    removeAttribute(k) { delete this.attrs[k]; }, replaceWith(n) { this.replaced = n; }};
  const doc = {querySelectorAll: () => [video], createElement: (tag) => {
    const el = {tag, style: {}, dataset: {}, getBoundingClientRect: () => ({left: 0, top: 0, right: 10, bottom: 10})};
    created.push(el); return el;
  }};
  const env = fakeEnv();
  const engineVideo = api.upgrade(doc, env);
  assert.equal(video.replaced, created[0]);
  assert.equal(video.attrs.src, undefined, 'the <video> never receives a source (no WebKit pipeline/decoder)');
  assert.deepEqual([created[0].className, created[0].style.cssText, created[0].style.background], ['pip', 'left:10px', 'transparent']);
  await settle();
  env.send({event: 'engine-ready'});
  await settle(); await settle();
  const play = env.posted.find((p) => p.body?.cmd === 'play').body;
  assert.deepEqual([play.asset.url, play.loop, play.at], ['/media/promo-a.mp4', true, 'now']);
  engineVideo.destroy();
  env.close();
});

test('engine video adapter gives existing page code video semantics without a media element', async () => {
  const env = fakeEnv();
  let clock = 1000; env.now = () => clock;
  const doc = {createElement: (tag) => ({tag, style: {}, dataset: {}, removeAttribute() {}, remove() { this.removed = true; },
    getBoundingClientRect: () => ({left: 0, top: 0, right: 640, bottom: 360})})};
  const v = api.createVideo(doc, env);
  assert.equal(v.tag, 'div', 'no <video> element');
  const seen = [];
  v.addEventListener('playing', () => seen.push('playing'), {once: true});
  v.onended = () => seen.push('ended');
  v.src = '/media/promo-b.mp4';
  v.currentTime = 2; // before play: applied after the first presented frame
  await settle(); env.send({event: 'engine-ready'}); await settle();
  const played = v.play();
  env.send({event: 'started', play_id: 999}); // someone else's instance is ignored
  await played; await settle();
  const play = env.posted.find((p) => p.body?.cmd === 'play').body;
  assert.deepEqual([play.asset.url, play.at], ['/media/promo-b.mp4', 'now']);
  env.send({event: 'started', play_id: play.play_id}); await settle();
  assert.deepEqual(seen, ['playing']);
  assert.equal(env.posted.at(-1).body.cmd, 'seek'); assert.equal(env.posted.at(-1).body.position_ms, 2000);
  clock += 1500; assert.ok(Math.abs(v.currentTime - 3.5) < 1e-6);
  v.pause(); await settle();
  assert.equal(env.posted.at(-1).body.cmd, 'pause');
  await v.play(); await settle();
  assert.equal(env.posted.at(-1).body.cmd, 'resume', 'play after pause resumes, it does not restart the clip');
  env.send({event: 'ended', play_id: play.play_id, reason: 'handoff'});
  env.send({event: 'ended', play_id: play.play_id, reason: 'complete'}); await settle();
  assert.deepEqual(seen, ['playing', 'ended']);
  assert.equal(v.ended, true);
  v.remove();
  assert.equal(env.posted.at(-1).body.cmd, 'hide');
  env.close();
});
