// Theme sound effects. A package declares `"sounds": {"style": "hud"}` and
// every event gets a sound synthesized here with Web Audio. `sounds.files`
// replaces some of them with recordings shipped in the package (Ogg Opus,
// served by the daemon); a recording that cannot be decoded falls back to the
// synthesized sound.
//
// Nothing plays unless the active theme declares a known style and the
// title-bar mute button is off: components/ThemeSoundLayer.jsx configures this
// module, call sites only call playThemeSound(name).

import { THEME_SOUND_NAMES, THEME_SOUND_STYLES } from './themePackages.js';

export { THEME_SOUND_NAMES };

// Voice helpers. Times are seconds from the sound's start, gains are peak
// amplitudes; each recipe's `level` scales its whole bus.
const tone = (props) => ({ kind: 'tone', at: 0, attack: 0.002, wave: 'square', ...props });
const noise = (props) => ({ kind: 'noise', at: 0, attack: 0.001, ...props });
const gated = (voice, spans) => spans.map(([at, dur]) => ({ ...voice, at: voice.at + at, dur }));

// Cyberpunk HUD set: short digital ticks for routine clicks, chirps and short
// arpeggios for the agent's turn, a glitch burst for the variant switch.
const HUD_SOUNDS = {
  click: { level: 1.15, voices: [
    tone({ from: 2600, to: 1700, dur: 0.026, gain: 0.16, filter: { type: 'lowpass', freq: 6500 } }),
    noise({ dur: 0.012, gain: 0.1, filter: { type: 'highpass', freq: 4500 } }),
  ] },
  select: { level: 0.42, voices: [
    tone({ wave: 'triangle', from: 1480, dur: 0.04, gain: 0.26 }),
    tone({ wave: 'triangle', at: 0.045, from: 2220, dur: 0.055, gain: 0.22 }),
    tone({ at: 0.045, from: 4440, dur: 0.016, gain: 0.04 }),
  ] },
  toggle: { level: 1.45, voices: [
    noise({ dur: 0.03, gain: 0.24, filter: { type: 'bandpass', freq: 2600, q: 2.2 } }),
    tone({ from: 900, to: 620, dur: 0.035, gain: 0.12, filter: { type: 'lowpass', freq: 3000 } }),
  ] },
  send: { level: 0.71, voices: [
    tone({ wave: 'sawtooth', from: 420, to: 1800, dur: 0.12, gain: 0.13, attack: 0.006,
      filter: { type: 'lowpass', freq: 3600, q: 3 } }),
    tone({ wave: 'sine', at: 0.1, from: 1800, to: 2400, dur: 0.08, gain: 0.2 }),
    noise({ dur: 0.14, gain: 0.05, attack: 0.03, filter: { type: 'bandpass', freq: 6000, q: 0.9 } }),
    tone({ at: 0.13, from: 3200, dur: 0.018, gain: 0.07 }),
    tone({ at: 0.16, from: 3800, dur: 0.018, gain: 0.06 }),
  ] },
  queue: { level: 0.75, voices: [
    tone({ wave: 'triangle', from: 1200, dur: 0.05, gain: 0.22 }),
    tone({ wave: 'triangle', at: 0.06, from: 900, dur: 0.07, gain: 0.2 }),
  ] },
  stop: { level: 1.42, voices: [
    tone({ wave: 'sawtooth', from: 900, to: 180, dur: 0.18, gain: 0.3, filter: { type: 'lowpass', freq: 2200 } }),
    noise({ dur: 0.1, gain: 0.2, filter: { type: 'lowpass', freq: 1400 } }),
  ] },
  complete: { level: 0.65, voices: [
    tone({ wave: 'triangle', from: 880, dur: 0.09, gain: 0.2 }),
    tone({ wave: 'triangle', at: 0.07, from: 1320, dur: 0.09, gain: 0.2 }),
    tone({ wave: 'triangle', at: 0.14, from: 1760, dur: 0.18, gain: 0.22 }),
    tone({ wave: 'sine', at: 0.14, from: 3520, dur: 0.12, gain: 0.04 }),
    noise({ at: 0.14, dur: 0.08, gain: 0.03, filter: { type: 'highpass', freq: 7000 } }),
  ] },
  alert: { level: 1.03, voices: [
    ...[0, 0.2].flatMap((at) => [
      tone({ at, from: 1046, dur: 0.07, gain: 0.13, filter: { type: 'lowpass', freq: 3600 } }),
      tone({ at: at + 0.09, from: 784, dur: 0.08, gain: 0.13, filter: { type: 'lowpass', freq: 3600 } }),
    ]),
  ] },
  error: { level: 1.03, voices: [
    tone({ wave: 'sawtooth', from: 160, to: 120, dur: 0.22, gain: 0.3, filter: { type: 'lowpass', freq: 900 } }),
    tone({ from: 166, to: 124, dur: 0.22, gain: 0.14, filter: { type: 'lowpass', freq: 900 } }),
    ...gated(noise({ gain: 0.24, filter: { type: 'bandpass', freq: 800, q: 1.4 } }),
      [[0, 0.03], [0.06, 0.03], [0.12, 0.04]]),
  ] },
  boot: { level: 0.78, voices: [
    tone({ wave: 'sine', from: 55, to: 110, dur: 1.1, gain: 0.22, attack: 0.15 }),
    tone({ wave: 'sawtooth', from: 180, to: 2400, dur: 0.7, gain: 0.07, attack: 0.05,
      filter: { type: 'lowpass', freq: 4000, q: 2 } }),
    noise({ dur: 0.6, gain: 0.04, attack: 0.2, filter: { type: 'highpass', freq: 3000 } }),
    tone({ at: 0.55, from: 2000, dur: 0.03, gain: 0.07 }),
    tone({ at: 0.62, from: 2600, dur: 0.03, gain: 0.07 }),
    tone({ at: 0.69, from: 3200, dur: 0.03, gain: 0.07 }),
    tone({ wave: 'triangle', at: 0.85, from: 880, dur: 0.38, gain: 0.16, attack: 0.008 }),
    tone({ wave: 'triangle', at: 0.85, from: 1320, dur: 0.38, gain: 0.12, attack: 0.008 }),
  ] },
  switch: { level: 1.17, voices: [
    ...gated(noise({ gain: 0.2, filter: { type: 'bandpass', freq: 1800, q: 0.8 } }),
      [[0, 0.05], [0.08, 0.03], [0.14, 0.06], [0.24, 0.02], [0.3, 0.05]]),
    tone({ from: 1600, to: 300, dur: 0.25, gain: 0.08, filter: { type: 'lowpass', freq: 3000 } }),
    tone({ wave: 'sawtooth', at: 0.35, from: 300, to: 1400, dur: 0.25, gain: 0.08,
      filter: { type: 'lowpass', freq: 3000 } }),
    tone({ wave: 'triangle', at: 0.62, from: 1320, dur: 0.2, gain: 0.16 }),
    tone({ wave: 'triangle', at: 0.7, from: 1760, dur: 0.25, gain: 0.12 }),
  ] },
};

