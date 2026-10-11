(() => {
  'use strict';
  const $ = (id) => document.getElementById(id);
  const escape = (value) => String(value).replace(/[&<>"']/g, (c) => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
  const timezone = Intl.DateTimeFormat().resolvedOptions().timeZone;
  let state = null, dirty = false, filter = 'all', editing = null, uploading = false, saving = false, toastTimer;
  const assets = () => new Map(state.assets.map((a) => [a.id, a]));
  const size = (bytes) => bytes ? (bytes / 1048576).toFixed(1) + ' MB' : 'Included demo';
  const runtime = (seconds) => seconds < 60 ? Math.round(seconds) + ' sec' : Math.floor(seconds / 60) + 'm ' + Math.round(seconds % 60) + 's';
  const localDate = (iso) => { if (!iso) return ''; const d = new Date(iso); return new Date(d.getTime() - d.getTimezoneOffset() * 60000).toISOString().slice(0,16); };
  const schedule = (e) => {
    if (!e.enabled) return 'Paused';
    if (!e.start && !e.end) return 'Always on';
    const format = (value) => new Date(value).toLocaleString([], {month:'short',day:'numeric',hour:'2-digit',minute:'2-digit'});
    return (e.start ? format(e.start) : 'Now') + ' → ' + (e.end ? format(e.end) : 'Ongoing');
  };
  function toast(message, error = false) {
    clearTimeout(toastTimer); $('toast').textContent = message; $('toast').classList.toggle('error', error); $('toast').hidden = false;
    toastTimer = setTimeout(() => { $('toast').hidden = true; }, error ? 9000 : 4000);
  }
  async function api(path, options = {}) {
    const response = await fetch(path, {cache:'no-store', ...options, headers:{'X-Signage-Request':'studio', ...options.headers}});
    const data = await response.json();
    if (!response.ok) throw new Error(data.error || 'The request could not be completed.');
    return data;
  }
  async function refreshDisplay() {
    try {
      const [status, config] = await Promise.all([api('/api/status'), api('/api/config')]);
      showDisplay(window.signageDisplayStatus(status, config));
    } catch (_) {
      showDisplay({tone:'neutral',title:'Device status unavailable',detail:'The preview is separate from the physical display. Check the device connection.'});
    }
  }
  function showDisplay(status) {
    $('display-status').className = 'display-status ' + status.tone;
    $('display-title').textContent = status.title;
    $('display-detail').textContent = status.detail;
    $('display-help').hidden = !status.help;
  }
  async function pollDisplay() {
    await refreshDisplay();
    setTimeout(pollDisplay, 5000);
  }
  function markDirty() { dirty = true; renderLoop(); }
  function thumb(asset, card = false) {
    if (asset.poster || asset.kind === 'image') return `<img ${card ? '' : 'class="entry-thumb"'} src="${escape(asset.poster || asset.url)}" alt="" loading="lazy">`;
    if (!card) return `<span class="entry-thumb">${asset.kind === 'video' ? '▷' : '⌘'}</span>`;
    if (asset.id.startsWith('menuboard')) return `<div class="html-art menu-art"><strong>SUBSTATION</strong><small>${asset.id === 'menuboard-pip' ? 'MENU + PICTURE-IN-PICTURE VIDEO' : 'FRESH · FILLED · YOUR WAY'}</small>${asset.id === 'menuboard-pip' ? '<span class="pip-art">▷</span>' : ''}</div>`;
    return `<div class="html-art admiral-art">${asset.id === 'welcome' ? '<img src="/brand/admiral-logo.svg" alt=""><strong>Made to be seen.</strong>' : '<strong>&lt; / &gt;</strong>'}<small>${asset.id === 'welcome' ? 'YOUR CONTENT. EVERY SCREEN.' : 'DYNAMIC HTML CONTENT'}</small></div>`;
  }
  function renderLoop() {
    const byId = assets();
    $('entry-count').textContent = state.entries.length;
    $('loop-count').textContent = state.entries.length + ' items';
    const total = state.entries.filter((e) => e.enabled).reduce((sum,e) => sum + (e.duration || byId.get(e.asset_id)?.video_duration || 0),0);
    $('loop-runtime').textContent = runtime(total);
    $('save-status').textContent = dirty ? '● Unpublished changes' : '✓ Published · Saved on this device';
    $('save-status').classList.toggle('dirty',dirty);
    $('publish').disabled = !dirty || uploading;
    $('entries').innerHTML = state.entries.length ? state.entries.map((e,i) => {
      const a = byId.get(e.asset_id);
      return `<article class="entry ${e.enabled ? '' : 'paused'}" data-entry="${escape(e.id)}"><span class="entry-number">${String(i+1).padStart(2,'0')}</span>${thumb(a)}<div class="entry-text"><strong>${escape(a.name)}</strong><small><span class="schedule-dot">●</span> ${escape(schedule(e))}</small></div><span class="entry-duration">${e.duration ? e.duration + 's' : 'Full'}</span><div class="entry-tools"><button class="icon-button" data-action="up" aria-label="Move ${escape(a.name)} up" ${i===0?'disabled':''}>↑</button><button class="icon-button" data-action="edit" aria-label="Schedule ${escape(a.name)}" title="Schedule & duration">◷</button><button class="icon-button" data-action="down" aria-label="Move ${escape(a.name)} down" ${i===state.entries.length-1?'disabled':''}>↓</button><button class="icon-button" data-action="remove" aria-label="Remove ${escape(a.name)} from loop" title="Remove from loop">×</button></div></article>`;
    }).join('') : '<p class="empty">Your next great loop starts here.<br>Add something from the media library below.</p>';
  }
  function renderLibrary() {
    $('asset-count').textContent = state.assets.length + ' assets';
    const items = state.assets.filter((a) => filter === 'all' || a.kind === filter);
    $('assets').innerHTML = items.length ? items.map((a) => `<article class="asset" data-asset="${escape(a.id)}"><div class="asset-art">${thumb(a,true)}<span class="type-badge">${a.kind === 'html' ? 'HTML PACKAGE' : a.kind.toUpperCase()}</span></div><div class="asset-details"><p class="asset-title" title="${escape(a.name)}">${escape(a.name)}</p><p class="asset-meta">${a.video_duration ? runtime(a.video_duration) + ' · ' : ''}${size(a.size)}</p><div class="asset-bottom"><button class="add-button" data-action="add">＋ Add to loop</button>${a.builtin ? '<span class="asset-meta">DEMO</span>' : '<button class="asset-remove" data-action="delete">Delete</button>'}</div></div></article>`).join('') : '<p class="empty">No content here yet. Upload a file to get started.</p>';
  }
  async function load() {
    state = await api('/api/library'); dirty = false; renderLoop(); renderLibrary();
  }
  $('entries').addEventListener('click', (event) => {
    const button = event.target.closest('button[data-action]'); if (!button || saving) return;
    const index = state.entries.findIndex((e) => e.id === button.closest('[data-entry]').dataset.entry);
    const entry = state.entries[index];
    if (button.dataset.action === 'edit') {
      editing = entry.id; const asset = assets().get(entry.asset_id);
      $('edit-title').textContent = asset.name; $('edit-duration').value = entry.duration;
      $('edit-duration').min = asset.kind === 'video' ? 0 : 1;
      $('duration-help').textContent = asset.kind === 'video' ? 'Use 0 to play the full video, or set a fixed duration.' : 'How long this image or HTML page stays on screen.';
      $('edit-start').value = localDate(entry.start); $('edit-end').value = localDate(entry.end); $('edit-enabled').checked = entry.enabled;
      $('edit-error').textContent = ''; $('editor').showModal(); return;
    }
    if (button.dataset.action === 'remove') state.entries.splice(index,1);
    else { const next = index + (button.dataset.action === 'up' ? -1 : 1); [state.entries[index],state.entries[next]] = [state.entries[next],state.entries[index]]; }
    markDirty();
  });
  $('edit-form').addEventListener('submit', (event) => {
    event.preventDefault();
    const entry = state.entries.find((e) => e.id === editing);
    const start = $('edit-start').value ? new Date($('edit-start').value).toISOString() : null;
    const end = $('edit-end').value ? new Date($('edit-end').value).toISOString() : null;
    if (start && end && start >= end) { $('edit-error').textContent = 'The end time must be after the start time.'; return; }
    Object.assign(entry,{start,end,duration:Number($('edit-duration').value),enabled:$('edit-enabled').checked});
    $('editor').close(); markDirty();
  });
  $('close-editor').onclick = () => $('editor').close();
  $('clear-dates').onclick = () => { $('edit-start').value = ''; $('edit-end').value = ''; };
  $('publish').onclick = async () => {
    saving = true; $('publish').disabled = true;
    try { state = await api('/api/playlist',{method:'PUT',headers:{'Content-Type':'application/json'},body:JSON.stringify({revision:state.revision,entries:state.entries})}); dirty = false; renderLoop(); toast('Loop published.'); refreshDisplay(); }
    catch (error) { toast(error.message,true); $('publish').disabled = false; }
    finally { saving = false; }
  };
  $('reload').onclick = async () => { if (saving || uploading) return; if (dirty && !confirm('Discard unpublished changes and reload the published loop?')) return; try { await load(); } catch (error) { toast(error.message,true); } };
  $('assets').addEventListener('click',async (event) => {
    const button = event.target.closest('button[data-action]'); if (!button || saving) return;
    const id = button.closest('[data-asset]').dataset.asset, asset = assets().get(id);
    if (button.dataset.action === 'add') {
      if (state.entries.length >= 200) return toast('A loop can contain up to 200 items.',true);
      const identity = window.crypto?.randomUUID ? crypto.randomUUID() : Date.now().toString(36) + Math.random().toString(36).slice(2);
      state.entries.push({id:identity,asset_id:id,duration:asset.kind === 'video'?0:asset.kind==='image'?12:asset.id==='menuboard'?80:asset.id==='menuboard-pip'?20:30,enabled:true,start:null,end:null});
      markDirty(); toast('Added to your loop. Publish when you’re ready.');
    } else {
      if (dirty) return toast('Publish or reload your loop before deleting library content.',true);
      if (!confirm('Delete “' + asset.name + '” from the library?')) return;
      try { state = await api('/api/assets/' + id,{method:'DELETE'}); renderLoop(); renderLibrary(); toast('Content deleted.'); } catch(error) { toast(error.message,true); }
    }
  });
  document.querySelectorAll('[data-filter]').forEach((button) => { button.onclick = () => { filter = button.dataset.filter; document.querySelectorAll('[data-filter]').forEach((b) => b.classList.toggle('active',b===button)); if (state) renderLibrary(); }; });
  function upload(file) {
    return new Promise((resolve,reject) => {
      const xhr = new XMLHttpRequest(); xhr.open('POST','/api/assets?name=' + encodeURIComponent(file.name)); xhr.setRequestHeader('X-Signage-Request','studio'); xhr.setRequestHeader('Content-Type','application/octet-stream'); xhr.timeout = 600000;
      xhr.upload.onprogress = (event) => { if (event.lengthComputable) { $('progress').value = event.loaded / event.total * 100; if (event.loaded === event.total) $('upload-label').textContent = 'Preparing ' + file.name + ' for playback…'; } };
      xhr.onload = () => { try { const data = JSON.parse(xhr.responseText); if (xhr.status >= 400) reject(new Error(data.error)); else resolve(data); } catch (_) { reject(new Error('The upload could not be completed.')); } };
      xhr.onerror = () => reject(new Error('Upload interrupted. Check the connection and try again.'));
      xhr.ontimeout = () => reject(new Error('Upload timed out. Please try again.'));
      xhr.send(file);
    });
  }
  async function uploadFiles(files) {
    if (uploading || saving || !files.length || !state) return;
    uploading = true; $('upload-progress').hidden = false; $('browse').disabled = $('upload-top').disabled = true; $('publish').disabled = true;
    try {
      for (const file of files) {
        if (file.size > 512*1024*1024 || file.size === 0) { toast(file.name + ': choose a non-empty file up to 512 MB.',true); continue; }
        $('upload-label').textContent = 'Uploading ' + file.name + '…'; $('progress').value = 0;
        const oldRevision = state.revision;
        await upload(file);
        const latest = await api('/api/library');
        if (!dirty) state = latest;
        else { state.assets = latest.assets; if (latest.revision === oldRevision + 1) state.revision = latest.revision; }
        renderLibrary(); renderLoop(); toast(file.name + ' is ready. Add it to your loop.');
      }
    } catch(error) { toast(error.message,true); }
    finally { uploading = false; $('upload-progress').hidden = true; $('browse').disabled = $('upload-top').disabled = false; $('files').value = ''; renderLoop(); }
  }
  $('browse').onclick = $('upload-top').onclick = () => $('files').click();
  $('files').onchange = () => uploadFiles(Array.from($('files').files));
  $('dropzone').ondragover = (event) => { event.preventDefault(); $('dropzone').classList.add('dragover'); };
  $('dropzone').ondragleave = () => $('dropzone').classList.remove('dragover');
  $('dropzone').ondrop = (event) => { event.preventDefault(); $('dropzone').classList.remove('dragover'); uploadFiles(Array.from(event.dataTransfer.files)); };
  $('timezone').textContent = 'Your timezone · ' + timezone;
  $('edit-timezone').textContent = 'Times are in ' + timezone + '. Leave both blank to play any time.';
  window.addEventListener('beforeunload', (event) => { if (dirty || uploading) { event.preventDefault(); event.returnValue=''; } });
  window.addEventListener('message',(event) => { if (event.origin === location.origin && event.source === $('preview').contentWindow && event.data?.type === 'signage-status') $('playback-status').textContent = event.data.empty ? 'Standby' : 'Loop playing'; });
  pollDisplay();
  load().catch((error) => { $('entries').innerHTML='<p class="empty">Unable to load the library. Use Reload to try again.</p>'; toast(error.message,true); });
})();
