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

function sendFrame() {
  if (!Module._st3_render()) return;
  const w = Module._st3_width(), h = Module._st3_height(), p = Module._st3_pixels();
  const px = Module.HEAPU8.slice(p, p + w * h * 4);
  postMessage({ type: 'frame', w, h, px }, [px.buffer]);
}

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
  else if (m.type === 'shift') { if (running) Module._st3_shift(m.f); }
  else if (m.type === 'audioport') {
    audioPort = m.port;
    audioPort.onmessage = (ev) => { queued = ev.data.queued; underruns = ev.data.underruns; lastReport = performance.now(); sentSinceReport = 0; };
  } else if (m.type === 'upload') {
    if (!FS.analyzePath(ROOT + '/s').exists) FS.mkdir(ROOT + '/s');
    FS.writeFile(ROOT + '/s/' + m.name, new Uint8Array(m.data));
    save();
    postMessage({ type: 'status', text: 'Added ' + m.name + ' to C:\\ST3\\S' });
  } else if (m.type === 'list') {
    postMessage({ type: 'list', files: listFiles(ROOT, []).sort() });
  } else if (m.type === 'get') {
    const data = FS.readFile(m.path);
    postMessage({ type: 'file', path: m.path, data }, [data.buffer]);
  } else if (m.type === 'stats') {
    postMessage({ type: 'stats', stats: { ...stats, busy: stats.emuTotal / (performance.now() - stats.since), queued: queuedSeconds(), underruns, level } });
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
