import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import {
  ROOT_ERROR_AUTO_RELOAD_COOLDOWN_MS,
  ROOT_ERROR_AUTO_RELOAD_KEY,
  isChunkLoadError,
  planRootErrorRecovery,
  readRootErrorAutoReloadAt,
  rootErrorMessage,
  writeRootErrorAutoReloadAt,
} from './rootErrorRecovery.js';

const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

function source(relativePath) {
  return fs.readFileSync(path.join(srcRoot, relativePath), 'utf8').replace(/\r\n?/g, '\n');
}

function test(name, fn) {
  try { fn(); console.log('ok -', name); }
  catch (error) { console.error('not ok -', name); throw error; }
}

function memoryStorage(initial = {}) {
  const values = new Map(Object.entries(initial));
  return {
    getItem: (key) => (values.has(key) ? values.get(key) : null),
    setItem: (key, value) => { values.set(key, String(value)); },
  };
}

// 触发场景:页面开着时前端被重建(或 daemon 升级),旧页面按旧文件名取按需加载的
// 代码块,服务端回的是首页 HTML,动态 import 以这段文本报错。
// 期望行为:识别为「代码块加载失败」。
// 回归:2026-11-19 开发版桌面窗口对 Markdown 预览点编辑后整页变白,日志里正是这一句。
test('root error recovery recognises a failed dynamic import', () => {
  const error = new TypeError(
    'Failed to fetch dynamically imported module: http://127.0.0.1:59543/assets/MarkdownWysiwygEditor-BYLg2eNv.js',
  );
  assert.equal(isChunkLoadError(error), true);
  assert.equal(isChunkLoadError('error loading dynamically imported module'), true);
  assert.equal(isChunkLoadError(new Error('Importing a module script failed.')), true);
  assert.equal(isChunkLoadError(new Error('Unable to preload CSS for /assets/ConsoleDock-x.css')), true);
});

// 触发场景:组件自己的渲染错误(读了 undefined 的属性等),或拿到的根本不是 Error。
// 期望行为:不当成代码块加载失败,取消息文本时也不抛错。
test('root error recovery leaves ordinary render errors alone', () => {
  assert.equal(isChunkLoadError(new TypeError("Cannot read properties of undefined (reading 'map')")), false);
  assert.equal(isChunkLoadError(null), false);
  assert.equal(isChunkLoadError(undefined), false);
  assert.equal(rootErrorMessage({ message: 42 }), '[object Object]');
  const hostile = { get message() { throw new Error('boom'); } };
  assert.equal(rootErrorMessage(hostile), '');
});

// 触发场景:代码块加载失败,此前没有自动重载过(或上一次已经过了冷却时间)。
// 期望行为:自动重新加载页面,拿到新的文件名。
test('root error recovery reloads once for a failed dynamic import', () => {
  const error = new TypeError('Failed to fetch dynamically imported module: /assets/a.js');
  assert.equal(planRootErrorRecovery({ error, now: 1_000_000, lastAutoReloadAt: null }), 'reload');
  assert.equal(
    planRootErrorRecovery({
      error,
      now: 1_000_000,
      lastAutoReloadAt: 1_000_000 - ROOT_ERROR_AUTO_RELOAD_COOLDOWN_MS,
    }),
    'reload',
  );
});

// 触发场景:自动重载之后代码块仍然取不到(静态目录本身不完整),同一个错误马上再来。
// 期望行为:冷却时间内不再重载,停在兜底界面;时钟回拨(elapsed 为负)同样不重载。
// 阈值说明:冷却 60 秒,远大于一次重载加再次触发的时间,又不至于让用户长时间等不到自动恢复。
test('root error recovery does not loop when the reload did not help', () => {
  const error = new TypeError('Failed to fetch dynamically imported module: /assets/a.js');
  assert.equal(planRootErrorRecovery({ error, now: 1_000_000, lastAutoReloadAt: 999_000 }), 'fallback');
  assert.equal(planRootErrorRecovery({ error, now: 1_000_000, lastAutoReloadAt: 1_000_500 }), 'fallback');
});

