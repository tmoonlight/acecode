// Browser check of the crystal ball companion (assets/desktop_pet/crystal_pet.html):
// the WebGL renderer compiles, a fake native host drives frames / cursor / sessions through the same
// messages as src/apps/desktop/desktop_pet_crystal_win.cpp, and screenshots land in ACE_PET_CAPTURE_DIR.
// ACE_PLAYWRIGHT_MODULE / ACE_CHROMIUM_EXECUTABLE pick the local Playwright and browser.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const modulePath = process.env.ACE_PLAYWRIGHT_MODULE;
const { chromium } = await import(modulePath ? pathToFileURL(path.resolve(modulePath)).href : 'playwright');
const root = fileURLToPath(new URL('../../', import.meta.url));
const assets = path.join(root, 'assets/desktop_pet');
const output = process.env.ACE_PET_CAPTURE_DIR || fs.mkdtempSync(path.join(os.tmpdir(), 'ace-crystal-'));
fs.mkdirSync(output, { recursive: true });
const html = fs.readFileSync(path.join(assets, 'crystal_pet.html'), 'utf8').replace(/<script src="([^"]+)"><\/script>/g,
  (_, source) => '<script>' + fs.readFileSync(path.join(assets, source), 'utf8') + '</script>');
assert.ok(Buffer.byteLength(html) < 2 * 1024 * 1024);
assert.ok(!html.includes('<script src='), 'embedded page needs no external scripts');
const file = path.join(output, 'crystal_pet.html');
fs.writeFileSync(file, html);

const browser = await chromium.launch({ executablePath: process.env.ACE_CHROMIUM_EXECUTABLE || undefined,
  args: ['--use-angle=swiftshader', '--enable-unsafe-swiftshader', '--ignore-gpu-blocklist'] });
const wait = ms => new Promise(resolve => setTimeout(resolve, ms));
const card = (id, status, state = 'think', turn = '1') => ({ sessionId: id, workspaceHash: 'p', title: `会话 ${id}`, status, state,
  body: status === 'review' ? '完成了。' : '正在处理', sortAt: `2026-11-05T01:00:0${turn}Z`, key: [id, status, turn].join('|') });
const snapshot = list => ({ version: 1, connected: true, follow: true, selected: null, offices: [], agents: [], activities: list });

async function open({ host = false, query = '', scale = 1.5 } = {}) {
  const context = await browser.newContext({ viewport: { width: 344, height: 252 }, deviceScaleFactor: scale });
  const page = await context.newPage();
  const errors = [];
  page.on('pageerror', error => errors.push(String(error)));
  page.on('console', message => { if (message.type() === 'error') errors.push(message.text()); });
  if (host) {
    // A stand-in for window.chrome.webview: records what the page posts, lets the test deliver messages.
    await page.addInitScript(() => {
      const listeners = {};
      window.__posted = [];
      window.chrome = { webview: {
        postMessage: value => window.__posted.push(value),
        addEventListener: (type, fn) => { (listeners[type] ||= []).push(fn); },
        releaseBuffer: () => { window.__released = (window.__released || 0) + 1; },
      } };
      window.__deliver = (type, event) => (listeners[type] || []).forEach(fn => fn(event));
    });
  }
  await page.goto(pathToFileURL(file).href + query);
  await page.waitForFunction(() => window.CrystalPet);
  return { context, page, errors };
}
const posted = page => page.evaluate(() => window.__posted.map(value => {
  try { return typeof value === 'string' && value.startsWith('{') ? JSON.parse(value) : value; } catch { return value; }
}));

