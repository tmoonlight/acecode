// Crystal ball companion pet: a solid glass sphere that refracts the desktop behind it, with two eyes
// that blink, glance around and follow the pointer. On Windows the native host
// (src/apps/desktop/desktop_pet_crystal_win.cpp) hides this window from screen capture, grabs the screen
// around the ball and hands it over through a WebView2 shared buffer ("crystal-frame"); without host
// frames (macOS, remote desktop sessions, browser fixtures) the ball refracts a soft built-in backdrop.
// Session cards, dragging, the native menu and resizing follow the other companion pets (classic_pet.js).
(() => {
  'use strict';
  const $ = id => document.getElementById(id);
  const cv = $('cv'), scene = $('scene'), stack = $('stack'), deck = $('deck'), grip = $('grip');
  const SCENE_W = 344, SCENE_H = 252;
  // The ball in scene units; the window keeps the 344x252 layout of the other companion pets.
  const BALL = { x: 172, y: 166, r: 60 };
  const BOB = 0.07 * BALL.r, BOUNCE = 0.12 * BALL.r;
  const host = window.chrome?.webview;
  const post = value => host?.postMessage(value);
  const action = value => post(JSON.stringify(value));
  const timing = { greeting: 6000, sleepAfter: 5 * 60 * 1000 };
  const rand = (min, max) => min + Math.random() * (max - min);
  const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));

  // ---- Renderer (WebGL2: everything is computed per pixel on the GPU) ----
  const VERT = `#version 300 es
void main() { vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2)); gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }`;
  const FRAG = `#version 300 es
precision highp float;
uniform vec2 uSize;
uniform vec3 uBall;   // center (device px, top-left origin) and radius
uniform vec3 uRest;   // resting center and lift 0..1 (the shadow stays on the ground)
uniform sampler2D uBg;
uniform vec4 uCap;    // captured screen rectangle in canvas px
uniform float uHasBg, uCapR, uTime, uWiden, uDisp;
uniform vec4 uEye;    // yaw, pitch, open, happy
uniform vec4 uGlow;   // inner mist color and strength
out vec4 outColor;
const float IOR = 1.5, DIST = 3.5, F0 = 0.04;
const vec3 LS = normalize(vec3(-0.50, 0.62, 0.60));
const vec3 LP = normalize(vec3(-0.62, 0.70, 0.36));

vec3 backdrop(vec2 q) {
  vec2 u = q / uSize;
  vec3 c = mix(vec3(0.80, 0.86, 0.95), vec3(0.96, 0.92, 0.86), smoothstep(0.15, 0.95, u.y));
  return c + 0.05 * vec3(0.4, 0.5, 1.0) * (1.0 - smoothstep(0.0, 0.5, length(u - vec2(0.3, 0.2))));
}
// The host uploads BGRA bytes as RGBA.
vec3 bgSample(vec2 q) { return uHasBg < 0.5 ? backdrop(q) : texture(uBg, (q - uCap.xy) / uCap.zw).bgr; }
vec3 bgBlur(vec2 q, float lod) { return uHasBg < 0.5 ? backdrop(q) : textureLod(uBg, (q - uCap.xy) / uCap.zw, lod).bgr; }
vec3 bgAverage() { return uHasBg < 0.5 ? vec3(0.85, 0.87, 0.90) : textureLod(uBg, vec2(0.5), 14.0).bgr; }
// Offsets beyond the captured area are compressed smoothly back into it, so the image stays continuous to the rim.
vec2 squash(vec2 off) {
  float m = length(off), knee = 0.55 * uCapR;
  if (m <= knee) return off;
  float span = uCapR - knee;
  return off * ((knee + span * (1.0 - exp(-(m - knee) / span))) / m);
}
// Solid glass ball: refract in, cross the ball, refract out, land on the background plane (radius units, y up).
vec2 throughBall(vec3 N, float n, out float chord) {
  vec3 T1 = refract(vec3(0.0, 0.0, -1.0), N, 1.0 / n);
  chord = -2.0 * dot(N, T1);
  vec3 Q = N + T1 * chord;
  vec3 T2 = refract(T1, -Q, n);
  if (dot(T2, T2) < 1e-6) T2 = reflect(T1, -Q);
  if (T2.z < -0.03) return Q.xy + T2.xy * ((-DIST - Q.z) / T2.z);
  return Q.xy + normalize(T2.xy + vec2(1e-5)) * 50.0;
}
// Studio lighting seen in the glass: a window-shaped soft box and one sharp highlight.
vec3 env(vec3 R) {
  float g = R.y >= 0.0 ? 0.30 + 0.30 * sqrt(R.y) : 0.30 + 0.22 * R.y;
  vec3 col = g * vec3(0.97, 0.99, 1.03);
  vec3 e1 = normalize(cross(LS, vec3(0.0, 1.0, 0.0))), e2 = cross(e1, LS);
  float c = dot(R, LS);
  if (c > 0.2) {
    vec2 ab = vec2(dot(R, e1), dot(R, e2)) / c;
    vec2 q = abs(ab) - vec2(0.50, 0.36) + 0.12;
    float sd = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - 0.12;
    float bars = smoothstep(0.010, 0.024, abs(ab.x)) * smoothstep(0.010, 0.024, abs(ab.y));
    col += vec3(7.5 * (1.0 - smoothstep(-0.05, 0.05, sd)) * (0.40 + 0.60 * bars));
  }
  float sp = max(dot(R, LP), 0.0);
  return col + vec3(1.0, 0.985, 0.96) * (pow(sp, 1600.0) * 50.0 + pow(sp, 40.0) * 2.0);
}
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
float noise(vec2 p) {
  vec2 i = floor(p), f = fract(p), u = f * f * (3.0 - 2.0 * f);
  return mix(mix(hash(i), hash(i + vec2(1.0, 0.0)), u.x), mix(hash(i + vec2(0.0, 1.0)), hash(i + vec2(1.0, 1.0)), u.x), u.y);
}
// Swirling mist inside the ball while a session works or waits.
float mist(vec2 d) {
  float a = uTime * 0.35, v = 0.0, w = 0.5;
  vec2 q = mat2(cos(a), -sin(a), sin(a), cos(a)) * d * 2.2;
  for (int i = 0; i < 4; i++) { v += w * noise(q + vec2(uTime * 0.15, -uTime * 0.1)); q = q * 2.03 + vec2(1.7, 9.2); w *= 0.5; }
  return smoothstep(0.35, 0.85, v) * (1.0 - smoothstep(0.15, 0.95, length(d)));
}
float sdArc(vec2 p, vec2 sc, float ra) { p.x = abs(p.x); return sc.y * p.x > sc.x * p.y ? length(p - sc * ra) : abs(length(p) - ra); }
// Two eyes on the sphere surface, shaped in each eye's own tangent plane so they foreshorten as the face turns.
vec3 eyes(vec3 base, vec3 S, float R, vec3 gloss) {
  float yaw = uEye.x, pitch = uEye.y, open = uEye.z, happy = uEye.w;
  vec3 up = vec3(0.0, 1.0, 0.0);
  vec3 Fd = vec3(sin(yaw) * cos(pitch), sin(pitch), cos(yaw) * cos(pitch));
  vec3 Tf = normalize(cross(up, Fd)), Uf = cross(Fd, Tf);
  float w = 0.092 * (1.0 + 0.08 * uWiden), hF = 0.145 * (1.0 + 0.15 * uWiden);
  float hE = 0.26 * w + (hF - 0.26 * w) * open, bOff = -(hF - hE) * 0.55;
  float catchK = smoothstep(0.45, 0.9, open) * (1.0 - happy);
  for (int i = 0; i < 2; i++) {
    vec3 Pe = normalize(Fd + Tf * (i == 0 ? -0.27 : 0.27) - Uf * 0.05);
    if (Pe.z < 0.15 || dot(S, Pe) < 0.6) continue;
    vec3 Te = normalize(cross(up, Pe)), Ue = cross(Pe, Te), D = S - Pe;
    float a = dot(D, Te), b0 = dot(D, Ue), b = b0 - bOff, ppu = R * max(S.z, 0.35);
    float k = length(vec2(a / w, b / hE));
    float grad = length(vec2(a / (w * w), b / (hE * hE))) / max(k, 1e-5);
    float shape = (k - 1.0) / max(grad, 1e-5);
    float arc = sdArc(vec2(a, b0 + 0.42 * hF), vec2(sin(1.0), cos(1.0)), w) - 0.34 * w;
    float dp = mix(shape, arc, happy) * ppu;
    float ea = clamp(0.5 - dp, 0.0, 1.0);
    float halo = 0.62 * (1.0 - smoothstep(1.2, 4.0, max(dp, 0.0)));
    float lower = smoothstep(0.25, 1.0, clamp(-b / hE, 0.0, 1.0)) * 0.85 * (1.0 - happy);
    vec3 ink = (vec3(24.0, 26.0, 34.0) + vec3(34.0, 50.0, 80.0) * lower) / 255.0;
    float c1 = (length(vec2(a + 0.32 * w, b - 0.36 * hE)) - 0.30 * w) * ppu;
    float c2 = (length(vec2(a - 0.30 * w, b + 0.40 * hE)) - 0.12 * w) * ppu;
    ink = mix(ink, vec3(1.0), max(clamp(0.5 - c1, 0.0, 1.0) * 0.95, clamp(0.5 - c2, 0.0, 1.0) * 0.6) * catchK);
    base = mix(base, vec3(1.0), halo);
    base = mix(base, min(ink + gloss, vec3(1.0)), ea);
  }
  return base;
}
// Soft ground shadow with the bright spot a glass ball focuses into it.
vec4 shadow(vec2 p) {
  float R = uBall.z, s = 1.0 - 0.15 * uRest.z, k = 1.0 - 0.30 * uRest.z;
  vec2 g = vec2(uRest.x, uRest.y + 1.12 * R);
  vec2 e = (p - g - vec2(0.10 * R, 0.0)) / (vec2(0.75, 0.13) * R * s);
  vec2 q = (p - g - vec2(0.22 * R, 0.0)) / (vec2(0.26, 0.05) * R * s);
  float sa = 0.30 * k * exp(-2.5 * dot(e, e)), ca = 0.28 * k * exp(-2.2 * dot(q, q));
  return vec4(vec3(1.0, 0.97, 0.88) * ca, ca + sa * (1.0 - ca));
}
void main() {
  vec2 p = vec2(gl_FragCoord.x, uSize.y - gl_FragCoord.y);
  vec2 c = uBall.xy;
  float R = uBall.z, dist = length(p - c), cover = clamp(R - dist + 0.5, 0.0, 1.0);
  vec2 d = (p - c) / R;
  float r = length(d);
  if (r > 0.997) { d *= 0.997 / r; r = 0.997; }
  vec3 N = vec3(d.x, -d.y, sqrt(max(0.0, 1.0 - r * r)));
  float F = F0 + (1.0 - F0) * pow(1.0 - N.z, 5.0);
  float chordR, chordG, chordB;
  vec2 hr = throughBall(N, IOR - 0.011 * uDisp, chordR);
  vec2 hg = throughBall(N, IOR, chordG);
  vec2 hb = throughBall(N, IOR + 0.016 * uDisp, chordB);
  vec3 refr = vec3(bgSample(c + squash(vec2(hr.x, -hr.y) * R)).r,
                   bgSample(c + squash(vec2(hg.x, -hg.y) * R)).g,
                   bgSample(c + squash(vec2(hb.x, -hb.y) * R)).b);
  float rim = smoothstep(1.0 - 4.0 / R, 1.0, dist / R) * 0.35;
  vec3 col = refr * (1.0 - F) * exp(-vec3(0.035, 0.018, 0.010) * chordG) * (1.0 - rim)
           + (bgAverage() * 0.62 + 6.0 / 255.0) * (1.0 - F) * rim;
  vec3 Rv = reflect(vec3(0.0, 0.0, -1.0), N);
  float rw = 1.0 - smoothstep(-0.25, -0.01, Rv.z);
  vec2 rh = N.xy + Rv.xy * ((-DIST - N.z) / min(Rv.z, -0.01));
  vec3 mirror = bgBlur(c + squash(vec2(rh.x, -rh.y) * R), 3.0);
  float rr = length(N.xy);
  float crescent = pow(max(dot(N.xy / max(rr, 1e-4), vec2(0.628, -0.778)), 0.0), 3.0)
                 * smoothstep(0.5, 0.88, rr) * (1.0 - smoothstep(0.9, 0.985, rr)) * 0.24 * (1.0 - F);
  vec3 gloss = F * (1.0 - rw) * env(Rv) + crescent * vec3(1.0, 0.96, 0.88);
  col += F * rw * 0.9 * mirror + gloss;
  if (uGlow.a > 0.001) col += mist(d) * uGlow.rgb * uGlow.a * (1.0 - F);
  if (r < 0.992) col = eyes(col, N, R, gloss);
  col = min(col, vec3(1.0));
  vec4 sh = shadow(p);
  outColor = vec4(col * cover + sh.rgb * (1.0 - cover), cover + sh.a * (1.0 - cover));
}`;

  const gl = cv.getContext('webgl2', { alpha: true, premultipliedAlpha: true, antialias: false });
  const flat = gl ? null : cv.getContext('2d');
  let program = null, uniforms = {}, bgTex = null;
  function buildProgram() {
    const shader = (type, source) => {
      const s = gl.createShader(type);
      gl.shaderSource(s, source); gl.compileShader(s);
      if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(s) || 'shader');
      return s;
    };
    const p = gl.createProgram();
    gl.attachShader(p, shader(gl.VERTEX_SHADER, VERT)); gl.attachShader(p, shader(gl.FRAGMENT_SHADER, FRAG));
    gl.linkProgram(p);
    if (!gl.getProgramParameter(p, gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(p) || 'link');
    for (const name of ['uSize', 'uBall', 'uRest', 'uBg', 'uCap', 'uHasBg', 'uCapR', 'uTime', 'uWiden', 'uDisp', 'uEye', 'uGlow'])
      uniforms[name] = gl.getUniformLocation(p, name);
    bgTex = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, bgTex);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR_MIPMAP_LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, 1, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE, new Uint8Array(4));
    gl.generateMipmap(gl.TEXTURE_2D);
    program = p;
  }
  let renderError = '';
  if (gl) { try { buildProgram(); } catch (error) { renderError = String(error?.message || error); program = null; } }

  // Background frames from the native host: [x, y, w, h] in canvas px once received.
  let shared = null, hasBg = false, capRect = null, framesSeen = 0;
  host?.addEventListener('sharedbufferreceived', event => {
    if (shared) host.releaseBuffer?.(shared);
    shared = event.getBuffer();
  });
  function uploadFrame(bytes, w, h) {
    if (!program) return false;
    gl.bindTexture(gl.TEXTURE_2D, bgTex);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 4);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, w, h, 0, gl.RGBA, gl.UNSIGNED_BYTE, bytes);
    gl.generateMipmap(gl.TEXTURE_2D);
    return true;
  }
  function onFrame(value) {
    const w = Math.floor(value.w), h = Math.floor(value.h);
    if (shared && w > 0 && h > 0 && w * h * 4 <= shared.byteLength && value.ww > 0 && value.wh > 0 &&
        uploadFrame(new Uint8Array(shared, 0, w * h * 4), w, h)) {
      const sx = cv.width / value.ww, sy = cv.height / value.wh;
      capRect = [value.x * sx, value.y * sy, w * sx, h * sy];
      hasBg = true; framesSeen++;
    }
    // Acknowledge even a frame that could not be used: the host captures the next one only after this.
    action({ type: 'crystal-ack', seq: value.seq });
  }

  // ---- State ----
  let snapshot = { version: 1, follow: true, connected: false, agents: [], offices: [], selected: null, activities: [] };
  const eye = { yaw: 0, pitch: 0, vYaw: 0, vPitch: 0, tYaw: 0, tPitch: 0, open: 1, happy: 0, widen: 0,
    nextWander: 800, nextBlink: 1500, blinkAt: -1, blinksLeft: 0, happyUntil: 0, sadUntil: 0 };
  const glow = { r: 0.62, g: 0.5, b: 1, a: 0, flash: null };
  let cursor = null, cursorMovedAt = -1e9, mood = { base: 'idle', variant: '' };
  let lift = 0, bounceAt = -1e9, shakeAt = -1e9, lastFrameAt = 0, rafId = 0, startedAt = performance.now();
  let dragging = false, dragDir = 0, dragEndedAt = 0, dragPauseTimer = 0, dragWatchTimer = 0;
  let hovered = false, hoverOffTimer = 0, lastClickAt = 0, sleeping = false, lastActivity = performance.now();
  let seenCards = new Set(), cardsSeeded = false, dismissed = new Set(), expanded = false, collapseTimer = 0, cardsKey = '';
  let greetingUntil = 0, greetingTimer = 0, overlayKey = '', badgeTimer = 0;

  const ballAt = now => {
    const since = now - bounceAt, shook = now - shakeAt;
    const hop = since < 900 ? Math.abs(Math.sin(since / 90)) * BOUNCE * (1 - since / 900) : 0;
    const dx = shook < 700 ? Math.sin(shook / 28) * 3 * (1 - shook / 700) : 0;
    return { x: BALL.x + dx, y: BALL.y - lift - hop };
  };
  function wake() {
    lastActivity = performance.now();
    if (sleeping) { sleeping = false; eye.nextBlink = lastActivity + 400; }
  }

  // ---- Eyes: glance around, follow the pointer, blink, squint when happy ----
  function updateEyes(now, dt) {
    const c = ballAt(now), R = BALL.r;
    const dx = cursor ? cursor.x - c.x : 0, dy = cursor ? cursor.y - c.y : 0, d = Math.hypot(dx, dy);
    const hover = !dragging && (hovered || (cursor && d < R));
    const attend = !dragging && cursor && now - cursorMovedAt < 2500 && d < 10 * R;
    if (!sleeping && mood.base === 'idle' && now - lastActivity >= timing.sleepAfter) sleeping = true;
    if (hover || attend) wake();
    if (sleeping) { eye.tYaw = 0; eye.tPitch = -0.12; }
    else if (dragging) { eye.tYaw = dragDir * 0.32; eye.tPitch = 0.1; eye.nextWander = now + 1000; }
    else if (now < eye.sadUntil) { eye.tYaw = 0; eye.tPitch = -0.28; }
    else if (hover) { eye.tYaw = dx / R * 0.28; eye.tPitch = -dy / R * 0.2; eye.nextWander = now + rand(1000, 2000); }
    else if (attend) {
      const k = 1 / (d + 1.2 * R);
      eye.tYaw = dx * k * 0.45; eye.tPitch = -dy * k * 0.32; eye.nextWander = now + rand(1200, 2400);
    } else if (mood.base === 'waiting') { eye.tYaw = 0; eye.tPitch = 0.34; }
    else if (now >= eye.nextWander) {
      if (mood.base === 'running') {   // thinking: quick glances, mostly upward
        eye.tYaw = rand(-0.35, 0.35); eye.tPitch = rand(0.08, 0.32); eye.nextWander = now + rand(700, 1800);
      } else {
        if (Math.random() < 0.3) { eye.tYaw = 0; eye.tPitch = 0; } else { eye.tYaw = rand(-0.42, 0.42); eye.tPitch = rand(-0.26, 0.3); }
        eye.nextWander = now + rand(1000, 3400);
      }
    }
    // Critically damped spring: quick like a saccade, then still.
    const w = 17, steps = Math.max(1, Math.ceil(dt / 0.008)), h = dt / steps;
    for (let i = 0; i < steps; i++) {
      eye.vYaw += (w * w * (eye.tYaw - eye.yaw) - 2 * w * eye.vYaw) * h; eye.yaw += eye.vYaw * h;
      eye.vPitch += (w * w * (eye.tPitch - eye.pitch) - 2 * w * eye.vPitch) * h; eye.pitch += eye.vPitch * h;
    }
    // Blink: close 70ms, hold 40ms, open 110ms; now and then twice.
    const happyNow = dragging || now < eye.happyUntil;
    if (eye.blinkAt < 0 && now >= eye.nextBlink && eye.happy < 0.3 && !sleeping) {
      eye.blinkAt = now; eye.blinksLeft = Math.random() < 0.22 ? 1 : 0;
    }
    let closed = 0;
    if (eye.blinkAt >= 0) {
      const t = now - eye.blinkAt;
      if (t < 0) closed = 0;
      else if (t < 70) closed = t / 70;
      else if (t < 110) closed = 1;
      else if (t < 220) closed = 1 - (t - 110) / 110;
      else if (eye.blinksLeft > 0) { eye.blinksLeft--; eye.blinkAt = now + 70; }
      else { eye.blinkAt = -1; eye.nextBlink = now + rand(2000, 5500); }
    }
    const ease = (value, target, rate) => value + (target - value) * (1 - Math.exp(-dt * rate));
    eye.open = sleeping ? ease(eye.open, 0, 3) : Math.min(ease(eye.open, 1, 12), 1 - closed * closed * (3 - 2 * closed));
    eye.happy = ease(eye.happy, happyNow ? 1 : 0, 14);
    eye.widen = ease(eye.widen, hover ? 1 : mood.base === 'waiting' ? 0.6 : 0, 9);
    // Inner mist: violet while working, amber while waiting for the user, a short flash for results.
    let target = { r: 0.62, g: 0.5, b: 1, a: 0 };
    if (mood.base === 'running') target = { r: 0.55, g: 0.42, b: 1, a: 0.62 + 0.18 * Math.sin(now / 600) };
    else if (mood.base === 'waiting') target = { r: 1, g: 0.68, b: 0.28, a: 0.6 + 0.22 * Math.sin(now / 260) };
    if (glow.flash && now < glow.flash.until) target = glow.flash.color;
    if (sleeping) target = { ...target, a: 0 };
    for (const key of ['r', 'g', 'b', 'a']) glow[key] = ease(glow[key], target[key], 4);
    // Gentle floating; resting on the hand while carried.
    const bob = dragging || sleeping ? 0 : BOB * (0.5 - 0.5 * Math.cos(now / 3800 * 2 * Math.PI));
    lift = ease(lift, sleeping ? BOB * 0.2 * (0.5 - 0.5 * Math.cos(now / 5200 * 2 * Math.PI)) : bob, 10);
  }

  // ---- Drawing ----
  function draw(now) {
    const sx = cv.width / SCENE_W, c = ballAt(now), R = BALL.r * sx;
    if (!program) { drawFlat(c, sx); return; }
    gl.viewport(0, 0, cv.width, cv.height);
    gl.clearColor(0, 0, 0, 0); gl.clear(gl.COLOR_BUFFER_BIT);
    gl.useProgram(program);
    const u = uniforms;
    gl.uniform2f(u.uSize, cv.width, cv.height);
    gl.uniform3f(u.uBall, c.x * sx, c.y * sx, R);
    gl.uniform3f(u.uRest, BALL.x * sx, BALL.y * sx, clamp(lift / BOB, 0, 1));
    gl.activeTexture(gl.TEXTURE0); gl.bindTexture(gl.TEXTURE_2D, bgTex); gl.uniform1i(u.uBg, 0);
    const cap = hasBg && capRect ? capRect : [0, 0, cv.width, cv.height];
    gl.uniform4f(u.uCap, cap[0], cap[1], cap[2], cap[3]);
    gl.uniform1f(u.uHasBg, hasBg ? 1 : 0);
    // Squash radius: distance from the ball to the nearest edge of the captured area.
    const reach = hasBg ? Math.min(c.x * sx - cap[0], cap[0] + cap[2] - c.x * sx, c.y * sx - cap[1], cap[1] + cap[3] - c.y * sx) - 2 : 3.2 * R;
    gl.uniform1f(u.uCapR, Math.max(1.6 * R, reach));
    gl.uniform1f(u.uTime, (now - startedAt) / 1000);
    gl.uniform1f(u.uWiden, eye.widen);
    gl.uniform1f(u.uDisp, 1);
    gl.uniform4f(u.uEye, eye.yaw, eye.pitch, eye.open, eye.happy);
    gl.uniform4f(u.uGlow, glow.r, glow.g, glow.b, glow.a);
    gl.drawArrays(gl.TRIANGLES, 0, 3);
  }
  // Without WebGL2: a plain shaded sphere with the same eyes, so the pet never disappears.
  function drawFlat(c, sx) {
    if (!flat) return;
    const x = c.x * sx, y = c.y * sx, R = BALL.r * sx;
    flat.setTransform(1, 0, 0, 1, 0, 0); flat.clearRect(0, 0, cv.width, cv.height);
    const shade = flat.createRadialGradient(x - R * 0.35, y - R * 0.4, R * 0.1, x, y, R);
    shade.addColorStop(0, 'rgba(255,255,255,0.95)'); shade.addColorStop(0.6, 'rgba(205,220,240,0.9)'); shade.addColorStop(1, 'rgba(120,140,170,0.95)');
    flat.fillStyle = shade; flat.beginPath(); flat.arc(x, y, R, 0, Math.PI * 2); flat.fill();
    flat.fillStyle = '#181a22';
    for (const side of [-1, 1]) {
      flat.beginPath();
      flat.ellipse(x + (side * 0.27 + eye.yaw * 0.9) * R, y + (0.05 - eye.pitch * 0.9) * R, 0.09 * R, Math.max(0.03, 0.14 * eye.open * (1 - eye.happy * 0.7)) * R, 0, 0, Math.PI * 2);
      flat.fill();
    }
  }
  function frame(now) {
    rafId = 0;
    const dt = Math.min(0.1, Math.max(0, (now - (lastFrameAt || now)) / 1000));
    lastFrameAt = now;
    updateEyes(now, dt);
    draw(now);
    if (!document.hidden) rafId = requestAnimationFrame(frame);
  }
  const startLoop = () => { if (!rafId && !document.hidden) { lastFrameAt = 0; rafId = requestAnimationFrame(frame); } };

  // ---- Window region, resize grip and the lens report for the native capture ----
  const normalized = r => [r.left / innerWidth, r.top / innerHeight, r.right / innerWidth, r.bottom / innerHeight];
  // Bands follow the ball over its whole floating range, so the region does not change every frame.
  function bodyBands() {
    const top = BALL.y - BOB - BOUNCE, bands = [];
    for (let y = Math.floor(top - BALL.r); y < BALL.y + BALL.r; y += 8) {
      const near = clamp(y + 4, top, BALL.y), dy = Math.max(0, Math.abs(y + 4 - near) - 4);
      if (dy >= BALL.r) continue;
      const hw = Math.sqrt(BALL.r * BALL.r - dy * dy) + 4;
      bands.push([Math.max(0, BALL.x - hw), Math.max(0, y), Math.min(SCENE_W, BALL.x + hw), Math.min(SCENE_H, y + 8)]);
    }
    return bands;
  }
  const BANDS = bodyBands();
  function reportRegions() {
    const r = cv.getBoundingClientRect();
    if (!(r.width > 0)) return;
    const toWindow = b => [b[0] / SCENE_W, b[1] / SCENE_H, b[2] / SCENE_W, b[3] / SCENE_H];
    if (!grip.matches(':hover')) {
      const belowCards = stack.hidden || expanded ? 0 : stack.getBoundingClientRect().bottom;
      const left = (BALL.x - BALL.r * 0.78) * r.width / SCENE_W - grip.offsetWidth;
      const top = Math.max(0, belowCards, (BALL.y - BALL.r * 0.78) * r.height / SCENE_H - grip.offsetHeight);
      grip.hidden = false;
      grip.style.left = Math.max(0, left) + 'px';
      grip.style.top = Math.min(Math.max(0, innerHeight - grip.offsetHeight), top) + 'px';
    }
    const message = { type: 'overlay', controls: null, rect: stack.hidden ? null : normalized(stack.getBoundingClientRect()),
      grip: normalized(grip.getBoundingClientRect()), bubbles: BANDS.map(toWindow) };
    const key = JSON.stringify(message);
    if (key !== overlayKey) { overlayKey = key; action(message); }
  }
  const reportLens = () => action({ type: 'crystal-lens', x: BALL.x / SCENE_W, y: BALL.y / SCENE_H, r: BALL.r / SCENE_W });

  // ---- Session cards (same deck as the other companion pets) ----
  const RESULT = window.PetActivityCards.results;
  const sessionCards = () => (snapshot.connected ? snapshot.activities || [] : []).filter(card => !dismissed.has(card.key));
  function shownCards() {
    const cards = [];
    if (performance.now() < greetingUntil)
      cards.push({ key: 'greeting', status: 'info', title: '你好，我是水晶球', body: '双击我就能打开 ACECode', local: true });
    if (!snapshot.connected && performance.now() - startedAt > 1500)
      cards.push({ key: 'offline', status: 'info', title: '连接已中断', body: '正在重新连接…', local: true });
    return cards.concat(sessionCards());
  }
  const cardElement = card => window.PetActivityCards.create(card, {
    open: activateCard, dismiss: dismissCard, collapse: () => setExpanded(false),
    stop: value => action({ type: 'stop', session_id: value.sessionId, workspace_hash: value.workspaceHash || '' }),
  }).element;
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
  // Shrink the collapsed stack (not below 9px text) or pin it to the top edge in a small window.
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
  function activateCard(card) {
    if (!expanded && shownCards().length > 1) { setExpanded(true); return; }
    if (card.local) { dismissCard(card); return; }
    action({ type: 'open', session_id: card.sessionId, workspace_hash: card.workspaceHash || '' });
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

  // Mood follows the cards: waiting for the user outranks work; fresh results get a one-off reaction.
  function moodOf(cards) {
    if (cards.some(card => card.status === 'waiting')) return { base: 'waiting', variant: '' };
    const running = cards.find(card => card.status === 'running');
    return running ? { base: 'running', variant: running.state || 'work' } : { base: 'idle', variant: '' };
  }
  function celebrate(now = performance.now()) {
    eye.happyUntil = now + 1600; bounceAt = now;
    glow.flash = { until: now + 1600, color: { r: 0.55, g: 1, b: 0.62, a: 0.5 } };
  }
  function sulk(now = performance.now()) {
    eye.sadUntil = now + 1400; shakeAt = now;
    glow.flash = { until: now + 1200, color: { r: 1, g: 0.35, b: 0.35, a: 0.5 } };
  }
  function syncState() {
    const cards = sessionCards();
    const fresh = cards.filter(card => !seenCards.has(card.key)), first = !cardsSeeded;
    cardsSeeded = true;
    for (const card of cards) seenCards.add(card.key);
    for (const old of seenCards) { if (seenCards.size <= 400) break; seenCards.delete(old); }
    const next = moodOf(cards);
    if (next.base !== mood.base) wake();
    mood = next;
    if (!first && fresh.some(card => card.status === 'review')) { wake(); celebrate(); }
    else if (!first && fresh.some(card => card.status === 'failed')) { wake(); sulk(); }
  }
  function applySnapshot(value) {
    if (value?.version !== 1 || !Array.isArray(value.agents) || !Array.isArray(value.offices)) return;
    snapshot = { ...value, activities: Array.isArray(value.activities) ? value.activities : [] };
    if (window.PetActivityCards.applyTranscriptFont(value.transcriptFont)) fitStack();
    const live = new Set(snapshot.activities.map(card => card.key));
    for (const key of dismissed) if (!live.has(key)) dismissed.delete(key);
    renderCards(); syncState();
  }

  // ---- Dragging (directions come from the native window move, see desktop_pet*.cpp/mm) ----
  function armDragWatch() { clearTimeout(dragWatchTimer); dragWatchTimer = setTimeout(() => endDrag(), 8000); }
  function beginDrag() {
    if (dragging || performance.now() - dragEndedAt < 300) return;
    dragging = true; dragDir = 0; wake(); armDragWatch();
  }
  function dragMove(direction) {
    if (!dragging) beginDrag();
    if (!dragging) return;
    armDragWatch();
    clearTimeout(dragPauseTimer);
    dragPauseTimer = setTimeout(() => { dragDir = 0; }, 450);
    dragDir = direction;
  }
  function endDrag() {
    clearTimeout(dragPauseTimer); clearTimeout(dragWatchTimer);
    if (!dragging) return;
    dragging = false; dragDir = 0; dragEndedAt = performance.now(); lastActivity = dragEndedAt;
    eye.happyUntil = dragEndedAt + 400;
  }

  // ---- Pointer interaction ----
  function scenePoint(event) {
    const r = cv.getBoundingClientRect();
    return { x: (event.clientX - r.left) * SCENE_W / r.width, y: (event.clientY - r.top) * SCENE_H / r.height };
  }
  const overBall = event => {
    const p = scenePoint(event), c = ballAt(performance.now());
    return Math.hypot(p.x - c.x, p.y - c.y) <= BALL.r + 2;
  };
  function setHovered(value) {
    clearTimeout(hoverOffTimer);
    if (hovered === value) return;
    hovered = value;
    if (value) wake();
  }
  const hoverOff = () => { clearTimeout(hoverOffTimer); hoverOffTimer = setTimeout(() => setHovered(false), 150); };
  let press = null, dragged = false;
  cv.addEventListener('pointerdown', event => {
    if (event.button !== 0) return;
    press = { x: event.clientX, y: event.clientY }; dragged = false;
    cv.setPointerCapture(event.pointerId);
  });
  cv.addEventListener('pointermove', event => {
    if (!host) { cursor = scenePoint(event); cursorMovedAt = performance.now(); }
    if (press && (event.buttons & 1)) {
      const dx = event.clientX - press.x;
      if (!dragged && Math.hypot(dx, event.clientY - press.y) >= 4) {
        dragged = true; post('drag'); beginDrag();
        if (Math.abs(dx) >= 4) dragMove(dx > 0 ? 1 : -1);
      }
      return;
    }
    if (dragging && !(event.buttons & 1)) endDrag();
    if (overBall(event)) setHovered(true); else hoverOff();
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
    if (!second && overBall(event)) { wake(); eye.happyUntil = now + 900; bounceAt = now; }
  });
  cv.addEventListener('dblclick', () => { if (!dragged) action({ type: 'activate' }); });
  scene.addEventListener('contextmenu', event => { event.preventDefault(); post('menu'); });
  grip.addEventListener('pointerdown', event => { if (event.button === 0) { event.preventDefault(); post('resize'); } });
  grip.addEventListener('dblclick', event => { event.preventDefault(); post('size-reset'); });
  let wheel = 0;
  scene.addEventListener('wheel', event => {
    if (event.target.closest('.stack') || !host) return;
    event.preventDefault(); wheel += event.deltaY * (event.deltaMode === 1 ? 33 : 1);
    while (Math.abs(wheel) >= 100) {
      const step = wheel < 0 ? 1 : -1; wheel += step * 100;
      post(`zoom ${step} ${(event.clientX / innerWidth).toFixed(3)} ${(event.clientY / innerHeight).toFixed(3)}`);
    }
  }, { passive: false });

  function onMessage(event) {
    const value = event.data;
    if (value?.type === 'crystal-frame') { onFrame(value); return; }
    if (value?.type === 'crystal-cursor') {
      if (Number.isFinite(value.x) && Number.isFinite(value.y)) {
        const next = { x: value.x * SCENE_W, y: value.y * SCENE_H };
        if (!cursor || Math.hypot(next.x - cursor.x, next.y - cursor.y) > 0.5) cursorMovedAt = performance.now();
        cursor = next;
      }
      return;
    }
    if (value?.type === 'crystal-off') { hasBg = false; return; }
    if (value?.type === 'pet-window-state' || value?.type === 'pet-dock' || value?.type === 'pet-character') return;
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
  host?.addEventListener('message', onMessage);
  document.addEventListener('visibilitychange', () => { if (document.hidden) { cancelAnimationFrame(rafId); rafId = 0; } else startLoop(); });
  function fitCanvas() {
    const r = cv.getBoundingClientRect(), dpr = window.devicePixelRatio || 1;
    const width = Math.max(1, Math.round(r.width * dpr)), height = Math.max(1, Math.round(r.height * dpr));
    if (cv.width === width && cv.height === height) return;
    // A host frame is laid out for the old canvas size; the next frame (a few ms away) replaces it.
    if (capRect) { const kx = width / cv.width, ky = height / cv.height; capRect = [capRect[0] * kx, capRect[1] * ky, capRect[2] * kx, capRect[3] * ky]; }
    cv.width = width; cv.height = height;
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
    for (const id of [collapseTimer, badgeTimer, greetingTimer, dragPauseTimer, dragWatchTimer, hoverOffTimer]) clearTimeout(id);
    cancelAnimationFrame(rafId);
    if (shared) host?.releaseBuffer?.(shared);
  });

  // Browser fixtures: ?backdrop=grid fakes a captured screen (BGRA, like the host) to check the refraction.
  function fixtureBackdrop() {
    const w = 640, h = 640, paint = document.createElement('canvas');
    paint.width = w; paint.height = h;
    const g = paint.getContext('2d');
    g.fillStyle = '#faf8f0'; g.fillRect(0, 0, w, h);
    const colors = ['#e63946', '#f4a261', '#2a9d8f', '#264653', '#8338ec', '#3a86ff'];
    for (let i = 0; i < 16; i++) { g.fillStyle = colors[i % 6]; g.fillRect(i * 40, 0, 40, 60); }
    g.strokeStyle = '#3c3c46'; g.lineWidth = 2;
    for (let k = 0; k <= w; k += 32) { g.beginPath(); g.moveTo(k, 60); g.lineTo(k, h); g.stroke(); g.beginPath(); g.moveTo(0, k + 60); g.lineTo(w, k + 60); g.stroke(); }
    g.fillStyle = '#111'; g.font = 'bold 44px sans-serif'; g.fillText('ACE 水晶球', 150, 330);
    const rgba = g.getImageData(0, 0, w, h).data, bgra = new Uint8Array(rgba.length);
    for (let i = 0; i < rgba.length; i += 4) { bgra[i] = rgba[i + 2]; bgra[i + 1] = rgba[i + 1]; bgra[i + 2] = rgba[i]; bgra[i + 3] = 255; }
    const sx = cv.width / SCENE_W, cx = BALL.x * sx, cy = BALL.y * sx;
    if (uploadFrame(bgra, w, h)) { capRect = [cx - w / 2, cy - h / 2, w, h]; hasBg = true; framesSeen++; }
  }

  window.CrystalPet = { applySnapshot, onFrame,
    pose(value) { Object.assign(eye, value); },
    setTiming(value) { Object.assign(timing, value); },
    get eye() { return { ...eye }; }, get mood() { return { ...mood }; }, get glow() { return { ...glow }; },
    get hasBackdrop() { return hasBg; }, get framesSeen() { return framesSeen; }, get renderer() { return program ? 'webgl2' : flat ? '2d' : 'none'; },
    get renderError() { return renderError; }, get sleeping() { return sleeping; }, get dragging() { return dragging; },
    get hovered() { return hovered; }, get expanded() { return expanded; }, get snapshot() { return snapshot; },
    get cards() { return shownCards().map(card => ({ key: card.key, status: card.status, title: card.title, body: card.body })); } };

  watchResolution();
  greetingUntil = performance.now() + timing.greeting;
  greetingTimer = setTimeout(renderCards, timing.greeting + 50);
  eye.happyUntil = performance.now() + 900; bounceAt = performance.now();
  if (!host && new URLSearchParams(location.search).get('backdrop') === 'grid') fixtureBackdrop();
  // The offline notice waits a moment for the first snapshot instead of flashing on every start.
  setTimeout(renderCards, 1600);
  renderCards(); reportRegions(); startLoop();
  post('ready'); reportLens();
})();