export const THEME_SOUND_RECIPES = Object.freeze({ hud: HUD_SOUNDS });

export function themeSoundDuration(recipe) {
  return (recipe?.voices || []).reduce((end, voice) => Math.max(end, (voice.at || 0) + voice.dur), 0);
}

// Routine clicks may repeat quickly; the long sounds would pile up into noise.
const MIN_GAP_MS = { click: 40, select: 50, toggle: 60, boot: 1200, switch: 700 };
const DEFAULT_GAP_MS = 150;

export function createThemeSoundThrottle(now = () => Date.now()) {
  const last = new Map();
  return {
    allow(name) {
      const at = now();
      const previous = last.get(name);
      if (previous !== undefined && at - previous < (MIN_GAP_MS[name] ?? DEFAULT_GAP_MS)) return false;
      last.set(name, at);
      return true;
    },
  };
}

// Which sound a pointer press on `target` should make. The composer's send
// button, variant pickers and the mute button play their own sounds.
const EXCLUDED_TARGETS = '[data-theme-sound="off"], .ace-composer-send, [data-theme-variant-option], [data-theme-sound-toggle]';
const SELECT_TARGETS = '.ace-sidebar-session-row, .ace-conversation-turn-scrubber-hit, [role="tab"], [role="option"], [role="menuitem"], [role="menuitemradio"], [role="menuitemcheckbox"]';
const TOGGLE_TARGETS = '[role="switch"], input[type="checkbox"], input[type="radio"]';
const CLICK_TARGETS = 'button, [role="button"], a[href], summary, select';
const INTERACTIVE_TARGETS = `${SELECT_TARGETS}, ${TOGGLE_TARGETS}, ${CLICK_TARGETS}`;

export function themeSoundForTarget(target) {
  const element = typeof target?.closest === 'function' ? target : target?.parentElement;
  if (typeof element?.closest !== 'function' || element.closest(EXCLUDED_TARGETS)) return null;
  const hit = element.closest(INTERACTIVE_TARGETS);
  if (!hit || hit.disabled === true || hit.getAttribute?.('aria-disabled') === 'true') return null;
  if (hit.matches(SELECT_TARGETS)) return 'select';
  if (hit.matches(TOGGLE_TARGETS)) return 'toggle';
  return 'click';
}

const state = { style: null, muted: true, samples: null };
let throttle = createThemeSoundThrottle();

