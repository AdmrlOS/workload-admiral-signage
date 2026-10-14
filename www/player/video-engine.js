/* Persistent video engine component for signage pages.
 *
 * One native engine owns the only hardware decoder for the whole presentation. Pages
 * place video with this component instead of <video>:
 *
 *   const video = SignageVideo.attach(element);           // element: a transparent box
 *   await video.ready;                                    // engine preflight done
 *   video.play('/media/promo-a.mp4', {loop: true});       // or {at:'boundary'|'gop'|'now'}
 *   video.addEventListener('started', e => ...);          // first frame actually presented
 *   video.pause(); video.resume(); video.seek(5000); video.hide(); video.show();
 *
 * The box must stay transparent (the video plane is below the page; alpha around it is
 * preserved). Its physical rectangle (CSS px × devicePixelRatio, plus iframe offsets) is
 * tracked and sent on change. Opt-in adapter: <video data-signage-engine src=... loop>
 * is replaced by an engine box with the same layout. A plain <video> while the engine
 * owns the decoder is refused by the native broker (media error), never a second decoder.
 *
 * Transport: top-level pages use HTTP (/api/video/command + streamed /api/video/events);
 * sandboxed iframes post to the parent player, which proxies and adds the frame offset.
 */
(function (global) {
  'use strict';
  const HEADERS = {'Content-Type': 'application/json', 'X-Signage-Request': 'player'};
  const randomId = () => 'p-' + Math.random().toString(36).slice(2) + Date.now().toString(36);

  class Emitter {
    constructor() { this._l = new Map(); }
    addEventListener(type, fn) { if (!this._l.has(type)) this._l.set(type, new Set()); this._l.get(type).add(fn); }
    removeEventListener(type, fn) { this._l.get(type)?.delete(fn); }
    emit(type, detail) { for (const fn of [...(this._l.get(type) || [])]) { try { fn({type, detail}); } catch (e) { console.error(e); } } }
  }

  /* HTTP transport: commands are POSTs; events are a streamed fetch body (EventSource cannot
   * send the same-origin request header). Closing the stream releases ownership. */
  class HttpTransport extends Emitter {
    constructor(env, client) { super(); this.env = env; this.client = client; this.abort = null; this.closed = false; }
    async open() {
      const {fetch, AbortController, TextDecoder} = this.env;
      this.abort = AbortController ? new AbortController() : null;
      const response = await fetch('/api/video/events?client=' + encodeURIComponent(this.client),
        {headers: HEADERS, signal: this.abort?.signal, cache: 'no-store'});
      if (!response.ok || !response.body) throw new Error('video engine unavailable (' + response.status + ')');
      const reader = response.body.getReader(), decoder = new TextDecoder();
      let buffer = '';
      (async () => {
        try {
          for (;;) {
            const {value, done} = await reader.read();
            if (done) break;
            buffer += decoder.decode(value, {stream: true});
            let cut;
            while ((cut = buffer.indexOf('\n\n')) >= 0) {
              const block = buffer.slice(0, cut); buffer = buffer.slice(cut + 2);
              for (const line of block.split('\n')) if (line.startsWith('data: ')) this.emit('event', JSON.parse(line.slice(6)));
            }
          }
        } catch (_) { /* aborted or dropped */ }
        if (!this.closed) this.emit('event', {event: 'engine-disconnected'});
      })();
    }
    async command(cmd, fields = {}) {
      const response = await this.env.fetch('/api/video/command',
        {method: 'POST', headers: HEADERS, body: JSON.stringify({...fields, cmd, client: this.client})});
      return response.json();
    }
    close() { this.closed = true; this.abort?.abort(); }
  }

  /* Sandboxed iframe transport: the parent player proxies to the engine. */
  class FrameTransport extends Emitter {
    constructor(env, client) {
      super(); this.env = env; this.client = client; this.pending = new Map(); this.seq = 0;
      this.onMessage = (event) => {
        const data = event.data;
        if (event.source !== env.parent || !data || data.type !== 'signage-video-host') return;
        if (data.reply != null && this.pending.has(data.reply)) { this.pending.get(data.reply)(data.ack); this.pending.delete(data.reply); }
        else if (data.event) this.emit('event', data.event);
      };
      env.window.addEventListener('message', this.onMessage);
    }
    async open() { this.env.parent.postMessage({type: 'signage-video', op: 'open', client: this.client}, '*'); }
    command(cmd, fields = {}) {
      const id = ++this.seq;
      return new Promise((resolve) => {
        this.pending.set(id, resolve);
        this.env.parent.postMessage({type: 'signage-video', op: 'command', id, client: this.client, cmd, fields}, '*');
      });
    }
    close() {
      this.env.window.removeEventListener('message', this.onMessage);
      this.env.parent.postMessage({type: 'signage-video', op: 'close', client: this.client}, '*');
    }
  }

  /* bridge.js fits fixed-canvas pages with CSS `zoom` on <body>. WPE's getBoundingClientRect
   * reports coordinates inside a zoomed body unscaled, so the rect must be multiplied by the zoom;
   * an engine that already reports zoomed coordinates is detected with a 100px probe. */
  function bodyZoom(element) {
    const doc = element.ownerDocument, win = doc?.defaultView;
    if (!doc?.body || !win?.getComputedStyle) return 1;
    const zoom = parseFloat(win.getComputedStyle(doc.body).zoom) || 1;
    if (Math.abs(zoom - 1) < 1e-3) return 1;
    if (doc.__signageZoom !== zoom) {
      const probe = doc.createElement('div');
      probe.style.cssText = 'position:absolute;left:0;top:0;width:100px;height:1px;visibility:hidden';
      doc.body.appendChild(probe);
      const width = probe.getBoundingClientRect().width;
      probe.remove();
      doc.__signageZoom = zoom;
      doc.__signageRectScale = Math.abs(width - 100) < Math.abs(width - 100 * zoom) ? zoom : 1;
    }
    return doc.__signageRectScale;
  }
  function physicalRect(element, env, offset = {x: 0, y: 0}) {
    const r = element.getBoundingClientRect(), dpr = env.window.devicePixelRatio || 1, s = bodyZoom(element);
    const x = Math.round((r.left * s + offset.x) * dpr), y = Math.round((r.top * s + offset.y) * dpr);
    return {x, y, w: Math.max(0, Math.round((r.right * s + offset.x) * dpr) - x), h: Math.max(0, Math.round((r.bottom * s + offset.y) * dpr) - y)};
  }

  /* One controller per page instance; only one independent video at a time. */
  class SignageVideo extends Emitter {
    constructor(element, env, options = {}) {
      super();
      this.element = element; this.env = env; this.client = options.client || randomId();
      this.transport = options.transport || (env.parent && env.parent !== env.window
        ? new FrameTransport(env, this.client) : new HttpTransport(env, this.client));
      this.lastRect = ''; this.playId = 0; this.current = null; this.closed = false;
      if (element && element.style) element.style.background = 'transparent';
      this.transport.addEventListener('event', ({detail}) => this._event(detail));
      this.ready = new Promise((resolve, reject) => {
        this._resolveReady = resolve; this._rejectReady = reject;
      });
      this.acquired = false; this.rectBusy = false;
      this.transport.open().then(() => this._acquire(options.acquireWaitMs ?? 90000))
        .then((ack) => {
          if (!ack?.ok) { this._rejectReady(new Error(ack?.code || 'acquire failed')); return; }
          this.acquired = true; this.lastRect = ''; this.trackNow();
        })
        .catch((error) => this._rejectReady(error));
      this._track();
    }
    /* The engine process (GStreamer init + decoder preflight) takes seconds to tens of
     * seconds after a cold start, and the page can load first: wait for it instead of
     * permanently falling back to a second (legacy) decoder. Only "not up yet" codes retry. */
    async _acquire(waitMs) {
      const retry = new Set(['engine-unavailable', 'engine-disconnected', 'timeout', 'not-ready']);
      const deadline = Date.now() + waitMs;
      for (;;) {
        const ack = await this.transport.command('acquire');
        if (ack?.ok || !retry.has(ack?.code) || this.closed || Date.now() >= deadline) return ack;
        await new Promise((resolve) => this.env.setTimeout(resolve, 500));
      }
    }
    _event(event) {
      if (!event || !event.event) return;
      if (event.event === 'engine-ready') { this._resolveReady(event); this.lastRect = ''; this.trackNow(); }
      if (event.event === 'engine-disconnected') this.lastRect = '';
      if (event.event === 'preempted' && event.client === this.client) this.emit('preempted', event);
      else if (event.event !== 'preempted') this.emit(event.event, event);
    }
    _track() {
      const tick = () => { if (this.closed) return; this.trackNow(); this._timer = this.env.setTimeout(tick, 200); };
      this._timer = this.env.setTimeout(tick, 0);
      if (this.env.ResizeObserver && this.element) {
        this._ro = new this.env.ResizeObserver(() => this.trackNow());
        this._ro.observe(this.element);
      }
    }
    trackNow() {
      if (!this.element || this.closed || !this.acquired) return;
      const rect = physicalRect(this.element, this.env);
      if (rect.w <= 0 || rect.h <= 0) return;
      const key = JSON.stringify(rect);
      if (key === this.lastRect || this.rectBusy) return;
      /* Remember a rectangle only once the engine acknowledged it: a rect sent while the engine
       * was still starting (or after it restarted) must be re-sent, never silently dropped. */
      this.rectBusy = true;
      this.rectPromise = Promise.resolve(this.transport.command('rect', rect)).then((ack) => {
        this.rectBusy = false;
        if (ack?.ok) { this.lastRect = key; this.trackNow(); } /* re-checks a size change meanwhile */
      }, () => { this.rectBusy = false; });
    }
    /* (Re)take ownership of the engine. The engine keeps ONE rectangle shared by whoever owns
     * it, so after another page/iframe held it (e.g. a picture-in-picture menu) the geometry it
     * left behind is not ours: forget what we think was sent and send it again before any play. */
    acquire() {
      return Promise.resolve(this.rectPromise).catch(() => {})
        .then(() => this.transport.command('acquire'))
        .then((ack) => { if (ack?.ok) { this.lastRect = ''; this.trackNow(); } return ack; });
    }
    play(url, options = {}) {
      const playId = (this.playId = this.playId + 1) + Math.floor(Math.random() * 1e6) * 1000;
      this.trackNow();
      /* The rectangle must be in place before the first frame can be shown. */
      return (this.rectPromise || Promise.resolve())
        .then(() => this.transport.command('play', {asset: {url}, at: options.at || 'boundary', loop: !!options.loop,
                                                    show: options.show !== false, play_id: playId}))
        .then((ack) => ({...ack, play_id: ack?.play_id ?? playId}));
    }
    prepare(url) { return this.transport.command('prepare', {asset: {url}}); }
    cancel(url) { return this.transport.command('cancel', {asset: {url}}); }
    pause() { return this.transport.command('pause'); }
    resume() { return this.transport.command('resume'); }
    seek(ms) { return this.transport.command('seek', {position_ms: Math.max(0, Math.round(ms))}); }
    show() { return this.transport.command('show'); }
    hide() { return this.transport.command('hide'); }
    stop() { return this.transport.command('stop'); }
    release() { return this.transport.command('release'); }
    destroy() {
      if (this.closed) return;
      this.closed = true; this.env.clearTimeout(this._timer); this._ro?.disconnect();
      this.transport.command('hide').catch?.(() => {});
      this.transport.close();
    }
  }

  /* Parent-side proxy for iframe components (used by the player). */
  class FrameHost {
    constructor(env, makeTransport) {
      this.env = env; this.makeTransport = makeTransport || ((client) => new HttpTransport(env, client));
      this.frames = new Map();
      this.onMessage = (event) => this._message(event);
      env.window.addEventListener('message', this.onMessage);
    }
    _frameFor(source) {
      for (const frame of this.env.frames()) if (frame.contentWindow === source) return frame;
      return null;
    }
    async _message(event) {
      const data = event.data;
      if (!data || data.type !== 'signage-video') return;
      const frame = this._frameFor(event.source);
      if (!frame || typeof data.client !== 'string') return;
      const key = data.client;
      if (data.op === 'open' && !this.frames.has(key)) {
        const transport = this.makeTransport(key);
        transport.addEventListener('event', ({detail}) => event.source.postMessage({type: 'signage-video-host', event: detail}, '*'));
        this.frames.set(key, {transport, frame, source: event.source});
        this.hooks?.open?.(frame);
        try { await transport.open(); } catch (_) { event.source.postMessage({type: 'signage-video-host', event: {event: 'engine-disconnected'}}, '*'); }
      } else if (data.op === 'command' && this.frames.has(key)) {
        const entry = this.frames.get(key);
        let fields = data.fields || {};
        if (data.cmd === 'rect') { // frame-relative CSS*DPR -> page physical pixels
          const r = entry.frame.getBoundingClientRect(), dpr = this.env.window.devicePixelRatio || 1;
          fields = {...fields, x: fields.x + Math.round(r.left * dpr), y: fields.y + Math.round(r.top * dpr)};
        }
        const ack = await entry.transport.command(data.cmd, fields).catch(() => ({ok: false, code: 'proxy'}));
        try { this.hooks?.command?.({client: key, cmd: data.cmd, fields, ack, acked: Date.now(), frameSrc: entry.frame.getAttribute?.('src') || null}); } catch (_) { /* accounting only */ }
        event.source.postMessage({type: 'signage-video-host', reply: data.id, ack}, '*');
      } else if (data.op === 'close') this.drop(key);
    }
    drop(key) { const entry = this.frames.get(key); if (entry) { entry.transport.close(); this.frames.delete(key); this.hooks?.close?.(entry.frame); } }
    /* Called when the player removes an iframe: its stream closes -> engine hides+pauses. */
    dropFrame(frame) { for (const [key, entry] of this.frames) if (entry.frame === frame) this.drop(key); }
  }

  /* Opt-in adapter: <video data-signage-engine src loop> -> engine box. */
  function upgradeVideos(doc, env) {
    const videos = [...doc.querySelectorAll('video[data-signage-engine]')];
    if (videos.length > 1) console.warn('Only one engine video is supported at a time; using the first');
    const v = videos[0];
    if (!v) return null;
    const box = doc.createElement('div');
    box.className = v.className; box.style.cssText = v.style.cssText; box.style.background = 'transparent';
    box.dataset.signageEngine = '';
    const src = v.getAttribute('src'), loop = v.hasAttribute('loop');
    v.removeAttribute('src'); v.replaceWith(box);
    const video = new SignageVideo(box, env);
    video.ready.then(() => video.play(src, {loop, at: 'now'})).catch((e) => console.warn('engine video unavailable', e));
    return video;
  }


  /* Explicit intercept adapter for existing page code: a transparent element with the
   * subset of the HTMLVideoElement API that the bundled pages use (src/load/play/pause/
   * currentTime/duration/ended + playing/canplay/loadedmetadata/seeked/ended/error events),
   * backed by the one engine. It never creates a WebKit media pipeline. */
  function createEngineVideo(doc, env) {
    const el = doc.createElement('div');
    el.dataset.signageEngine = '';
    el.className = 'engine-video';
    const controller = new SignageVideo(el, env);
    const handlers = new Map();
    let src = '', playId = null, startedAt = 0, offset = 0, playing = false, ended = false, duration = NaN, pendingSeek = 0;
    const dispatch = (type) => {
      for (const fn of [...(handlers.get(type) || [])]) { try { fn.call(el, {type, target: el}); } catch (e) { console.error(e); } }
      const prop = el['on' + type]; if (typeof prop === 'function') prop.call(el, {type, target: el});
    };
    const now = () => (env.now ? env.now() : Date.now()) / 1000;
    el.addEventListener = (type, fn, options) => {
      if (!handlers.has(type)) handlers.set(type, new Set());
      const once = options && options.once;
      const wrapped = once ? function (event) { handlers.get(type).delete(wrapped); fn.call(this, event); } : fn;
      handlers.get(type).add(wrapped);
    };
    el.removeEventListener = (type, fn) => handlers.get(type)?.delete(fn);
    controller.addEventListener('ready', ({detail}) => {
      if (detail.duration_ms) duration = detail.duration_ms / 1000;
      dispatch('loadedmetadata'); dispatch('canplay');
    });
    const recent = [];
    const onStarted = () => {
      startedAt = now(); playing = true; ended = false;
      if (pendingSeek) { offset = pendingSeek; controller.seek(pendingSeek * 1000); pendingSeek = 0; }
      dispatch('playing');
    };
    controller.addEventListener('started', ({detail}) => {
      recent.push(detail.play_id); if (recent.length > 8) recent.shift();
      if (playId && detail.play_id === playId) onStarted();
    });
    controller.addEventListener('looped', ({detail}) => { if (detail.play_id === playId) { startedAt = now(); offset = 0; } });
    controller.addEventListener('ended', ({detail}) => {
      if (detail.play_id !== playId || detail.reason !== 'complete') return;
      playing = false; ended = true; offset = duration || 0; dispatch('ended');
    });
    const fail = () => dispatch('error');
    controller.ready.catch(fail);
    Object.defineProperties(el, {
      src: {get: () => src, set: (value) => { src = value ? String(value) : ''; }},
      duration: {get: () => duration},
      ended: {get: () => ended},
      paused: {get: () => !playing},
      readyState: {get: () => (Number.isFinite(duration) ? 4 : 0)},
      currentTime: {
        get: () => (playing ? offset + now() - startedAt : offset),
        set: (t) => {
          offset = Math.max(0, Number(t) || 0); startedAt = now();
          if (!playId) { pendingSeek = offset; queueMicrotask(() => dispatch('seeked')); return; } // applied after start
          controller.ready.then(() => controller.seek(offset * 1000)).then(() => dispatch('seeked'), fail);
        },
      },
    });
    el.load = () => { if (src) controller.ready.then(() => controller.prepare(src)).then((ack) => { if (!ack.ok) fail(); }, fail); };
    el.play = () => controller.ready.then(() => {
      if (playId && !ended) {
        return controller.resume().then((ack) => { if (ack.ok && !playing) { playing = true; startedAt = now(); dispatch('playing'); } });
      }
      return controller.play(src, {at: 'now', loop: !!el.loop}).then((ack) => {
        if (!ack.ok) { fail(); throw new Error('engine refused video: ' + ack.code); }
        playId = ack.play_id;
        if (recent.includes(playId)) onStarted(); // PRESENTED raced the HTTP acknowledgement
      });
    });
    el.pause = () => { if (playing) { offset = el.currentTime; playing = false; } controller.ready.then(() => controller.pause()).catch(() => {}); };
    const removeAttribute = el.removeAttribute ? el.removeAttribute.bind(el) : () => {};
    el.removeAttribute = (name) => { if (name === 'src') src = ''; else removeAttribute(name); };
    const remove = el.remove ? el.remove.bind(el) : () => {};
    el.remove = () => { controller.destroy(); remove(); };
    el.engine = controller;
    return el;
  }

  function browserEnv() {
    return {window: global, parent: global.parent, fetch: global.fetch?.bind(global), AbortController: global.AbortController,
            TextDecoder: global.TextDecoder, ResizeObserver: global.ResizeObserver,
            setTimeout: global.setTimeout.bind(global), clearTimeout: global.clearTimeout.bind(global),
            frames: () => [...(global.document?.querySelectorAll('iframe') || [])]};
  }

  const api = {
    attach: (element, options) => new SignageVideo(element, options?.env || browserEnv(), options),
    upgrade: (doc, env) => upgradeVideos(doc || global.document, env || browserEnv()),
    createVideo: (doc, env) => createEngineVideo(doc || global.document, env || browserEnv()),
    host: (env, makeTransport) => new FrameHost(env || browserEnv(), makeTransport),
    SignageVideo, HttpTransport, FrameTransport, FrameHost, physicalRect,
  };
  global.SignageVideo = api;
  if (typeof module !== 'undefined') module.exports = api;
})(typeof window !== 'undefined' ? window : globalThis);
