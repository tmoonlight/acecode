import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const modulePath = process.env.ACE_PLAYWRIGHT_MODULE;
const { chromium } = await import(modulePath ? pathToFileURL(path.resolve(modulePath)).href : 'playwright');
const root = fileURLToPath(new URL('../../', import.meta.url));
const assets = path.join(root, 'assets/desktop_pet');
const output = process.env.ACE_PET_CAPTURE_DIR || fs.mkdtempSync(path.join(os.tmpdir(), 'ace-pet-browser-'));
fs.mkdirSync(output, { recursive: true });
const html = fs.readFileSync(path.join(assets, 'classic_pet.html'), 'utf8').replace(/<script src="([^"]+)"><\/script>/g,
  (_, source) => '<script>' + fs.readFileSync(path.join(assets, source), 'utf8') + '</script>');
assert.ok(Buffer.byteLength(html) < 2 * 1024 * 1024);
assert.ok(!html.includes('<script src='), 'embedded page needs no external scripts');
// Session cards as projected by desktopOfficeState.projectActivities (Codex Mini cards).
const card = (id, status, state, { turn = '1', title = `会话 ${id}`, body } = {}) => ({ sessionId: id, workspaceHash: 'project', title,
  status, state, body: body ?? (status === 'review' ? '30 秒已过。' : status === 'failed' ? '执行失败' : '收到，再等待 30 秒。'),
  sortAt: `2026-10-16T01:00:0${turn}Z`, key: [id, status, turn].join('|') });
const cards = (list, extra = {}) => ({ version: 1, connected: true, follow: true, selected: null, offices: [], agents: [], activities: list, ...extra });
const BUSY = ['think', 'type', 'work', 'compact', 'retry'];
// Shorthand: one live session in the given activity state.
const snapshot = (state = 'think') => cards(BUSY.includes(state) ? [card('live', 'running', state)]
  : state === 'permission' || state === 'question' ? [card('live', 'waiting', state)] : []);