// `samples` = { key, names, load(name) -> Promise<ArrayBuffer> } for the
// recordings of the active theme; the same key keeps the decoded set.
export function configureThemeSounds({ style = null, muted = false, samples = null, scope = globalThis } = {}) {
  state.style = THEME_SOUND_STYLES.includes(style) ? style : null;
  state.muted = muted !== false;
  const key = state.style && samples?.key ? samples.key : null;
  if (key !== (state.samples?.key ?? null)) state.samples = key ? loadSamples(samples, scope) : null;
}

export function themeSoundsEnabled() {
  return !!state.style && !state.muted;
}

// Tests reset the module between cases.
export function resetThemeSoundsForTest() {
  state.style = null;
  state.muted = true;
  state.samples = null;
  throttle = createThemeSoundThrottle();
  engine = null;
  decoder = null;
}

// Returns whether the sound was handed to the audio engine. `force` plays
// while muted (the mute button's own "sound on" confirmation).
export function playThemeSound(name, { force = false, scope = globalThis } = {}) {
  if (!state.style || (state.muted && !force)) return false;
  const recipe = THEME_SOUND_RECIPES[state.style]?.[name];
  if (!recipe || !throttle.allow(name)) return false;
  const sample = state.samples?.entries.get(name);
  return sample && !sample.failed ? startSample(sample, recipe, scope) : startSound(recipe, scope);
}

// Recordings are fetched and decoded as soon as the theme is active, before
// any user gesture, so the first click already has its buffer.
function loadSamples(samples, scope) {
  const entries = new Map();
  for (const name of samples.names || []) {
    if (!THEME_SOUND_NAMES.includes(name) || entries.has(name)) continue;
    const entry = { buffer: null, failed: false };
    entry.ready = Promise.resolve()
      .then(() => samples.load(name))
      .then((data) => decodeSample(data, scope))
      .then((buffer) => { entry.buffer = buffer; }, () => { entry.failed = true; });
    entries.set(name, entry);
  }
  return { key: samples.key, entries };
}

let decoder = null;

function decodeSample(data, scope) {
  if (!decoder) {
    const Offline = scope?.OfflineAudioContext || scope?.webkitOfflineAudioContext;
    if (!Offline) return Promise.reject(new Error('Audio decoding is unavailable'));
    decoder = new Offline(1, 1, 48000);
  }
  return new Promise((resolve, reject) => {
    const result = decoder.decodeAudioData(data, resolve, reject);
    if (typeof result?.then === 'function') result.then(resolve, reject);
  });
}

// Recipe levels are tuned against this master and the compressor's automatic
// makeup gain: peaks land between about -18 dBFS (click) and -9 dBFS (boot).
const MASTER_GAIN = 1.5;
// A sound requested while the context was suspended plays only if the
// browser lets it resume right away; never as a late burst after a click.
const RESUME_GRACE_MS = 200;
let engine = null;

function audioEngine(scope) {
  if (engine) return engine;
  const Context = scope?.AudioContext || scope?.webkitAudioContext;
  // Before the first user gesture a new context starts suspended and logs a
  // warning; skip instead (only the boot sound can be that early).
  if (!Context || scope?.navigator?.userActivation?.hasBeenActive === false) return null;
  try {
    const ctx = new Context({ latencyHint: 'interactive' });
    // Recordings are level-matched when the package is built and skip the
    // synthesizer's master gain and compressor.
    const samples = ctx.createGain();
    samples.gain.value = 1;
    samples.connect(ctx.destination);
    engine = { ctx, output: createThemeSoundOutput(ctx, ctx.destination), samples };
  } catch {
    return null;
  }
  return engine;
}

export function createThemeSoundOutput(ctx, destination) {
  const compressor = ctx.createDynamicsCompressor();
  // Only catches overlapping sounds (a completion chime over a click).
  compressor.threshold.value = -6;
  compressor.knee.value = 6;
  compressor.ratio.value = 4;
  compressor.attack.value = 0.002;
  compressor.release.value = 0.12;
  const master = ctx.createGain();
  master.gain.value = MASTER_GAIN;
  master.connect(compressor);
  compressor.connect(destination);
  return master;
}

function whenRunning(ctx, play) {
  if (ctx.state === 'running') {
    play(ctx.currentTime + 0.005);
    return true;
  }
  const requested = Date.now();
  Promise.resolve(ctx.resume?.()).then(() => {
    if (ctx.state === 'running' && Date.now() - requested <= RESUME_GRACE_MS) play(ctx.currentTime + 0.005);
  }).catch(() => {});
  return true;
}

function startSound(recipe, scope) {
  const current = audioEngine(scope);
  if (!current) return false;
  return whenRunning(current.ctx, (at) => scheduleThemeSound(current.ctx, current.output, recipe, at));
}

