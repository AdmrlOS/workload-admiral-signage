/* SUBSTATION menuboard demo: scene controller + original SVG artwork.
 *
 * Sequence (loops forever): A (main menu) -> B (fullscreen video) -> A -> C (menu + corner video)
 *
 * Query params (QA / screenshots):
 *   ?scene=A|B|C   show one scene only
 *   &freeze=1      no timers, no transitions, no advancing (B/C still start their video)
 *   &t=3           with freeze: seek the video to t seconds and pause it
 *   &adur=18 &cdur=20   scene A / C durations in seconds (A: 15-20, C: ~20)
 *   &srca=URL &srcb=URL   override clip URLs (e.g. a missing file to test the error fallback)
 *   &log=1         mirror the transition log to console (always kept in window.__menuLog)
 *
 * Holepunch rules honoured: one <video> alive at a time (created on scene entry, src removed +
 * element removed on exit); no transform/opacity/filter ever applied to the video or its
 * ancestors; transitions are a page-only curtain that covers the video box.
 */
(() => {
  'use strict';
  const Q = new URLSearchParams(location.search);
  const num = (k, d) => { const v = parseFloat(Q.get(k)); return isFinite(v) && v > 0 ? v : d; };
  const CFG = {
    adur: Math.min(20, Math.max(15, num('adur', 18))) * 1000,
    cdur: num('cdur', 20) * 1000,
    seq: ['A', 'B', 'A', 'C'],
    srcA: Q.get('srca') || '/media/promo-a.mp4',   // ?srca= / ?srcb= override (error-path QA)
    srcB: Q.get('srcb') || '/media/promo-b.mp4',
    wipeMs: 550,
    canplayTimeout: 4000,   // start anyway if the video is not ready by then
    stallTimeout: 9000,     // skip the scene if the video never reaches 'playing'
    errorFallbackMs: 1500,
  };
  const freeze = Q.get('freeze') === '1';
  const only = (Q.get('scene') || '').toUpperCase();
  const $ = (s) => document.querySelector(s);
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  const t0 = performance.now();
  window.__menuLog = [];
  let current = '-';
  function log(ev, extra) {
    const e = Object.assign({ t: +((performance.now() - t0) / 1000).toFixed(2), scene: current, ev }, extra || {});
    window.__menuLog.push(e);
    if (Q.get('log') === '1') console.log('[menuboard] ' + JSON.stringify(e));
  }

  /* ---------------- artwork ---------------- */
  const wave = (x0, x1, y, amp, len, ph = 0) => {
    let d = `M${x0},${y.toFixed(1)}`;
    for (let x = x0; x <= x1; x += 6) d += ` L${x},${(y + Math.sin(x / len + ph) * amp).toFixed(1)}`;
    return d;
  };
  const band = (top, h, fill, stroke, amp = 3, len = 9, ph = 0, x0 = 8, x1 = 292) => {
    let d = wave(x0, x1, top, amp, len, ph);
    for (let x = x1; x >= x0; x -= 6) d += ` L${x},${(top + h + Math.sin(x / len + ph + 1) * amp).toFixed(1)}`;
    return `<path d="${d}Z" fill="${fill}" stroke="${stroke}" stroke-width="1.6" stroke-linejoin="round"/>`;
  };
  const row = (n, x0, step) => Array.from({ length: n }, (_, i) => x0 + i * step);
  const LAYERS = {
    lettuce: { h: 15, draw: (y, h) => band(y, h, 'url(#gLettuce)', '#4a8a1c', 5, 7) + band(y + 2, h - 9, '#BDE878', 'none', 4, 6, 1.6) },
    tomato: { h: 11, draw: (y, h) => row(6, 38, 45).map((x, i) => `<ellipse cx="${x}" cy="${y + h / 2 + (i % 2) * 1.5}" rx="26" ry="${h / 2}" fill="#E23B2B" stroke="#a8190f" stroke-width="1.5"/><ellipse cx="${x - 5}" cy="${y + h / 2 - 1.5}" rx="13" ry="${h / 4}" fill="#F7826F" opacity=".7"/>`).join('') },
    cheese: { h: 9, draw: (y, h) => `<path d="M16,${y} H284 V${y + h} H252 L240,${y + h + 9} L228,${y + h} H80 L68,${y + h + 8} L56,${y + h} H16Z" fill="#FFC20D" stroke="#c78f00" stroke-width="1.6" stroke-linejoin="round"/><path d="M22,${y + 2.5} H278" stroke="#FFE27A" stroke-width="2"/>` },
    swiss: { h: 9, draw: (y, h) => `<path d="M16,${y} H284 V${y + h} H16Z" fill="#FFEBA1" stroke="#cfa534" stroke-width="1.6"/>` + [60, 130, 190, 250].map((x, i) => `<ellipse cx="${x}" cy="${y + h / 2}" rx="${4 + i % 2 * 2}" ry="${2.8}" fill="#e8c35f"/>`).join('') },
    ham: { h: 11, draw: (y, h) => band(y, h, '#F09AA9', '#b84a5f', 3.5, 11, 0.6) + band(y + 2, 3, '#F8BFC8', 'none', 3, 11, 0.6) },
    turkey: { h: 11, draw: (y, h) => band(y, h, '#F2CFAE', '#b98b5f', 3.5, 12, 2) + band(y + 2, 3, '#FBE5CF', 'none', 3, 12, 2) },
    roastbeef: { h: 12, draw: (y, h) => band(y, h, '#B24C3C', '#6e2216', 4, 10, 1) + band(y + 3, 3, '#D4786A', 'none', 3, 10, 1) },
    steak: { h: 14, draw: (y, h) => band(y, h, '#6F3C22', '#3b1c0b', 5, 8, 0.3) + row(9, 24, 31).map((x, i) => `<ellipse cx="${x}" cy="${y + 5 + (i % 3) * 3}" rx="9" ry="2.6" fill="#8e5230" transform="rotate(${i * 17 % 40 - 20} ${x} ${y + 6})"/>`).join('') },
    brisket: { h: 14, draw: (y, h) => band(y, h, '#7A3F25', '#3b1c0b', 4, 9, 0.9) + band(y + h - 5, 5, '#4d2412', 'none', 3, 9, 0.9) + row(8, 30, 34).map((x, i) => `<rect x="${x}" y="${y + 3 + i % 2 * 3}" width="14" height="3" rx="1.5" fill="#a5623a"/>`).join('') },
    chicken: { h: 14, draw: (y, h) => row(7, 22, 40).map((x, i) => `<rect x="${x}" y="${y + (i % 2) * 2}" width="46" height="${h - 2}" rx="7" fill="#DDA45F" stroke="#9a6425" stroke-width="1.5"/><path d="M${x + 6},${y + 4 + (i % 2) * 2} h30" stroke="#8a4a14" stroke-width="2.4" stroke-linecap="round" opacity=".7"/>`).join('') },
    meatball: { h: 22, draw: (y, h) => band(y + 12, 10, '#C8301F', '#7f1209', 3, 8, 0) + row(7, 34, 38).map((x, i) => `<circle cx="${x}" cy="${y + 11}" r="12" fill="#8B4A2B" stroke="#4d230f" stroke-width="1.5"/><circle cx="${x - 4}" cy="${y + 7}" r="3.6" fill="#b2704c" opacity=".7"/>`).join('') },
    tuna: { h: 14, draw: (y, h) => band(y, h, '#EBDDB5', '#a7945d', 4, 6, 0.2) + row(14, 20, 20).map((x, i) => `<circle cx="${x}" cy="${y + 5 + (i * 7) % 8}" r="2" fill="${i % 3 ? '#cdb987' : '#9ac15d'}"/>`).join('') },
    onion: { h: 7, draw: (y, h) => row(8, 30, 34).map((x, i) => `<ellipse cx="${x}" cy="${y + 3.5}" rx="17" ry="3.6" fill="none" stroke="${i % 2 ? '#B070C9' : '#d6a6e6'}" stroke-width="2.2"/>`).join('') },
    cucumber: { h: 9, draw: (y, h) => row(7, 36, 38).map((x) => `<ellipse cx="${x}" cy="${y + 4.5}" rx="20" ry="4.5" fill="#B7DB8E" stroke="#5f8f33" stroke-width="1.6"/><ellipse cx="${x}" cy="${y + 4.5}" rx="13" ry="2.4" fill="#E3F3C9"/>`).join('') },
    pickle: { h: 8, draw: (y, h) => row(7, 40, 36).map((x) => `<ellipse cx="${x}" cy="${y + 4}" rx="17" ry="4" fill="#8DAE4A" stroke="#506f1b" stroke-width="1.6"/>`).join('') },
    jalapeno: { h: 8, draw: (y, h) => row(8, 30, 34).map((x, i) => `<ellipse cx="${x}" cy="${y + 4}" rx="14" ry="4" fill="#4f9b2a" stroke="#285f10" stroke-width="1.5"/><ellipse cx="${x}" cy="${y + 4}" rx="6" ry="1.8" fill="#bfe08d"/>`).join('') },
    pepperoni: { h: 11, draw: (y, h) => row(7, 34, 38).map((x, i) => `<ellipse cx="${x}" cy="${y + 5.5 + (i % 2)}" rx="22" ry="5.8" fill="#BC2A1D" stroke="#6d0f06" stroke-width="1.5"/>` + [-9, 0, 8].map((dx, j) => `<circle cx="${x + dx}" cy="${y + 5 + (i % 2) + (j % 2)}" r="1.5" fill="#e0675a"/>`).join('')).join('') },
    salami: { h: 10, draw: (y, h) => row(6, 48, 41).map((x, i) => `<ellipse cx="${x}" cy="${y + 5}" rx="24" ry="5.2" fill="#9C3B3B" stroke="#5c1414" stroke-width="1.5"/>` + [-12, -2, 9, 15].map((dx, j) => `<circle cx="${x + dx}" cy="${y + 5 + (j % 2) - .5}" r="1.7" fill="#f2c9c0"/>`).join('')).join('') },
    bacon: { h: 8, draw: (y, h) => row(4, 24, 66).map((x, i) => `<path d="${wave(x, x + 60, y + 3, 2.6, 6, i)}" fill="none" stroke="#9C2D1C" stroke-width="6" stroke-linecap="round"/><path d="${wave(x, x + 60, y + 3, 2.6, 6, i)}" fill="none" stroke="#e98a6b" stroke-width="2" stroke-linecap="round"/>`).join('') },
    avocado: { h: 11, draw: (y, h) => band(y, h, '#9CBE57', '#587b24', 3, 12, 0.5) + band(y + 2, 3, '#D3E79D', 'none', 3, 12, 0.5) },
    capsicum: { h: 9, draw: (y, h) => row(7, 30, 38).map((x, i) => `<path d="M${x - 17},${y + 8} Q${x},${y - 6} ${x + 17},${y + 8}" fill="none" stroke="${['#D9372B', '#F2A20C', '#4FA12B'][i % 3]}" stroke-width="5" stroke-linecap="round"/>`).join('') },
    ranch: { h: 4, draw: (y, h) => `<path d="${wave(14, 286, y + 2, 2.4, 5)}" fill="none" stroke="#fffdf0" stroke-width="4" stroke-linecap="round" opacity=".95"/>` },
    bbq: { h: 4, draw: (y, h) => `<path d="${wave(14, 286, y + 2, 2.4, 5, 1)}" fill="none" stroke="#6b2410" stroke-width="4.5" stroke-linecap="round"/>` },
    teriyaki: { h: 3, draw: (y, h) => `<path d="${wave(14, 286, y + 1.5, 2, 5, 2)}" fill="none" stroke="#5a2a0a" stroke-width="3.5" stroke-linecap="round" opacity=".9"/>` },
  };
  const SEEDS = [[62, -22, 20], [100, -33, -15], [150, -38, 5], [200, -33, 25], [240, -22, -10], [125, -22, 40], [178, -23, -30], [88, -10, 10], [212, -10, -20], [150, -13, 0], [48, -9, -35], [255, -9, 35]];
  function sub(layers, opts = {}) {
    const defs = layers.map((k) => LAYERS[k]);
    const sum = defs.reduce((a, l) => a + l.h - 2, 0);
    const k = Math.min(1, 62 / sum);
    let y = 112;
    let out = `<ellipse cx="150" cy="146" rx="136" ry="4.5" fill="#000" opacity=".22"/>`;
    out += `<path d="M14,108 H286 Q297,108 297,120 Q297,142 268,142 H32 Q3,142 3,120 Q3,108 14,108Z" fill="url(#gBreadBot)" stroke="#9a5d1d" stroke-width="2"/><path d="M16,126 Q150,137 284,126" fill="none" stroke="#fff" stroke-width="3" opacity=".25" stroke-linecap="round"/>`;
    defs.forEach((l) => { const h = l.h * k; y -= h - 2; out += l.draw(y, h); });
    const Y = y + 3;
    out += `<path d="M5,${Y + 8} C2,${Y - 34} 70,${Y - 48} 150,${Y - 48} C230,${Y - 48} 298,${Y - 34} 295,${Y + 8} Q295,${Y + 12} 285,${Y + 12} H15 Q5,${Y + 12} 5,${Y + 8}Z" fill="url(#gBreadTop)" stroke="#9a5d1d" stroke-width="2"/>`;
    out += `<path d="M30,${Y - 12} C48,${Y - 36} 92,${Y - 42} 140,${Y - 42}" fill="none" stroke="#fff" stroke-width="5" opacity=".38" stroke-linecap="round"/>`;
    if (opts.seeds !== false) out += SEEDS.map(([x, dy, r]) => `<ellipse cx="${x}" cy="${Y + dy}" rx="4.2" ry="2.2" fill="#FFF1C9" stroke="#b98534" stroke-width=".8" transform="rotate(${r} ${x} ${Y + dy})"/>`).join('');
    return `<svg viewBox="0 0 300 150" xmlns="http://www.w3.org/2000/svg">${out}</svg>`;
  }
  const sv = (vb, body) => `<svg viewBox="${vb}" xmlns="http://www.w3.org/2000/svg">${body}</svg>`;
  const ICON = {
    cookie: sv('0 0 100 100', `<ellipse cx="50" cy="90" rx="38" ry="5" fill="#000" opacity=".2"/><circle cx="50" cy="48" r="40" fill="url(#gCookie)" stroke="#7d4a1f" stroke-width="3"/>${[[34, 34], [58, 30], [66, 52], [44, 56], [30, 62], [56, 70], [72, 36]].map(([x, y], i) => `<path d="M${x - 6},${y} l6,-6 l7,4 l-2,8 l-8,1Z" fill="#4a2410" transform="rotate(${i * 40} ${x} ${y})"/>`).join('')}`),
    chips: sv('0 0 100 100', `<ellipse cx="50" cy="92" rx="30" ry="4" fill="#000" opacity=".2"/><path d="M22,18 L30,12 L38,18 L46,12 L54,18 L62,12 L70,18 L78,12 L84,20 L76,88 H24Z" fill="#E1281E" stroke="#8b1009" stroke-width="3" stroke-linejoin="round"/><path d="M27,48 H79 L77,70 H29Z" fill="#FFC20D"/><text x="53" y="64" font-size="17" text-anchor="middle" fill="#005f0e" font-weight="700" font-family="DejaVu Sans Condensed,sans-serif">CHIPS</text>`),
    drink: sv('0 0 100 100', `<ellipse cx="50" cy="93" rx="22" ry="3.5" fill="#000" opacity=".2"/><path d="M60,4 L66,4 L63,26" fill="none" stroke="#FFC20D" stroke-width="5" stroke-linecap="round"/><rect x="26" y="22" width="48" height="8" rx="3" fill="#2b2b2b"/><path d="M29,30 H71 L65,90 H35Z" fill="#008C15" stroke="#004d0b" stroke-width="3" stroke-linejoin="round"/><path d="M32,50 H68 L66,68 H34Z" fill="#FFC20D"/><circle cx="50" cy="59" r="5" fill="#008C15"/>`),
    water: sv('0 0 100 100', `<ellipse cx="50" cy="94" rx="18" ry="3" fill="#000" opacity=".2"/><rect x="42" y="6" width="16" height="10" rx="3" fill="#008C15"/><path d="M42,16 H58 L72,34 V88 Q72,92 68,92 H32 Q28,92 28,88 V34Z" fill="#CDEFFA" stroke="#2a86a8" stroke-width="3" stroke-linejoin="round"/><path d="M29,52 H71 V76 H29Z" fill="#008C15"/><path d="M35,40 V84" stroke="#fff" stroke-width="4" opacity=".6" stroke-linecap="round"/>`),
    coldbrew: sv('0 0 100 100', `<ellipse cx="50" cy="93" rx="22" ry="3.5" fill="#000" opacity=".2"/><path d="M58,2 L64,2 L61,24" fill="none" stroke="#fff" stroke-width="5" stroke-linecap="round"/><rect x="26" y="22" width="48" height="8" rx="3" fill="#f4f4f4" stroke="#aaa" stroke-width="1.5"/><path d="M29,30 H71 L65,90 H35Z" fill="#4a2c17" stroke="#26140a" stroke-width="3" stroke-linejoin="round"/><path d="M31,44 H69 L67,52 H32Z" fill="#c9a077" opacity=".9"/><rect x="40" y="2" width="0" height="0"/>`),
    soup: sv('0 0 100 100', `<ellipse cx="50" cy="90" rx="34" ry="4" fill="#000" opacity=".2"/><path d="M16,50 H84 Q84,86 50,86 Q16,86 16,50Z" fill="#fff" stroke="#8a8a8a" stroke-width="3"/><ellipse cx="50" cy="50" rx="34" ry="8" fill="#E8742A" stroke="#a34b11" stroke-width="2.5"/><circle cx="40" cy="49" r="3" fill="#f6c07a"/><circle cx="58" cy="51" r="3.5" fill="#8dbd4a"/><path d="M36,36 q-6,-9 0,-16 M52,36 q-6,-9 0,-16 M68,36 q-6,-9 0,-16" fill="none" stroke="#b9b9b9" stroke-width="3.5" stroke-linecap="round"/>`),
    coffee: sv('0 0 100 100', `<ellipse cx="50" cy="93" rx="22" ry="3.5" fill="#000" opacity=".2"/><path d="M62,2 q-6,8 0,14" fill="none" stroke="#bbb" stroke-width="3" stroke-linecap="round"/><rect x="26" y="22" width="48" height="9" rx="4" fill="#2b2b2b"/><path d="M29,31 H71 L65,90 H35Z" fill="#fff" stroke="#8d8d8d" stroke-width="3" stroke-linejoin="round"/><path d="M31,46 H69 L67,68 H33Z" fill="#008C15"/><path d="M40,58 h20" stroke="#FFC20D" stroke-width="5" stroke-linecap="round"/>`),
    wrap: sv('0 0 140 100', `<ellipse cx="70" cy="92" rx="56" ry="5" fill="#000" opacity=".2"/><g transform="rotate(-14 70 52)"><rect x="12" y="26" width="116" height="48" rx="24" fill="#F0D6A0" stroke="#a87a2b" stroke-width="3"/><path d="M30,28 v44 M52,27 v46 M74,27 v46 M96,27 v46" stroke="#c99a4a" stroke-width="3" opacity=".6"/><path d="M12,50 a24,24 0 0 1 24,-24 v48 a24,24 0 0 1 -24,-24Z" fill="#F7E7C0" stroke="#a87a2b" stroke-width="3"/><circle cx="25" cy="42" r="7" fill="#7BC043"/><circle cx="29" cy="56" r="6" fill="#DDA45F"/><circle cx="22" cy="50" r="4" fill="#E23B2B"/><path d="M92,26 h36 v48 h-36Z" fill="#fff" opacity=".35"/></g>`),
    muffin: sv('0 0 140 100', `<ellipse cx="70" cy="92" rx="52" ry="5" fill="#000" opacity=".2"/><path d="M18,66 Q70,84 122,66 L118,76 Q70,94 22,76Z" fill="#E3AA5F" stroke="#9a5d1d" stroke-width="3" stroke-linejoin="round"/><path d="M22,60 H118 V68 Q70,82 22,68Z" fill="#F4C87F" stroke="#9a5d1d" stroke-width="2.5"/><path d="M26,56 Q70,66 112,56 L116,64 Q70,76 24,64Z" fill="#FFC20D" stroke="#c78f00" stroke-width="2"/><path d="M26,44 Q36,36 52,42 Q66,34 84,41 Q100,36 114,46 Q112,58 70,58 Q28,58 26,44Z" fill="#fff" stroke="#c9c9c9" stroke-width="2"/><circle cx="70" cy="46" r="11" fill="#FFB400" stroke="#d98800" stroke-width="2"/><path d="M20,34 Q70,-6 120,34 Q120,42 112,42 H28 Q20,42 20,34Z" fill="url(#gBreadTop)" stroke="#9a5d1d" stroke-width="3"/>`),
    platter: sv('0 0 140 100', `<ellipse cx="70" cy="92" rx="56" ry="5" fill="#000" opacity=".2"/><rect x="10" y="62" width="120" height="22" rx="8" fill="#E3AA5F" stroke="#9a5d1d" stroke-width="3"/><path d="M18,62 Q70,6 122,62Z" fill="url(#gBreadTop)" stroke="#9a5d1d" stroke-width="3"/><path d="M22,64 q10,-8 20,0 t20,0 t20,0 t20,0" fill="none" stroke="#7BC043" stroke-width="6" stroke-linecap="round"/>`),
  };

  /* ---------------- wordmark (original: yellow roundel + italic type) ---------------- */
  function roundel(px) {
    return `<svg width="${px}" height="${px}" viewBox="0 0 64 64"><circle cx="32" cy="32" r="30" fill="#FFC20D" stroke="#fff" stroke-width="3"/><rect x="9" y="29" width="46" height="9" rx="4.5" fill="#008C15"/><path d="M10,30 Q32,6 54,30Z" fill="#005f0e"/><path d="M12,41 H52 Q50,50 32,50 Q14,50 12,41Z" fill="#005f0e"/><path d="M14,34 q4,-3 8,0 t8,0 t8,0 t8,0" fill="none" stroke="#fff" stroke-width="2.4" stroke-linecap="round"/></svg>`;
  }
  const wordmark = (tag = true) => `<span class="wm">${roundel(1.5 * 40)}<span class="t">SUB<b>STATION</b>${tag ? '<small>FRESH · FILLED · YOUR WAY</small>' : ''}</span></span>`;
  const money = (v) => { const [d, c] = v.toFixed(2).split('.'); return `$${d}<sup style="font-size:.55em;vertical-align:top;position:relative;top:.05em">${c}</sup>`; };
  const mon = (v) => '$' + v.toFixed(2);

  /* ---------------- data ---------------- */
  const SUBS = [
    ['Spicy Italian Stack', 'Pepperoni, salami, jalapeños, cheese', [480, 960], [10.5, 16.9], ['lettuce', 'tomato', 'cheese', 'salami', 'pepperoni']],
    ['Teriyaki Chicken', 'Glazed chicken strips, onion, sweet teriyaki', [370, 740], [10.2, 15.9], ['lettuce', 'onion', 'chicken', 'teriyaki']],
    ['Meatball Marinara', 'Beef meatballs, marinara, melted cheese', [480, 960], [9.9, 15.2], ['cheese', 'meatball']],
    ['Steak & Cheese', 'Shaved steak, capsicum, melted cheese', [380, 760], [11.9, 18.5], ['lettuce', 'capsicum', 'cheese', 'steak']],
    ['Roast Beef Classic', 'Rare roast beef, onion, pickles', [320, 640], [10.9, 16.9], ['lettuce', 'pickle', 'onion', 'roastbeef']],
    ['Turkey Breast', 'Sliced turkey, lettuce, tomato, cucumber', [280, 560], [9.2, 14.5], ['lettuce', 'tomato', 'cucumber', 'turkey']],
    ['Ham & Swiss', 'Leg ham, Swiss cheese, lettuce, tomato', [290, 580], [8.9, 13.9], ['lettuce', 'tomato', 'swiss', 'ham']],
    ['Chicken Bacon Ranch', 'Chicken, crisp bacon, cheddar, ranch', [510, 1020], [11.5, 17.9], ['lettuce', 'bacon', 'cheese', 'chicken', 'ranch']],
    ['Tuna Melt', 'Creamy tuna, melted cheese, cucumber', [450, 900], [9.9, 15.5], ['cucumber', 'cheese', 'tuna']],
    ['Garden Crunch (V)', 'Avocado, cucumber, capsicum, lettuce, tomato', [230, 460], [8.5, 13.5], ['lettuce', 'tomato', 'cucumber', 'capsicum', 'avocado']],
  ];
  const SIDES = [
    ['cookie', 'Choc Chip Cookie', '210 Cal · 3 for $5.00', 1.9],
    ['chips', 'Crinkle Chips', '150 Cal', 2.9],
    ['soup', 'Soup of the Day', '190 Cal · with bread roll', 7.9],
    ['drink', 'Fountain Drink', '600 mL · 0–210 Cal', 3.9],
    ['water', 'Still Water', '600 mL · 0 Cal', 3.5],
    ['coldbrew', 'Iced Cold Brew', '60 Cal', 5.5],
  ];
  const DEALS = [
    ['Solo Lunch', 'Any 6-inch sub + chips + 600 mL drink', 'from 640 Cal', 15.9, ['lettuce', 'tomato', 'cheese', 'turkey']],
    ['Duo Share', 'Two 6-inch subs + 2 chips + 2 drinks', 'from 1,290 Cal', 29.9, ['lettuce', 'pickle', 'cheese', 'roastbeef', 'bbq']],
    ['Family Feast', '2 footlongs + 2 kids packs + 4 cookies', 'feeds 4', 54.9, ['lettuce', 'tomato', 'swiss', 'ham', 'ranch']],
  ];
  const BKFST = [
    ['muffin', 'Egg & Cheese Muffin', 'Free-range egg, cheddar, toasted muffin', '320 Cal', 6.5],
    ['wrap', 'Chicken Caesar Wrap', 'Chicken, cos, parmesan, caesar dressing', '480 Cal', 10.9],
    ['muffin', 'Bacon & Egg Roll', 'Smoky bacon, egg, tomato relish', '410 Cal', 8.9],
    ['wrap', 'Falafel Garden Wrap (V)', 'Falafel, hummus, cucumber, tomato', '390 Cal', 9.9],
    ['platter', 'Sausage & Hash Brown Sub', 'Pork sausage, hash brown, cheese', '520 Cal', 9.5],
    ['wrap', 'Steak & Onion Wrap', 'Shaved steak, caramelised onion, cheese', '520 Cal', 11.9],
  ];

  /* ---------------- scene markup ---------------- */
  const hdr = (right1, right2) => `<header class="hdr">${wordmark(true)}<div class="tag"><b>${right1}</b>${right2}</div></header>`;
  function buildA() {
    const cards = SUBS.map(([n, d, c, p, l], i) => `<div class="card"><div class="ico">${sub(l)}</div><div class="info"><h3>${n}</h3><p>${d}</p><div class="cal">6″ ${c[0]} Cal · FL ${c[1]} Cal</div></div>
      <div class="prices"><div class="pr" style="--d:${(i * 0.45).toFixed(2)}s"><small>6-INCH</small><b>${mon(p[0])}</b></div><div class="pr fl" style="--d:${(i * 0.45 + 0.15).toFixed(2)}s"><small>FOOTLONG</small><b>${mon(p[1])}</b></div></div></div>`).join('');
    const sides = SIDES.map(([ic, n, d, p]) => `<li>${ICON[ic]}<div><h4>${n}</h4><p>${d}</p></div><b>${mon(p)}</b></li>`).join('');
    const tick = ['Fresh bread baked every few hours', 'Add avocado +$2.00', 'Double meat +$4.50', 'Kids pack $9.50', 'Order ahead in the SUBSTATION app', 'All prices in AUD incl. GST', 'Calories are per serve, adults need ~8,700 kJ a day'].map((t) => `<span>${t}</span>`).join('');
    $('#sceneA').innerHTML = `<div class="stripe" style="position:absolute;inset:0"></div>${hdr('Main menu', 'All prices AUD')}
      <div class="strip-title" style="left:30px;top:114px;width:1270px">Signature subs <span>Pick your bread · cheese · veggies · sauce</span></div>
      <div class="grid">${cards}</div>
      <div class="hero"><div class="new">NEW</div><div class="art">${sub(['lettuce', 'pickle', 'onion', 'cheese', 'brisket', 'bbq'])}</div>
        <div class="cap"><div><h2>Smokehouse Brisket Melt</h2><p>Smoked brisket, BBQ glaze · 520 Cal</p></div><div class="pp"><div><small>6-INCH</small><b>${mon(12.9)}</b></div><div class="y"><small>FOOTLONG</small><b>${mon(19.5)}</b></div></div></div></div>
      <div class="sides"><h2>Sides <em>&amp;</em> Drinks</h2><ul>${sides}</ul></div>
      <div class="banner"><div class="art">${sub(['lettuce', 'tomato', 'cheese', 'turkey', 'ranch'])}</div>
        <div class="txt"><h2>Meal deal <span>★</span></h2><p>Any footlong + chips + 600 mL drink</p></div>
        <div class="big"><small>only<br>save $6.00</small><b>$19<sup>90</sup></b></div></div>
      <div class="ticker"><div class="run">${tick}${tick}</div></div>`;
  }
  function buildC() {
    const deals = DEALS.map(([n, d, c, p, l], i) => `<div class="deal"><div class="top">${sub(l)}</div><h3>${n}</h3><p>${d}</p><div class="cal">${c}</div>
      <div class="pbadge" style="--d:${i * 0.8}s"><small>ONLY</small><b>${money(p).replace(/<sup[^>]*>/, '<sup>')}</b></div></div>`).join('');
    const items = BKFST.map(([ic, n, d, c, p]) => `<div class="item"><div class="ico">${ICON[ic]}</div><div class="info"><h4>${n}</h4><p>${d}</p><div class="cal">${c}</div></div><div class="pc">${mon(p)}</div></div>`).join('');
    const burst = (() => { let pts = ''; for (let i = 0; i < 32; i++) { const a = i * Math.PI / 16, r = i % 2 ? 66 : 82; pts += `${(85 + Math.cos(a) * r).toFixed(1)},${(85 + Math.sin(a) * r).toFixed(1)} `; } return `<svg class="starburst" viewBox="0 0 170 170"><polygon points="${pts}" fill="#D7261E" stroke="#fff" stroke-width="3"/></svg>`; })();
    $('#sceneC').innerHTML = `<div class="stripe" style="position:absolute;inset:0"></div>${hdr('Meal deals', 'Breakfast &amp; wraps')}
      <div class="strip-title" style="left:30px;top:116px;width:1210px">Meal deals <span>Save when you bundle</span></div>
      <div class="deals">${deals}</div>
      <div class="strip-title" style="left:30px;top:524px;width:1210px">Breakfast &amp; wraps <span>Breakfast served until 10:30 am</span></div>
      <div class="bkfst">${items}</div>
      <div class="note">Add a fresh brew from $3.90 · All prices AUD</div>
      <div class="feature"><div class="ey">★ SUB OF THE MONTH</div><h2>Smokehouse Brisket Melt</h2>
        <div class="art">${sub(['lettuce', 'pickle', 'onion', 'cheese', 'brisket', 'bbq'])}</div>${burst}<div class="starburst-t"><small>NEW!</small><b>TRY IT</b></div>
        <div class="pl"><div><small>6-INCH</small><b>${mon(12.9)}</b></div><u></u><div><small>FOOTLONG</small><b>${mon(19.5)}</b></div></div></div>
      <div class="vframe"><i></i>Now showing</div><div class="vside"></div>
      <div class="corner" id="cornerHost"><div class="poster" id="poster"><div class="ph">Fresh today</div><div class="art">${sub(['lettuce', 'tomato', 'cheese', 'turkey', 'ranch'])}</div><div class="pf">Made your way at SUBSTATION</div></div></div>`;
  }
  function buildMarks() {
    $('#bugMark').innerHTML = wordmark(false);
    $('#curtainMark').innerHTML = wordmark(true);
  }

  /* ---------------- curtain ---------------- */
  const curtain = $('#curtain');
  const OFF_L = 'translateX(-2120px)', ON = 'translateX(0px)', OFF_R = 'translateX(2120px)';
  async function wipe(from, to) {
    const a = curtain.animate([{ transform: from }, { transform: to }], { duration: CFG.wipeMs, easing: 'cubic-bezier(.6,0,.3,1)', fill: 'forwards' });
    await a.finished;
    curtain.style.transform = to;
    a.cancel();
  }
  const coverIn = () => wipe(OFF_L, ON);
  const reveal = () => wipe(ON, OFF_R);

  /* ---------------- video lifecycle ---------------- */
  let video = null;
  function killVideo() {
    if (!video) return;
    const v = video; video = null;
    try { v.pause(); } catch (e) { /* ignore */ }
    v.onended = v.onerror = v.onplaying = null;
    v.removeAttribute('src');
    try { v.load(); } catch (e) { /* ignore */ }
    v.remove();
    log('video-released');
  }
  // Create the one live <video>. Resolves with {ready:boolean}. Caller starts playback via start().
  function makeVideo(host, src, { before } = {}) {
    killVideo();
    const v = document.createElement('video');
    v.muted = true; v.defaultMuted = true; v.playsInline = true; v.controls = false; v.loop = false;
    v.preload = 'auto'; v.setAttribute('muted', ''); v.setAttribute('playsinline', '');
    v.disablePictureInPicture = true;
    if (before) host.insertBefore(v, before); else host.appendChild(v);
    video = v;
    return v;
  }
  function loadVideo(v, src) {
    return new Promise((resolve) => {
      let done = false;
      const fin = (ok, why) => { if (done) return; done = true; clearTimeout(tm); v.removeEventListener('canplay', onc); v.removeEventListener('error', one); resolve({ ok, why }); };
      const onc = () => fin(true, 'canplay');
      const one = () => fin(false, 'error');
      const tm = setTimeout(() => fin(true, 'timeout'), CFG.canplayTimeout);
      v.addEventListener('canplay', onc); v.addEventListener('error', one);
      v.src = src;
      v.load();
    });
  }
  // Start playback; resolves 'playing' | 'error' | 'stall'.
  function startPlay(v) {
    return new Promise((resolve) => {
      let done = false;
      const fin = (r) => { if (done) return; done = true; clearTimeout(tm); resolve(r); };
      const tm = setTimeout(() => fin('stall'), CFG.stallTimeout);
      v.addEventListener('playing', () => { log('video-playing', { pos: +v.currentTime.toFixed(2) }); fin('playing'); }, { once: true });
      v.addEventListener('error', () => fin('error'), { once: true });
      const p = v.play();
      if (p && p.catch) p.catch((e) => { log('play-rejected', { msg: String(e && e.name) }); fin('error'); });
    });
  }

  /* ---------------- scenes ---------------- */
  const scenes = { A: $('#sceneA'), B: $('#sceneB'), C: $('#sceneC') };
  function show(name) { for (const k in scenes) scenes[k].classList.toggle('on', k === name); current = name; }

  // Each runner: prepare() -> runs under the curtain (builds video); play() -> after reveal, resolves when scene is done.
  const runners = {
    A: {
      async prepare() { return {}; },
      async play(_, stop) { await Promise.race([sleep(CFG.adur), stop.promise]); log('scene-end', { reason: 'timer' }); },
    },
    B: {
      async prepare() {
        const host = $('#videoHostB');
        const prog = $('#progressB'); prog.classList.remove('run'); prog.style.removeProperty('--vd');
        const v = makeVideo(host);
        v.onerror = () => log('video-error', { code: v.error && v.error.code });
        const r = await loadVideo(v, CFG.srcA);
        log('video-loaded', { why: r.why, dur: isFinite(v.duration) ? +v.duration.toFixed(2) : null });
        return { v, failed: !r.ok };
      },
      async play({ v, failed }, stop) {
        if (failed) { log('video-fallback'); await Promise.race([sleep(CFG.errorFallbackMs), stop.promise]); return; }
        const ended = new Promise((res) => { v.onended = () => { log('video-ended', { pos: +v.currentTime.toFixed(2) }); res('ended'); }; v.onerror = () => { log('video-error', { code: v.error && v.error.code }); res('error'); }; });
        const dur = isFinite(v.duration) ? v.duration : 10;
        const prog = $('#progressB'); prog.style.setProperty('--vd', dur + 's'); prog.classList.add('run');
        const started = startPlay(v);
        const watchdog = started.then((s) => (s === 'playing' ? new Promise(() => {}) : s)); // only stall/error resolve here
        const hard = sleep((dur + 4) * 1000).then(() => 'hard-timeout');
        const r = await Promise.race([ended, watchdog, hard, stop.promise]);
        if (r === 'error' || r === 'stall' || r === 'hard-timeout') { log('scene-end', { reason: r }); await sleep(r === 'error' ? CFG.errorFallbackMs : 0); }
        else log('scene-end', { reason: r });
      },
    },
    C: {
      async prepare() {
        const host = $('#cornerHost');
        const poster = $('#poster'); poster.classList.remove('show');
        const v = makeVideo(host, null, { before: poster });
        v.onerror = () => log('video-error', { code: v.error && v.error.code });
        const r = await loadVideo(v, CFG.srcB);
        log('video-loaded', { why: r.why, dur: isFinite(v.duration) ? +v.duration.toFixed(2) : null });
        return { v, poster, failed: !r.ok };
      },
      /* Choice: play the clip once (10 s), then fade a styled page-drawn poster over the 640x360 box
         and release the video (frees the hardware sink). The remaining ~10 s hold the poster.
         No looping => no black flash/decoder restart on the plane, and no opacity on the <video>. */
      async play({ v, poster, failed }, stop) {
        const total = sleep(CFG.cdur);
        const showPoster = async () => { poster.classList.add('show'); await sleep(700); killVideo(); log('poster-shown'); };
        const clip = (async () => {
          if (failed) { await showPoster(); return; }
          const ended = new Promise((res) => { v.onended = () => { log('video-ended', { pos: +v.currentTime.toFixed(2) }); res('ended'); }; v.onerror = () => res('error'); });
          const s = await Promise.race([startPlay(v), ended]);
          if (s === 'playing') await Promise.race([ended, sleep(((isFinite(v.duration) ? v.duration : 10) + 4) * 1000)]);
          await showPoster();
        })();
        await Promise.race([total, stop.promise]);
        log('scene-end', { reason: 'timer' });
        await Promise.race([clip, sleep(0)]);
      },
    },
  };

  async function runScene(name) {
    const R = runners[name];
    const stop = { promise: new Promise(() => {}) };
    const ctx = await R.prepare();
    return { ctx, stop, R };
  }

  async function loop() {
    let i = 0;
    show('A'); // behind the curtain
    for (;;) {
      const name = CFG.seq[i % CFG.seq.length]; i++;
      // 1. under the (already covering) curtain: switch scene, build video
      show(name);
      log('scene-start');
      const { ctx, stop, R } = await runScene(name);
      // 2. reveal and play
      const playing = R.play(ctx, stop);
      await reveal();
      log('revealed');
      await playing;
      // 3. cover again, then release resources
      await coverIn();
      killVideo();
    }
  }

  /* ---------------- freeze / single-scene QA mode ---------------- */
  async function staticScene(name) {
    curtain.style.display = 'none';
    show(name);
    if (name === 'B' || name === 'C') {
      const host = name === 'B' ? $('#videoHostB') : $('#cornerHost');
      const v = makeVideo(host, null, name === 'C' ? { before: $('#poster') } : {});
      await loadVideo(v, name === 'B' ? CFG.srcA : CFG.srcB);
      const at = parseFloat(Q.get('t'));
      if (isFinite(at)) { v.currentTime = at; await new Promise((r) => { v.onseeked = r; setTimeout(r, 1500); }); await startPlay(v).catch(() => {}); v.pause(); }
      else await startPlay(v);
      if (name === 'B' && isFinite(at)) { const p = $('#progressB'); p.style.transform = `scaleX(${(at / (v.duration || 10)).toFixed(3)})`; }
      if (Q.get('poster') === '1' && name === 'C') $('#poster').classList.add('show');
      document.title += ' [ready]';
      window.__ready = true;
    } else { window.__ready = true; }
  }

  /* ---------------- boot ---------------- */
  buildA(); buildC(); buildMarks();
  const boot = () => {
    if (/^[ABC]$/.test(only) && freeze) return staticScene(only);
    if (/^[ABC]$/.test(only)) { CFG.seq = [only]; }
    curtain.style.transform = ON; // start covered
    loop();
  };
  if (document.fonts && document.fonts.ready) document.fonts.ready.then(boot); else boot();
})();
