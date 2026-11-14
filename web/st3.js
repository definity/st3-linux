// st3.js — the page side of the browser version: filesystem, main loop, display, keyboard, audio, file transfer.
'use strict';

const $ = (id) => document.getElementById(id);
const status = (t) => { $('status').textContent = t; };

// ---------------------------------------------------------------- keyboard: browser key -> PC BIOS key code
// Scan codes (set 1) by KeyboardEvent.code, as on a US 101-key keyboard.  The ASCII part comes from the browser
// (event.key), so other keyboard layouts type the right characters.
const SCAN = {
  Escape: 0x01, Digit1: 0x02, Digit2: 0x03, Digit3: 0x04, Digit4: 0x05, Digit5: 0x06, Digit6: 0x07, Digit7: 0x08,
  Digit8: 0x09, Digit9: 0x0A, Digit0: 0x0B, Minus: 0x0C, Equal: 0x0D, Backspace: 0x0E, Tab: 0x0F,
  KeyQ: 0x10, KeyW: 0x11, KeyE: 0x12, KeyR: 0x13, KeyT: 0x14, KeyY: 0x15, KeyU: 0x16, KeyI: 0x17, KeyO: 0x18,
  KeyP: 0x19, BracketLeft: 0x1A, BracketRight: 0x1B, Enter: 0x1C, KeyA: 0x1E, KeyS: 0x1F, KeyD: 0x20, KeyF: 0x21,
  KeyG: 0x22, KeyH: 0x23, KeyJ: 0x24, KeyK: 0x25, KeyL: 0x26, Semicolon: 0x27, Quote: 0x28, Backquote: 0x29,
  Backslash: 0x2B, KeyZ: 0x2C, KeyX: 0x2D, KeyC: 0x2E, KeyV: 0x2F, KeyB: 0x30, KeyN: 0x31, KeyM: 0x32,
  Comma: 0x33, Period: 0x34, Slash: 0x35, NumpadMultiply: 0x37, Space: 0x39,
  Numpad7: 0x47, Numpad8: 0x48, Numpad9: 0x49, NumpadSubtract: 0x4A, Numpad4: 0x4B, Numpad5: 0x4C, Numpad6: 0x4D,
  NumpadAdd: 0x4E, Numpad1: 0x4F, Numpad2: 0x50, Numpad3: 0x51, Numpad0: 0x52, NumpadDecimal: 0x53,
  IntlBackslash: 0x56, NumpadEnter: 0x1C, NumpadDivide: 0x35,
};
// navigation keys: [plain, ctrl, alt]
const NAV = {
  Home: [0x4700, 0x7700, 0x9700], ArrowUp: [0x4800, 0x8D00, 0x9800], PageUp: [0x4900, 0x8400, 0x9900],
  ArrowLeft: [0x4B00, 0x7300, 0x9B00], ArrowRight: [0x4D00, 0x7400, 0x9D00], End: [0x4F00, 0x7500, 0x9F00],
  ArrowDown: [0x5000, 0x9100, 0xA000], PageDown: [0x5100, 0x7600, 0xA100], Insert: [0x5200, 0x9200, 0xA200],
  Delete: [0x5300, 0x9300, 0xA300],
};
const NUMPAD_NAV = { Numpad7: 'Home', Numpad8: 'ArrowUp', Numpad9: 'PageUp', Numpad4: 'ArrowLeft', Numpad6: 'ArrowRight',
  Numpad1: 'End', Numpad2: 'ArrowDown', Numpad3: 'PageDown', Numpad0: 'Insert', NumpadDecimal: 'Delete' };

function fnKey(n, mod) {            // F1..F12; mod 0 none, 1 shift, 2 ctrl, 3 alt
  if (n <= 10) return ([0x3B, 0x54, 0x5E, 0x68][mod] + n - 1) << 8;
  return (n === 11 ? [0x85, 0x87, 0x89, 0x8B] : [0x86, 0x88, 0x8A, 0x8C])[mod] << 8;
}