// 触发场景:普通渲染错误,或调用方没给出当前时间。
// 期望行为:只显示兜底界面,不自动重载(重载未必有用,还会打断用户)。
test('root error recovery only offers the button for other errors', () => {
  assert.equal(
    planRootErrorRecovery({ error: new Error('boom'), now: 1_000_000, lastAutoReloadAt: null }),
    'fallback',
  );
  const chunk = new TypeError('Failed to fetch dynamically imported module: /assets/a.js');
  assert.equal(planRootErrorRecovery({ error: chunk, now: Number.NaN, lastAutoReloadAt: null }), 'fallback');
  assert.equal(planRootErrorRecovery(), 'fallback');
});

// 触发场景:sessionStorage 可用、不可用(隐私模式下读写抛错)、存了坏数据。
// 期望行为:读不到一律当没有记录;写不进去返回 false,调用方据此放弃自动重载。
test('root error recovery tolerates unavailable storage', () => {
  const storage = memoryStorage();
  assert.equal(readRootErrorAutoReloadAt(storage), null);
  assert.equal(writeRootErrorAutoReloadAt(storage, 123), true);
  assert.equal(readRootErrorAutoReloadAt(storage), 123);
  assert.equal(readRootErrorAutoReloadAt(memoryStorage({ [ROOT_ERROR_AUTO_RELOAD_KEY]: 'nope' })), null);

  const broken = {
    getItem() { throw new Error('denied'); },
    setItem() { throw new Error('denied'); },
  };
  assert.equal(readRootErrorAutoReloadAt(broken), null);
  assert.equal(writeRootErrorAutoReloadAt(broken, 123), false);
  assert.equal(readRootErrorAutoReloadAt(null), null);
  assert.equal(writeRootErrorAutoReloadAt(null, 123), false);
});

// 触发场景:有人重构入口文件,把根级错误边界拿掉或挪到 ThemeProvider 里面。
// 期望行为:入口最外层始终是 RootErrorBoundary,主题、应用本体的渲染错误都被它接住。
// 回归:没有这一层时,任何一处未被接住的渲染错误都会让整个窗口只剩底色。
test('root error boundary wraps the whole application at the entry point', () => {
  const entry = source('main.jsx');
  const open = entry.indexOf('<RootErrorBoundary>');
  const theme = entry.indexOf('<ThemeProvider>');
  const close = entry.indexOf('</RootErrorBoundary>');
  assert.ok(open >= 0 && close > open, 'main.jsx must render RootErrorBoundary');
  assert.ok(theme > open && theme < close, 'ThemeProvider must sit inside RootErrorBoundary');
});

// 触发场景:兜底界面本身依赖了懒加载块、主题上下文或别的组件;或者翻译函数出错
// (构建时中文字面量会被换成翻译函数调用,这是它唯一的运行时依赖)。
// 期望行为:它只引用 react 与判定逻辑;两处文案都经过带英文回退的 copy(),
// 翻译抛错时兜底界面照样画得出来。
test('root error boundary fallback has no application dependencies', () => {
  const boundary = source('components/RootErrorBoundary.jsx');
  assert.match(boundary, /copy\(\(\) => '界面出错了', 'Something went wrong'\)/);
  assert.match(boundary, /copy\(\(\) => '重新加载', 'Reload'\)/);
  assert.ok(!/>\s*[一-鿿]/.test(boundary), 'no bare Chinese JSX text in the fallback');
  // 多行 import 的最后一行是「} from '...';」,所以只认行尾的 from 子句。
  const imports = [...boundary.matchAll(/ from '([^']+)';$/gm)].map((match) => match[1]);
  assert.deepEqual(imports.sort(), ['../lib/rootErrorRecovery.js', 'react']);
  assert.ok(!boundary.includes('lazy('), 'fallback must not lazy-load anything');
});
