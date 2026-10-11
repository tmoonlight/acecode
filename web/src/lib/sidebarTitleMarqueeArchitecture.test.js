import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

function source(relativePath) {
  return fs.readFileSync(path.join(srcRoot, relativePath), 'utf8').replace(/\r\n?/g, '\n');
}

function test(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

// 触发场景:侧栏会话行的标题过长时 hover 走跑马灯。
// 期望行为:跑马灯滚动的就是显示标题本身(sessionDisplayTitle 派生的 title /
// summary),行内不再另拉消息全文「水合」成更长的标题。
// 回归(bug 表现):旧实现在 summary 以 "..." 结尾时请求整份消息、把最后一条 user
// 消息全文塞进跑马灯 —— 对 @session 引用展开的 9k 字符消息,侧栏 hover 会滚出一段
// 与顶部标题栏不一致、且长度不受限的文本。
test('SessionRow measures only its non-editing title viewport', () => {
  const sidebar = source('components/Sidebar.jsx');
  const titleStart = sidebar.indexOf('function SidebarSessionTitle({ title, animation })');
  const rowStart = sidebar.indexOf('\nfunction SessionRow({', titleStart);
  const rowEnd = sidebar.indexOf('\nfunction OpencodeImportSelectAllCheckbox(', rowStart);
  assert.ok(titleStart >= 0 && rowStart > titleStart && rowEnd > rowStart);

  const titleComponent = sidebar.slice(titleStart, rowStart);
  const row = sidebar.slice(rowStart, rowEnd);
  assert.match(sidebar, /import \{ sidebarTitleMarqueeMetrics \} from '\.\.\/lib\/sidebarTitleMarquee\.js';/);
  assert.doesNotMatch(sidebar, /sidebarFullTitle|sidebarTitleHydrationState|loadSidebarFullTitle|marqueeTitle|marqueeReady/);
  assert.match(titleComponent, /sidebarTitleMarqueeMetrics\(content\.scrollWidth, viewport\.clientWidth\)/);
  assert.match(titleComponent, /new ResizeObserver\(measure\)/);
  assert.match(titleComponent, /observer\?\.observe\(viewport\)/);
  assert.match(titleComponent, /observer\?\.observe\(content\)/);
  assert.match(titleComponent, /document\.fonts\?\.ready/);
  assert.match(titleComponent, /metrics\.overflowing && 'is-overflowing'/);
  assert.match(titleComponent, /metrics\.overflowing && !animation\?\.typing && 'is-marquee-ready'/);
  assert.doesNotMatch(titleComponent, /<span[^>]*\btitle=\{/);
  assert.match(row, /const title = sessionDisplayTitle\(s, s\.name \|\| ''\);/);
  assert.match(
    row,
    /aria-label=\{remoteControlBound[\s\S]*\? tr\('remoteControl\.connectedSessionAria', \{ title \}\)[\s\S]*: title\}/,
  );
  assert.match(
    row,
    /\{editing \? \([\s\S]*?<input[\s\S]*?\) : \([\s\S]*?<SidebarSessionTitle title=\{title\} animation=\{titleAnimation\} \/>/,
  );
  assert.doesNotMatch(row, /className="block min-w-0 truncate"/);
  assert.doesNotMatch(row, /getMessages\(/);
});

test('overflow styling clips with a fade and animates only measured overflow', () => {
  const styles = source('styles/globals.css');
  const start = styles.indexOf('.ace-sidebar-session-title-viewport {');
  const end = styles.indexOf('.ace-session-hover-card {', start);
  assert.ok(start >= 0 && end > start);
  const titleStyles = styles.slice(start, end);

  assert.match(
    titleStyles,
    /\.ace-sidebar-session-title-viewport\s*\{[\s\S]*overflow: hidden;[\s\S]*white-space: nowrap;[\s\S]*text-overflow: clip;/,
  );
  assert.doesNotMatch(titleStyles, /text-overflow: ellipsis/);
  assert.match(
    titleStyles,
    /\.ace-sidebar-session-title-viewport\.is-overflowing\s*\{[\s\S]*-webkit-mask-image: linear-gradient\([\s\S]*transparent 100%/,
  );
  assert.match(
    titleStyles,
    /\.ace-sidebar-session-row:hover \.ace-sidebar-session-title-viewport\.is-overflowing\.is-marquee-ready \.ace-sidebar-session-title-content/,
  );
  assert.match(
    titleStyles,
    /\.ace-sidebar-session-title-button:focus-visible \.ace-sidebar-session-title-viewport\.is-overflowing\.is-marquee-ready \.ace-sidebar-session-title-content/,
  );
  assert.match(
    titleStyles,
    /\.ace-sidebar-session-row:hover \.ace-sidebar-session-title-viewport\.is-overflowing,[\s\S]*transparent 0,[\s\S]*#000 2px,[\s\S]*#000 calc\(100% - 12px\)/,
  );
  assert.doesNotMatch(titleStyles, /transparent 0,\s*#000 8px/);
  assert.match(
    titleStyles,
    /animation: ace-sidebar-session-title-marquee[\s\S]*linear\s+1\s+forwards;/,
  );
  assert.doesNotMatch(titleStyles, /\binfinite\b|\balternate\b/);
  assert.match(
    titleStyles,
    /@keyframes ace-sidebar-session-title-marquee[\s\S]*0%,[\s\S]*8\.8235%[\s\S]*translate3d\(0, 0, 0\)[\s\S]*91\.1765%,[\s\S]*100%[\s\S]*translate3d\(var\(--ace-sidebar-title-marquee-distance\), 0, 0\)/,
  );
  assert.doesNotMatch(titleStyles, /\n\s*15%\s*\{|\n\s*85%,/);
  assert.doesNotMatch(titleStyles, /position: absolute/);
});

test('reduced motion keeps the title fixed', () => {
  const styles = source('styles/globals.css');
  const start = styles.indexOf('.ace-sidebar-session-title-viewport {');
  const end = styles.indexOf('.ace-session-hover-card {', start);
  assert.ok(start >= 0 && end > start);
  const titleStyles = styles.slice(start, end);

  assert.match(
    titleStyles,
    /@media \(prefers-reduced-motion: reduce\) \{[\s\S]*\.ace-sidebar-session-row:hover \.ace-sidebar-session-title-content,[\s\S]*animation: none;[\s\S]*transform: translate3d\(0, 0, 0\);/,
  );
});