function biosKey(e) {
  const c = e.code, ctrl = e.ctrlKey, alt = e.altKey, shift = e.shiftKey;
  const fm = /^F(\d+)$/.exec(e.key) || /^F(\d+)$/.exec(c);
  if (fm && +fm[1] >= 1 && +fm[1] <= 12) return fnKey(+fm[1], alt ? 3 : ctrl ? 2 : shift ? 1 : 0);
  let nav = NAV[c] ? c : (NUMPAD_NAV[c] && !/^[0-9.,]$/.test(e.key) ? NUMPAD_NAV[c] : null);
  if (nav) return NAV[nav][alt ? 2 : ctrl ? 1 : 0];
  if (c === 'Numpad5' && !/^[0-9]$/.test(e.key)) return 0x4C00;
  switch (c) {
    case 'Escape': return alt ? 0x0100 : 0x011B;
    case 'Enter': case 'NumpadEnter': return alt ? 0x1C00 : ctrl ? 0x1C0A : 0x1C0D;
    case 'Backspace': return alt ? 0x0E00 : ctrl ? 0x0E7F : 0x0E08;
    case 'Tab': return alt ? 0xA500 : ctrl ? 0x9400 : shift ? 0x0F00 : 0x0F09;
    case 'Space': return 0x3920;
    case 'NumpadAdd': return alt ? 0x4E00 : ctrl ? 0x9000 : 0x4E2B;
    case 'NumpadSubtract': return alt ? 0x4A00 : ctrl ? 0x8E00 : 0x4A2D;
    case 'NumpadMultiply': return alt ? 0x3700 : ctrl ? 0x9600 : 0x372A;
    case 'NumpadDivide': return alt ? 0xA400 : ctrl ? 0x9500 : 0x352F;
  }
  const sc = SCAN[c];
  if (sc === undefined) return 0;
  if (alt) {                                            // Alt+key: scan code only (digits and -= have their own)
    if (sc >= 0x02 && sc <= 0x0D) return (0x78 + sc - 0x02) << 8;
    return sc << 8;
  }
  if (ctrl) {
    if (e.key.length === 1) {
      const k = e.key.toUpperCase().charCodeAt(0);
      if (k >= 0x40 && k <= 0x5F) return (sc << 8) | (k - 0x40);   // Ctrl+letter, Ctrl+[ \ ] ^ _ @
      if (c === 'Minus') return 0x0C1F;
      if (c === 'Digit6') return 0x071E;
      if (c === 'Digit2') return 0x0300;
    }
    return 0;
  }
  if (e.key.length !== 1) return 0;
  const a = e.key.charCodeAt(0);
  return a < 128 ? (sc << 8) | a : 0;
}

// live modifier state for BIOS INT 16h/02,12 (ST3 reads it for Alt and Shift combinations)
const held = new Set();
function shiftFlags(e) {
  let f = 0;
  if (held.has('ShiftRight')) f |= 0x01;
  if (held.has('ShiftLeft')) f |= 0x02;
  if (e.ctrlKey || held.has('ControlLeft') || held.has('ControlRight')) f |= 0x04;
  if (e.altKey || held.has('AltLeft') || held.has('AltRight')) f |= 0x08;
  if (e.getModifierState && e.getModifierState('NumLock')) f |= 0x20;
  if (e.getModifierState && e.getModifierState('CapsLock')) f |= 0x40;
  if (held.has('ControlLeft')) f |= 0x0100;
  if (held.has('AltLeft')) f |= 0x0200;
  if (held.has('ControlRight')) f |= 0x0400;
  if (held.has('AltRight')) f |= 0x0800;
  return f;
}

