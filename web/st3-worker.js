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

// ---------------------------------------------------------------- backup and restore: the whole of C:\ST3 as a .zip
const CRC = new Uint32Array(256).map((_, n) => { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1; return c >>> 0; });
function crc32(d) { let c = ~0; for (let i = 0; i < d.length; i++) c = CRC[(c ^ d[i]) & 255] ^ (c >>> 8); return ~c >>> 0; }

// Stored (uncompressed) entries: any unzip tool reads them, and songs barely compress anyway.
function makeZip() {
  const enc = new TextEncoder(), parts = [], central = [];
  let off = 0;
  for (const p of listFiles(ROOT, []).sort()) {
    const name = enc.encode(p.slice(ROOT.length + 1)), data = FS.readFile(p), crc = crc32(data);
    const t = new Date(FS.stat(p).mtime);
    const time = t.getHours() << 11 | t.getMinutes() << 5 | t.getSeconds() >> 1;
    const date = Math.max(0, t.getFullYear() - 1980) << 9 | (t.getMonth() + 1) << 5 | t.getDate();
    const lh = new DataView(new ArrayBuffer(30));
    lh.setUint32(0, 0x04034b50, true); lh.setUint16(4, 20, true); lh.setUint16(6, 0x0800, true);
    lh.setUint16(10, time, true); lh.setUint16(12, date, true); lh.setUint32(14, crc, true);
    lh.setUint32(18, data.length, true); lh.setUint32(22, data.length, true); lh.setUint16(26, name.length, true);
    const ch = new DataView(new ArrayBuffer(46));
    ch.setUint32(0, 0x02014b50, true); ch.setUint16(4, 20, true); ch.setUint16(6, 20, true); ch.setUint16(8, 0x0800, true);
    ch.setUint16(12, time, true); ch.setUint16(14, date, true); ch.setUint32(16, crc, true);
    ch.setUint32(20, data.length, true); ch.setUint32(24, data.length, true); ch.setUint16(28, name.length, true);
    ch.setUint32(42, off, true);
    parts.push(lh.buffer, name, data); central.push(ch.buffer, name);
    off += 30 + name.length + data.length;
  }
  const csize = central.reduce((s, b) => s + b.byteLength, 0), n = central.length / 2;
  const end = new DataView(new ArrayBuffer(22));
  end.setUint32(0, 0x06054b50, true); end.setUint16(8, n, true); end.setUint16(10, n, true);
  end.setUint32(12, csize, true); end.setUint32(16, off, true);
  return { blob: new Blob([...parts, ...central, end.buffer], { type: 'application/zip' }), count: n };
}

// Reads stored and deflated entries (so a zip made by any tool works). Files from the zip replace files of the same
// name; everything else on the drive stays. A zip of the ST3 folder itself (ST3/S/..., ST3/ST3.SET) also works.
async function restoreZip(buf) {
  const u8 = new Uint8Array(buf), dv = new DataView(buf), dec = new TextDecoder();
  let e = u8.length - 22;
  while (e >= 0 && dv.getUint32(e, true) !== 0x06054b50) e--;
  if (e < 0) throw new Error('not a .zip file');
  const entries = [];
  for (let i = 0, p = dv.getUint32(e + 16, true), n = dv.getUint16(e + 10, true); i < n; i++) {
    if (dv.getUint32(p, true) !== 0x02014b50) throw new Error('damaged .zip file');
    const method = dv.getUint16(p + 10, true), csize = dv.getUint32(p + 20, true);
    const nlen = dv.getUint16(p + 28, true), xlen = dv.getUint16(p + 30, true), clen = dv.getUint16(p + 32, true);
    const lho = dv.getUint32(p + 42, true);
    const name = dec.decode(u8.subarray(p + 46, p + 46 + nlen));
    const parts = name.split(/[\\/]/).filter((s) => s && s !== '.' && s !== '..');
    p += 46 + nlen + xlen + clen;
    if (!parts.length || /[\\/]$/.test(name)) continue;   // folder entry
    const start = lho + 30 + dv.getUint16(lho + 26, true) + dv.getUint16(lho + 28, true);
    entries.push({ parts, method, raw: u8.subarray(start, start + csize) });
  }
  // a zip of the folder itself: drop the one top-level folder all entries share
  const top = entries.length && entries.every((x) => x.parts.length > 1 && x.parts[0] === entries[0].parts[0]) ? 1 : 0;
  let count = 0, skipped = 0;
  for (const x of entries) {
    let data;
    if (x.method === 0) data = x.raw;
    else if (x.method === 8) data = new Uint8Array(await new Response(new Blob([x.raw]).stream().pipeThrough(new DecompressionStream('deflate-raw'))).arrayBuffer());
    else { skipped++; continue; }
    // DOS names ignore case: S\ARMANI.S3M in the zip is the drive's s\ARMANI.S3M, not a second folder or file
    const same = (dir, n) => FS.readdir(dir).find((h) => h.toUpperCase() === n.toUpperCase()) || n;
    let dir = ROOT;
    for (const d of x.parts.slice(top, -1)) { dir += '/' + same(dir, d); if (!FS.analyzePath(dir).exists) FS.mkdir(dir); }
    FS.writeFile(dir + '/' + same(dir, x.parts[x.parts.length - 1]), data);
    count++;
  }
  return { count, skipped };
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
  } else if (m.type === 'backup') {
    const { blob, count } = makeZip();
    postMessage({ type: 'backup', blob, count });
  } else if (m.type === 'restore') {
    restoreZip(m.data).then(({ count, skipped }) => {
      save();
      postMessage({ type: 'status', text: 'Restored ' + count + ' file' + (count === 1 ? '' : 's') + ' to C:\\ST3' + (skipped ? ' (' + skipped + ' skipped: unsupported compression)' : '') });
    }, (err) => postMessage({ type: 'status', text: 'Could not restore ' + m.name + ': ' + err.message }));
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