// 1. Standalone: WebGL2 compiles, the built-in backdrop and the grid fixture both render.
{
  const { context, page, errors } = await open({ query: '?backdrop=grid' });
  await wait(1200);
  const state = await page.evaluate(() => ({ renderer: CrystalPet.renderer, error: CrystalPet.renderError, backdrop: CrystalPet.hasBackdrop }));
  assert.equal(state.error, '', 'shader compiles');
  assert.equal(state.renderer, 'webgl2');
  assert.equal(state.backdrop, true, 'grid fixture stands in for a host frame');
  await page.evaluate(() => CrystalPet.pose({ yaw: 0, pitch: 0, tYaw: 0, tPitch: 0, nextWander: 1e12, nextBlink: 1e12, happyUntil: 0 }));
  await wait(600);
  const shot = await page.screenshot({ path: path.join(output, 'crystal-grid.png'), omitBackground: true });
  // The middle of the ball is opaque glass, the corners of the window are transparent (read from the
  // screenshot: a WebGL canvas without preserveDrawingBuffer reads back empty).
  const pixels = await page.evaluate(async data => {
    const image = new Image();
    image.src = 'data:image/png;base64,' + data;
    await image.decode();
    const copy = document.createElement('canvas');
    copy.width = image.width; copy.height = image.height;
    const g = copy.getContext('2d'); g.drawImage(image, 0, 0);
    const at = (x, y) => [...g.getImageData(Math.round(x * image.width), Math.round(y * image.height), 1, 1).data];
    return { center: at(0.5, 140 / 252), corner: at(0.02, 0.98) };
  }, shot.toString('base64'));
  assert.equal(pixels.center[3], 255, 'ball interior is opaque');
  assert.equal(pixels.corner[3], 0, 'window corner is see-through');
  assert.deepEqual(errors, []);
  await context.close();
}

// 2. With a fake native host: lens report, shared-buffer frames with acknowledgements, cursor, sessions.
{
  const { context, page, errors } = await open({ host: true });
  await wait(300);
  let messages = await posted(page);
  assert.ok(messages.includes('ready'), 'page announces ready');
  const lens = messages.find(value => value?.type === 'crystal-lens');
  assert.ok(lens && Math.abs(lens.x - 0.5) < 1e-6 && lens.r > 0.1 && lens.r < 0.25, 'lens reported as window fractions');
  const overlay = messages.filter(value => value?.type === 'overlay').at(-1);
  assert.ok(overlay?.bubbles?.length >= 10 && overlay.bubbles.length <= 22, 'ball silhouette bands fit the native overlay limit');

  // Host frame: 64x64 BGRA, the window is 344x252 device px in this fake (ww/wh), so the frame covers its centre.
  await page.evaluate(() => {
    const buffer = new ArrayBuffer(64 * 64 * 4), bytes = new Uint8Array(buffer);
    for (let i = 0; i < bytes.length; i += 4) { bytes[i] = 200; bytes[i + 1] = 40; bytes[i + 2] = 30; bytes[i + 3] = 0; }
    window.__deliver('sharedbufferreceived', { getBuffer: () => buffer, additionalData: null });
    window.__deliver('message', { data: { type: 'crystal-frame', seq: 7, x: 140, y: 134, w: 64, h: 64, ww: 344, wh: 252 } });
  });
  messages = await posted(page);
  assert.ok(messages.some(value => value?.type === 'crystal-ack' && value.seq === 7), 'each frame is acknowledged');
  assert.equal(await page.evaluate(() => CrystalPet.hasBackdrop), true);
  // An oversized frame is still acknowledged (so the host never stalls) but not used.
  await page.evaluate(() => window.__deliver('message', { data: { type: 'crystal-frame', seq: 8, x: 0, y: 0, w: 4096, h: 4096, ww: 344, wh: 252 } }));
  assert.ok((await posted(page)).some(value => value?.type === 'crystal-ack' && value.seq === 8));
  assert.equal(await page.evaluate(() => CrystalPet.framesSeen), 1);
  await wait(400);
  await page.screenshot({ path: path.join(output, 'crystal-host-frame.png'), omitBackground: true });
  await page.evaluate(() => window.__deliver('message', { data: { type: 'crystal-off' } }));
  assert.equal(await page.evaluate(() => CrystalPet.hasBackdrop), false, 'remote session falls back to the built-in backdrop');

  // Cursor far to the right: the eyes turn right (positive yaw).
  await page.evaluate(() => window.__deliver('message', { data: { type: 'crystal-cursor', x: 1.4, y: 0.66 } }));
  await wait(500);
  const eyeRight = await page.evaluate(() => CrystalPet.eye);
  assert.ok(eyeRight.tYaw > 0.15 && eyeRight.yaw > 0.1, `eyes follow the pointer (yaw ${eyeRight.yaw})`);
  await page.evaluate(() => window.__deliver('message', { data: { type: 'crystal-cursor', x: -0.4, y: 0.66 } }));
  await wait(500);
  assert.ok((await page.evaluate(() => CrystalPet.eye)).yaw < -0.1, 'and back to the left');

  // A running session turns on the violet mist; waiting for the user looks up at the card.
  await page.evaluate(value => window.__deliver('message', { data: value }), snapshot([card('a', 'running')]));
  await wait(1500);
  let state = await page.evaluate(() => ({ mood: CrystalPet.mood, glow: CrystalPet.glow, cards: CrystalPet.cards }));
  assert.equal(state.mood.base, 'running');
  assert.ok(state.glow.a > 0.2, 'mist glows while a session runs');
  assert.ok(state.cards.some(value => value.key === 'a|running|1'), 'session card shown');
  await page.screenshot({ path: path.join(output, 'crystal-running.png'), omitBackground: true });
  await page.evaluate(value => window.__deliver('message', { data: value }), snapshot([card('a', 'waiting', 'permission')]));
  await wait(800);
  state = await page.evaluate(() => ({ mood: CrystalPet.mood, eye: CrystalPet.eye }));
  assert.equal(state.mood.base, 'waiting');
  // A fresh finished result: happy squint.
  await page.evaluate(value => window.__deliver('message', { data: value }), snapshot([card('a', 'review', 'done', '2')]));
  await wait(250);
  state = await page.evaluate(() => CrystalPet.eye);
  assert.ok(state.happy > 0.5, 'finished session makes it squint happily');
  await page.screenshot({ path: path.join(output, 'crystal-happy.png'), omitBackground: true });

  // Native drag messages keep it in the carried pose; double-click activates ACECode.
  await page.evaluate(() => window.__deliver('message', { data: { type: 'pet-drag', phase: 'start' } }));
  assert.equal(await page.evaluate(() => CrystalPet.dragging), true);
  await page.evaluate(() => window.__deliver('message', { data: { type: 'pet-drag', phase: 'end' } }));
  assert.equal(await page.evaluate(() => CrystalPet.dragging), false);
  await page.mouse.dblclick(172, 170);
  assert.ok((await posted(page)).some(value => value?.type === 'activate'), 'double-click opens ACECode');
  await page.mouse.click(172, 170, { button: 'right' });
  assert.ok((await posted(page)).includes('menu'), 'right-click opens the native menu');
  assert.deepEqual(errors, []);
  await context.close();
}

