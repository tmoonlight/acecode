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

test('shared SessionRow gates the portal on workspace hover or focus', () => {
  const sidebar = source('components/Sidebar.jsx');
  const rowStart = sidebar.indexOf('function SessionRow({');
  const rowEnd = sidebar.indexOf('\nfunction OpencodeImportSelectAllCheckbox(', rowStart);
  assert.ok(rowStart >= 0 && rowEnd > rowStart);
  const row = sidebar.slice(rowStart, rowEnd);

  assert.match(row, /const hoverDetails = sessionHoverDetails\(s\);/);
  assert.match(row, /const hoverCardVisible = !!hoverDetails && \(hovered \|\| focusWithin\);/);
  assert.match(row, /onMouseEnter=\{hoverDetails \? \(\) => setHovered\(true\) : undefined\}/);
  assert.match(row, /onFocusCapture=\{hoverDetails \? \(\) => setFocusWithin\(true\) : undefined\}/);
  assert.match(row, /\{hoverCardVisible && \(\s*<SessionHoverCard/);
  assert.match(row, /aria-describedby=\{hoverCardVisible \? hoverCardId : undefined\}/);
});

test('hover card lazily shares Git lookup and invalidates on Git state changes', () => {
  const sidebar = source('components/Sidebar.jsx');
  const cardStart = sidebar.indexOf('function SessionHoverCard({');
  const cardEnd = sidebar.indexOf('\nfunction SessionRow({', cardStart);
  assert.ok(cardStart >= 0 && cardEnd > cardStart);
  const card = sidebar.slice(cardStart, cardEnd);

  // git info 缓存必须是**跨组件共享的单例**(lib/gitInfoCache.js),不能由
  // Sidebar 自己 new 一份 —— GitSessionPill 读同一个 cwd,各建一份就等于
  // 每次切会话都重复打一次 /api/git/info(daemon 侧 5~7 个 git 子进程)。
  assert.match(sidebar, /import \{ gitInfoCache \} from '\.\.\/lib\/gitInfoCache\.js'/);
  assert.doesNotMatch(sidebar, /createSessionHoverGitInfoCache\(/);
  assert.match(card, /gitInfoCache\.get\(cwd\)/);
  assert.match(card, /window\.addEventListener\(GIT_STATE_CHANGED_EVENT, handleGitStateChanged\)/);
  assert.match(card, /gitInfoCache\.invalidate\(changedCwd\)/);
  assert.match(card, /computeSessionHoverCardPosition/);
  assert.match(card, /role="tooltip"/);
  assert.match(card, /createPortal\([\s\S]*document\.body/);
  assert.match(card, /details\.isGitRepository && \(/);
  assert.doesNotMatch(card, /加载|失败|错误/);
});

test('hover card styling escapes sidebar clipping without intercepting controls', () => {
  const styles = source('styles/globals.css');
  const start = styles.indexOf('.ace-session-hover-card {');
  const end = styles.indexOf('/*\n * /rc 绑定态', start);
  assert.ok(start >= 0 && end > start);
  const cardStyles = styles.slice(start, end);

  assert.match(cardStyles, /position: fixed/);
  assert.match(cardStyles, /pointer-events: none/);
  assert.match(cardStyles, /max-width: min\(420px, calc\(100vw - 16px\)\)/);
  assert.match(cardStyles, /background: var\(--ace-surface\)/);
  assert.match(cardStyles, /border: 1px solid var\(--ace-border\)/);
  assert.match(cardStyles, /box-shadow: var\(--ace-shadow-lg\)/);
  assert.match(cardStyles, /overflow-wrap: anywhere/);
  assert.match(
    cardStyles,
    /@media \(prefers-reduced-motion: reduce\) \{\s*\.ace-session-hover-card \{\s*animation: none;/,
  );
});
