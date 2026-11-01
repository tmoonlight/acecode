// 浏览器回归:输入框换了同步身份(syncKey)后,Slate 迟到的变更回报不能覆盖父组件更新的值。
//
// 触发场景:首页勾选 worktree 后发送。发送回执同时带来 worktree 路径(会话 cwd 变 →
// InputBar 的 syncKey 变,RichComposer 按当时的值「测试」重建文档)和「清空已发送内容」,
// 后者在父组件 effect 里执行,晚于 RichComposer 的同步 effect。
// 期望行为:输入框与父组件的值都变成空。
// 修复前的表现:Slate 在微任务里才报告重建文档的操作,RichComposer 把「测试」当成用户
// 编辑回写给父组件,清空被覆盖,发出去的「测试」一直留在新会话的输入框里。
//
// 运行(在 web 目录):node scripts/test-composer-external-sync-echo.mjs
// --baseline 用 HEAD 的 RichComposer.jsx 跑同一用例,用来确认用例能打出修复前的问题。
// ACE_PLAYWRIGHT_MODULE / ACE_CHROMIUM_EXECUTABLE 指定已有的 Playwright 与浏览器。
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { createServer } from 'vite';

const web = fileURLToPath(new URL('../', import.meta.url));
const { chromium } = await import(process.env.ACE_PLAYWRIGHT_MODULE
  ? pathToFileURL(path.resolve(process.env.ACE_PLAYWRIGHT_MODULE)).href : 'playwright');
const fixture = `
import React, { useEffect, useState } from 'react';
import { createRoot } from 'react-dom/client';
import { flushSync } from 'react-dom';
import { InputBar } from '/src/components/InputBar.jsx';
import '/src/styles/globals.css';
const MAIN = '/fixture/main';
function Fixture() {
  const [state, setState] = useState({ text: '', cwd: MAIN });
  const [clearPending, setClearPending] = useState(false);
  window.fixtureState = state;
  window.fixture = {
    // 发送回执:同一次提交里换成 worktree 目录,并请求清空已发送的内容。用同步优先级
    // 提交,复现真实应用里埋点看到的顺序:父组件的清空在 Slate 的微任务回报之前就已重新
    // 渲染 InputBar,迟到的「测试」回报因此穿过 InputBar 的相等判断到达父组件。
    // 换成普通优先级时 InputBar 会把这次回报当成重复值挡掉,用例在修复前也会通过。
    receipt: () => flushSync(() => {
      setState((prev) => ({ ...prev, cwd: MAIN + '/.acecode/worktrees/ses-1' }));
      setClearPending(true);
    }),
    // 只换目录,不清空:重建后的内容与父组件的值都应保持不变。
    moveOnly: () => setState((prev) => ({ ...prev, cwd: prev.cwd + '-moved' })),
  };
  useEffect(() => {
    if (!clearPending) return;
    setClearPending(false);
    setState((prev) => ({ ...prev, text: '' }));
  }, [clearPending]);
  return <InputBar value={state.text} composerContent={null} cwd={state.cwd} currentSessionId="s1"
    onChange={(text) => setState((prev) => ({ ...prev, text }))} />;
}
createRoot(document.getElementById('root')).render(<Fixture />);
`;
const virtual = path.join(web, '__composer-external-sync-echo.jsx').replaceAll('\\', '/');
const html = `<html><body><div id="root"></div><script type="module">
import RefreshRuntime from '/@react-refresh'; RefreshRuntime.injectIntoGlobalHook(window);
window.$RefreshReg$=()=>{}; window.$RefreshSig$=()=>type=>type;
window.__vite_plugin_react_preamble_installed__=true;</script>
<script type="module" src="/__composer-external-sync-echo.jsx"></script></body></html>`;
const baseline = process.argv.includes('--baseline');
const composerPath = path.join(web, 'src/components/RichComposer.jsx').replaceAll('\\', '/');
const baselineComposer = baseline
  ? execFileSync('git', ['show', 'HEAD:web/src/components/RichComposer.jsx'], { cwd: web, encoding: 'utf8' })
  : null;
const server = await createServer({ root: web, logLevel: 'error',
  plugins: [{ name: 'composer-external-sync-echo-fixture', enforce: 'pre',
    resolveId(id) { if (id === '/__composer-external-sync-echo.jsx') return virtual; },
    load(id) {
      const file = id.replaceAll('\\', '/');
      if (file === virtual) return fixture;
      if (baselineComposer && file === composerPath) return baselineComposer;
    },
    configureServer(vite) { vite.middlewares.use((req, res, next) => {
      if (req.url?.split('?')[0] !== '/__composer-external-sync-echo') return next();
      res.setHeader('Content-Type', 'text/html'); res.end(html);
    }); },
  }], server: { host: '127.0.0.1', port: 0, open: false, hmr: false } });
let browser;
try {
  await server.listen();
  browser = await chromium.launch({ headless: true,
    executablePath: process.env.ACE_CHROMIUM_EXECUTABLE || undefined });
  const page = await browser.newPage({ viewport: { width: 900, height: 600 }, reducedMotion: 'reduce' });
  const errors = [];
  page.on('pageerror', (error) => errors.push(String(error)));
  const editor = page.locator('[data-ace-rich-composer]');
  const editorText = () => page.evaluate(() => {
    const input = document.querySelector('[data-ace-rich-composer]')?.cloneNode(true);
    input?.querySelectorAll('[data-slate-placeholder]').forEach((node) => node.remove());
    return input?.textContent.replaceAll('﻿', '').trim();
  });
  const settle = () => page.evaluate(() => new Promise((resolve) => setTimeout(resolve, 150)));
  const parentText = () => page.evaluate(() => window.fixtureState.text);

  await page.goto(`http://127.0.0.1:${server.httpServer.address().port}/__composer-external-sync-echo`);
  await editor.click();
  await page.keyboard.type('测试');
  await settle();
  assert.equal(await parentText(), '测试');

  // 场景 1:换目录与清空落在同一次提交 —— 输入框和父组件的值都必须变空。
  await page.evaluate(() => window.fixture.receipt());
  await settle();
  assert.equal(await parentText(), '', 'the late Slate report must not restore the submitted text');
  assert.equal(await editorText(), '', 'the editor must show the cleared value');
  console.log('[pass] a sync-key change in the same commit as a clear leaves the composer empty');

  // 场景 2:重建之后的正常输入照常回传给父组件(没有被当成迟到回报吞掉)。
  await editor.click();
  await page.keyboard.type('下一条');
  await settle();
  assert.equal(await parentText(), '下一条');
  assert.equal(await editorText(), '下一条');
  console.log('[pass] typing after the external sync still reaches the parent');

  // 场景 3:只换目录不清空 —— 重建后内容保持,父组件的值不被改动。
  await page.evaluate(() => window.fixture.moveOnly());
  await settle();
  assert.equal(await parentText(), '下一条');
  assert.equal(await editorText(), '下一条');
  console.log('[pass] a sync-key change alone keeps the current text');

  assert.deepEqual(errors, []);
  await page.goto('about:blank');
} finally {
  await browser?.close();
  await server.close();
}
