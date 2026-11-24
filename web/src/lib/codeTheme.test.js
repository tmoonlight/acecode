import assert from 'node:assert/strict';
import fs from 'node:fs';
import { createRequire } from 'node:module';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import {
  applyCodeThemeAttribute,
  effectiveCodeTheme,
  hexColorScheme,
  isCodeThemeToggleTarget,
  naturalCodeTheme,
  normalizeCodeTheme,
  toggledCodeTheme,
} from './codeTheme.js';
import { renderMarkdown } from './markdown.js';

const here = path.dirname(fileURLToPath(import.meta.url));
const source = (relative) => fs.readFileSync(path.join(here, relative), 'utf8');

async function run(name, fn) {
  try {
    await fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

function fakeRoot() {
  const attributes = new Map();
  const events = [];
  return {
    events,
    getAttribute: (name) => (attributes.has(name) ? attributes.get(name) : null),
    setAttribute: (name, value) => { attributes.set(name, value); events.push(['set', name, value]); },
    removeAttribute: (name) => { attributes.delete(name); events.push(['remove', name]); },
    get offsetWidth() { events.push(['flush']); return 0; },
  };
}

// 场景:默认主题(蓝 / 橙)不手动切换时,代码明暗跟着界面明暗走——这正是这次要改的
// 「浅色界面里代码块也是深色」。主题包自带代码配色,按它的代码底色判断明暗。
// 期望:浅色界面 = 浅色代码,深色界面 = 深色代码;主题包深色代码底色在浅色界面里仍判成深色。
await run('默认主题的代码明暗跟随界面，主题包按自己的代码底色判断', () => {
  assert.equal(naturalCodeTheme({ mode: 'light' }), 'light');
  assert.equal(naturalCodeTheme({ mode: 'dark' }), 'dark');
  assert.equal(naturalCodeTheme({ mode: 'light', installedCodeBg: '#1e1e2e' }), 'dark');
  assert.equal(naturalCodeTheme({ mode: 'dark', installedCodeBg: '#fafafa' }), 'light');
  // 主题包的底色解析不了时退回界面明暗,不抛错。
  assert.equal(naturalCodeTheme({ mode: 'dark', installedCodeBg: 'red' }), 'dark');
  assert.equal(hexColorScheme('#111111'), 'dark');
  assert.equal(hexColorScheme('#ffffff'), 'light');
  assert.equal(hexColorScheme(null), null);
});

// 场景:用户点了代码块 / 文件预览上的明暗按钮。
// 期望:手动值压过跟随结果;主题包不接受手动值(按钮也被隐藏);切到与主题一致的那一种时
// 存 auto,之后界面再换明暗还能继续跟随;不认识的存量值按 auto。
await run('手动切换压过跟随，切回主题一致的一侧存 auto', () => {
  assert.equal(effectiveCodeTheme({ preference: 'dark', natural: 'light' }), 'dark');
  assert.equal(effectiveCodeTheme({ preference: 'auto', natural: 'dark' }), 'dark');
  assert.equal(effectiveCodeTheme({ preference: 'light', natural: 'dark', installed: true }), 'dark');
  assert.equal(toggledCodeTheme({ effective: 'light', natural: 'light' }), 'dark');
  assert.equal(toggledCodeTheme({ effective: 'dark', natural: 'light' }), 'auto');
  assert.equal(toggledCodeTheme({ effective: 'dark', natural: 'dark' }), 'light');
  assert.equal(toggledCodeTheme({ effective: 'light', natural: 'dark' }), 'auto');
  assert.equal(normalizeCodeTheme('sepia'), 'auto');
});

// 场景:用户要求切换「不要有过渡效果，直接切」,而全局样式给所有元素都配了 0.25s 颜色过渡。
// 期望:instant 时先挂关过渡的标记、写新值、读一次布局让浏览器在关过渡的状态下算完新配色,
// 再撤标记;跟着界面一起换明暗时(instant=false)不碰标记;值没变时什么都不写。
await run('手动切换先关过渡再写属性并同步算完，跟随界面时不碰过渡', () => {
  const root = fakeRoot();
  assert.equal(applyCodeThemeAttribute(root, 'light'), true);
  assert.deepEqual(root.events, [['set', 'data-code-theme', 'light']]);
  root.events.length = 0;
  assert.equal(applyCodeThemeAttribute(root, 'light', { instant: true }), false);
  assert.deepEqual(root.events, []);
  assert.equal(applyCodeThemeAttribute(root, 'dark', { instant: true }), true);
  assert.deepEqual(root.events, [
    ['set', 'data-code-theme-switching', ''],
    ['set', 'data-code-theme', 'dark'],
    ['flush'],
    ['remove', 'data-code-theme-switching'],
  ]);
  const styles = source('../styles/globals.css');
  assert.match(styles, /:root\[data-code-theme-switching\] \*,[\s\S]*?transition: none !important;/);
});

// 场景:聊天里的代码块是 markdown 渲染出的静态 HTML,明暗按钮要在复制按钮左边(用户截图的红框)。
// 期望:普通代码块的操作区里先是明暗按钮再是复制按钮;Mermaid 图不放明暗按钮;复制按钮那段
// HTML 原样不动(它整串是 i18n 目录的 key,改一个字就会丢翻译)。
await run('markdown 代码块在复制按钮左边渲染明暗按钮，Mermaid 不加', () => {
  const html = renderMarkdown('```js\nconst a = 1;\n```');
  const actions = html.slice(html.indexOf('data-code-actions="true"'));
  assert.ok(actions.indexOf('data-code-theme-toggle="true"') > 0);
  assert.ok(actions.indexOf('data-code-theme-toggle="true"') < actions.indexOf('data-code-copy-button="true"'));
  assert.match(html, /aria-label="切换代码深浅色"/);
  assert.match(html, /<button type="button" class="ace-code-copy-btn" data-code-copy-button="true" title="复制代码" aria-label="复制代码">/);
  const mermaid = renderMarkdown('```mermaid\ngraph TD\nA-->B\n```');
  assert.doesNotMatch(mermaid, /data-code-theme-toggle/);
  const button = { closest: (selector) => (selector === '[data-code-theme-toggle="true"]' ? button : null) };
  assert.equal(isCodeThemeToggleTarget(button), true);
  assert.equal(isCodeThemeToggleTarget({ closest: () => null }), false);
});

// 场景:github-dark.css 是全局引入的深色 token 配色,深色界面还有一段 dracula 覆盖。浅色代码只要
// 漏掉其中一个 class,那类 token 就会变成白底上的浅色字(看不见)。
// 期望:两份深色配色里给过颜色的每个 .hljs-* class,浅色代码段都有对应规则。
await run('浅色代码覆盖 github-dark 与 dracula 给过颜色的全部 token class', () => {
  const require = createRequire(import.meta.url);
  const githubDark = fs.readFileSync(require.resolve('highlight.js/styles/github-dark.css'), 'utf8');
  const styles = source('../styles/globals.css');
  const colored = new Set();
  // 只取每个选择器最后一段(真正被上色的元素):`.hljs-class .hljs-title` 上色的是 title。
  const addSubjects = (selectorList) => {
    for (const selector of selectorList.split(',')) {
      const subject = selector.trim().split(/\s+/).at(-1) || '';
      const name = subject.match(/^\.hljs-[\w-]+/);
      if (name) colored.add(name[0]);
    }
  };
  for (const rule of githubDark.matchAll(/([^{}]+)\{([^}]*)\}/g)) {
    if (/\bcolor\s*:/.test(rule[2])) addSubjects(rule[1]);
  }
  for (const line of styles.split('\n')) {
    if (line.startsWith('[data-theme="dark"] .hljs-')) addSubjects(line.replace(/\{.*$/, ''));
  }
  const light = styles.split('\n')
    .filter((line) => line.startsWith(':root[data-code-theme="light"]'))
    .join('\n');
  const missing = [...colored].filter((name) => !new RegExp(`${name.replace(/[-]/g, '\\-')}(?![\\w-])`).test(light));
  assert.deepEqual(missing, []);
  assert.ok(colored.size > 25, `只找到 ${colored.size} 个 class,解析可能失效`);
});

// 场景:主题包把代码配色写在根元素的内联样式上,自带一套 token 规则(如 HUD)。
// 期望:主题包下明暗按钮整体隐藏,浅色代码变量也不作用于主题包;文件预览顶栏和流式代码块
// 用同一个按钮组件,点击都走 document 上的统一处理。
await run('主题包隐藏明暗按钮，各处按钮共用同一个组件和点击入口', () => {
  const styles = source('../styles/globals.css');
  assert.match(styles, /:root\[data-installed-theme\] \.ace-code-theme-btn \{|:root\[data-installed-theme\] \.ace-code-theme-btn\s*\{/);
  assert.match(styles, /:root\[data-code-theme="light"\]:not\(\[data-installed-theme\]\) \{[^}]*--ace-code-bg:\s+#ffffff;/);
  assert.match(source('../components/FilePreviewContent.jsx'), /<CodeThemeToggleButton className="ace-preview-details-action" size=\{16\} \/>/);
  assert.match(source('../components/Message.jsx'), /<CodeThemeToggleButton \/>/);
  assert.match(source('../App.jsx'), /useCodeThemeToggle\(\(\) => changeAppearance\(\{/);
});
