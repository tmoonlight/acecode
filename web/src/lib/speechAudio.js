import { speechAudioConstraints } from './speechDevices.js';

// WAV is accepted by all supported services, without a bundled decoder.
export function encodeSpeechWav(chunks, sampleRate, maxSeconds = 60) {
  if (!Number.isFinite(sampleRate) || sampleRate <= 0) throw new Error('Invalid sample rate');
  const total = Math.min(chunks.reduce((sum, chunk) => sum + chunk.length, 0), Math.floor(sampleRate * maxSeconds));
  const input = new Float32Array(total);
  let offset = 0;
  for (const chunk of chunks) {
    const count = Math.min(chunk.length, total - offset);
    if (count <= 0) break;
    input.set(chunk.subarray(0, count), offset); offset += count;
  }
  const samples = Math.floor(total * 16000 / sampleRate);
  if (!samples) throw Object.assign(new Error('Empty recording'), { code: 'SPEECH_NO_SPEECH' });
  const buffer = new ArrayBuffer(44 + samples * 2);
  const data = new DataView(buffer);
  const text = (at, value) => { [...value].forEach((letter, i) => data.setUint8(at + i, letter.charCodeAt(0))); };
  text(0, 'RIFF'); data.setUint32(4, buffer.byteLength - 8, true);
  text(8, 'WAVE'); text(12, 'fmt ');
  data.setUint32(16, 16, true); data.setUint16(20, 1, true); data.setUint16(22, 1, true);
  data.setUint32(24, 16000, true); data.setUint32(28, 32000, true);
  data.setUint16(32, 2, true); data.setUint16(34, 16, true);
  text(36, 'data'); data.setUint32(40, samples * 2, true);
  const ratio = sampleRate / 16000;
  for (let i = 0; i < samples; i += 1) {
    const begin = Math.floor(i * ratio);
    const end = Math.min(total, Math.max(begin + 1, Math.floor((i + 1) * ratio)));
    let value = 0;
    for (let j = begin; j < end; j += 1) value += input[j];
    value = Math.max(-1, Math.min(1, value / (end - begin)));
    data.setInt16(44 + i * 2, Math.round(value < 0 ? value * 32768 : value * 32767), true);
  }
  return new Blob([buffer], { type: 'audio/wav' });
}

export async function captureSpeechAudio({ signal, maxSeconds, deviceId, onLevel, onLimit, onError }, environment = globalThis) {
  if (environment.isSecureContext === false)
    throw Object.assign(new Error('Microphone requires a secure page'), { code: 'SPEECH_INSECURE_CONTEXT' });
  const Context = environment.AudioContext || environment.webkitAudioContext;
  if (!environment.navigator?.mediaDevices?.getUserMedia || !Context)
    throw Object.assign(new Error('Microphone is unavailable'), { code: 'SPEECH_UNSUPPORTED' });
  let context = null, stream = null, source = null, processor = null, mute = null, timer = null;
  let closed = false, total = 0;
  let chunks = [];
  const cleanup = () => {
    if (closed) return;
    closed = true;
    if (timer != null) environment.clearTimeout(timer);
    signal.removeEventListener('abort', cleanup);
    if (processor) processor.onaudioprocess = null;
    for (const node of [source, processor, mute]) { try { node?.disconnect(); } catch {} }
    for (const track of stream?.getTracks() || []) track.stop();
    if (context) Promise.resolve(context.close()).catch(() => {});
  };
  signal.addEventListener('abort', cleanup, { once: true });
  try {
    if (signal.aborted) throw new DOMException('Cancelled', 'AbortError');
    context = new Context();
    // Resume in the user gesture before waiting on the permission prompt.
    const resumed = context.resume().then(() => null, (error) => error);
    stream = await environment.navigator.mediaDevices.getUserMedia(speechAudioConstraints(deviceId));
    if (closed || signal.aborted) {
      stream.getTracks().forEach((track) => track.stop());
      throw new DOMException('Cancelled', 'AbortError');
    }
    const resumeError = await resumed;
    if (resumeError) throw resumeError;
    if (closed || signal.aborted) throw new DOMException('Cancelled', 'AbortError');
    source = context.createMediaStreamSource(stream);
    processor = context.createScriptProcessor(2048, 1, 1);
    mute = context.createGain(); mute.gain.value = 0;
    processor.onaudioprocess = (event) => {
      if (closed) return;
      const incoming = event.inputBuffer.getChannelData(0);
      const count = Math.min(incoming.length, Math.floor(context.sampleRate * maxSeconds) - total);
      if (count <= 0) return;
      const chunk = new Float32Array(incoming.subarray(0, count));
      chunks.push(chunk); total += count;
      const energy = Math.sqrt(chunk.reduce((sum, sample) => sum + sample * sample, 0) / count);
      onLevel?.(Math.min(1, energy * 4), total / context.sampleRate);
      if (total >= Math.floor(context.sampleRate * maxSeconds)) onLimit?.();
    };
    for (const track of stream.getAudioTracks()) track.addEventListener('ended', () => {
      if (!closed) onError?.({ code: 'SPEECH_DEVICE_ENDED' });
    }, { once: true });
    source.connect(processor); processor.connect(mute); mute.connect(context.destination);
    timer = environment.setTimeout(() => { if (!closed) onLimit?.(); }, maxSeconds * 1000);
    return {
      stop() {
        cleanup();
        try { return encodeSpeechWav(chunks, context.sampleRate, maxSeconds); }
        finally { chunks = []; }
      },
      cancel() { cleanup(); chunks = []; },
    };
  } catch (error) {
    cleanup();
    throw error;
  }
}
