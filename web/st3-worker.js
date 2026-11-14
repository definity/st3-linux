// st3-worker.js — runs the emulator on its own thread, so drawing and other page work can never starve the audio.
// Talks to the page (keys in; frames, status and file lists out) and straight to the audio player (chunks out,
// queue reports in) over a MessagePort the page hands over once sound is on.
'use strict';

let audioPort = null, queued = 0, lastReport = 0, sentSinceReport = 0, underruns = 0, level = 0;
let running = false, lastFrameSent = 0;
const stats = { ticks: 0, maxGap: 0, maxEmu: 0, maxSync: 0, last: 0, emuTotal: 0, since: 0 };
const ROOT = '/st3';

function queuedSeconds() {
  if (!audioPort || !lastReport) return -1;
  return (queued + sentSinceReport) / 48000 - (performance.now() - lastReport) / 1000;
}

function pumpAudio() {
  const n = Module._st3_audio_take();
  if (!n || !audioPort) return;
  const p = Module._st3_audio_buf() >> 2, chunk = Module.HEAPF32.slice(p, p + n * 2);
  let e = 0; for (let i = 0; i < chunk.length; i += 8) e += chunk[i] * chunk[i];
  level = level * 0.9 + 0.1 * Math.sqrt(e / (chunk.length / 8));
  audioPort.postMessage(chunk, [chunk.buffer]);
  sentSinceReport += n;
}

// Frames go to the page as 8-bit pixels + palette in recycled buffers: the page sends each buffer back after
// drawing it, so steady playback allocates nothing (fresh buffers at 60 fps made the page's memory climb).
const framePool = [];
function sendFrame() {
  if (!Module._st3_render()) return;
  const w = Module._st3_width(), h = Module._st3_height(), n = w * h;
  let buf = framePool.pop();
  if (!buf || buf.byteLength !== n + 768) {
    if (inFlight >= 3) return;                    // page is behind: skip this frame rather than pile up buffers
    buf = new ArrayBuffer(n + 768); stats.frameAllocs = (stats.frameAllocs || 0) + 1;
  }
  const u8 = new Uint8Array(buf), p = Module._st3_pixels(), pal = Module._st3_palette();
  u8.set(Module.HEAPU8.subarray(p, p + n), 0);
  u8.set(Module.HEAPU8.subarray(pal, pal + 768), n);
  inFlight++; stats.framesSent = (stats.framesSent || 0) + 1;
  postMessage({ type: 'frame', w, h, buf }, [buf]);
}
let inFlight = 0;

function tick() {
  if (!running) return;
  const t = performance.now();
  if (stats.last) { const d = t - stats.last; if (d > stats.maxGap) stats.maxGap = d; }
  stats.last = t; stats.ticks++;
  const r = Module._st3_frame(t / 1000, queuedSeconds());
  const te = performance.now() - t; if (te > stats.maxEmu) stats.maxEmu = te; stats.emuTotal += te;
  pumpAudio();
  if (t - lastFrameSent >= 1000 / 60) { lastFrameSent = t; sendFrame(); }
  if (r === 2) { running = false; save(); postMessage({ type: 'status', text: 'ST3 has quit to DOS. Reload the page to start again.' }); return; }
  if (r < 0) { running = false; postMessage({ type: 'status', text: 'The emulator stopped (CPU error).' }); return; }
  setTimeout(tick, 4);
}

