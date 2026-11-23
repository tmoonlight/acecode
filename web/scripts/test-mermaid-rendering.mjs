// Mermaid 渲染的浏览器验证(手动跑,不在 pnpm test 里):流程图 / 状态图 / 类图的真解析和绘制都要
// DOM,Node 测试只能断言改写后的文本,这里用真浏览器把整条链路走一遍。
//
//   $env:ACE_PLAYWRIGHT_MODULE = '<playwright 的 index.mjs>'; node scripts/test-mermaid-rendering.mjs
//
// 检查三件事(样例在 mermaid-rendering-cases.mjs):
// 1. 模型常见写法画得出来,该靠自动修复的确实是修过才画出来的;
// 2. 应当显示源码的,卡在预期的那一层,图框上有说明;
// 3. 恶意写法不会让页面向外发请求。
import assert from 'node:assert/strict';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { createServer } from 'vite';
import { drawn, hides, hostile, HOSTILE_HOST, pageNotes, pages, refused, repaired, shows } from './mermaid-rendering-cases.mjs';

const web = fileURLToPath(new URL('../', import.meta.url));
const modulePath = process.env.ACE_PLAYWRIGHT_MODULE;
const { chromium } = await import(modulePath ? pathToFileURL(path.resolve(modulePath)).href : 'playwright');

const PAGE = '/__mermaid-rendering';
const server = await createServer({
  root: web,
  logLevel: 'error',
  server: { host: '127.0.0.1', port: 0, open: false, hmr: false },
  plugins: [{
    name: 'mermaid-rendering-fixture',
    configureServer(vite) {
      vite.middlewares.use((req, res, next) => {
        if (req.url !== PAGE) return next();
        res.setHeader('Content-Type', 'text/html; charset=utf-8');
        res.end('<!doctype html><html data-theme="light"><body style="background:#fff"><div id="host" class="ace-md"></div></body></html>');
      });
    },
  }],
});

