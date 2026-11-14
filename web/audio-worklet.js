// audio-worklet.js — plays the emulator's mixed output (48 kHz stereo float, interleaved chunks).  The chunks arrive
// straight from the emulator thread over a MessagePort; the player reports how much it holds back over the same
// port, so the emulator can keep the queue near its target (the audio device is the clock).
class ST3Player extends AudioWorkletProcessor {
  constructor() {
    super();
    this.buf = new Float32Array(48000 * 2 * 2);   // 2 s ring
    this.r = 0; this.w = 0; this.n = 0;           // n = floats held
    this.lastL = 0; this.lastR = 0; this.blocks = 0; this.under = 0; this.started = false;
    this.feed = null;
    this.port.onmessage = (e) => {               // the page hands over the emulator's port once
      this.feed = e.data.port;
      this.feed.onmessage = (ev) => this.push(ev.data);
    };
  }
  push(d) {
    const len = this.buf.length;
    for (let i = 0; i < d.length && this.n < len; i++) { this.buf[this.w] = d[i]; this.w = (this.w + 1) % len; this.n++; }
  }
  process(inputs, outputs) {
    const out = outputs[0], L = out[0], R = out[1] || out[0], len = this.buf.length;
    for (let i = 0; i < L.length; i++) {
      if (this.n >= 2) {
        this.started = true;
        this.lastL = this.buf[this.r]; this.lastR = this.buf[(this.r + 1) % len];
        this.r = (this.r + 2) % len; this.n -= 2;
      } else { this.lastL *= 0.999; this.lastR *= 0.999; if (this.started) this.under++; }   // underrun: hold and decay, no click
      L[i] = this.lastL; R[i] = this.lastR;
    }
    if (this.feed && ++this.blocks % 4 === 0) this.feed.postMessage({ queued: this.n / 2, underruns: this.under });
    return true;
  }
}
registerProcessor('st3-player', ST3Player);
