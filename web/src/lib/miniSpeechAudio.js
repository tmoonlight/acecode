import { encodeSpeechWav } from './speechAudio.js';
import { speechAudioConstraints } from './speechDevices.js';

// MediaRecorder can capture in the background renderer without an autoplay
// gesture. Offline decoding produces the same PCM WAV as composer dictation.
export async function captureMiniSpeechAudio({ signal, maxSeconds, deviceId, onLimit, onError }, env = globalThis) {
  const OfflineContext = env.OfflineAudioContext || env.webkitOfflineAudioContext;
  if (!env.navigator?.mediaDevices?.getUserMedia || !env.MediaRecorder || !OfflineContext)
    throw Object.assign(new Error('Microphone unavailable'), { code: 'SPEECH_UNSUPPORTED' });
  let stream, recorder, timer, stopping, rejectStop;
  let canceled = false, chunks = [];
  const abortError = () => new DOMException('Canceled', 'AbortError');
  const release = () => {
    env.clearTimeout(timer);
    for (const track of stream?.getTracks() || []) track.stop();
  };
  const cancel = () => {
    canceled = true; chunks = [];
    if (recorder?.state !== 'inactive') { try { recorder?.stop(); } catch {} }
    release(); rejectStop?.(abortError()); signal.removeEventListener('abort', cancel);
  };
  signal.addEventListener('abort', cancel, { once: true });
  try {
    if (signal.aborted) throw abortError();
    stream = await env.navigator.mediaDevices.getUserMedia(speechAudioConstraints(deviceId));
    if (canceled || signal.aborted) { release(); throw abortError(); }
    const mimeType = ['audio/webm;codecs=opus', 'audio/mp4', 'audio/webm']
      .find(type => env.MediaRecorder.isTypeSupported(type));
    recorder = new env.MediaRecorder(stream, mimeType ? { mimeType } : undefined);
    recorder.ondataavailable = event => { if (!canceled && event.data.size) chunks.push(event.data); };
    recorder.onerror = event => { if (!canceled) { onError?.(event.error || { code: 'SPEECH_UNSUPPORTED' }); cancel(); } };
    for (const track of stream.getAudioTracks()) track.addEventListener('ended', () => {
      if (!canceled && !stopping) { onError?.({ code: 'SPEECH_DEVICE_ENDED' }); cancel(); }
    }, { once: true });
    recorder.start(250);
    timer = env.setTimeout(() => { if (!canceled) onLimit?.(); }, maxSeconds * 1000);
    return {
      stop() {
        if (stopping) return stopping;
        if (canceled) return Promise.reject(abortError());
        stopping = new Promise((resolve, reject) => {
          rejectStop = reject;
          recorder.onstop = async () => {
            release();
            try {
              if (canceled) throw abortError();
              const encoded = new Blob(chunks, { type: recorder.mimeType }); chunks = [];
              if (!encoded.size) throw { code: 'SPEECH_NO_SPEECH' };
              const decoder = new OfflineContext(1, 1, 16000);
              const decoded = await decoder.decodeAudioData(await encoded.arrayBuffer());
              if (canceled || signal.aborted) throw abortError();
              const mono = new Float32Array(decoded.length);
              for (let channel = 0; channel < decoded.numberOfChannels; channel++) {
                const samples = decoded.getChannelData(channel);
                for (let i = 0; i < mono.length; i++) mono[i] += samples[i] / decoded.numberOfChannels;
              }
              resolve(encodeSpeechWav([mono], decoded.sampleRate, maxSeconds));
            } catch (error) { reject(error); }
            finally { signal.removeEventListener('abort', cancel); }
          };
          env.clearTimeout(timer);
          recorder.stop();
        });
        return stopping;
      },
      cancel,
    };
  } catch (error) { cancel(); throw error; }
}