// ---------------------------------------------------------------- files: C:\ST3 lives in IndexedDB
function copyMissing(from, to) {
  if (!FS.analyzePath(to).exists) FS.mkdir(to);
  for (const name of FS.readdir(from)) {
    if (name === '.' || name === '..') continue;
    const a = from + '/' + name, b = to + '/' + name;
    if (FS.isDir(FS.stat(a).mode)) copyMissing(a, b);
    else if (!FS.analyzePath(b).exists) FS.writeFile(b, FS.readFile(a));
  }
}
let syncing = false;
function save() {
  if (syncing) return; syncing = true; const ts = performance.now();
  FS.syncfs(false, (err) => {
    syncing = false; const d = performance.now() - ts; if (d > stats.maxSync) stats.maxSync = d;
    if (err) console.warn('save failed', err);
  });
}
// ST3 only sees DOS 8.3 names (the emulator's DOS skips anything else), so uploaded files get one, Windows style:
// "My Cool Song (final).s3m" -> MYCOOL~1.S3M.  A name that already fits is kept (uppercased); the same name again
// replaces the old file, as a copy would on DOS.
function dosName(name, existing) {
  const clean = (t) => t.toUpperCase().replace(/[^A-Z0-9_\-!#$%&'()@^`{}~]/g, '');
  const dot = name.lastIndexOf('.');
  const base = clean(dot > 0 ? name.slice(0, dot) : name), ext = clean(dot > 0 ? name.slice(dot + 1) : '').slice(0, 3);
  const taken = new Set(existing.map((n) => n.toUpperCase()));
  const join = (b) => (ext ? b + '.' + ext : b);
  if (base.length > 0 && base.length <= 8 && base === (dot > 0 ? name.slice(0, dot) : name).toUpperCase()) return join(base);
  const stem = (base || 'SONG').slice(0, 6);
  for (let i = 1; i < 10000; i++) {
    const tail = '~' + i, cand = join(stem.slice(0, 8 - tail.length) + tail);
    if (!taken.has(cand)) return cand;
  }
  return join(stem);
}

function listFiles(dir, out) {
  for (const name of FS.readdir(dir)) {
    if (name === '.' || name === '..') continue;
    const p = dir + '/' + name;
    if (FS.isDir(FS.stat(p).mode)) listFiles(p, out); else out.push(p);
  }
  return out;
}

onmessage = (e) => {
  const m = e.data;
  if (m.type === 'key') { if (running) Module._st3_key(m.k); }
  else if (m.type === 'frameback') { inFlight--; if (framePool.length < 3) framePool.push(m.buf); }
  else if (m.type === 'shift') { if (running) Module._st3_shift(m.f); }
  else if (m.type === 'audioport') {
    audioPort = m.port;
    audioPort.onmessage = (ev) => { queued = ev.data.queued; underruns = ev.data.underruns; lastReport = performance.now(); sentSinceReport = 0; };
  } else if (m.type === 'upload') {
    const dir = ROOT + '/s';
    if (!FS.analyzePath(dir).exists) FS.mkdir(dir);
    const name = dosName(m.name, FS.readdir(dir));
    FS.writeFile(dir + '/' + name, new Uint8Array(m.data));
    save();
    postMessage({ type: 'status', text: 'Added ' + (name === m.name.toUpperCase() ? name : m.name + ' as ' + name) + ' to C:\\ST3\\S' });
  } else if (m.type === 'list') {
    postMessage({ type: 'list', files: listFiles(ROOT, []).sort() });
  } else if (m.type === 'get') {
    const data = FS.readFile(m.path);
    postMessage({ type: 'file', path: m.path, data }, [data.buffer]);
  } else if (m.type === 'stats') {
    postMessage({ type: 'stats', stats: { ...stats, busy: stats.emuTotal / (performance.now() - stats.since), vtime: running ? Module._st3_vtime() : 0, wall: performance.now() / 1000, mem: Module.HEAPU8.length, queued: queuedSeconds(), underruns, level } });
  } else if (m.type === 'resetstats') {
    stats.maxGap = stats.maxEmu = stats.maxSync = stats.emuTotal = 0; stats.ticks = 0; stats.since = performance.now();
  }
};

var Module = {
  print: (t) => console.log(t), printErr: (t) => console.warn(t),
  onRuntimeInitialized() {
    FS.mkdir(ROOT);
    FS.mount(IDBFS, {}, ROOT);
    FS.syncfs(true, (err) => {
      if (err) console.warn('could not load saved files', err);
      copyMissing('/dist', ROOT);
      save();
      if (Module._st3_init() !== 0) { postMessage({ type: 'status', text: 'Could not start ST3.' }); return; }
      running = true;
      postMessage({ type: 'status', text: '' });
      tick();
      setInterval(save, 5000);
    });
  },
};
importScripts('st3core.js');