// A recording still decoding plays only if it is ready within the grace
// window; one that failed to decode falls back to the synthesized sound.
function startSample(sample, recipe, scope) {
  const current = audioEngine(scope);
  if (!current) return false;
  const play = (buffer) => whenRunning(current.ctx, (at) => scheduleThemeSample(current.ctx, current.samples, buffer, at));
  if (sample.buffer) return play(sample.buffer);
  const requested = Date.now();
  sample.ready.then(() => {
    if (Date.now() - requested > RESUME_GRACE_MS) return;
    if (sample.buffer) play(sample.buffer);
    else startSound(recipe, scope);
  });
  return true;
}

export function scheduleThemeSample(ctx, destination, buffer, startAt = ctx.currentTime) {
  const source = ctx.createBufferSource();
  source.buffer = buffer;
  source.connect(destination);
  source.onended = () => {
    try { source.disconnect(); } catch { /* already gone */ }
  };
  source.start(startAt);
  return startAt + buffer.duration;
}

const noiseBuffers = new WeakMap();
let noiseCursor = 0;

// One second of white noise per context, from a fixed seed so every render of
// a recipe is identical; voices start at rotating offsets inside it.
function noiseBuffer(ctx) {
  let buffer = noiseBuffers.get(ctx);
  if (buffer) return buffer;
  buffer = ctx.createBuffer(1, ctx.sampleRate, ctx.sampleRate);
  const data = buffer.getChannelData(0);
  let seed = 0x2f6b1d3;
  for (let i = 0; i < data.length; i += 1) {
    seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0;
    data[i] = seed / 0x80000000 - 1;
  }
  noiseBuffers.set(ctx, buffer);
  return buffer;
}

// Schedules every voice of `recipe` on `ctx` (an AudioContext or an
// OfflineAudioContext) and returns the time the last voice ends.
export function scheduleThemeSound(ctx, destination, recipe, startAt = ctx.currentTime) {
  const bus = ctx.createGain();
  bus.gain.value = recipe.level ?? 1;
  bus.connect(destination);
  let pending = recipe.voices.length;
  let end = startAt;
  const finished = () => {
    pending -= 1;
    if (pending === 0) {
      try { bus.disconnect(); } catch { /* already gone */ }
    }
  };
  for (const voice of recipe.voices) end = Math.max(end, scheduleVoice(ctx, bus, voice, startAt, finished));
  return end;
}

function scheduleVoice(ctx, bus, voice, startAt, finished) {
  const t0 = startAt + (voice.at || 0);
  const t1 = t0 + voice.dur;
  const amp = ctx.createGain();
  amp.gain.setValueAtTime(0.0001, t0);
  amp.gain.linearRampToValueAtTime(voice.gain, t0 + Math.min(voice.attack ?? 0.002, voice.dur / 2));
  amp.gain.exponentialRampToValueAtTime(0.0001, t1);
  let source;
  let offset;
  if (voice.kind === 'noise') {
    source = ctx.createBufferSource();
    source.buffer = noiseBuffer(ctx);
    source.loop = true;
    noiseCursor = (noiseCursor + 0.173) % 0.8;
    offset = noiseCursor;
  } else {
    source = ctx.createOscillator();
    source.type = voice.wave;
    source.frequency.setValueAtTime(voice.from, t0);
    if (voice.to && voice.to !== voice.from) source.frequency.exponentialRampToValueAtTime(voice.to, t1);
  }
  let node = source;
  if (voice.filter) {
    const filter = ctx.createBiquadFilter();
    filter.type = voice.filter.type;
    filter.frequency.value = voice.filter.freq;
    filter.Q.value = voice.filter.q ?? 0.8;
    node.connect(filter);
    node = filter;
  }
  node.connect(amp);
  amp.connect(bus);
  source.onended = () => {
    try { source.disconnect(); amp.disconnect(); } catch { /* already gone */ }
    finished();
  };
  source.start(t0, offset);
  source.stop(t1 + 0.02);
  return t1 + 0.02;
}

// Pointer presses and keyboard activation (Enter / Space on a focused
// control) play the matching interaction sound. Returns the cleanup.
export function installThemeSoundInteractions(target = globalThis.document) {
  if (typeof target?.addEventListener !== 'function') return () => {};
  const onPointerDown = (event) => {
    if (event.button !== 0 || event.isPrimary === false) return;
    const name = themeSoundForTarget(event.target);
    if (name) playThemeSound(name);
  };
  const onKeyDown = (event) => {
    if (event.repeat || (event.key !== 'Enter' && event.key !== ' ')) return;
    if (typeof event.target?.matches !== 'function' || !event.target.matches(INTERACTIVE_TARGETS)) return;
    const name = themeSoundForTarget(event.target);
    if (name) playThemeSound(name);
  };
  target.addEventListener('pointerdown', onPointerDown, true);
  target.addEventListener('keydown', onKeyDown, true);
  return () => {
    target.removeEventListener('pointerdown', onPointerDown, true);
    target.removeEventListener('keydown', onKeyDown, true);
  };
}