// Ctrl-F prefix, as in the terminal version: Ctrl-F then 1..0 = F1..F10, Shift+1..0 = Shift-F1..F10, Ctrl-F = Ctrl-F
let fnPrefix = 0;
const DIGITS = ['Digit1', 'Digit2', 'Digit3', 'Digit4', 'Digit5', 'Digit6', 'Digit7', 'Digit8', 'Digit9', 'Digit0'];

const worker = new Worker('st3-worker.js');
const toWorker = (m, t) => worker.postMessage(m, t || []);
let running = false;

function onKeyDown(e) {
  if (!running) return;
  startAudio();
  held.add(e.code);
  toWorker({ type: 'shift', f: shiftFlags(e) });
  if (/^(Shift|Control|Alt|Meta|CapsLock|NumLock|ScrollLock|OS)/.test(e.code) || e.key === 'Meta') { e.preventDefault(); return; }
  if (e.metaKey) return;                                 // leave the system's own shortcuts alone
  e.preventDefault();
  const key = (k) => { if (k) toWorker({ type: 'key', k }); };
  if (fnPrefix && performance.now() - fnPrefix > 3000) fnPrefix = 0;
  if (fnPrefix) {
    fnPrefix = 0;
    const d = DIGITS.indexOf(e.code);
    if (d >= 0 && !e.ctrlKey && !e.altKey) { key(fnKey(d + 1, e.shiftKey ? 1 : 0)); return; }
    if (e.code === 'KeyF' && e.ctrlKey && !e.altKey) { key(0x2106); return; }
  } else if (e.code === 'KeyF' && e.ctrlKey && !e.altKey && !e.shiftKey) { fnPrefix = performance.now(); return; }
  key(biosKey(e));
}
function onKeyUp(e) {
  held.delete(e.code);
  if (running) toWorker({ type: 'shift', f: shiftFlags(e) });
  e.preventDefault();
}

// ---------------------------------------------------------------- audio: the player is fed directly by the worker
let actx = null, node = null;
async function startAudio() {
  if (actx) { if (actx.state === 'suspended') actx.resume(); return; }
  try {
    actx = new AudioContext({ sampleRate: 48000, latencyHint: 'interactive' });
    await actx.audioWorklet.addModule('audio-worklet.js');
    node = new AudioWorkletNode(actx, 'st3-player', { outputChannelCount: [2] });
    node.connect(actx.destination);
    const ch = new MessageChannel();
    node.port.postMessage({ port: ch.port1 }, [ch.port1]);
    toWorker({ type: 'audioport', port: ch.port2 }, [ch.port2]);
    $('hint').hidden = true;
  } catch (err) { console.error(err); actx = null; status('No sound: ' + err.message); }
}

// ---------------------------------------------------------------- display
// ST3's 640x400 picture is shown at 4:3, as on a VGA monitor: scaled up by a whole number with sharp pixels,
// then smoothly to the final size, so text stays crisp without uneven rows.  Frames from the worker are drawn on
// the next animation frame (only the newest one).
const screen = $('screen'), sctx = screen.getContext('2d');
const src = document.createElement('canvas'), srcCtx = src.getContext('2d');
const mid = document.createElement('canvas'), midCtx = mid.getContext('2d');
let img = null, pending = null;
function layout() {
  const dpr = window.devicePixelRatio || 1, bar = $('bar').offsetHeight;
  const W = window.innerWidth, H = window.innerHeight - bar - 8;
  let w = W, h = Math.round(W * 3 / 4);
  if (h > H) { h = H; w = Math.round(H * 4 / 3); }
  screen.style.width = w + 'px'; screen.style.height = h + 'px';
  screen.width = Math.round(w * dpr); screen.height = Math.round(h * dpr);
  draw();
}
function draw() {
  if (!img) return;
  const k = Math.max(1, Math.ceil(screen.height / img.height));
  if (mid.width !== img.width * k || mid.height !== img.height * k) { mid.width = img.width * k; mid.height = img.height * k; }
  midCtx.imageSmoothingEnabled = false; midCtx.drawImage(src, 0, 0, mid.width, mid.height);
  sctx.imageSmoothingEnabled = true; sctx.imageSmoothingQuality = 'high';
  sctx.drawImage(mid, 0, 0, screen.width, screen.height);
}
function paint() {
  if (pending) {
    const { w, h, px } = pending; pending = null;
    if (!img || img.width !== w || img.height !== h) { img = new ImageData(w, h); src.width = w; src.height = h; }
    img.data.set(px);
    srcCtx.putImageData(img, 0, 0);
    draw();
  }
  requestAnimationFrame(paint);
}

