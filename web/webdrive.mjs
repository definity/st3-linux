// webdrive.mjs — drive the browser build in headless Chrome: real key events, screenshots, console log, audio state.
// usage: node webdrive.mjs URL OUTPREFIX "STEPS"
//   STEPS: space-separated tokens:  wait:MS  key:NAME (KeyboardEvent.code, e.g. KeyL, F5, ArrowDown, Enter, Space)
//          ctrl:NAME alt:NAME shift:NAME  type:TEXT  shot:NAME  eval:JS
import { spawn } from 'node:child_process';
import { writeFileSync, mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const [url, out, steps] = process.argv.slice(2);
const chrome = process.env.CHROME || 'google-chrome';
const prof = mkdtempSync(join(tmpdir(), 'st3web-'));
const port = 9300 + Math.floor(Math.random() * 500);
const proc = spawn(chrome, ['--headless=new', `--remote-debugging-port=${port}`, `--user-data-dir=${prof}`,
  '--autoplay-policy=no-user-gesture-required', '--window-size=1280,1000', '--no-first-run', 'about:blank'],
  { stdio: 'ignore', detached: true });
const cleanup = () => { try { process.kill(-proc.pid, 'SIGKILL'); } catch {} };
process.on('exit', cleanup); process.on('SIGTERM', () => process.exit(1)); process.on('SIGINT', () => process.exit(1));
setTimeout(() => { console.log('webdrive: overall timeout'); process.exit(1); }, +(process.env.WD_TIMEOUT || 200000));
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

let ws, seq = 0; const pending = new Map(), logs = [];
async function connect() {
  for (let i = 0; i < 50; i++) {
    try {
      const list = await (await fetch(`http://127.0.0.1:${port}/json`)).json();
      const page = list.find((t) => t.type === 'page');
      if (page) { ws = new WebSocket(page.webSocketDebuggerUrl); break; }
    } catch { /* not up yet */ }
    await sleep(200);
  }
  await new Promise((r) => ws.addEventListener('open', r));
  ws.addEventListener('message', (m) => {
    const d = JSON.parse(m.data);
    if (d.id && pending.has(d.id)) { pending.get(d.id)(d); pending.delete(d.id); }
    if (d.method === 'Runtime.consoleAPICalled') logs.push(`[${d.params.type}] ` + d.params.args.map((a) => a.value ?? a.description).join(' '));
    if (d.method === 'Page.javascriptDialogOpening') { logs.push('[dialog] ' + d.params.type); ws.send(JSON.stringify({ id: ++seq, method: 'Page.handleJavaScriptDialog', params: { accept: true } })); }
    if (d.method === 'Runtime.exceptionThrown') logs.push('[exception] ' + JSON.stringify(d.params.exceptionDetails.exception?.description || d.params.exceptionDetails.text));
  });
}
const send = (method, params = {}) => new Promise((r) => { const id = ++seq; pending.set(id, r); ws.send(JSON.stringify({ id, method, params })); });

const KEYS = { Space: [' ', 32], Enter: ['Enter', 13], Escape: ['Escape', 27], Tab: ['Tab', 9], Backspace: ['Backspace', 8],
  ArrowUp: ['ArrowUp', 38], ArrowDown: ['ArrowDown', 40], ArrowLeft: ['ArrowLeft', 37], ArrowRight: ['ArrowRight', 39],
  PageUp: ['PageUp', 33], PageDown: ['PageDown', 34], Home: ['Home', 36], End: ['End', 35], Delete: ['Delete', 46], Insert: ['Insert', 45] };
function keyInfo(code, shift) {
  if (KEYS[code]) return KEYS[code];
  let m;
  if ((m = /^F(\d+)$/.exec(code))) return [code, 111 + +m[1]];
  if ((m = /^Key([A-Z])$/.exec(code))) return [shift ? m[1] : m[1].toLowerCase(), m[1].charCodeAt(0)];
  if ((m = /^Digit(\d)$/.exec(code))) return [shift ? ')!@#$%^&*('[+m[1]] : m[1], 48 + +m[1]];
  return [code, 0];
}
async function press(code, mods = 0) {   // mods: 1 alt, 2 ctrl, 8 shift
  const [key, vk] = keyInfo(code, mods & 8);
  const text = key.length === 1 && !(mods & 3) ? key : '';
  await send('Input.dispatchKeyEvent', { type: text ? 'keyDown' : 'rawKeyDown', code, key, windowsVirtualKeyCode: vk, modifiers: mods, text });
  await send('Input.dispatchKeyEvent', { type: 'keyUp', code, key, windowsVirtualKeyCode: vk, modifiers: mods });
  await sleep(120);
}

await connect();
await send('Runtime.enable');
await send('Page.enable');
await send('Page.navigate', { url });
await sleep(1500);
for (const tok of steps.split(' ').filter(Boolean)) {
  const [cmd, ...rest] = tok.split(':'); const arg = rest.join(':');
  if (cmd === 'wait') await sleep(+arg);
  else if (cmd === 'key') await press(arg);
  else if (cmd === 'reload') { await send('Page.reload'); await sleep(1500); }
  else if (cmd === 'ctrl') await press(arg, 2);
  else if (cmd === 'alt') await press(arg, 1);
  else if (cmd === 'shift') await press(arg, 8);
  else if (cmd === 'type') for (const ch of arg) await press(/[0-9]/.test(ch) ? 'Digit' + ch : 'Key' + ch.toUpperCase());
  else if (cmd === 'shot') {
    const r = await send('Page.captureScreenshot', { format: 'png' });
    writeFileSync(`${out}_${arg}.png`, Buffer.from(r.result.data, 'base64'));
  } else if (cmd === 'eval') {
    const r = await send('Runtime.evaluate', { expression: arg.replace(/_/g, ' '), returnByValue: true, awaitPromise: true });
    logs.push(`[eval] ${arg} => ${JSON.stringify(r.result.result?.value ?? r.result.exceptionDetails?.text)}`);
  }
}
writeFileSync(`${out}.log`, logs.join('\n') + '\n');
console.log(logs.join('\n'));
ws.close(); process.exit(0);