// 3. Settings preview (transparent PNG) over a wallpaper-like host frame; written into the repo only when asked.
{
  const { context, page } = await open({ host: true, scale: 2 });
  await page.addStyleTag({ content: '.stack,.grip{display:none!important}' });
  await page.evaluate(() => {
    const w = 688, h = 504, paint = document.createElement('canvas');
    paint.width = w; paint.height = h;
    const g = paint.getContext('2d'), sky = g.createLinearGradient(0, 0, 0, h);
    sky.addColorStop(0, '#5b8def'); sky.addColorStop(0.55, '#f2a7c3'); sky.addColorStop(1, '#ffd89b');
    g.fillStyle = sky; g.fillRect(0, 0, w, h);
    for (const [x, y, r, color] of [[150, 120, 90, '#ffffffaa'], [520, 90, 70, '#7ee0c3cc'], [430, 380, 110, '#ff8a65bb'], [120, 400, 80, '#8e7cf0bb']]) {
      g.fillStyle = color; g.beginPath(); g.arc(x, y, r, 0, Math.PI * 2); g.fill();
    }
    const rgba = g.getImageData(0, 0, w, h).data, buffer = new ArrayBuffer(rgba.length), bgra = new Uint8Array(buffer);
    for (let i = 0; i < rgba.length; i += 4) { bgra[i] = rgba[i + 2]; bgra[i + 1] = rgba[i + 1]; bgra[i + 2] = rgba[i]; bgra[i + 3] = 255; }
    window.__deliver('sharedbufferreceived', { getBuffer: () => buffer });
    window.__deliver('message', { data: { type: 'crystal-frame', seq: 1, x: 0, y: 0, w, h, ww: 688, wh: 504 } });
    CrystalPet.pose({ yaw: 0.1, pitch: 0.06, tYaw: 0.1, tPitch: 0.06, nextWander: 1e12, nextBlink: 1e12, happyUntil: 0 });
  });
  await wait(900);
  const clip = { x: 172 - 66, y: 166 - 66 - 6, width: 132, height: 158 };
  const shot = path.join(output, 'crystal-preview.png');
  await page.screenshot({ path: shot, omitBackground: true, clip });
  if (process.env.ACE_WRITE_CRYSTAL_PREVIEW === '1') fs.copyFileSync(shot, path.join(assets, 'crystal/preview.png'));
  await context.close();
}

await browser.close();
console.log(`[pass] crystal ball pet: WebGL renderer, host frames and acks, cursor, moods, menu; captures in ${output}`);
