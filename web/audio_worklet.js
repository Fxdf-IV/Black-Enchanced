// Processador WebAudio (AudioWorklet) para saída estéreo 48 kHz de Black Enhanced Online.

const RING_FRAMES = 16384; // ~341 ms a 48 kHz

class BlackAudioWorkletProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.ring = new Float32Array(RING_FRAMES * 2);
    this.readFrame = 0;
    this.writeFrame = 0;
    this.availableFrames = 0;
    this.underruns = 0;

    this.port.onmessage = (ev) => {
      const msg = ev.data;
      if (!msg) return;
      if (msg.type === "reset") {
        this.readFrame = 0;
        this.writeFrame = 0;
        this.availableFrames = 0;
        return;
      }
      const samples = msg instanceof Float32Array ? msg : msg.samples;
      if (!samples || samples.length < 2) return;
      const frames = (samples.length / 2) | 0;
      for (let i = 0; i < frames; i++) {
        if (this.availableFrames >= RING_FRAMES) {
          this.readFrame = (this.readFrame + 1) % RING_FRAMES;
          this.availableFrames--;
        }
        const dst = this.writeFrame * 2;
        this.ring[dst] = samples[i * 2];
        this.ring[dst + 1] = samples[i * 2 + 1];
        this.writeFrame = (this.writeFrame + 1) % RING_FRAMES;
        this.availableFrames++;
      }
    };
  }

  process(_inputs, outputs) {
    const out = outputs[0];
    if (!out || out.length === 0) return true;
    const chL = out[0];
    const chR = out[1] || out[0];
    const n = chL.length;

    if (this.availableFrames === 0) {
      this.underruns++;
      chL.fill(0);
      if (chR !== chL) chR.fill(0);
      return true;
    }

    const toRead = Math.min(n, this.availableFrames);
    for (let i = 0; i < toRead; i++) {
      const src = this.readFrame * 2;
      chL[i] = this.ring[src];
      if (chR !== chL) chR[i] = this.ring[src + 1];
      this.readFrame = (this.readFrame + 1) % RING_FRAMES;
    }
    this.availableFrames -= toRead;

    for (let i = toRead; i < n; i++) {
      chL[i] = 0;
      if (chR !== chL) chR[i] = 0;
    }
    return true;
  }
}

registerProcessor("black-audio-processor", BlackAudioWorkletProcessor);
