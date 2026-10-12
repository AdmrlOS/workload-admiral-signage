/* Admiral Display Lab: browser observations are separate from native proof. */
"use strict";

(function () {
  const byId = (id) => document.getElementById(id);
  const video = byId("video");
  const stage = byId("video-stage");
  const config = { video_url: "/media/test-h264.mp4", video_mode: "holepunch" };
  const observations = { webgl: {}, video: {}, mediaCapabilities: null };
  let nativeStatus = null;
  let backendError = null;
  let mediaCapabilityGeneration = 0;
  let videoSize = [480, 270];

  function setText(id, text) { const node = byId(id); if (node.textContent !== String(text)) node.textContent = text; }
  function summary(kind, label, detail, state) {
    setText(kind + "-state", label);
    setText(kind + "-detail", detail);
    byId(kind + "-dot").className = "status-dot " + state;
  }
  function printable(value) {
    if (value == null) return "—";
    if (typeof value === "string") return value;
    if (Array.isArray(value)) return value.length ? value.map(printable).join(" · ") : "None found";
    if (typeof value === "object") {
      const name = value.path || value.node || value.name;
      return name ? String(name) + (value.driver ? " (" + value.driver + ")" : "") : JSON.stringify(value);
    }
    return String(value);
  }
  function evidenceText(value) {
    return printable(value == null || (Array.isArray(value) && !value.length) ? "No verified native evidence yet" : value);
  }
  function hasEvidence(value) {
    return typeof value === "string" ? value.trim().length > 0 : Array.isArray(value) ? value.some(hasEvidence) : value != null && typeof value === "object" && Object.keys(value).length > 0;
  }
  function nativeProof(section, fallbackVerified, fallbackEvidence) {
    const candidate = section && typeof section === "object" ? section : {};
    const evidence = candidate.evidence || fallbackEvidence;
    return { verified: (candidate.verified === true || fallbackVerified === true) && hasEvidence(evidence), evidence: evidence };
  }

  function renderNative() {
    const status = nativeStatus || {};
    const gpu = status.gpu || {};
    const media = status.video || {};
    const holepunch = status.holepunch || {};
    const decode = nativeProof(media.hardware_decode || media.decode, media.hardware_decode_verified, media.decode_evidence);
    const plane = nativeProof(holepunch, holepunch.active_verified, holepunch.plane_evidence);
    const nativeError = media.native && media.native.error;
    const planeError = status.drm && status.drm.error;
    summary("decode", nativeError ? "Playback error" : decode.verified ? "Hardware verified" : "Unverified", nativeError || (decode.verified ? evidenceText(decode.evidence) : "Playback alone does not prove hardware decode"), nativeError ? "bad" : decode.verified ? "good" : "pending");
    summary("plane", planeError ? "Plane unavailable" : plane.verified ? "Holepunch verified" : holepunch.configured ? "Configured · unverified" : "Unverified", planeError || (plane.verified ? evidenceText(plane.evidence) : "Native logs / KMS plane state must confirm the path"), planeError ? "bad" : plane.verified ? "good" : "pending");
    setText("gpu-nodes", printable(gpu.nodes || status.gpu_nodes || "No device report"));
    setText("video-nodes", printable(media.nodes || status.video_nodes || "No device report"));
    const runtime = status.runtime || {};
    const versions = status.versions || {};
    const wpeVersion = versions.output ? versions.output.split("\n").find(function (line) { return line.indexOf("libwpewebkit") === 0; }) : null;
    setText("runtime-detail", [status.kernel, status.architecture, wpeVersion, runtime.SIGNAGE_VIDEO_MODE].filter(Boolean).map(printable).join(" · ") || "Awaiting runtime report");
    setText("decode-evidence", evidenceText(decode.evidence));
    updateJSON();
  }
  function updateJSON() {
    setText("diagnostic-json", JSON.stringify({ config: config, browser: { userAgent: navigator.userAgent, secureContext: window.isSecureContext, viewport: { width: innerWidth, height: innerHeight, devicePixelRatio: devicePixelRatio }, observations: observations }, workload: nativeStatus, backendError: backendError }, null, 2));
  }
  async function refreshStatus() {
    try {
      const response = await fetch("/api/status", { cache: "no-store" });
      if (!response.ok) throw new Error("HTTP " + response.status);
      nativeStatus = await response.json();
      backendError = null;
      setText("backend-state", "Live · updates every 3s");
      setText("status-updated", "Updated " + new Date().toLocaleTimeString());
    } catch (error) {
      backendError = String(error.message || error);
      setText("backend-state", "Backend unavailable");
      setText("status-updated", "Native diagnostics unavailable");
    }
    renderNative();
  }

  function layoutVideo() {
    const selected = byId("video-size").value;
    const availableWidth = Math.floor(stage.clientWidth);
    const availableHeight = Math.floor(stage.clientHeight);
    const desired = selected === "stage" ? [availableWidth, Math.floor(availableWidth * 9 / 16)] : selected.split("x").map(Number);
    const scale = Math.min(1, availableWidth / desired[0], availableHeight / desired[1]);
    videoSize = [Math.floor(desired[0] * scale), Math.floor(desired[1] * scale)];
    const xInput = byId("video-x");
    const yInput = byId("video-y");
    xInput.max = String(Math.max(0, availableWidth - videoSize[0]));
    yInput.max = String(Math.max(0, availableHeight - videoSize[1]));
    const x = Math.min(Number(xInput.value), Number(xInput.max));
    const y = Math.min(Number(yInput.value), Number(yInput.max));
    xInput.value = String(x);
    yInput.value = String(y);
    video.style.width = videoSize[0] + "px";
    video.style.height = videoSize[1] + "px";
    video.style.left = x + "px";
    video.style.top = y + "px";
    setText("video-x-value", String(x));
    setText("video-y-value", String(y));
    byId("video-message").style.left = x + "px";
    byId("video-message").style.top = y + "px";
    updateVideoRectangle();
  }
  function updateVideoRectangle() {
    const rect = video.getBoundingClientRect();
    const rectangle = { x: Math.round(rect.left), y: Math.round(rect.top), width: Math.round(rect.width), height: Math.round(rect.height), coordinate_space: "CSS pixels" };
    observations.video.rectangle = rectangle;
    setText("video-rectangle", rectangle.width + " × " + rectangle.height + " @ " + rectangle.x + ", " + rectangle.y);
  }
  function videoMessage(message) {
    byId("video-message").hidden = !message;
    setText("video-message", message || "");
  }
  function formatTime(seconds) {
    if (!Number.isFinite(seconds)) return "—";
    return Math.floor(seconds / 60) + ":" + String(Math.floor(seconds % 60)).padStart(2, "0");
  }
  function updatePlayback() {
    let quality = null;
    if (typeof video.getVideoPlaybackQuality === "function") quality = video.getVideoPlaybackQuality();
    const total = quality ? quality.totalVideoFrames : video.webkitDecodedFrameCount;
    const dropped = quality ? quality.droppedVideoFrames : video.webkitDroppedFrameCount;
    observations.video = Object.assign(observations.video, { currentTime: video.currentTime, duration: Number.isFinite(video.duration) ? video.duration : null, paused: video.paused, readyState: video.readyState, width: video.videoWidth, height: video.videoHeight, totalFrames: total == null ? null : total, droppedFrames: dropped == null ? null : dropped, error: video.error ? { code: video.error.code, message: video.error.message } : null });
    const state = video.error ? "Error" : video.readyState < 2 ? "Loading" : video.paused ? "Paused" : "Playing";
    setText("video-playback", state + " · " + formatTime(video.currentTime));
    setText("video-resolution", video.videoWidth ? video.videoWidth + " × " + video.videoHeight : "Awaiting metadata");
    setText("video-drops", dropped == null ? "API unavailable" : dropped + " / " + (total || 0));
    setText("play-toggle", video.paused ? "Play" : "Pause");
    updateVideoRectangle();
  }
  async function checkMediaCapabilities() {
    const generation = ++mediaCapabilityGeneration;
    const codec = config.video_content_type || 'video/mp4; codecs="avc1.42E01E"';
    const canPlayType = video.canPlayType(codec);
    observations.video.canPlayType = { contentType: codec, result: canPlayType || "unsupported" };
    if (!navigator.mediaCapabilities || !navigator.mediaCapabilities.decodingInfo) {
      observations.mediaCapabilities = { available: false, canPlayType: canPlayType || "unsupported" };
      setText("media-capabilities", "Codec hint: " + (canPlayType || "unsupported") + ". MediaCapabilities unavailable; hardware decode is unverified.");
      return;
    }
    const query = { type: "file", video: { contentType: codec, width: video.videoWidth || 1280, height: video.videoHeight || 720, bitrate: Number(config.video_bitrate) || 2000000, framerate: Number(config.video_framerate) || 30 } };
    try {
      const result = await navigator.mediaCapabilities.decodingInfo(query);
      if (generation !== mediaCapabilityGeneration) return;
      observations.mediaCapabilities = { available: true, query: query, supported: result.supported, smooth: result.smooth, powerEfficient: result.powerEfficient, note: "Browser hint only; powerEfficient is not proof of hardware decode." };
      setText("media-capabilities", "Browser hints: supported " + result.supported + " · smooth " + result.smooth + " · power efficient " + result.powerEfficient + ". Hints do not prove hardware decode.");
    } catch (error) {
      if (generation !== mediaCapabilityGeneration) return;
      observations.mediaCapabilities = { available: true, error: String(error.message || error), query: query };
      setText("media-capabilities", "Codec hint: " + (canPlayType || "unsupported") + ". MediaCapabilities query failed; native decoder evidence is required.");
    }
  }
  function loadMedia(url) {
    const trimmed = url.trim();
    if (!trimmed) return;
    config.video_url = trimmed;
    byId("media-url").value = trimmed;
    videoMessage("Loading test video…");
    video.src = trimmed;
    video.load();
    const attempt = video.play();
    if (attempt && attempt.catch) attempt.catch(function (error) { videoMessage("Playback requires Play: " + error.message); });
    checkMediaCapabilities();
  }
  video.addEventListener("playing", function () { videoMessage(""); });
  video.addEventListener("waiting", function () { videoMessage("Buffering video…"); });
  video.addEventListener("loadedmetadata", checkMediaCapabilities);
  video.addEventListener("error", function () { videoMessage("Video error " + (video.error ? video.error.code : "") + ". Check the media URL and native browser logs."); });
  byId("load-media").addEventListener("click", function () { loadMedia(byId("media-url").value); });
  byId("media-url").addEventListener("keydown", function (event) { if (event.key === "Enter") loadMedia(event.target.value); });
  byId("video-size").addEventListener("change", layoutVideo);
  byId("video-x").addEventListener("input", layoutVideo);
  byId("video-y").addEventListener("input", layoutVideo);
  byId("play-toggle").addEventListener("click", function () {
    if (video.paused) {
      const attempt = video.play();
      if (attempt && attempt.catch) attempt.catch(function (error) { videoMessage("Playback failed: " + error.message); });
    } else video.pause();
  });
  byId("video-fullscreen").addEventListener("click", function () {
    if (document.fullscreenElement && document.exitFullscreen) document.exitFullscreen();
    else if (video.requestFullscreen) {
      const attempt = video.requestFullscreen();
      if (attempt && attempt.catch) attempt.catch(function (error) { videoMessage("Fullscreen unavailable: " + error.message); });
    } else if (video.webkitEnterFullscreen) video.webkitEnterFullscreen();
    else videoMessage("Fullscreen API unavailable in this browser.");
  });
  document.addEventListener("fullscreenchange", function () { setText("video-fullscreen", document.fullscreenElement ? "Exit fullscreen" : "Fullscreen"); updateVideoRectangle(); });
  byId("refresh-status").addEventListener("click", refreshStatus);
  window.addEventListener("resize", layoutVideo);
  window.addEventListener("scroll", updateVideoRectangle, { passive: true });

  function createScene() {
    const canvas = byId("gl-canvas");
    const gl = canvas.getContext("webgl", { alpha: false, antialias: true }) || canvas.getContext("experimental-webgl", { alpha: false, antialias: true });
    function fail(message) {
      summary("gpu", "WebGL unavailable", message, "bad");
      byId("webgl-error").hidden = false;
      setText("webgl-error", message);
      observations.webgl.error = message;
    }
    if (!gl) { fail("This browser could not create a WebGL context. Inspect the native EGL / DRM logs."); return; }
    const debug = gl.getExtension("WEBGL_debug_renderer_info");
    const renderer = gl.getParameter(debug ? debug.UNMASKED_RENDERER_WEBGL : gl.RENDERER);
    const vendor = gl.getParameter(debug ? debug.UNMASKED_VENDOR_WEBGL : gl.VENDOR);
    const software = /llvmpipe|softpipe|swiftshader|lavapipe|software rasterizer|swrast/i.test(renderer + " " + vendor);
    const extensions = gl.getSupportedExtensions() || [];
    observations.webgl = { renderer: renderer, vendor: vendor, version: gl.getParameter(gl.VERSION), shadingLanguage: gl.getParameter(gl.SHADING_LANGUAGE_VERSION), extensions: extensions, unmaskedRendererAvailable: !!debug, softwareRendererDetected: software, contextLost: false };
    setText("gl-renderer", renderer);
    setText("gl-vendor", vendor);
    setText("gl-version", observations.webgl.version + " · " + observations.webgl.shadingLanguage);
    setText("gl-acceleration", software ? "Software renderer detected" : debug ? "Renderer reported; verify against native EGL / DRM logs" : "Renderer masked; acceleration unverified");
    setText("extension-count", String(extensions.length));
    const list = byId("gl-extensions");
    extensions.forEach(function (extension) { const tag = document.createElement("span"); tag.textContent = extension; list.appendChild(tag); });
    summary("gpu", software ? "Software rendering" : "WebGL active", renderer, software ? "bad" : "good");
    canvas.addEventListener("webglcontextlost", function (event) { event.preventDefault(); observations.webgl.contextLost = true; fail("WebGL context lost. Reload after checking native GPU logs."); });
    function shader(type, source) {
      const value = gl.createShader(type);
      gl.shaderSource(value, source);
      gl.compileShader(value);
      if (!gl.getShaderParameter(value, gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(value));
      return value;
    }
    let program;
    try {
      program = gl.createProgram();
      gl.attachShader(program, shader(gl.VERTEX_SHADER, "attribute vec3 p;attribute vec3 n;uniform mat4 m;uniform mat4 r;varying vec3 v;varying float l;void main(){gl_Position=m*vec4(p,1.0);vec3 nn=normalize((r*vec4(n,0.0)).xyz);l=0.27+0.73*max(dot(nn,normalize(vec3(-0.4,0.8,1.0))),0.0);v=abs(n);}"));
      gl.attachShader(program, shader(gl.FRAGMENT_SHADER, "precision mediump float;varying vec3 v;varying float l;uniform float wire;void main(){vec3 color=mix(vec3(0.07,0.58,0.52),vec3(0.35,0.92,0.75),v.y);color=mix(color,vec3(0.35,0.72,0.90),v.x*0.6);gl_FragColor=vec4(mix(color*l,vec3(0.30,0.75,0.67),wire),1.0);}"));
      gl.linkProgram(program);
      if (!gl.getProgramParameter(program, gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(program));
    } catch (error) { fail("WebGL shader setup failed: " + error.message); return; }
    gl.useProgram(program);
    const faces = [
      [[-1,-1,1],[1,-1,1],[1,1,1],[-1,1,1],[0,0,1]],
      [[1,-1,-1],[-1,-1,-1],[-1,1,-1],[1,1,-1],[0,0,-1]],
      [[1,-1,1],[1,-1,-1],[1,1,-1],[1,1,1],[1,0,0]],
      [[-1,-1,-1],[-1,-1,1],[-1,1,1],[-1,1,-1],[-1,0,0]],
      [[-1,1,1],[1,1,1],[1,1,-1],[-1,1,-1],[0,1,0]],
      [[-1,-1,-1],[1,-1,-1],[1,-1,1],[-1,-1,1],[0,-1,0]]
    ];
    const positions = [];
    const normals = [];
    faces.forEach(function (face) { [0,1,2,0,2,3].forEach(function (index) { positions.push.apply(positions, face[index]); normals.push.apply(normals, face[4]); }); });
    function bindAttribute(name, data) { const buffer = gl.createBuffer(); gl.bindBuffer(gl.ARRAY_BUFFER, buffer); gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(data), gl.STATIC_DRAW); const location = gl.getAttribLocation(program, name); gl.enableVertexAttribArray(location); gl.vertexAttribPointer(location, 3, gl.FLOAT, false, 0, 0); }
    bindAttribute("p", positions);
    bindAttribute("n", normals);
    const mUniform = gl.getUniformLocation(program, "m");
    const rUniform = gl.getUniformLocation(program, "r");
    const wireUniform = gl.getUniformLocation(program, "wire");
    gl.enable(gl.DEPTH_TEST);
    gl.clearColor(0.043,0.094,0.133,1);
    function multiply(a, b) { const out = new Float32Array(16); for (let col = 0; col < 4; col++) for (let row = 0; row < 4; row++) for (let k = 0; k < 4; k++) out[col * 4 + row] += a[k * 4 + row] * b[col * 4 + k]; return out; }
    let count = 0;
    let last = performance.now();
    function draw(now) {
      if (gl.isContextLost()) return;
      const dpr = Math.min(window.devicePixelRatio || 1, 2);
      const w = Math.max(1, Math.floor(canvas.clientWidth * dpr));
      const h = Math.max(1, Math.floor(canvas.clientHeight * dpr));
      if (canvas.width !== w || canvas.height !== h) { canvas.width = w; canvas.height = h; }
      gl.viewport(0,0,w,h);
      gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
      const t = now * 0.00045;
      const cy = Math.cos(t), sy = Math.sin(t), cx = Math.cos(t * 0.7), sx = Math.sin(t * 0.7);
      const ry = new Float32Array([cy,0,-sy,0,0,1,0,0,sy,0,cy,0,0,0,0,1]);
      const rx = new Float32Array([1,0,0,0,0,cx,sx,0,0,-sx,cx,0,0,0,0,1]);
      const rotation = multiply(ry, rx);
      const model = rotation.slice(); model[13] = 0.06 * Math.sin(t * 2); model[14] = -5.2;
      const f = 1 / Math.tan(Math.PI / 8), near = 0.1, far = 100;
      const perspective = new Float32Array([f / (w / h),0,0,0,0,f,0,0,0,0,(far+near)/(near-far),-1,0,0,(2*far*near)/(near-far),0]);
      gl.uniformMatrix4fv(mUniform,false,multiply(perspective,model));
      gl.uniformMatrix4fv(rUniform,false,rotation);
      gl.uniform1f(wireUniform,0);
      gl.drawArrays(gl.TRIANGLES,0,36);
      count++;
      if (now - last >= 1000) { const fps = count * 1000 / (now-last); observations.webgl.fps = Math.round(fps * 10) / 10; setText("fps", Math.round(fps) + " FPS"); count = 0; last = now; }
      requestAnimationFrame(draw);
    }
    requestAnimationFrame(draw);
  }

  async function start() {
    const webkit = /AppleWebKit\//.test(navigator.userAgent);
    summary("browser", "Browser running", webkit ? "WebKit user agent reported · native details below" : "Browser user agent reported · native details below", "good");
    try {
      const response = await fetch("/api/config", { cache: "no-store" });
      if (!response.ok) throw new Error("HTTP " + response.status);
      Object.assign(config, await response.json());
    } catch (error) { config.config_error = String(error.message || error); }
    setText("video-mode", config.video_mode === "holepunch" ? "KMS holepunch adapter" : String(config.video_mode || "unconfigured"));
    loadMedia(config.video_url || "/media/test-h264.mp4");
    layoutVideo();
    createScene();
    refreshStatus();
    setInterval(refreshStatus, 3000);
    setInterval(function () { updatePlayback(); updateJSON(); }, 1000);
    function clock() { setText("clock", new Date().toLocaleTimeString([], { hour: "2-digit", minute: "2-digit", hour12: false })); }
    clock();
    setInterval(clock, 1000);
  }
  start();
}());
