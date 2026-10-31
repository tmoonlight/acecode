// Shared offline renderer and behaviour engine for the companion pets (Clippy, Rover, Whale Girl).
// Interaction follows the Codex Mini pet: hovering makes the pet jump, dragging runs toward the drag
// direction, session states play their animation and results play a few times before settling into
// the idle loop. At rest the pet loops its idle animation and randomly mixes in preset actions, so it
// is never completely still. The system "reduce motion" preference is deliberately ignored (user
// decision): a remote desktop session turns it on whenever window animations are off, which froze the pets.
(() => {
  'use strict';
  const $ = id => document.getElementById(id);
  const cv = $('cv'), ctx = cv.getContext('2d'), scene = $('scene'), stack = $('stack'), deck = $('deck');
  // Layout units: the scene is laid out on a 344×252 grid (geometry, hit bands, motion margins), while the
  // canvas itself holds the window's real device pixels, so the high-resolution art is never blown up.
  const SCENE_W = 344, SCENE_H = 252;
  // Silhouette bands are measured on an unmoved copy so per-frame motion never reflows the window region.
  const scratch = document.createElement('canvas');
  scratch.width = SCENE_W; scratch.height = SCENE_H;
  const sctx = scratch.getContext('2d', { willReadFrequently: true });
  const post = value => window.chrome?.webview?.postMessage(value);
  const action = value => post(JSON.stringify(value));
  const NAMES = { clippy: '大眼回形针', rover: 'rover', whale: '鲸鱼娘' };
  const BUSY = ['think', 'type', 'work', 'compact', 'retry'];
  // Codex pet vocabulary mapped onto each character's own animations. A clip is an animation name or
  // { name, times, loop, flip, hold }: times repeats it, loop repeats until replaced, flip mirrors it,
  // hold pauses on that frame until released (used to keep looking toward the drag direction).
  // breathe adds a subtle continuous breathing motion to every clip without its own motion.
  const PROFILES = {
    clippy: {
      idle: 'Idle1_1',
      fidgets: ['IdleAtom', 'IdleEyeBrowRaise', 'IdleFingerTap', 'IdleHeadScratch', 'IdleRopePile', 'IdleSideToSide',
        'LookUp', 'LookDown', 'LookLeft', 'LookRight', 'LookUpLeft', 'LookUpRight', 'GetArtsy', 'GetWizardy', 'Hearing_1'],
      greet: 'Greeting', jumping: 'GetAttention', click: 'Wave',
      dragLeft: { name: 'LookLeft', hold: 3 }, dragRight: { name: 'LookRight', hold: 3 },
      running: { think: ['Thinking', 'CheckingSomething'], type: ['Writing'],
        work: ['Searching', 'Processing', 'GetTechy', 'CheckingSomething'], compact: ['EmptyTrash', 'Save'], retry: ['CheckingSomething'] },
      attention: 'GetAttention', waiting: 'IdleFingerTap', failed: ['Alert'], review: ['Congratulate'],
      sleep: 'IdleSnooze', wake: 'IdleEyeBrowRaise',
    },
    rover: {
      idle: 'Idle', breathe: true,
      fidgets: ['Sports', 'Cooking', 'Shopping', 'Books', 'Money', 'Celebrity', 'Travel', 'LookUp', 'LookUpLeft', 'Acknowledge'],
      greet: ['Show', 'Greet'], jumping: 'Pleased', click: 'ClickedOn', drag: 'Surprised',
      // Rover's pixel-art GestureLeft (Grok video, 12 fps) holds its pointing pose from frame 9 to 24.
      dragLeft: { name: 'GestureLeft', hold: 10 }, dragRight: { name: 'GestureLeft', hold: 10, flip: true },
      running: { think: ['Thinking'], type: ['Writing'], work: ['Searching', 'ImageSearching', 'Books'], compact: ['Books'], retry: ['Searching'] },
      attention: 'GetAttention', waiting: 'GetAttentionMinor', failed: ['Embarrassed'], review: ['Congratulate', 'CharacterSucceeds'],
    },
    whale: {
      idle: 'idle', breathe: true,
      fidgets: [{ name: 'play', times: 4 }, { name: 'eat', times: 6 }, { name: 'welcome', times: 3 }, { name: 'joy', times: 5 }],
      greet: { name: 'welcome', times: 3 }, jumping: { name: 'joy', times: 3 }, click: { name: 'welcome', times: 2 },
      // Held up by a hand for the whole drag, swinging gently, whichever way it moves.
      drag: { name: 'drag', loop: true },
      running: { think: ['think'], type: ['working'], work: ['working'], compact: ['working'], retry: ['think'] },
      attention: { name: 'welcome', times: 2 }, waiting: 'wait',
      failed: [{ name: 'error', times: 4 }, { name: 'disappointed', times: 3 }], review: [{ name: 'celebrate', times: 3 }],
      sleep: 'sleep', wake: 'wake',
    },
  };
  // Docked pets peek in from the screen edge. `hidden` scene px of the art stays past the edge so
  // they can lean in without revealing where the drawing is cut. `ratio` is how much larger the peek
  // art is drawn than the animation frames: it is shown at the frame scale divided by it, so the head
  // keeps the size it has in every other pose instead of growing at the edge. Whale Girl's peek art
  // is 0.9x her HD frames (1.8x the original 256px frames: eye height and headband-to-mouth both
  // measure 1.8x); Clippy's and Rover's are redrawn at their HD frame scale. Their art is narrow,
  // so less of it hides past the edge.
  const PEEK = {
    whale: { ratio: 0.9, hidden: 9 },
    clippy: { ratio: 1, hidden: 4 },
    rover: { ratio: 1, hidden: 4 },
  };
  const peekStyle = () => PEEK[kind] || PEEK.whale;
  // Extra hit margin (scene px) so moving silhouettes stay clickable without per-frame region updates.
  const MOTION_MARGIN = { float: [0, 4], breathe: [0, 3], shake: [3, 0], wiggle: [8, 0], tilt: [16, 2] };
  const timing = { fidgetMin: 7000, fidgetMax: 16000, sleepAfter: 5 * 60 * 1000, greeting: 6000 };
  // The original agents hold a pose through self-looping branches (Clippy's idle stares ~6s).
  // Once one picture has been shown this long, branches are skipped so the clip moves on.
  const MAX_HOLD_MS = 1200;

  let snapshot = { version: 1, follow: true, connected: false, agents: [], offices: [], selected: null, activities: [] };
  // `art`: the loaded pet's HD frames (see makeArt); `data` its animation timing from agent.js.
  let kind = '', data = null, art = null, profile = null, loadRevision = 0, pendingIntro = null;
  let animation = '', frameIndex = 0, flipped = false, motionKind = '', images = null, imageKey = '', imageSince = 0;
  // Edge docking (pets with peek art): the peek sheet, which edge, eyes open/closed, lean and bounce.
  // `windowEdge` is the edge the native window sits on; it becomes `docked` once the pet has peek art,
  // so a pet switched in (or still loading) while the window is docked peeks from the same edge.
  let peek = null, docked = '', windowEdge = '', peekEyes = 0, peekLean = 0, peekBounce = '', peekBounceAt = 0;
  let timer = 0, generation = 0, current = null;
  let base = 'idle', variant = '', queue = [], stateKey = '';
  // Session cards: keys already reacted to, cards the user dismissed, and whether the deck is spread out.
  let seenCards = new Set(), cardsSeeded = false, dismissed = new Set(), expanded = false, collapseTimer = 0, cardsKey = '';
  let dragging = false, dragDir = 0, dragPauseTimer = 0, dragWatchTimer = 0, dragEndedAt = 0;
  let hovered = false, hoverOffTimer = 0, hoverReadyAt = 0, lastClickAt = 0;
  let sleeping = false, lastActivity = performance.now(), fidgetAt = 0, fidgetTimer = 0, lastFidget = '';
  let greetingUntil = 0, greetingTimer = 0, rafId = 0, bodyRegions = [], bodyBounds = null, bandCache = new Map();
  let hoverPoint = null;
  let overlayKey = '', badgeTimer = 0;

  const pick = list => list[Math.floor(Math.random() * list.length)];
  const rand = (min, max) => min + Math.random() * (max - min);
  const clipOf = spec => {
    if (!spec) return null;
    const clip = typeof spec === 'string' ? { name: spec } : { ...spec };
    return data?.animations[clip.name] ? clip : null;
  };
  function normalizeProfile(raw) {
    const many = list => (Array.isArray(list) ? list : list ? [list] : []).map(clipOf).filter(Boolean);
    const idle = clipOf(raw.idle) || clipOf('RestPose');
    const running = {};
    for (const state of BUSY) running[state] = many(raw.running?.[state]);
    return { breathe: Boolean(raw.breathe), idle, fidgets: many(raw.fidgets), greet: many(raw.greet),
      jumping: clipOf(raw.jumping), click: clipOf(raw.click), drag: clipOf(raw.drag),
      dragLeft: clipOf(raw.dragLeft), dragRight: clipOf(raw.dragRight), running,
      attention: clipOf(raw.attention), waiting: clipOf(raw.waiting) || idle,
      failed: many(raw.failed), review: many(raw.review), sleep: clipOf(raw.sleep), wake: clipOf(raw.wake) };
  }

  // ---- Rendering ----
  const normalized = r => [r.left / innerWidth, r.top / innerHeight, r.right / innerWidth, r.bottom / innerHeight];
  const peeking = () => Boolean(docked && peek);
  // Breathing squashes the picture by fractions of a pixel, which makes pixel art shimmer row by row.
  const activeMotion = () => (peeking() ? 'peek' : motionKind || (profile?.breathe && !art?.pixelArt ? 'breathe' : ''));
  function geometry() {
    const [w, h] = art.framesize;
    const frameScale = Math.min(184 / w, 140 / h);
    if (peeking()) {
      const { ratio, hidden } = peekStyle();
      const scale = frameScale / ratio, width = peek.fw * scale, height = peek.fh * scale;
      return { peek: true, w: peek.fw, h: peek.fh, scale, width, height,
        x: docked === 'right' ? SCENE_W - width + hidden : -hidden, y: SCENE_H - height - 2 };
    }
    const width = w * frameScale, height = h * frameScale;
    return { w, h, scale: frameScale, width, height, x: (SCENE_W - width) / 2, y: SCENE_H - height - 8 };
  }
  function offsetAt(now, g) {
    const kindOfMotion = activeMotion();
    if (!kindOfMotion) return null;
    const o = { dx: 0, dy: 0, angle: 0, sy: 1, px: g.x + g.width / 2, py: g.y + g.height };
    if (kindOfMotion === 'float') o.dy = Math.sin(now / 500) * 3;
    else if (kindOfMotion === 'breathe') o.sy = 1 + Math.sin(now / 900) * 0.018;
    else if (kindOfMotion === 'shake') o.dx = Math.sin(now / 35) * 2.5;
    else if (kindOfMotion === 'wiggle') o.angle = Math.sin(now / 160) * 0.05;
    else if (kindOfMotion === 'peek') {
      // A shy, slow lean in and out; hovering or a waiting session makes her lean in further.
      // The sway spans two thirds of the hidden strip, so it never stalls against that limit.
      const sway = (1 - Math.cos(now / 1900)) * peekStyle().hidden / 3;
      peekLean += ((hovered ? 12 : base === 'waiting' ? 9 : 0) - peekLean) * 0.12;
      o.dx = (docked === 'right' ? -1 : 1) * Math.min(peekStyle().hidden, sway + peekLean);
      const since = now - peekBounceAt;
      if (peekBounce === 'happy' && since < 900) o.dy = -Math.abs(Math.sin(since / 90)) * 6 * (1 - since / 900);
      else if (peekBounce === 'shake' && since < 700) o.dx += Math.sin(since / 28) * 3 * (1 - since / 700);
    }
    else if (kindOfMotion === 'tilt') {
      // Whale Girl's drag frame is held by a hand at the upper right: swing like a pendulum from that grip.
      o.px = g.x + g.width * (flipped ? 0.16 : 0.84); o.py = g.y + g.height * 0.15;
      o.angle = Math.sin(now / 420) * 0.1;
    }
    return o;
  }
  // Pixel art (Rover) is first enlarged to the next whole multiple with nearest-neighbour, then smoothed
  // down to its real size ("sharp bilinear"): every art pixel stays an even, crisp square at any window
  // scale, where plain nearest-neighbour would mix one- and two-pixel-wide columns.
  const pixelCanvas = document.createElement('canvas'), pixelCtx = pixelCanvas.getContext('2d');
  function drawArt(target, image, sx, sy, sw, sh, dx, dy, dw, dh) {
    if (!art?.pixelArt) { target.drawImage(image, sx, sy, sw, sh, dx, dy, dw, dh); return; }
    const n = Math.max(1, Math.ceil(Math.abs(target.getTransform().a) * dw / sw - 0.01));
    if (pixelCanvas.width < sw * n || pixelCanvas.height < sh * n) {
      pixelCanvas.width = Math.max(pixelCanvas.width, sw * n); pixelCanvas.height = Math.max(pixelCanvas.height, sh * n);
    }
    // Clear it all: smoothing samples one pixel past the copied area, where an earlier, larger frame may linger.
    pixelCtx.clearRect(0, 0, pixelCanvas.width, pixelCanvas.height);
    pixelCtx.imageSmoothingEnabled = false;
    pixelCtx.drawImage(image, sx, sy, sw, sh, 0, 0, sw * n, sh * n);
    target.imageSmoothingQuality = 'low';
    target.drawImage(pixelCanvas, 0, 0, sw * n, sh * n, dx, dy, dw, dh);
    target.imageSmoothingQuality = 'high';
  }
  function paint(target, g, o) {
    // Scene units to the target's pixels (device pixels on screen, 1:1 on the scratch copy).
    target.setTransform(target.canvas.width / SCENE_W, 0, 0, target.canvas.height / SCENE_H, 0, 0);
    target.clearRect(0, 0, SCENE_W, SCENE_H);
    // High-resolution art is drawn at or below its own size: always smooth (pixel art: see drawArt).
    target.imageSmoothingEnabled = true;
    target.imageSmoothingQuality = 'high';
    if (g.peek) {
      target.save();
      if (o) target.translate(o.dx, o.dy);
      // The art peeks from the right edge; mirror it for the left edge.
      if (docked === 'left') { target.translate(g.x * 2 + g.width, 0); target.scale(-1, 1); }
      drawArt(target, peek.image, peekEyes * peek.fw, 0, peek.fw, peek.fh, g.x, g.y, g.width, g.height);
      target.restore();
      return;
    }
    if (!images?.length) return;
    target.save();
    if (o) {
      target.translate(o.px, o.py); target.rotate(o.angle); target.scale(1, o.sy);
      target.translate(o.dx - o.px, o.dy - o.py);
    }
    if (flipped) { target.translate(g.x * 2 + g.width, 0); target.scale(-1, 1); }
    // Frames are listed by their original sprite offset; the HD table gives the trimmed picture
    // ([sheet, x, y, width, height, offset x, offset y], in HD frame pixels) on its sheet.
    for (const [sx, sy] of images) {
      const hd = art.frames[sx + ',' + sy], image = hd && art.images.get(hd[0]);
      if (image) drawArt(target, image, hd[1], hd[2], hd[3], hd[4],
        g.x + hd[5] * g.scale, g.y + hd[6] * g.scale, hd[3] * g.scale, hd[4] * g.scale);
    }
    target.restore();
  }
  const continuous = () => !document.hidden && Boolean(activeMotion()) && (peeking() || Boolean(images?.length));
  function render() {
    if (!art || !data) return;
    const g = geometry();
    paint(ctx, g, offsetAt(performance.now(), g));
    if (!rafId && continuous()) rafId = requestAnimationFrame(() => { rafId = 0; render(); });
  }
  function updateBands() {
    const g = geometry(), key = g.peek ? `peek|${docked}|${peekEyes}` : flipped + '|' + JSON.stringify(images);
    let bands = bandCache.get(key);
    if (!bands) {
      // Narrow horizontal bands follow the visible silhouette instead of the whole window.
      paint(sctx, g, null);
      const pixels = sctx.getImageData(0, 0, SCENE_W, SCENE_H).data;
      bands = [];
      for (let top = Math.max(0, Math.floor(g.y)); top < g.y + g.height; top += 8) {
        const bottom = Math.min(SCENE_H, top + 8);
        let left = SCENE_W, right = 0;
        for (let py = top; py < bottom; py++) for (let px = Math.max(0, Math.floor(g.x)); px < Math.min(SCENE_W, g.x + g.width); px++) {
          if (pixels[(py * SCENE_W + px) * 4 + 3] > 32) { left = Math.min(left, px); right = Math.max(right, px + 1); }
        }
        if (right > left) bands.push([left, top, right, bottom]);
      }
      bandCache.set(key, bands);
    }
    const motion = activeMotion();
    const [mx, my] = motion === 'peek' ? [peekStyle().hidden + 4, 8] : MOTION_MARGIN[motion] || [0, 0];
    bodyRegions = bands.map(([l, t, r, b]) => [Math.max(0, l - mx), Math.max(0, t - my), Math.min(SCENE_W, r + mx), Math.min(SCENE_H, b + my)]);
    bodyBounds = bands.length ? [Math.min(...bands.map(b => b[0])), Math.min(...bands.map(b => b[1])),
      Math.max(...bands.map(b => b[2])), Math.max(...bands.map(b => b[3]))] : null;
  }
  function setFrame(frame) {
    if (!Array.isArray(frame?.images)) return;   // frames without images keep the previous picture
    images = frame.images;
    const key = flipped + '|' + JSON.stringify(images);
    if (key !== imageKey) { imageKey = key; imageSince = performance.now(); }
    updateBands(); render(); reportRegions();
  }
  function reportRegions() {
    const r = cv.getBoundingClientRect();
    const toWindow = b => [b[0] / SCENE_W, b[1] / SCENE_H, b[2] / SCENE_W, b[3] / SCENE_H];
    const bodies = bodyRegions.map(toWindow);
    // Keep only a small anchor under a stationary pointer when the animation jumps away.
    // The rest of the transparent sprite frame remains click-through during hover.
    if (hovered && hoverPoint) bodies.push(toWindow([hoverPoint.x - 3, hoverPoint.y - 3, hoverPoint.x + 3, hoverPoint.y + 3]));
    const grip = $('grip');
    grip.hidden = !bodyBounds;
    // It follows the animated outline, but holds still under the pointer so it can be grabbed.
    // It also stays below the collapsed card stack: that box (with its shadow padding) sits above
    // the grip and would swallow the pointer, and macOS treats it as foreground, not the grip.
    if (bodyBounds && !grip.matches(':hover')) {
      const belowCards = stack.hidden || expanded ? 0 : stack.getBoundingClientRect().bottom;
      const top = Math.max(0, belowCards, bodyBounds[1] * r.height / SCENE_H - grip.offsetHeight);
      grip.style.left = Math.max(0, bodyBounds[0] * r.width / SCENE_W - grip.offsetWidth) + 'px';
      grip.style.top = Math.min(Math.max(0, innerHeight - grip.offsetHeight), top) + 'px';
    }
    // No card, no rectangle: the empty space above the pet stays click-through.
    const message = { type: 'overlay', controls: null,
      rect: stack.hidden ? null : normalized(stack.getBoundingClientRect()),
      grip: grip.hidden ? null : normalized(grip.getBoundingClientRect()), bubbles: bodies };
    const key = JSON.stringify(message);
    if (key !== overlayKey && r.width > 0) { overlayKey = key; action(message); }
  }

  // ---- HD art: sheets decode on demand around the playhead ----
  // The redrawn frames total tens of megapixels per pet (Rover's idle alone has 330 frames), far too
  // much to keep decoded, so the sheets follow playback order and only those holding the next AHEAD
  // frames of the playing clip are decoded; the least recently used ones go above the budget.
  const AHEAD = 16, SHEET_BUDGET = 16e6;
  function makeArt(name, sources) {
    const hd = window.ClassicPetHD?.[name];
    if (!hd || !Array.isArray(sources) || sources.length !== hd.sheets.length) return null;
    return { ...hd, sources, images: new Map(), loading: new Map(), used: new Map() };
  }
  const framesOf = name => (name && data?.animations[name]?.frames) || [];
  function sheetsAhead(target, frames, from = 0) {
    const sheets = new Set();
    for (let k = 0; k < Math.min(AHEAD, frames.length); k++) {
      for (const [sx, sy] of frames[(from + k) % frames.length].images || []) {
        const hd = target.frames[sx + ',' + sy];
        if (hd) sheets.add(hd[0]);
      }
    }
    return [...sheets];
  }
  const loaded = (target, sheets) => sheets.every(index => target.images.has(index));
  const loadAll = (target, sheets) => Promise.all(sheets.map(index => loadSheet(target, index)));
  function loadSheet(target, index) {
    target.used.set(index, performance.now());
    if (target.images.has(index)) return Promise.resolve();
    let pending = target.loading.get(index);
    if (!pending) {
      const image = new Image();
      image.src = target.sources[index];
      pending = image.decode().then(() => {
        target.loading.delete(index);
        target.images.set(index, image);
        trimSheets(target);
      }, error => { target.loading.delete(index); throw error; });
      target.loading.set(index, pending);
    }
    return pending;
  }
  function trimSheets(target) {
    if (target !== art) return;   // a pet still loading keeps everything until it is shown
    // Never drop what is on screen, what plays next, or the start of the idle loop the pet returns to.
    const keep = new Set([...sheetsAhead(target, framesOf(animation), frameIndex), ...sheetsAhead(target, framesOf(profile?.idle?.name))]);
    for (const [sx, sy] of images || []) { const hd = target.frames[sx + ',' + sy]; if (hd) keep.add(hd[0]); }
    const size = index => target.sheets[index][0] * target.sheets[index][1];
    let pixels = [...target.images.keys()].reduce((sum, index) => sum + size(index), 0);
    const old = [...target.images.keys()].filter(index => !keep.has(index)).sort((a, b) => target.used.get(a) - target.used.get(b));
    for (const index of old) {
      if (pixels <= SHEET_BUDGET) break;
      target.images.delete(index); pixels -= size(index);
    }
  }

  // ---- Playback ----
  function stopPlayback() { clearTimeout(timer); timer = 0; generation++; current = null; }
  // `pending`: the state of this same clip while its sheets were decoding (exit / release requests carry over).
  function play(spec, done, pending = null) {
    const clip = spec && typeof spec === 'object' && data?.animations[spec.name] ? spec : clipOf(spec);
    stopPlayback();
    if (!clip || !art) return;
    const frames = data.animations[clip.name].frames;
    const token = generation, state = current = { clip, exit: Boolean(pending?.exit), release: Boolean(pending?.release) };
    animation = clip.name; flipped = Boolean(clip.flip);
    motionKind = data.animations[clip.name].motion || '';
    // The sheets of its first frames decode first (milliseconds); the previous picture stays up meanwhile.
    const opening = sheetsAhead(art, frames);
    if (!pending && !loaded(art, opening)) {
      const target = art;
      loadAll(target, opening).then(() => { if (token === generation && art === target) play(spec, done, state); },
        () => {
          // A sheet that cannot be decoded takes its animation out of rotation instead of retrying forever.
          if (art === target) delete data.animations[clip.name];
          if (token === generation) { current = null; done?.(); }
        });
      return;
    }
    if (!frames?.length) { timer = setTimeout(() => { if (token === generation) done?.(); }, 100); return; }
    let index = 0, steps = 0, repeats = 0, started = performance.now();
    const show = i => {
      frameIndex = i;
      const target = art, own = sheetsAhead(target, [frames[i]]);
      // Keep decoding ahead of the playhead; a frame whose sheet is not ready yet (a branch jumping
      // far ahead) holds the previous picture until it is.
      loadAll(target, sheetsAhead(target, frames, i)).catch(() => {});
      if (loaded(target, own)) setFrame(frames[i]);
      else loadAll(target, own).then(() => { if (token === generation && frameIndex === i) setFrame(frames[i]); }, () => {});
    };
    const schedule = () => {
      const frame = frames[index];
      timer = setTimeout(() => {
        if (token !== generation) return;
        if (index === clip.hold && !state.release && !state.exit) { schedule(); return; }
        let next = index + 1;
        const exiting = state.exit || performance.now() - started > 12000 || ++steps > 300;
        if (exiting && Number.isInteger(frame.exitBranch)) next = frame.exitBranch;
        else if (!exiting && frame.branching?.branches && performance.now() - imageSince < MAX_HOLD_MS) {
          let chance = Math.random() * 100;
          for (const branch of frame.branching.branches) {
            chance -= branch.weight;
            if (chance < 0) { next = branch.frameIndex; break; }
          }
        }
        if (steps > 600 || next >= frames.length) {
          if (!state.exit && (clip.loop || ++repeats < (clip.times || 1))) {
            index = 0; steps = 0; started = performance.now(); show(0); schedule(); return;
          }
          current = null; done?.(); return;
        }
        index = next; show(index); schedule();
      }, Math.max(16, Number(frame.duration) || 100));
    };
    show(0);
    if (!document.hidden) schedule();
  }
  function still() {
    stopPlayback();
    cancelAnimationFrame(rafId); rafId = 0;
    if (peeking()) { setPeekEyes(0); return; }
    const rest = clipOf('RestPose') || profile?.idle;
    if (!rest || !loaded(art, sheetsAhead(art, framesOf(rest.name).slice(0, 1)))) return;
    animation = rest.name; flipped = false; motionKind = ''; frameIndex = 0;
    setFrame(data.animations[rest.name].frames[0]);
  }
  // ---- Edge peek (docked pet): blink now and then, happy squint on attention ----
  function setPeekEyes(value) {
    peekEyes = value; frameIndex = value;
    updateBands(); render(); reportRegions();
  }
  function playPeek() {
    stopPlayback();
    const token = generation;
    animation = 'peek'; flipped = false; motionKind = '';
    current = { clip: { name: 'peek' }, exit: false, release: false };
    setPeekEyes(0);
    const later = (ms, fn) => { timer = setTimeout(() => { if (token === generation) fn(); }, ms); };
    const blink = () => later(rand(2200, 5200), () => {
      setPeekEyes(1);
      later(130, () => {
        setPeekEyes(0);
        // Now and then a quick double blink.
        if (Math.random() < 0.25) later(170, () => { setPeekEyes(1); later(120, () => { setPeekEyes(0); blink(); }); });
        else blink();
      });
    });
    blink();
  }
  function peekReact(kind) {
    stopPlayback();
    const token = generation;
    animation = 'peek-' + kind;
    peekBounce = kind; peekBounceAt = performance.now();
    setPeekEyes(kind === 'happy' ? 1 : 0);   // the closed-eye frame doubles as a happy squint
    timer = setTimeout(() => { if (token === generation) playPeek(); }, kind === 'happy' ? 900 : 700);
  }
  function setDocked(side, { quiet = false } = {}) {
    windowEdge = side === 'left' || side === 'right' ? side : '';
    const next_ = peek ? windowEdge : '';
    if (next_ === docked) return;
    docked = next_; peekLean = 0; peekBounce = '';
    if (!art || quiet) return;
    next();
  }
  function planFidget() {
    const now = performance.now();
    fidgetAt = now + rand(timing.fidgetMin, timing.fidgetMax);
    clearTimeout(fidgetTimer);
    // Leave the idle loop through its own exit branch so the preset action starts naturally.
    fidgetTimer = setTimeout(() => { if (current && current.clip === profile?.idle) current.exit = true; }, fidgetAt - now);
  }
  function chooseFidget() {
    const options = profile.fidgets.filter(clip => clip.name !== lastFidget);
    const clip = pick(options.length ? options : profile.fidgets);
    lastFidget = clip.name;
    return clip;
  }
  // Decides what plays after every clip, so the pet always continues with another animation.
  function next() {
    if (!art || !profile) return;
    if (document.hidden) { still(); return; }
    if (peeking()) { playPeek(); return; }
    if (dragging) { playDrag(); return; }
    if (queue.length) { play(queue.shift(), next); return; }
    if (base === 'running') {
      const options = profile.running[variant]?.length ? profile.running[variant] : profile.running.work;
      play(options.length ? pick(options) : profile.idle, next);
      return;
    }
    if (base === 'waiting') { play(profile.waiting, next); return; }
    const now = performance.now();
    if (!sleeping && profile.sleep && now - lastActivity >= timing.sleepAfter) sleeping = true;
    if (sleeping) { play(profile.sleep, next); return; }
    if (now >= fidgetAt && profile.fidgets.length) { planFidget(); play(chooseFidget(), next); return; }
    play(profile.idle, next);
  }
  function wake() {
    lastActivity = performance.now();
    if (!sleeping) return null;
    sleeping = false;
    planFidget();
    return profile?.wake || null;
  }
  // A transient reaction (hover / click) replaces the current clip, then the pet continues its state.
  function react(clip) {
    if (peeking()) { wake(); if (art && !document.hidden) peekReact('happy'); return; }
    const waking = wake();
    if (!art || dragging || document.hidden) return;
    if (waking && clip) queue.unshift(clip);
    if (waking || clip) play(waking || clip, next);
  }

  // ---- Session cards and mood (Codex Mini) ----
  const ICONS = {
    review: '<svg viewBox="0 0 16 16" aria-hidden="true"><circle cx="8" cy="8" r="8" fill="var(--ok)"/><path d="m4.6 8.3 2.2 2.2 4.6-4.8" fill="none" stroke="#fff" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"/></svg>',
    failed: '<svg viewBox="0 0 16 16" aria-hidden="true"><circle cx="8" cy="8" r="8" fill="var(--danger)"/><path d="M8 4.2v4.6M8 11.3v.1" stroke="#fff" stroke-width="1.9" stroke-linecap="round"/></svg>',
    waiting: '<svg viewBox="0 0 16 16" aria-hidden="true"><circle cx="8" cy="8" r="8" fill="var(--warn)"/><path d="M8 4.2v4.6M8 11.3v.1" stroke="#fff" stroke-width="1.9" stroke-linecap="round"/></svg>',
    stopped: '<svg viewBox="0 0 16 16" aria-hidden="true"><circle cx="8" cy="8" r="8" fill="var(--idle)"/><rect x="5.3" y="5.3" width="5.4" height="5.4" rx="1" fill="#fff"/></svg>',
    info: '<svg viewBox="0 0 16 16" aria-hidden="true"><circle cx="8" cy="8" r="8" fill="var(--idle)"/><path d="M8 7.2v4.4M8 4.6v.1" stroke="#fff" stroke-width="1.9" stroke-linecap="round"/></svg>',
  };
  const STATUS_TEXT = { running: '运行中', waiting: '需要你处理', review: '已完成', failed: '失败', stopped: '已停止' };
  const RESULT = ['review', 'failed', 'stopped'];
  const sessionCards = () => (snapshot.connected ? snapshot.activities || [] : []).filter(card => !dismissed.has(card.key));
  // Shown cards: the brief first-appearance greeting, a reconnect notice, then real sessions.
  function shownCards() {
    const cards = [];
    if (kind && performance.now() < greetingUntil)
      cards.push({ key: 'greeting', status: 'info', title: `你好，我是${NAMES[kind]}`, body: '双击我就能打开 ACECode', local: true });
    if (art && !snapshot.connected) cards.push({ key: 'offline', status: 'info', title: '连接已中断', body: '正在重新连接…', local: true });
    return cards.concat(sessionCards());
  }
  function cardElement(card) {
    const el = document.createElement('div');
    el.className = 'card'; el.tabIndex = 0; el.dataset.key = card.key; el.dataset.status = card.status;
    el.setAttribute('role', 'listitem');
    const label = STATUS_TEXT[card.status];
    el.setAttribute('aria-label', [card.title, label, card.body].filter(Boolean).join('，'));
    el.title = card.local ? '' : `${card.title}\n${card.body}\n点击打开这个会话`;
    const icon = card.status === 'running' ? '<i class="spinner"></i>' : ICONS[card.status] || '';
    el.innerHTML = `<span class="icon">${icon}</span><span class="text"><span class="head"><span class="name"></span></span><span class="body"></span></span>`;
    el.querySelector('.name').textContent = card.title;
    el.querySelector('.body').textContent = card.body || label || '';
    if (card.status === 'running') {
      const stop = document.createElement('button');
      stop.type = 'button'; stop.className = 'stop'; stop.title = '停止这个会话'; stop.setAttribute('aria-label', `停止「${card.title}」`);
      stop.innerHTML = '<i></i>';
      stop.addEventListener('click', event => {
        event.stopPropagation();
        action({ type: 'stop', session_id: card.sessionId, workspace_hash: card.workspaceHash || '' });
      });
      el.appendChild(stop);
    }
    const close = document.createElement('button');
    close.type = 'button'; close.className = 'dismiss'; close.textContent = '×';
    close.title = RESULT.includes(card.status) ? '标为已读并移除' : '暂时隐藏';
    close.setAttribute('aria-label', `移除「${card.title}」`);
    close.addEventListener('click', event => { event.stopPropagation(); dismissCard(card); });
    el.appendChild(close);
    el.addEventListener('click', () => activateCard(card));
    el.addEventListener('keydown', event => {
      if (event.key === 'Enter' || event.key === ' ') { event.preventDefault(); activateCard(card); }
      else if (event.key === 'Escape') setExpanded(false);
    });
    return el;
  }
  function renderCards() {
    const cards = shownCards();
    if (cards.length < 2) expanded = false;
    const key = JSON.stringify([expanded, cards.map(c => [c.key, c.title, c.body, c.status])]);
    if (key === cardsKey) return;
    cardsKey = key;
    stack.hidden = !cards.length;
    stack.classList.toggle('expanded', expanded);
    stack.classList.toggle('collapsed', !expanded);
    stack.classList.toggle('layers', !expanded && cards.length > 1);
    stack.classList.toggle('layers-2', !expanded && cards.length > 2);
    stack.setAttribute('aria-label', cards.length > 1 && !expanded ? `${cards.length} 个会话动态，点击展开` : '会话动态');
    deck.replaceChildren(...cards.map(cardElement));
    fitStack(); reportRegions();
  }
  // The cards keep the transcript font size, but the pet shrinks with the window: below roughly
  // 280px the collapsed stack would run past the top edge and be cut off. Shrink it just enough
  // to fit above the pet (not below 9px text), else pin it to the top edge. The spread-out list
  // scrolls inside the window and keeps the collapsed size.
  function fitStack() {
    if (expanded) return;
    stack.style.removeProperty('--stack-fit');
    stack.classList.remove('pinned');
    if (stack.hidden) return;
    const minFit = Math.min(1, 9 / parseFloat(getComputedStyle(stack).fontSize));
    let fit = 1;
    for (let pass = 0; pass < 3; pass++) {
      const box = stack.getBoundingClientRect();
      if (box.top >= -0.5 || fit <= minFit) break;
      fit = Math.max(minFit, fit * box.bottom / box.height);
      stack.style.setProperty('--stack-fit', fit.toFixed(3));
    }
    if (stack.getBoundingClientRect().top < -0.5) stack.classList.add('pinned');
  }
  function setExpanded(value) {
    clearTimeout(collapseTimer);
    if (expanded === value) return;
    expanded = value;
    renderCards();
  }
  // Collapsed with several cards a click spreads them out; otherwise it opens the session.
  function activateCard(card) {
    if (!expanded && shownCards().length > 1) { setExpanded(true); return; }
    if (card.local) { dismissCard(card); return; }
    action({ type: 'open', session_id: card.sessionId, workspace_hash: card.workspaceHash || '' });
    // Opening marks a finished session read; hide its card right away.
    if (RESULT.includes(card.status)) dismissed.add(card.key);
    setExpanded(false);
    renderCards();
  }
  function dismissCard(card) {
    if (card.key === 'greeting') greetingUntil = 0;
    else if (!card.local) {
      dismissed.add(card.key);
      if (RESULT.includes(card.status)) action({ type: 'read', session_id: card.sessionId, workspace_hash: card.workspaceHash || '' });
    }
    renderCards(); syncState();
  }
  stack.addEventListener('pointerleave', () => {
    clearTimeout(collapseTimer);
    if (expanded) collapseTimer = setTimeout(() => setExpanded(false), 1500);
  });
  stack.addEventListener('pointerenter', () => clearTimeout(collapseTimer));

  // The pet's mood follows the cards: waiting for the user outranks work; a newly finished
  // or failed session gets a one-off reaction; with no session activity the pet idles.
  function moodOf(cards) {
    if (cards.some(card => card.status === 'waiting')) return { base: 'waiting', variant: '' };
    const running = cards.find(card => card.status === 'running');
    if (running) return { base: 'running', variant: BUSY.includes(running.state) ? running.state : 'work' };
    return { base: 'idle', variant: '' };
  }
  function syncState(force = false) {
    const cards = sessionCards();
    const fresh = cards.filter(card => !seenCards.has(card.key));
    const first = !cardsSeeded;
    cardsSeeded = true;
    for (const card of cards) seenCards.add(card.key);
    // Bound the memory of reacted cards; Set iteration order drops the oldest first.
    for (const old of seenCards) { if (seenCards.size <= 400) break; seenCards.delete(old); }
    const finished = !first && fresh.some(card => card.status === 'review');
    const failed = !first && fresh.some(card => card.status === 'failed');
    const mood = moodOf(cards);
    const key = [mood.base, mood.variant, kind].join('|');
    if (!force && key === stateKey && !finished && !failed) return;
    stateKey = key;
    if (!profile) return;
    const intro = pendingIntro;
    pendingIntro = null;
    const variantOnly = !force && !intro && !finished && !failed && base === 'running' && mood.base === 'running';
    const wasWaiting = base === 'waiting';
    base = mood.base; variant = mood.variant;
    const waking = wake();
    if (peeking()) {
      // Docked she keeps peeking; results get a happy squint or a little shake.
      queue = [];
      if (dragging || !art) return;
      if (finished) peekReact('happy');
      else if (failed) peekReact('shake');
      else if (!current) playPeek();
      return;
    }
    queue = intro ? [...intro] : [];
    if (finished) queue.push(...profile.review);
    else if (failed) queue.push(...profile.failed);
    else if (base === 'waiting' && !wasWaiting && profile.attention) queue.push(profile.attention);
    if (waking) queue.unshift(waking);
    if (base === 'idle') planFidget();
    if (dragging || !art) return;
    // Only the busy phase changed: let the current work animation finish before switching.
    if (variantOnly && !queue.length && current) { current.exit = true; return; }
    next();
  }
  // Card text uses exactly the ACECode transcript body font (family and size follow the
  // main window's font-size setting); the defaults below match its medium setting.
  function applyTranscriptFont(font) {
    const root = document.documentElement.style;
    const size = String(font?.size || '').trim(), family = String(font?.family || '').trim();
    const before = root.cssText;
    if (/^\d+(\.\d+)?px$/.test(size)) root.setProperty('--pet-font-size', size);
    if (family && !/[;{}<>]/.test(family)) root.setProperty('--pet-font', family);
    if (root.cssText !== before) { fitStack(); reportRegions(); }
  }
  function applySnapshot(value) {
    if (value?.version !== 1 || !Array.isArray(value.agents) || !Array.isArray(value.offices)) return;
    snapshot = { ...value, activities: Array.isArray(value.activities) ? value.activities : [] };
    applyTranscriptFont(value.transcriptFont);
    // Forget dismissals once that card is gone (read elsewhere, or the session moved on).
    const live = new Set(snapshot.activities.map(card => card.key));
    for (const key of dismissed) if (!live.has(key)) dismissed.delete(key);
    renderCards(); syncState();
  }
  // value.sheets: the HD sheets of hd/frames.js in order (data: URLs from the native host).
  async function setCharacter(value) {
    const nextArt = PROFILES[value?.kind] ? makeArt(value.kind, value.sheets) : null;
    if (!nextArt) return;
    const revision = ++loadRevision;
    stopPlayback();
    let peekImage = null;
    if (typeof value.peek === 'string') {
      const candidate = new Image();
      candidate.src = value.peek;
      try { await candidate.decode(); peekImage = candidate; } catch { /* docking just stays unavailable */ }
    }
    // The opening frames of the greeting and the idle loop decode before the pet appears.
    // A pet animated from new footage (Rover) brings its own timing table instead of agent.js.
    const raw = PROFILES[value.kind], agent = nextArt.agent || window.ClassicPetAgents[value.kind];
    const first = [raw.idle, raw.greet].flat().map(spec => (typeof spec === 'string' ? spec : spec?.name))
      .flatMap(name => sheetsAhead(nextArt, agent.animations[name]?.frames || []));
    try { await loadAll(nextArt, first); } catch {
      if (revision === loadRevision) {
        deck.replaceChildren(cardElement({ key: 'load-failed', status: 'failed', title: '宠物素材加载失败', body: '请关闭后重新开启', local: true }));
        stack.hidden = false; fitStack(); reportRegions();
      }
      return;
    }
    if (revision !== loadRevision) return;
    // Only animations whose frames were all redrawn are playable.
    kind = value.kind; art = nextArt;
    data = { ...agent, animations: Object.fromEntries(Object.entries(agent.animations).filter(([name]) => art.animations[name])) };
    profile = normalizeProfile(PROFILES[kind]);
    peek = peekImage ? { image: peekImage, fw: peekImage.naturalWidth / 2, fh: peekImage.naturalHeight } : null;
    docked = peek ? windowEdge : ''; peekLean = 0; peekBounce = '';
    hovered = false; hoverPoint = null;
    bandCache = new Map(); images = null; base = 'idle'; variant = ''; queue = []; current = null;
    stateKey = ''; sleeping = false; lastFidget = '';
    // A newly shown pet greets instead of celebrating results that were already waiting.
    seenCards = new Set(); cardsSeeded = false; cardsKey = '';
    endDrag(true);
    lastActivity = performance.now();
    cv.setAttribute('aria-label', NAMES[kind] + '，双击打开 ACECode');
    // Codex greets with a wave when the pet first appears; ours also says how to open the app.
    greetingUntil = performance.now() + timing.greeting;
    clearTimeout(greetingTimer); greetingTimer = setTimeout(renderCards, timing.greeting + 50);
    pendingIntro = profile.greet;
    applySnapshot(snapshot);
    if (pendingIntro) syncState(true);
  }

  // ---- Dragging (directions come from the native window move, see desktop_pet*.cpp/mm) ----
  // Pets without directional drag clips keep their lifted pose (profile.drag) the whole time.
  const dragClip = () => (dragDir < 0 ? profile.dragLeft : dragDir > 0 ? profile.dragRight : null) || profile.drag || profile.idle;
  function playDrag() { play(dragClip(), next); }
  function armDragWatch() { clearTimeout(dragWatchTimer); dragWatchTimer = setTimeout(() => endDrag(), 8000); }
  function beginDrag() {
    if (dragging || performance.now() - dragEndedAt < 300) return;
    setDocked('', { quiet: true });   // picked up from the edge: back to the full figure
    dragging = true; dragDir = 0;
    wake(); armDragWatch();
    if (art && !document.hidden) playDrag();
  }
  function setDragDir(direction) {
    if (!dragging || direction === dragDir) return;
    const leaving = current;
    dragDir = direction;
    if (!art || document.hidden) return;
    // Finish looking back to the front before resting; the clip then continues through next().
    if (direction === 0 && leaving && Number.isInteger(leaving.clip.hold)) { leaving.release = true; return; }
    if (leaving && leaving.clip === dragClip()) return;   // same pose: keep it running, no restart
    playDrag();
  }
  function dragMove(direction) {
    if (!dragging) beginDrag();
    if (!dragging) return;
    armDragWatch();
    clearTimeout(dragPauseTimer);
    dragPauseTimer = setTimeout(() => setDragDir(0), 450);
    setDragDir(direction);
  }
  function endDrag(silent = false) {
    clearTimeout(dragPauseTimer); clearTimeout(dragWatchTimer);
    if (!dragging) return;
    dragging = false; dragDir = 0; dragEndedAt = performance.now();
    lastActivity = dragEndedAt;
    if (silent || !art) return;
    if (document.hidden) { still(); return; }
    if (current && Number.isInteger(current.clip.hold)) current.release = true;
    else next();
  }

  // ---- Pointer interaction ----
  function canvasPoint(event) {
    const r = cv.getBoundingClientRect();
    return { x: (event.clientX - r.left) * SCENE_W / r.width, y: (event.clientY - r.top) * SCENE_H / r.height };
  }
  function overSprite(event, updateAnchor = false) {
    const p = canvasPoint(event);
    if (bodyRegions.some(([l, t, r, b]) => p.x >= l && p.x < r && p.y >= t && p.y < b)) {
      if (updateAnchor) hoverPoint = p;
      return true;
    }
    return Boolean(hovered && hoverPoint && Math.abs(p.x - hoverPoint.x) <= 3 && Math.abs(p.y - hoverPoint.y) <= 3);
  }
  function setHovered(value) {
    clearTimeout(hoverOffTimer);
    if (hovered === value) return;
    hovered = value;
    if (!value) hoverPoint = null;
    reportRegions();
    if (!value || dragging) return;
    const now = performance.now();
    if (now < hoverReadyAt) { wake(); return; }
    hoverReadyAt = now + 3000;
    react(profile?.jumping);
  }
  function hoverOff() { clearTimeout(hoverOffTimer); hoverOffTimer = setTimeout(() => setHovered(false), 150); }
  // No toolbar on the companion pets: pinning, switching pets and hiding live in the
  // native right-click menu; the cards above replace the session picker.
  let press = null, dragged = false;
  cv.addEventListener('pointerdown', event => {
    if (event.button !== 0) return;
    press = { x: event.clientX, y: event.clientY }; dragged = false;
    cv.setPointerCapture(event.pointerId);
  });
  cv.addEventListener('pointermove', event => {
    if (press && (event.buttons & 1)) {
      const dx = event.clientX - press.x;
      if (!dragged && Math.hypot(dx, event.clientY - press.y) >= 4) {
        dragged = true; post('drag'); beginDrag();
        if (Math.abs(dx) >= 4) dragMove(dx > 0 ? 1 : -1);
      }
      return;
    }
    // The native move loop swallows the release; the next free move ends any stale drag pose.
    if (dragging && !(event.buttons & 1)) endDrag();
    if (overSprite(event, true)) {
      setHovered(true); reportRegions();
    } else hoverOff();
  });
  cv.addEventListener('pointerleave', hoverOff);
  cv.addEventListener('pointerup', event => {
    press = null;
    if (cv.hasPointerCapture(event.pointerId)) cv.releasePointerCapture(event.pointerId);
    endDrag();
  });
  cv.addEventListener('pointercancel', () => { press = null; post('gesture-cancel'); });
  cv.addEventListener('lostpointercapture', () => { if (press) { press = null; post('gesture-cancel'); } });
  cv.addEventListener('click', event => {
    if (dragged) return;
    const now = performance.now(), second = now - lastClickAt < 450;
    lastClickAt = now;
    // The second click of a double-click opens ACECode instead of restarting the reaction.
    if (!second && overSprite(event)) react(profile?.click);
  });
  // Double-clicking the pet brings the ACECode window forward (Codex Mini opens its main
  // window the same way); individual sessions open from their cards.
  cv.addEventListener('dblclick', () => {
    if (!dragged) action({ type: 'activate' });
  });
  scene.addEventListener('contextmenu', event => { event.preventDefault(); post('menu'); });
  $('grip').addEventListener('pointerdown', event => { if (event.button === 0) { event.preventDefault(); post('resize'); } });
  $('grip').addEventListener('dblclick', event => { event.preventDefault(); post('size-reset'); });
  let wheel = 0;
  scene.addEventListener('wheel', event => {
    if (event.target.closest('.stack') || !window.chrome?.webview) return;   // the open card list scrolls
    event.preventDefault(); wheel += event.deltaY * (event.deltaMode === 1 ? 33 : 1);
    while (Math.abs(wheel) >= 100) {
      const step = wheel < 0 ? 1 : -1; wheel += step * 100;
      post(`zoom ${step} ${(event.clientX / innerWidth).toFixed(3)} ${(event.clientY / innerHeight).toFixed(3)}`);
    }
  }, { passive: false });

  function onMessage(event) {
    const value = event.data;
    if (value?.type === 'pet-character') { void setCharacter(value); return; }
    if (value?.type === 'pet-window-state') return;   // pinning is shown in the native menu
    if (value?.type === 'pet-dock') { setDocked(value.side); return; }
    if (value?.type === 'pet-drag') {
      if (value.phase === 'start') beginDrag();
      else if (value.phase === 'move') dragMove(value.direction === 'left' ? -1 : value.direction === 'right' ? 1 : 0);
      else if (value.phase === 'end') endDrag();
      return;
    }
    if (typeof value === 'string' && value.startsWith('size ')) {
      $('badge').textContent = value.slice(5) + '%'; $('badge').hidden = false;
      clearTimeout(badgeTimer); badgeTimer = setTimeout(() => { $('badge').hidden = true; }, 900); return;
    }
    applySnapshot(value);
  }
  window.chrome?.webview?.addEventListener('message', onMessage);
  document.addEventListener('visibilitychange', () => {
    if (document.hidden) { stopPlayback(); cancelAnimationFrame(rafId); rafId = 0; } else if (art) next();
  });
  // The canvas holds device pixels: follow window size and display scaling (moving to another monitor).
  function fitCanvas() {
    const r = cv.getBoundingClientRect(), dpr = window.devicePixelRatio || 1;
    const width = Math.max(1, Math.round(r.width * dpr)), height = Math.max(1, Math.round(r.height * dpr));
    if (cv.width === width && cv.height === height) return;
    cv.width = width; cv.height = height;
    render();
  }
  let resolution = null;
  function watchResolution() {
    resolution?.removeEventListener('change', watchResolution);
    resolution = matchMedia(`(resolution: ${window.devicePixelRatio || 1}dppx)`);
    resolution.addEventListener('change', watchResolution);
    fitCanvas();
  }
  window.addEventListener('resize', () => { fitCanvas(); fitStack(); reportRegions(); });
  window.addEventListener('pagehide', () => {
    stopPlayback(); loadRevision++;
    for (const id of [collapseTimer, badgeTimer, fidgetTimer, greetingTimer, dragPauseTimer, dragWatchTimer, hoverOffTimer]) clearTimeout(id);
    cancelAnimationFrame(rafId);
  });
  window.ClassicPet = { applySnapshot, setCharacter,
    play(name) { const clip = clipOf(name); if (clip) play(clip, next); },
    setTiming(value) { Object.assign(timing, value); if (base === 'idle' && !sleeping) planFidget(); },
    get kind() { return kind; }, get animation() { return animation; }, get frame() { return frameIndex; },
    get flipped() { return flipped; }, get motion() { return activeMotion(); }, get base() { return base; },
    get sleeping() { return sleeping; }, get dragging() { return dragging; }, get hovered() { return hovered; },
    get queue() { return queue.map(clip => clip.name); }, get snapshot() { return snapshot; },
    get expanded() { return expanded; }, get docked() { return docked; }, get peekEyes() { return peekEyes; }, get cards() { return shownCards().map(card => ({ key: card.key, status: card.status, title: card.title, body: card.body })); } };
  // Browser-only fixture/asset inspection also works without a native host.
  if (!window.chrome?.webview) {
    const selected = new URLSearchParams(location.search).get('pet');
    const folder = selected === 'whale' ? './whale' : `./classic/${selected}`;
    if (PROFILES[selected]) void setCharacter({ kind: selected, peek: `${folder}/peek.png`,
      sheets: (window.ClassicPetHD?.[selected]?.sheets || []).map((_, i) => `${folder}/hd/sheet-${String(i).padStart(2, '0')}.webp`) });
  }
  watchResolution(); reportRegions(); post('ready');
})();
