import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { mkdir, writeFile } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { createServer, transformWithEsbuild } from 'vite';

// Run from web: node scripts/test-chat-scroll-to-bottom.mjs [--baseline]
// Playwright is optional; ACE_PLAYWRIGHT_MODULE can point to its index.mjs.
// ACE_CHROMIUM_EXECUTABLE selects an installed browser; otherwise use Chromium.
// ACE_CHAT_SCROLL_TEST_OUTPUT optionally saves results and screenshots.
// All API/WebSocket traffic uses fixtures. No daemon or Desktop is started.
const web = fileURLToPath(new URL('../', import.meta.url));
const modulePath = process.env.ACE_PLAYWRIGHT_MODULE;
const { chromium } = await import(modulePath ? pathToFileURL(path.resolve(modulePath)).href : 'playwright');
const output = process.env.ACE_CHAT_SCROLL_TEST_OUTPUT;
const baseline = process.argv.includes('--baseline');
const baselineSources = new Map(baseline ? [
  'src/components/ChatView.jsx', 'src/lib/chatScrollFollow.js',
].map(relative => [
  path.join(web, relative).replaceAll('\\', '/'),
  execFileSync('git', ['show', `HEAD:web/${relative}`], { cwd: web, encoding: 'utf8' }),
]) : []);
const entry = `
import React from 'react';
import {createRoot} from 'react-dom/client';
import {ChatView} from '/src/components/ChatView.jsx';
import {connection} from '/src/lib/connection.js';
import {applyLocalePreference} from '/src/i18n/index.js';
import '/src/styles/globals.css';
await applyLocalePreference('zh-CN');
const root = createRoot(document.getElementById('root'));
const empty = [];
let sid = 'scroll-long';
window.fixture = {
  render(id) {
    sid = id;
    root.render(<ChatView sessionRef={{sessionId:sid,title:'Scroll regression',workspaceHash:''}}
      health={{}} permissionRequests={empty} recentExpertIds={empty} messageAutoCollapse={false}/>);
  },
  event(type, payload) {
    connection.dispatchEvent(new CustomEvent('message', {detail:{type,session_id:sid,payload}}));
  },
};
window.fixture.render(sid);
`;
const html = `<!doctype html><html><head><meta charset="UTF-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1"/></head><body>
<div id="root" style="height:100vh;display:flex;width:100%;background:var(--ace-bg);color:var(--ace-fg)"></div>
<script>
class FixtureWebSocket extends EventTarget {
  static OPEN=1; static CLOSED=3;
  constructor(){super();this.readyState=1;setTimeout(()=>this.dispatchEvent(new Event('open')),0);}
  send(){} close(){this.readyState=3;}
}
window.WebSocket=FixtureWebSocket;
</script><script type="module" src="/@id/virtual:chat-scroll-fixture"></script></body></html>`;
const server = await createServer({
  root: web,
  configFile: path.join(web, 'vite.config.js'),
  server: { host: '127.0.0.1', port: 0 },
  plugins: [{
    name: 'chat-scroll-fixture',
    enforce: 'pre',
    resolveId(id) { if (id === 'virtual:chat-scroll-fixture') return '\0virtual:chat-scroll-fixture'; },
    async load(id) {
      if (id === '\0virtual:chat-scroll-fixture') {
        return (await transformWithEsbuild(entry, 'fixture.jsx', { loader: 'jsx', jsx: 'automatic' })).code;
      }
      return baselineSources.get(id.replaceAll('\\', '/'));
    },
    configureServer(vite) {
      vite.middlewares.use('/scroll-fixture', async (req, res) => {
        res.setHeader('Content-Type', 'text/html');
        res.end(await vite.transformIndexHtml('/scroll-fixture', html));
      });
    },
  }],
});
let browser;
const checks = [];
const errors = [];
try {
  await server.listen();
  browser = await chromium.launch({
    headless: true,
    executablePath: process.env.ACE_CHROMIUM_EXECUTABLE || undefined,
    args: ['--force-device-scale-factor=1.5'],
  });
  const page = await browser.newPage({ viewport: { width: 1325, height: 775 }, deviceScaleFactor: 1.5 });
  page.on('pageerror', error => errors.push(error.message));
  const messages = Array.from({ length: 12 }, (_, i) => ({
    id: `fixture-${i}`, role: i % 2 ? 'assistant' : 'user',
    content: i % 2 ? '滚动验证\n\n' + '一段用于检查消息到底的测试文本。\n\n'.repeat(5) : `问题 ${i}`,
    timestamp: '2026-10-04T01:00:00Z',
  }));
  await page.route('**/api/**', async route => {
    const pathname = new URL(route.request().url()).pathname;
    let data = {};
    if (pathname.includes('/messages')) {
      data = { messages: pathname.includes('scroll-short') ? messages.slice(0, 1) : messages, events: [], busy: pathname.includes('scroll-running') };
    } else if (pathname.includes('models')) data = { models: [], profiles: [] };
    else if (pathname.includes('workspaces')) data = { workspaces: [] };
    else if (pathname.includes('history')) data = { items: [], history: [] };
    else if (pathname.includes('commands')) data = { commands: [] };
    else if (pathname.includes('skills')) data = { skills: [] };
    else if (pathname.includes('experts')) data = { experts: [] };
    else if (pathname.includes('subagents')) data = { tasks: [] };
    await route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(data) });
  });
  const viewport = page.locator('.ace-chat-transcript-scroll');
  const button = page.getByRole('button', { name: '滚动到底部', exact: true });
  const dots = button.locator('.ace-chat-tail-progress > span');
  const arrow = button.locator('[data-icon-name="ArrowDown"]');
  const settle = () => page.waitForTimeout(200);
  const metrics = () => viewport.evaluate(el => ({
    top: el.scrollTop, distance: el.scrollHeight - el.clientHeight - el.scrollTop,
  }));
  const check = (name, valid, details) => {
    assert.ok(valid, `${name}${details ? ': ' + JSON.stringify(details) : ''}`);
    checks.push(name);
    console.log(`[pass] ${name}`);
  };
  const bottom = async name => {
    const current = await metrics();
    check(name, current.distance <= 2 && await button.count() === 0, current);
  };
  const away = async () => {
    await viewport.evaluate(el => { el.scrollTop = 100; });
    await button.waitFor();
    await settle();
  };
  await page.goto(`http://127.0.0.1:${server.httpServer.address().port}/scroll-fixture`);
  await page.locator('[data-chat-row="true"]').last().waitFor();
  await settle();
  await bottom('initial load hides the arrow at the tail');

  // Choose a real fractional layout where the browser's clamped scrollTop
  // leaves >1px after integer height rounding. Do not fake DOM metric getters.
  const fractionalGap = await viewport.evaluate(el => {
    document.documentElement.style.zoom = '0.9';
    const content = el.firstElementChild;
    let found = null;
    for (let i = 0; i < 20 && !found; i += 1) {
      for (let j = 0; j < 20 && !found; j += 1) {
        el.style.height = `${620 + i / 20}px`;
        content.style.paddingBottom = `${j / 20}px`;
        el.scrollTop = el.scrollHeight;
        const gap = el.scrollHeight - el.clientHeight - el.scrollTop;
        if (gap > 1 && gap <= 2) found = gap;
      }
    }
    return found;
  });
  check('scaled fixture reproduces a clamped bottom with more than 1px rounding error', fractionalGap > 1, fractionalGap);
  await away();
  await button.click();
  await settle();
  await bottom('click hides the arrow at the scaled clamped bottom');
  await away();
  const box = await viewport.boundingBox();
  await page.mouse.move(box.x + box.width / 2, box.y + box.height / 2);
  await page.mouse.wheel(0, 10000);
  await settle();
  await bottom('manual wheel to the bottom hides the arrow');
  await viewport.evaluate(el => { el.scrollTop = el.scrollHeight - el.clientHeight - 24; });
  await settle();
  check('24px of remaining content still displays the arrow', await button.count() === 1);
  await away();
  check('idle history button shows the down arrow', await arrow.count() === 1 && await dots.count() === 0);
  const idleBox = await button.boundingBox();
  await page.evaluate(() => window.fixture.event('reasoning', { text: 'Considering the next step.' }));
  await dots.first().waitFor();
  await settle();
  check('reasoning replaces the arrow with three dots without scrolling',
    await dots.count() === 3 && await arrow.count() === 0 && Math.abs((await metrics()).top - 100) <= 2);
  const runningBox = await button.boundingBox();
  check('running indicator preserves the button position and size',
    ['x', 'y', 'width', 'height'].every(key => Math.abs(idleBox[key] - runningBox[key]) < 1), { idleBox, runningBox });
  const animation = await dots.evaluateAll(nodes => ({
    delays: nodes.map(node => getComputedStyle(node).animationDelay),
    samples: [100, 300, 500].map(time => nodes.map(node => {
      const animation = node.getAnimations()[0];
      animation.pause();
      animation.currentTime = time;
      return new DOMMatrixReadOnly(getComputedStyle(node).transform).m42;
    })),
  }));
  check('dots hop one at a time from left to right',
    animation.delays.join(',') === '0s,0.2s,0.4s'
    && animation.samples.every((sample, index) => sample.every((y, dot) => dot === index ? y < -2.9 : Math.abs(y) < .1)), animation);
  await dots.evaluateAll(nodes => nodes.forEach(node => node.getAnimations().forEach(animation => {
    animation.currentTime = 0;
    animation.play();
  })));
  if (output) {
    await mkdir(output, { recursive: true });
    await page.screenshot({ path: path.join(output, 'history-running.png') });
  }
  await page.emulateMedia({ reducedMotion: 'reduce' });
  await settle();
  check('reduced motion keeps static dots and a usable button', await button.isEnabled()
    && await dots.evaluateAll(nodes => nodes.length === 3 && nodes.every(node => getComputedStyle(node).animationName === 'none')));
  await button.click();
  await settle();
  await bottom('clicking static running dots returns to the bottom');
  await page.emulateMedia({ reducedMotion: 'no-preference' });
  await away();
  for (const type of ['done', 'error', 'turn_aborted']) {
    await page.evaluate(() => window.fixture.event('busy_changed', { busy: true }));
    await dots.first().waitFor();
    await page.evaluate(type => window.fixture.event(type, type === 'error' ? { reason: 'Fixture failure' } : {}), type);
    await arrow.waitFor();
    await settle();
    check(`${type} restores the arrow without interrupting history review`,
      await dots.count() === 0 && Math.abs((await metrics()).top - 100) <= 2);
  }
  await page.evaluate(() => window.fixture.event('token', { text: '\n\n' + 'Streaming content.\n\n'.repeat(12) }));
  await settle();
  check('streaming preserves manual history review with running dots', Math.abs((await metrics()).top - 100) <= 2 && await dots.count() === 3);
  await button.focus();
  await page.keyboard.press('Enter');
  await settle();
  await bottom('keyboard activation returns to the bottom');
  await page.evaluate(() => window.fixture.event('token', { text: '\n\n' + 'Later content.\n\n'.repeat(8) }));
  await settle();
  await bottom('later stream growth follows with the arrow hidden');

  await viewport.evaluate(el => {
    document.documentElement.style.zoom = '';
    el.style.height = '';
    el.firstElementChild.style.paddingBottom = '';
  });
  await settle();
  await away();
  // Suppress native scroll delivery to verify that protected programmatic
  // writes refresh visibility independently of another browser scroll event.
  await viewport.evaluate(el => {
    const suppress = event => event.stopImmediatePropagation();
    el.addEventListener('scroll', suppress, true);
    window.removeScrollSuppression = () => el.removeEventListener('scroll', suppress, true);
  });
  await button.click();
  await settle();
  await bottom('programmatic scrolling refreshes visibility without a scroll event');
  await page.evaluate(() => window.removeScrollSuppression());
  await away();
  if (output) {
    await mkdir(output, { recursive: true });
    await page.screenshot({ path: path.join(output, 'history-arrow.png') });
  }
  await page.setViewportSize({ width: 390, height: 844 });
  await page.evaluate(() => { document.documentElement.dataset.theme = 'dark'; });
  await settle();
  // Re-establish history review after the responsive composer changes height.
  await away();
  check('narrow dark history shows the running indicator', await dots.count() === 3);
  if (output) await page.screenshot({ path: path.join(output, 'narrow-running.png') });
  await button.click();
  await settle();
  await bottom('narrow dark layout hides the arrow after returning');
  if (output) await page.screenshot({ path: path.join(output, 'narrow-bottom.png') });
  await page.evaluate(() => window.fixture.render('scroll-short'));
  await settle();
  await bottom('short conversation hides the arrow');
  await page.evaluate(() => window.fixture.render('scroll-running'));
  await settle();
  await away();
  await dots.first().waitFor();
  check('restored running session shows dots without a live token', await dots.count() === 3);
  await page.evaluate(() => window.fixture.render('scroll-long'));
  await settle();
  await away();
  check('switching to an idle session clears the previous running indicator', await arrow.count() === 1 && await dots.count() === 0);
  check('no browser runtime errors', errors.length === 0, errors);
} finally {
  if (output) {
    await mkdir(output, { recursive: true });
    await writeFile(path.join(output, baseline ? 'baseline.json' : 'results.json'), JSON.stringify({ checks, errors }, null, 2));
  }
  await browser?.close();
  await server.close();
}