// ---------------------------------------------------------------- files (C:\ST3 lives in the worker's IndexedDB store)
function upload(files) {
  for (const f of files) f.arrayBuffer().then((data) => toWorker({ type: 'upload', name: f.name, data }, [data]));
}
function showFiles() { toWorker({ type: 'list' }); }
function fileList(files) {
  const list = $('filelist');
  list.replaceChildren();
  for (const p of files) {
    const li = document.createElement('li'), a = document.createElement('a');
    a.textContent = 'C:\\ST3' + p.slice(4).replace(/\//g, '\\');
    a.href = '#';
    a.onclick = (ev) => { ev.preventDefault(); toWorker({ type: 'get', path: p }); };
    li.appendChild(a); list.appendChild(li);
  }
  $('files').hidden = false;
}
function download(path, data) {
  const url = URL.createObjectURL(new Blob([data]));
  const dl = document.createElement('a'); dl.href = url; dl.download = path.split('/').pop(); dl.click();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}

// for tests (webdrive.mjs): latest emulator statistics
let stats = null;
function getStats() { toWorker({ type: 'stats' }); return new Promise((r) => { statsWaiter = r; }); }
let statsWaiter = null;

worker.onmessage = (e) => {
  const m = e.data;
  if (m.type === 'frame') { pending = m; if (!running) { running = true; layout(); } }
  else if (m.type === 'status') { status(m.text); if (m.text && /quit|stopped|Could not/.test(m.text)) running = false; }
  else if (m.type === 'list') fileList(m.files);
  else if (m.type === 'file') download(m.path, m.data);
  else if (m.type === 'stats') { stats = m.stats; if (statsWaiter) { statsWaiter(stats); statsWaiter = null; } }
};
worker.onerror = (e) => { status('The emulator failed to start: ' + e.message); };

window.addEventListener('keydown', onKeyDown, true);
window.addEventListener('keyup', onKeyUp, true);
window.addEventListener('blur', () => { held.clear(); if (running) toWorker({ type: 'shift', f: 0 }); });
window.addEventListener('resize', layout);
screen.addEventListener('mousedown', () => startAudio());
$('hint').addEventListener('click', () => startAudio());
$('upload').addEventListener('change', (e) => { upload(e.target.files); e.target.value = ''; screen.focus(); });
$('filesbtn').addEventListener('click', showFiles);
$('closefiles').addEventListener('click', () => { $('files').hidden = true; });
$('fullbtn').addEventListener('click', async () => {
  startAudio();
  try {
    await document.documentElement.requestFullscreen();
    if (navigator.keyboard && navigator.keyboard.lock) await navigator.keyboard.lock();   // lets ST3 have Ctrl-W/T/N
  } catch (err) { console.warn(err); }
});
document.addEventListener('fullscreenchange', () => setTimeout(layout, 50));
// Chrome won't let a page block Ctrl-W (close tab) outside full screen, and ST3 uses Ctrl-W for "Save As":
// ask before leaving, so a stray Ctrl-W can't throw away an unsaved song.
window.addEventListener('beforeunload', (e) => { if (running) { e.preventDefault(); e.returnValue = ''; } });
window.addEventListener('dragover', (e) => e.preventDefault());
window.addEventListener('drop', (e) => { e.preventDefault(); upload(e.dataTransfer.files); });
layout();
requestAnimationFrame(paint);