let browser;
const failures = [];
const check = (condition, message) => { if (!condition) failures.push(message); };
try {
  await server.listen();
  const origin = server.resolvedUrls.local[0].replace(/\/$/, '');
  browser = await chromium.launch({ channel: 'msedge', headless: true });
  const page = await browser.newPage();
  const outbound = [];
  page.on('request', (request) => {
    const url = request.url();
    if (!url.startsWith(origin) && !url.startsWith('blob:') && !url.startsWith('data:')) outbound.push(url);
  });
  await page.route(`${HOSTILE_HOST}/**`, (route) => route.abort());
  page.on('pageerror', (error) => failures.push(`pageerror: ${error.message}`));
  await page.goto(origin + PAGE);

  // 一、二、三:直接过渲染适配器。
  const cases = [
    ...Object.entries(drawn).map(([id, source]) => ({ group: 'drawn', id, source })),
    ...Object.entries(refused).map(([id, [source]]) => ({ group: 'refused', id, source })),
    ...Object.entries(hostile).map(([id, source]) => ({ group: 'hostile', id, source })),
  ];
  const results = await page.evaluate(async (items) => {
    const { createMermaidRenderAdapter } = await import('/src/lib/mermaidRenderer.js');
    const render = createMermaidRenderAdapter();
    const out = [];
    for (const item of items) {
      const result = await render(item.source, 'light');
      out.push({
        group: item.group,
        id: item.id,
        svg: Boolean(result?.svg),
        repaired: Boolean(result?.repaired),
        stage: result?.failure?.stage || '',
        reason: result?.failure?.reason || '',
        // 画出来的图里不许留下指向外面的东西。
        leaked: Boolean(result?.svg) && /attacker\.example[^<]*["')]/.test(result.svg.replace(/<text[\s\S]*?<\/text>/g, '')),
        // 图上的文字(只取 <text>,不算样式表),去掉空白后比较。
        text: result?.svg
          ? Array.from(new DOMParser().parseFromString(result.svg, 'image/svg+xml').querySelectorAll('text'))
            .map((node) => node.textContent).join('\n').replace(/[ \t\r\n]+/g, '')
          : '',
      });
    }
    return out;
  }, cases);
  await page.waitForTimeout(500);

  for (const row of results) {
    const label = `${row.group}/${row.id}`;
    if (row.group === 'drawn') {
      check(row.svg, `${label}: 没画出来 (${row.stage}: ${row.reason.slice(0, 120)})`);
      check(row.repaired === repaired.has(row.id), `${label}: repaired=${row.repaired},期望 ${repaired.has(row.id)}`);
      for (const wanted of shows[row.id] || []) {
        check(row.text.includes(wanted.replace(/\s+/g, '')), `${label}: 图上没有「${wanted}」`);
      }
      for (const unwanted of hides[row.id] || []) {
        check(!row.text.includes(unwanted), `${label}: 图上不该出现「${unwanted}」`);
      }
    } else if (row.group === 'refused') {
      const [, stage, reason] = refused[row.id];
      check(!row.svg && row.stage === stage, `${label}: 期望卡在 ${stage},实际 ${row.svg ? '画出来了' : row.stage}`);
      if (reason) check(row.reason === reason, `${label}: 期望原因 ${reason},实际 ${row.reason}`);
    } else {
      check(!row.leaked, `${label}: 画出来的图里带着外部地址`);
    }
  }
  check(outbound.length === 0, `页面向外发了请求: ${[...new Set(outbound)].join(' ')}`);

  // 端到端:真实的 Markdown → 图框 → 渲染器,两种主题都等到终态。
  await page.evaluate(async () => {
    const { installMermaidRenderer } = await import('/src/lib/mermaidRenderer.js');
    const { renderMarkdown } = await import('/src/lib/markdown.js');
    window.__renderMarkdown = renderMarkdown;
    installMermaidRenderer(window, document);
  });
  for (const [id, [markdown, expected, error]] of Object.entries(pages)) {
    for (const theme of ['light', 'dark']) {
      const seen = await page.evaluate(async ({ markdown: text, theme: mode }) => {
        document.documentElement.setAttribute('data-theme', mode);
        document.body.style.background = mode === 'dark' ? '#1e1f22' : '#ffffff';
        const host = document.getElementById('host');
        host.innerHTML = window.__renderMarkdown(text);
        const frame = host.querySelector('[data-mermaid-diagram]');
        if (!frame) return { state: 'no-frame' };
        const deadline = Date.now() + 15000;
        while (Date.now() < deadline) {
          const state = frame.getAttribute('data-mermaid-diagram');
          if (state === 'ready' || state === 'error') {
            return {
              state,
              error: frame.getAttribute('data-mermaid-error') || '',
              note: frame.querySelector('.ace-mermaid-error-note')?.textContent || '',
              image: Boolean(frame.querySelector('img.ace-mermaid-svg')),
            };
          }
          await new Promise((resolve) => setTimeout(resolve, 50));
        }
        return { state: `timeout:${frame.getAttribute('data-mermaid-diagram')}` };
      }, { markdown, theme });
      const label = `page/${id}/${theme}`;
      check(seen.state === expected, `${label}: 期望 ${expected},实际 ${seen.state} ${seen.error || ''}`);
      if (expected === 'ready') check(seen.image && !seen.note, `${label}: 没有图或多了说明`);
      if (expected === 'error') {
        check(seen.error.startsWith(error), `${label}: data-mermaid-error=${seen.error}`);
        check(seen.note === pageNotes[id], `${label}: 说明是「${seen.note}」`);
      }
    }
  }

  const count = (group) => results.filter((row) => row.group === group).length;
  console.log(`drawn ${count('drawn')} (repaired ${repaired.size}), refused ${count('refused')}, hostile ${count('hostile')}, pages ${Object.keys(pages).length * 2}`);
} finally {
  await browser?.close();
  await server.close();
}
assert.deepEqual(failures, []);
console.log('[pass] mermaid rendering in a real browser');