// Expected clips per pet; mirrors the Codex-style profiles in classic_pet.js.
const EXPECT = {
  clippy: { greet: 'Greeting', think: ['Thinking', 'CheckingSomething'], work: ['Searching', 'Processing', 'GetTechy', 'CheckingSomething'],
    attention: 'GetAttention', waiting: 'IdleFingerTap', failed: 'Alert', review: 'Congratulate', idle: 'Idle1_1',
    fidgets: ['IdleAtom', 'IdleEyeBrowRaise', 'IdleFingerTap', 'IdleHeadScratch', 'IdleRopePile', 'IdleSideToSide',
      'LookUp', 'LookDown', 'LookLeft', 'LookRight', 'LookUpLeft', 'LookUpRight', 'GetArtsy', 'GetWizardy', 'Hearing_1'],
    jumping: 'GetAttention', click: 'Wave', drag: 'Idle1_1', left: ['LookLeft', false], right: ['LookRight', false],
    sleep: 'IdleSnooze', wake: 'IdleEyeBrowRaise', maxStillMs: 2500 },
  rover: { greet: 'Show', think: ['Thinking'], work: ['Searching', 'ImageSearching', 'Books'],
    attention: 'GetAttention', waiting: 'GetAttentionMinor', failed: 'Embarrassed', review: 'Congratulate', idle: 'Idle',
    fidgets: ['Sports', 'Cooking', 'Shopping', 'Books', 'Money', 'Celebrity', 'Travel', 'LookUp', 'LookUpLeft', 'Acknowledge'],
    jumping: 'Pleased', click: 'ClickedOn', drag: 'Surprised', left: ['GestureLeft', false], right: ['GestureLeft', true],
    sleep: null, maxStillMs: 2500 },
  whale: { greet: 'welcome', think: ['think'], work: ['working'], attention: 'welcome', waiting: 'wait', failed: 'error',
    review: 'celebrate', idle: 'idle', fidgets: ['play', 'eat', 'welcome', 'joy'], jumping: 'joy', click: 'welcome',
    drag: 'drag', left: ['drag', false], right: ['drag', false], sleep: 'sleep', wake: 'wake', maxStillMs: 1500 },   // breathing slows at each breath's turn
};
const mac = process.env.ACE_OFFICE_HOST === 'mac';
const browser = await chromium.launch({ channel: process.env.ACE_BROWSER_CHANNEL || 'msedge', headless: true });
const errors = [], external = [], checked = [];
try {
  const page = await browser.newPage({ viewport: { width: 430, height: 315 } });
  page.on('pageerror', e => errors.push(e.message));
  page.on('request', r => { if (/^https?:/.test(r.url())) external.push(r.url()); });
  const shim = mac ? fs.readFileSync(path.join(root, 'src/apps/desktop/desktop_pet_mac_support.hpp'), 'utf8')
    .match(/kBridgeShim = R"JS\(([\s\S]*?)\)JS";/)?.[1] : '';
  if (mac) assert.ok(shim);
  await page.addInitScript({ content: `window.actions=[];
    ${mac ? `window.webkit={messageHandlers:{acePet:{postMessage:value=>actions.push(value)}}};
    ${shim}
    window.hostMessage=event=>window.__acePetDeliver(event.data);` : `window.chrome={webview:{postMessage:value=>actions.push(value),addEventListener:(type,listener)=>{window.hostMessage=listener;}}};`}` });
  const send = value => page.evaluate(data => hostMessage({ data }), value);
  const pet = () => page.evaluate(() => ({ animation: ClassicPet.animation, flipped: ClassicPet.flipped, base: ClassicPet.base,
    sleeping: ClassicPet.sleeping, dragging: ClassicPet.dragging, hovered: ClassicPet.hovered, queue: ClassicPet.queue }));
  const parsed = () => page.evaluate(() => actions.map(raw => { try { return JSON.parse(raw); } catch { return {}; } }));
  const overlay = async () => (await parsed()).filter(a => a.type === 'overlay').at(-1);
  const waitAnimation = (names, timeout = 15000) => page.waitForFunction(list => list.includes(ClassicPet.animation), [names].flat(), { timeout });
  const away = async () => { await page.mouse.move(-10, -10); await page.waitForFunction(() => !ClassicPet.hovered); };
  // Longest stretch (ms) in which the canvas did not change, sampled every 50ms.
  const longestStill = ms => page.evaluate(duration => new Promise(resolve => {
    const canvas = document.getElementById('cv'), context = canvas.getContext('2d');
    const hash = () => { const d = context.getImageData(0, 0, canvas.width, canvas.height).data; let h = 0; for (let i = 3; i < d.length; i += 29) h = (h * 31 + d[i] + d[i - 1]) | 0; return h; };
    let last = hash(), since = performance.now(), longest = 0; const start = since;
    const id = setInterval(() => {
      const now = performance.now(), value = hash();
      if (value !== last) { longest = Math.max(longest, now - since); last = value; since = now; }
      if (now - start >= duration) { clearInterval(id); resolve(Math.max(longest, now - since)); }
    }, 50);
  }), ms);
  for (const kind of ['clippy', 'rover', 'whale']) {
    const expect = EXPECT[kind];
    await page.goto('about:blank');
    await page.setContent(html);
    await away();
    assert.ok(await page.evaluate(() => actions.includes('ready')));
    await send(snapshot());
    const map = path.join(assets, kind === 'whale' ? 'whale' : `classic/${kind}`, 'map.png');
    const peekSheet = kind === 'whale' ? 'data:image/png;base64,' + fs.readFileSync(path.join(assets, 'whale/peek.png')).toString('base64') : undefined;
    await send({ type: 'pet-character', kind, sprite: 'data:image/png;base64,' + fs.readFileSync(map).toString('base64'), peek: peekSheet });
    await page.waitForFunction(value => ClassicPet.kind === value, kind);
    // Codex greets with a wave when the pet appears; a greeting card also explains how to open ACECode.
    assert.equal((await pet()).animation, expect.greet, `${kind} greets first`);
    assert.match(await page.locator('.card').first().textContent(), /你好，我是.*双击/);
    assert.equal(await page.locator('#controls, #sessions, #follow, #pin, #close').count(), 0, 'companion pets have no toolbar');
    // Clippy's arrival starts from an empty frame, so wait for the first drawn picture.
    await page.waitForFunction(() => document.getElementById('cv').getContext('2d').getImageData(0, 0, 344, 252).data.some((v, i) => i % 4 === 3 && v > 0));
    assert.ok((await overlay()).bubbles.length > 1 && (await overlay()).bubbles.length < 22);
    await waitAnimation(expect.think);
    assert.equal((await pet()).base, 'running');
    const frames = await page.evaluate(data => {
      const before = ClassicPet.frame;
      for (let i = 0; i < 10; i++) ClassicPet.applySnapshot(data);
      return [before, ClassicPet.frame];
    }, snapshot());
    assert.equal(frames[1], frames[0], 'repeated snapshots preserve the current frame');

    // Session states: running → waiting (attention first) → failed → success celebration → idle loop.
    await send(snapshot('work'));
    await waitAnimation(expect.work, 10000);
    await send(snapshot('permission'));
    assert.equal((await pet()).animation, expect.attention, `${kind} asks for attention`);
    await waitAnimation(expect.waiting);
    assert.equal((await pet()).base, 'waiting');
    await send(cards([card('live', 'failed', 'error')]));
    assert.equal((await pet()).animation, expect.failed, `${kind} shows the failure`);
    await send(cards([card('live', 'running', 'work', { turn: '2' })]));
    const done = cards([card('live', 'review', 'sleep', { turn: '2' })]);
    await send(done);
    assert.equal((await pet()).animation, expect.review, `${kind} celebrates success`);
    await send(done);
    assert.equal((await pet()).animation, expect.review, 'repeated snapshots keep the celebration');
    await page.waitForFunction(names => ClassicPet.base === 'idle' && !ClassicPet.queue.length && names.includes(ClassicPet.animation),
      [expect.idle, ...expect.fidgets], { timeout: 20000 });

    // At rest the idle loop itself keeps moving; nothing ever freezes on one picture.
    await page.evaluate(() => ClassicPet.setTiming({ fidgetMin: 60000, fidgetMax: 60000 }));
    await waitAnimation(expect.idle, 20000);
    const still = await longestStill(5000);
    assert.ok(still <= expect.maxStillMs, `${kind} idle stayed still for ${Math.round(still)}ms`);
    // Preset actions are mixed in at random while resting.
    await page.evaluate(() => {
      window.seen = new Set();
      window.seenTimer = setInterval(() => seen.add(ClassicPet.animation), 40);
      ClassicPet.setTiming({ fidgetMin: 200, fidgetMax: 300 });
    });
    await page.waitForFunction(names => [...seen].filter(name => names.includes(name)).length >= 2, expect.fidgets.filter(n => n !== expect.idle), { timeout: 30000 });
    await page.evaluate(() => { clearInterval(seenTimer); ClassicPet.setTiming({ fidgetMin: 7000, fidgetMax: 16000 }); });

    // Session cards (Codex Mini): several sessions pile up as one card with layers behind it;
    // a click spreads them out, clicking a card opens that session, ■ stops it, × dismisses.
    await page.evaluate(() => { const greeting = document.querySelector('.card[data-key="greeting"]'); if (greeting) greeting.click(); });
    await send(cards([]));
    await page.waitForFunction(() => document.getElementById('stack').hidden);
    assert.equal((await overlay()).rect, null, 'no session, no bubble taking up space');
    const deck = cards([card('a', 'running', 'work', { title: '等待30秒' }), card('b', 'running', 'type', { title: '等待30秒', body: '收到，我等 30 秒。' }),
      card('c', 'review', 'sleep', { title: '等待30秒' }), card('d', 'review', 'sleep', { title: '整理日志' })]);
    await send(deck);
    assert.equal(await page.locator('.card:visible').count(), 1, 'collapsed deck shows one card');
    assert.equal(await page.evaluate(() => document.getElementById('stack').classList.contains('layers-2')), true);
    assert.ok(Array.isArray((await overlay()).rect), 'cards join the native hit region');
    await page.locator('.card').first().click();
    assert.equal((await page.evaluate(() => ClassicPet.expanded)), true);
    assert.equal(await page.locator('.card:visible').count(), 4, 'a click spreads the deck into a list');
    await page.locator('.card[data-key="b|running|1"] .stop').click();
    assert.ok((await parsed()).some(a => a.type === 'stop' && a.session_id === 'b'));
    await page.locator('.card[data-key="d|review|1"]').hover();
    await page.locator('.card[data-key="d|review|1"] .dismiss').click();
    assert.ok((await parsed()).some(a => a.type === 'read' && a.session_id === 'd'));
    assert.equal(await page.locator('.card[data-key="d|review|1"]').count(), 0);
    await page.locator('.card[data-key="c|review|1"]').click();
    assert.ok((await parsed()).some(a => a.type === 'open' && a.session_id === 'c'), 'clicking a card opens its session');
    assert.equal((await page.evaluate(() => ClassicPet.expanded)), false);
    assert.deepEqual((await page.evaluate(() => ClassicPet.cards)).map(c => c.key), ['a|running|1', 'b|running|1'], 'opened result card leaves');
    await send(cards([card('c', 'review', 'sleep'), card('e', 'review', 'sleep')]));
    assert.deepEqual((await page.evaluate(() => ClassicPet.cards)).map(c => c.key), ['e|review|1'], 'an opened card stays gone until the server clears it');
    await page.locator('.card').first().click();
    assert.ok((await parsed()).some(a => a.type === 'open' && a.session_id === 'e'), 'a single card opens directly');
    await page.waitForFunction(() => document.getElementById('stack').hidden);
    await page.mouse.move(215, 240); await away();
    await send({ type: 'pet-window-state', pinned: false });

    // Hover jumps (Codex "jumping"), a single click reacts, double-click opens ACECode.
    await page.mouse.move(215, 240);
    await page.waitForFunction(() => ClassicPet.hovered);
    assert.equal((await pet()).animation, expect.jumping, `${kind} jumps on hover`);
    assert.equal((await overlay()).bubbles.length, 2, 'hovered pet keeps its whole box hit-testable');
    await page.mouse.click(215, 240);
    assert.equal((await pet()).animation, expect.click, `${kind} reacts to a click`);
    await page.waitForTimeout(500);
    await page.mouse.dblclick(215, 240);
    assert.deepEqual((await parsed()).filter(a => a.type === 'activate' || a.type === 'open').at(-1), { type: 'activate' },
      'double-clicking the pet brings the ACECode window forward');
    await away();

    // Dragging: native window moves report a direction; Clippy/Rover look that way (Codex running-left/right),
    // Whale Girl stays lifted by the hand (her drag frame) for the whole drag.
    await page.waitForTimeout(350);
    await send({ type: 'pet-drag', phase: 'start' });
    assert.equal((await pet()).dragging, true);
    assert.equal((await pet()).animation, expect.drag);
    await send({ type: 'pet-drag', phase: 'move', direction: 'right' });
    assert.deepEqual([(await pet()).animation, (await pet()).flipped], expect.right, `${kind} heads right`);
    await send({ type: 'pet-drag', phase: 'move', direction: 'left' });
    assert.deepEqual([(await pet()).animation, (await pet()).flipped], expect.left, `${kind} heads left`);
    if (kind === 'whale') {
      // Pausing mid-drag keeps the lifted pose, and its swing never stops.
      await page.waitForTimeout(600);
      assert.equal((await pet()).animation, 'drag');
      assert.equal(await page.evaluate(() => ClassicPet.motion), 'tilt');
      assert.ok(await longestStill(600) < 300, 'lifted whale keeps swinging');
    }
    await send({ type: 'pet-drag', phase: 'end' });
    assert.equal((await pet()).dragging, false);
    await page.waitForFunction(names => names.includes(ClassicPet.animation), [expect.idle, ...expect.fidgets, expect.left[0]]);
    await page.waitForTimeout(350);
    const drags = await page.evaluate(() => actions.filter(a => a === 'drag').length);
    await page.mouse.move(215, 240); await page.mouse.down(); await page.mouse.move(-20, 240); await page.mouse.up();
    assert.equal(await page.evaluate(() => actions.filter(a => a === 'drag').length), drags + 1);
    await away();

    // Long rest falls asleep (still animated); any interaction wakes the pet up again.
    if (expect.sleep) {
      await page.evaluate(() => ClassicPet.setTiming({ sleepAfter: 300 }));
      await waitAnimation(expect.sleep, 20000);
      assert.equal((await pet()).sleeping, true);
      assert.ok(await longestStill(1500) < 1500, `${kind} keeps moving while asleep`);
      await page.evaluate(() => ClassicPet.setTiming({ sleepAfter: 300000 }));
      await page.waitForTimeout(3100);   // hover reaction cooldown
      await page.mouse.move(215, 240);
      await page.waitForFunction(() => ClassicPet.hovered);
      assert.equal((await pet()).animation, expect.wake, `${kind} wakes up`);
      assert.equal((await pet()).sleeping, false);
      await away();
      await send(snapshot('think'));
      await waitAnimation(expect.think);
    }

    // Card text matches the ACECode transcript body: 14px by default, then whatever the main
    // window reports for its font-size setting.
    await send(cards([card('font', 'running', 'work')]));
    const cardFont = () => page.evaluate(() => {
      const style = getComputedStyle(document.querySelector('.card .name'));
      return [style.fontSize, style.lineHeight, getComputedStyle(document.querySelector('.card .body')).fontSize];
    });
    assert.deepEqual(await cardFont(), ['14px', '22.4px', '14px']);
    await send(cards([card('font', 'running', 'work')], { transcriptFont: { size: '15px', family: '"Segoe UI", sans-serif' } }));
    assert.deepEqual(await cardFont(), ['15px', '24px', '15px']);
    assert.match(await page.evaluate(() => getComputedStyle(document.querySelector('.card')).fontFamily), /Segoe UI/);
    await send(cards([card('font', 'running', 'work')], { transcriptFont: { size: 'red;}', family: 'x{}' } }));
    assert.deepEqual(await cardFont(), ['15px', '24px', '15px'], 'malformed font values are ignored');
    await send(cards([], { transcriptFont: { size: '14px' } }));

    // Docking: only Whale Girl has edge-peek art. Docked she peeks in from that edge, blinks,
    // squints happily when hovered, and picking her up restores the full figure.
    const alphaAt = (x0, x1) => page.evaluate(([a, b]) => {
      const d = document.getElementById('cv').getContext('2d').getImageData(a, 150, b - a, 90).data;
      let n = 0; for (let i = 3; i < d.length; i += 4) if (d[i] > 32) n++; return n;
    }, [x0, x1]);
    await send({ type: 'pet-dock', side: 'right' });
    if (kind !== 'whale') {
      assert.equal(await page.evaluate(() => ClassicPet.docked), '', `${kind} has no edge-peek art`);
    } else {
      assert.equal(await page.evaluate(() => ClassicPet.docked), 'right');
      assert.equal((await pet()).animation, 'peek');
      assert.ok(await alphaAt(300, 344) > 500 && await alphaAt(0, 120) === 0, 'peeks in from the right edge');
      // 场景:贴右边换成偷看图。bug 表现:曾固定画成 170 像素高,头比站着时大将近一倍。
      // 期望:按常规帧缩放 ÷ 1.8(偷看原画比精灵帧大 1.8 倍)画,300 像素高的原画约 91 像素。
      const peekHeight = await page.evaluate(() => {
        const d = document.getElementById('cv').getContext('2d').getImageData(0, 0, 344, 252).data;
        let top = 252, bottom = -1;
        for (let y = 0; y < 252; y++) for (let x = 0; x < 344; x++) if (d[(y * 344 + x) * 4 + 3] > 32) { top = Math.min(top, y); bottom = y; break; }
        return bottom - top + 1;
      });
      assert.ok(peekHeight >= 80 && peekHeight <= 100, `peek head matches the standing pose (${peekHeight}px tall)`);
      await page.waitForFunction(() => ClassicPet.peekEyes === 1, null, { timeout: 12000 });
      await page.waitForFunction(() => ClassicPet.peekEyes === 0, null, { timeout: 2000 });
      assert.ok(await longestStill(1500) < 1500, 'docked whale keeps swaying');
      const region = await overlay();
      assert.ok(region.bubbles.length > 2 && region.bubbles.every(b => b[2] > 0.6 || b[0] < 0.1), 'hit region follows the peeking head');
      await page.screenshot({ path: path.join(output, 'whale-docked-right.png'), omitBackground: true });
      await page.mouse.move(400, 230);
      await page.waitForFunction(() => ClassicPet.hovered);
      assert.equal((await pet()).animation, 'peek-happy', 'hover gets a happy squint');
      await away();
      await send(cards([card('peek', 'running', 'work', { turn: '7' })]));
      await send(cards([card('peek', 'review', 'sleep', { turn: '7' })]));
      assert.equal((await pet()).animation, 'peek-happy', 'a finished session gets a happy squint while docked');
      await send({ type: 'pet-dock', side: 'left' });
      await page.waitForTimeout(100);
      assert.ok(await alphaAt(0, 44) > 500 && await alphaAt(220, 344) === 0, 'mirrored for the left edge');
      await page.screenshot({ path: path.join(output, 'whale-docked-left.png'), omitBackground: true });
      await page.waitForTimeout(350);
      await send({ type: 'pet-drag', phase: 'start' });
      assert.equal(await page.evaluate(() => ClassicPet.docked), '', 'picking her up leaves the edge');
      assert.equal((await pet()).animation, 'drag');
      await send({ type: 'pet-drag', phase: 'end' });
      await send(cards([]));
    }

    if (kind === 'whale') {
      const states = Object.keys(JSON.parse(fs.readFileSync(path.join(assets, 'whale/manifest.json'), 'utf8')).characters['whale-girl'].states);
      assert.equal(states.length, 15);
      for (const state of states) {
        await page.evaluate(name => ClassicPet.play(name), state);
        assert.ok(await page.evaluate(() => document.getElementById('cv').getContext('2d').getImageData(0, 0, 344, 252).data.some((v, i) => i % 4 === 3 && v > 0)), state);
      }
    }

    // The system "reduce motion" preference is deliberately ignored: remote desktop sessions turn it on
    // whenever window animations are off, and the pets must never freeze.
    await page.emulateMedia({ reducedMotion: 'reduce' });
    assert.equal(await page.evaluate(() => matchMedia('(prefers-reduced-motion: reduce)').matches), true);
    await send(snapshot('work'));
    await waitAnimation(expect.work, 10000);
    assert.notEqual((await pet()).animation, 'RestPose');
    const reducedStill = await longestStill(3000);
    assert.ok(reducedStill <= expect.maxStillMs, `${kind} froze for ${Math.round(reducedStill)}ms under reduce motion`);
    await page.mouse.move(215, 240);
    await page.waitForFunction(() => ClassicPet.hovered);
    await away();
    await page.emulateMedia({ reducedMotion: 'no-preference' });

    // Right-click is the only menu (native: switch pet, keep on top, hide).
    const menus = await page.evaluate(() => actions.filter(a => a === 'menu').length);
    await page.mouse.click(215, 240, { button: 'right' });
    assert.equal(await page.evaluate(() => actions.filter(a => a === 'menu').length), menus + 1);
    // Losing the connection shows a reconnect card instead of stale sessions.
    await send(cards([card('x', 'running', 'work')], { connected: false }));
    assert.deepEqual((await page.evaluate(() => ClassicPet.cards)).map(c => c.key), ['offline']);

    await send(snapshot('work'));
    for (const width of [172, 430, 860]) {
      await page.setViewportSize({ width, height: Math.round(width * 252 / 344) });
      await page.mouse.move(-10, -10); await page.waitForTimeout(1100);
      assert.equal(await page.evaluate(() => document.documentElement.scrollWidth), width);
      await page.screenshot({ path: path.join(output, `${kind}-${width}.png`), omitBackground: true });
    }
    await page.setViewportSize({ width: 430, height: 315 });
    checked.push(kind);
  }
  assert.deepEqual(errors, []); assert.deepEqual(external, []);
  console.log(JSON.stringify({ ok: true, host: mac ? 'mac-shim' : 'windows', checked,
    coverage: 'offline decoding, greeting, all 15 whale states, running/waiting/failed/success states, idle never still, random preset actions, session card deck (collapse/expand/open/stop/dismiss, nothing when idle), transcript font, whale edge docking (peek, blink, hover, left/right, pick up), hover jump, click reaction, double-click activate, drag directions, sleep/wake, keeps moving under reduce motion, silhouette, right-click menu, three sizes', output }));
} finally { await browser.close(); }
