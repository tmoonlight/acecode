// 对话内可视化(```visual 代码块)隔离框的外层纯逻辑(openspec add-inline-visuals,
// docs/inline-visuals.md):
//   - srcdoc 外壳:内容安全策略、基础样式、主题变量、色调类、预置箭头、框内运行时;
//   - 主题变量映射(ACECode 的 --ace-* → 框内短变量);
//   - 框内消息校验、追问文本与限频、外链白名单、高度夹取。
// 外壳每个隔离框只生成一次,之后的主题与内容更新都走 postMessage,所以 iframe 不会重新加载。
//
// 注意:这里生成的字符串会原样进入隔离框。不要写中文字符串字面量 —— 构建期的文案本地化
// 插件会把它们换成 tr() 调用,而框里没有 tr。要显示的文字由组件经 labels 传入。

import { inlineVisualRuntime } from './inlineVisualRuntime.js';

export const INLINE_VISUAL_PROTOCOL = 1;

// 框内脚本、样式、字体与网络请求只能去这四个公共 CDN;本机 daemon 与其它地址一律不放行。
export const INLINE_VISUAL_CDN_ORIGINS = Object.freeze([
  'https://cdnjs.cloudflare.com',
  'https://cdn.jsdelivr.net',
  'https://unpkg.com',
  'https://esm.sh',
]);

export const INLINE_VISUAL_MIN_HEIGHT = 40;
export const INLINE_VISUAL_MAX_HEIGHT = 4000;
export const INLINE_VISUAL_SEND_MAX_CHARS = 4000;
export const INLINE_VISUAL_SEND_INTERVAL_MS = 1000;

// 框内短变量 ← ACECode 主题变量。fg-3 / border-strong 没有对应的 ACECode 变量,由 fg 推出。
export const INLINE_VISUAL_TOKEN_SOURCES = Object.freeze([
  ['--fg', '--ace-fg'],
  ['--fg-2', '--ace-fg-2'],
  ['--surface', '--ace-surface'],
  ['--surface-2', '--ace-surface-hi'],
  ['--border', '--ace-border'],
  ['--accent', '--ace-accent'],
  ['--accent-bg', '--ace-accent-bg'],
  ['--ok', '--ace-ok'],
  ['--ok-bg', '--ace-ok-bg'],
  ['--warn', '--ace-warn'],
  ['--warn-bg', '--ace-warn-bg'],
  ['--danger', '--ace-danger'],
  ['--danger-bg', '--ace-danger-bg'],
  ['--font', '--font-sans'],
  ['--mono', '--font-mono'],
]);

export const INLINE_VISUAL_TOKENS = Object.freeze([
  '--fg', '--fg-2', '--fg-3', '--surface', '--surface-2', '--border', '--border-strong',
  '--accent', '--accent-bg', '--ok', '--ok-bg', '--warn', '--warn-bg', '--danger', '--danger-bg',
  '--font', '--mono',
]);

// 读不到 ACECode 变量(测试 / 非常规宿主)时的兜底,取值与 globals.css 的默认浅色 / 深色一致。
const FALLBACK_VARS = Object.freeze({
  light: {
    '--fg': '#1a1a1a', '--fg-2': '#6b6b6b', '--fg-3': 'rgba(26, 26, 26, 0.42)',
    '--surface': '#ffffff', '--surface-2': '#f0efeb', '--border': '#e2e2de',
    '--border-strong': 'rgba(26, 26, 26, 0.26)', '--accent': '#2563eb', '--accent-bg': '#eff6ff',
    '--ok': '#16a34a', '--ok-bg': '#f0fdf4', '--warn': '#ea580c', '--warn-bg': '#fff7ed',
    '--danger': '#dc2626', '--danger-bg': '#fef2f2',
    '--font': '-apple-system, BlinkMacSystemFont, "PingFang SC", "Segoe UI", sans-serif',
    '--mono': '"SF Mono", "Cascadia Code", "Fira Code", Menlo, Consolas, monospace',
  },
  dark: {
    '--fg': '#bfbfbf', '--fg-2': '#999999', '--fg-3': 'rgba(191, 191, 191, 0.42)',
    '--surface': '#1a1a1a', '--surface-2': '#2a2a2a', '--border': '#333333',
    '--border-strong': 'rgba(191, 191, 191, 0.26)', '--accent': '#3b82f6', '--accent-bg': '#172554',
    '--ok': '#22c55e', '--ok-bg': '#052e16', '--warn': '#f97316', '--warn-bg': '#431407',
    '--danger': '#ef4444', '--danger-bg': '#450a0a',
    '--font': '-apple-system, BlinkMacSystemFont, "PingFang SC", "Segoe UI", sans-serif',
    '--mono': '"SF Mono", "Cascadia Code", "Fira Code", Menlo, Consolas, monospace',
  },
});

// canvas 图表库读不了 CSS 变量,框内另给 8 个解析好的颜色(Tailwind 调色板,MIT)。
export const INLINE_VISUAL_PALETTES = Object.freeze({
  light: Object.freeze(['#3b82f6', '#14b8a6', '#f59e0b', '#ef4444', '#8b5cf6', '#ec4899', '#22c55e', '#64748b']),
  dark: Object.freeze(['#60a5fa', '#2dd4bf', '#fbbf24', '#f87171', '#a78bfa', '#f472b6', '#4ade80', '#94a3b8']),
});

// 色调类:只设置 --tone-bg / --tone-line / --tone-ink,形状与文字的规则读这三个变量,
// 所以最近的一层色调生效,嵌套不会串色。取值来自 Tailwind 调色板(MIT)。
const TONES = Object.freeze({
  blue: [['#eff6ff', '#3b82f6', '#1e40af'], ['#172554', '#60a5fa', '#93c5fd']],
  teal: [['#f0fdfa', '#14b8a6', '#115e59'], ['#042f2e', '#2dd4bf', '#5eead4']],
  green: [['#f0fdf4', '#22c55e', '#166534'], ['#052e16', '#4ade80', '#86efac']],
  amber: [['#fffbeb', '#f59e0b', '#92400e'], ['#451a03', '#fbbf24', '#fcd34d']],
  red: [['#fef2f2', '#ef4444', '#991b1b'], ['#450a0a', '#f87171', '#fca5a5']],
  purple: [['#f5f3ff', '#8b5cf6', '#5b21b6'], ['#2e1065', '#a78bfa', '#c4b5fd']],
  pink: [['#fdf2f8', '#ec4899', '#9d174d'], ['#500724', '#f472b6', '#f9a8d4']],
  gray: [['#f4f4f5', '#a1a1aa', '#3f3f46'], ['#27272a', '#71717a', '#d4d4d8']],
});

export const INLINE_VISUAL_TONES = Object.freeze(Object.keys(TONES));

export function inlineVisualCsp() {
  const cdn = INLINE_VISUAL_CDN_ORIGINS.join(' ');
  return [
    "default-src 'none'",
    `script-src 'unsafe-inline' 'unsafe-eval' ${cdn}`,
    `style-src 'unsafe-inline' ${cdn}`,
    `font-src ${cdn} data:`,
    `connect-src ${cdn}`,
    'img-src data: blob:',
    'media-src data: blob:',
    'worker-src blob:',
    "form-action 'none'",
    "base-uri 'none'",
  ].join('; ');
}

function parseRgb(value) {
  const text = String(value || '').trim();
  let match = /^#([0-9a-f]{3})$/i.exec(text);
  if (match) {
    const [r, g, b] = match[1].split('').map((digit) => parseInt(digit + digit, 16));
    return [r, g, b];
  }
  match = /^#([0-9a-f]{6})$/i.exec(text);
  if (match) {
    const hex = match[1];
    return [0, 2, 4].map((offset) => parseInt(hex.slice(offset, offset + 2), 16));
  }
  match = /^rgba?\(\s*(\d+(?:\.\d+)?)[\s,]+(\d+(?:\.\d+)?)[\s,]+(\d+(?:\.\d+)?)/i.exec(text);
  if (match) return [match[1], match[2], match[3]].map((part) => Math.round(Number(part)));
  return null;
}

function withAlpha(color, alpha, fallback) {
  const rgb = parseRgb(color);
  return rgb ? `rgba(${rgb[0]}, ${rgb[1]}, ${rgb[2]}, ${alpha})` : fallback;
}

// 只放行看起来像颜色 / 字体列表的值,防止奇怪的自定义主题把 } 之类写进框内样式。
function safeCssValue(value) {
  const text = String(value || '').trim();
  if (!text || text.length > 300 || /[{}<>;\\]/.test(text)) return '';
  return text;
}

// 外层页面当前主题 → 框内 theme 消息的内容。mode 跟随 <html data-theme>。
export function readInlineVisualTheme(doc = globalThis.document) {
  const root = doc?.documentElement;
  const mode = root?.getAttribute?.('data-theme') === 'dark' ? 'dark' : 'light';
  const fallback = FALLBACK_VARS[mode];
  let style = null;
  try {
    const view = doc?.defaultView || globalThis.window;
    style = root && view?.getComputedStyle ? view.getComputedStyle(root) : null;
  } catch {
    style = null;
  }
  const read = (name) => safeCssValue(style?.getPropertyValue?.(name));
  const vars = {};
  for (const [target, source] of INLINE_VISUAL_TOKEN_SOURCES) {
    vars[target] = read(source) || fallback[target];
  }
  vars['--fg-3'] = withAlpha(vars['--fg'], 0.42, fallback['--fg-3']);
  vars['--border-strong'] = withAlpha(vars['--fg'], 0.26, fallback['--border-strong']);
  return { mode, vars, palette: [...INLINE_VISUAL_PALETTES[mode]] };
}

export function inlineVisualThemeCss(vars) {
  const lines = INLINE_VISUAL_TOKENS
    .map((name) => {
      const value = safeCssValue(vars?.[name]);
      return value ? `${name}:${value};` : '';
    })
    .join('');
  return `:root{${lines}}`;
}

function toneCss() {
  const rules = [];
  for (const [name, [light, dark]] of Object.entries(TONES)) {
    rules.push(`.tone-${name}{--tone-bg:${light[0]};--tone-line:${light[1]};--tone-ink:${light[2]};}`);
    rules.push(`[data-mode="dark"] .tone-${name}{--tone-bg:${dark[0]};--tone-line:${dark[1]};--tone-ink:${dark[2]};}`);
  }
  return rules.join('\n');
}

// 框内基础样式与辅助类。类名、变量名与 assets/seed/skills/acecode/inline-visuals/SKILL.md
// 及 engine/prompt/diagram_guidance.cpp 一致,改名要三处一起改。
export function inlineVisualBaseCss() {
  return `
*,*::before,*::after{box-sizing:border-box;}
html{color-scheme:light;--tone-bg:var(--surface);--tone-line:var(--border-strong);--tone-ink:var(--fg);}
html[data-mode="dark"]{color-scheme:dark;}
html,body{margin:0;padding:0;background:transparent;}
body{font-family:var(--font);font-size:14px;line-height:1.55;color:var(--fg);overflow:hidden;-webkit-font-smoothing:antialiased;}
#ace-root{display:flow-root;padding:2px 1px;overflow-x:auto;overflow-y:hidden;}
#ace-defs{position:absolute;width:0;height:0;overflow:hidden;}
svg{max-width:100%;font-family:var(--font);}
svg[viewBox]{height:auto;}
#ace-root>svg{display:block;}
svg text{fill:var(--tone-ink);font-size:14px;}
.node{fill:var(--tone-bg);stroke:var(--tone-line);stroke-width:1.25;}
.edge{fill:none;stroke:var(--fg-3);stroke-width:1.5;}
.dashed{stroke-dasharray:5 4;}
.t{fill:var(--tone-ink);font-size:14px;}
.t-sm{fill:var(--fg-2);font-size:12px;}
.t-lg{fill:var(--tone-ink);font-size:16px;font-weight:600;}
.t-mono{font-family:var(--mono);}
.t-mid{text-anchor:middle;dominant-baseline:central;}
.clickable{cursor:pointer;}
.clickable:hover .node,.node.clickable:hover{stroke-width:2.25;}
.card{background:var(--surface);border:1px solid var(--border);border-radius:10px;padding:14px 16px;}
.card[class*="tone-"]{background:var(--tone-bg);border-color:var(--tone-line);color:var(--tone-ink);}
.grid{display:grid;gap:12px;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));}
.row{display:flex;gap:12px;align-items:center;flex-wrap:wrap;}
.stack{display:flex;flex-direction:column;gap:8px;}
.badge{display:inline-flex;align-items:center;gap:4px;padding:1px 8px;border-radius:999px;font-size:12px;line-height:1.6;background:var(--tone-bg);color:var(--tone-ink);border:1px solid var(--tone-line);}
.muted{color:var(--fg-2);}
.small{font-size:12px;}
.mono,code,pre{font-family:var(--mono);}
code{font-size:12.5px;background:var(--surface-2);padding:1px 5px;border-radius:4px;}
h1,h2,h3{margin:0 0 8px;line-height:1.35;color:var(--fg);}
h1{font-size:18px;}h2{font-size:16px;}h3{font-size:15px;}
p{margin:0 0 8px;}
a{color:var(--accent);}
button,.btn{font:inherit;font-size:13px;line-height:1.4;padding:5px 12px;border-radius:8px;border:1px solid var(--border-strong);background:var(--surface);color:var(--fg);cursor:pointer;}
button:hover,.btn:hover{background:var(--surface-2);}
button.primary,.btn.primary{background:var(--accent);border-color:var(--accent);color:#fff;}
button:disabled{opacity:.5;cursor:default;}
input,select,textarea{font:inherit;font-size:13px;color:var(--fg);}
input[type="text"],input[type="number"],select,textarea{padding:4px 8px;border-radius:6px;border:1px solid var(--border-strong);background:var(--surface);}
input[type="range"],input[type="checkbox"],input[type="radio"]{accent-color:var(--accent);}
input[type="range"]{width:100%;}
table{border-collapse:collapse;width:100%;font-size:13px;}
th,td{border-bottom:1px solid var(--border);padding:6px 8px;text-align:left;vertical-align:top;}
th{color:var(--fg-2);font-weight:600;}
.ace-visual-notice{margin:6px 0 0;font-size:12px;color:var(--fg-2);}
${toneCss()}
`;
}

// 预置的箭头与圆点标记。颜色先写固定值,再用 context-stroke 跟随连线;
// 不支持 context-stroke 的 WebKit 沿用固定值。
const MARKER_DEFS = '<svg id="ace-defs" aria-hidden="true" focusable="false"><defs>'
  + '<marker id="arrow" viewBox="0 0 10 10" refX="8.5" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse">'
  + '<path d="M1 1L9 5L1 9z" style="fill:var(--fg-3);fill:context-stroke;stroke:none"/></marker>'
  + '<marker id="dot" viewBox="0 0 10 10" refX="5" refY="5" markerWidth="6" markerHeight="6">'
  + '<circle cx="5" cy="5" r="4" style="fill:var(--fg-3);fill:context-stroke;stroke:none"/></marker>'
  + '</defs></svg>';

function escapeAttribute(value) {
  return String(value).replace(/&/g, '&amp;').replace(/"/g, '&quot;').replace(/</g, '&lt;');
}

// 内联脚本里不能出现 </script;JSON 与函数源码都按这个规则转义。
function scriptSafe(text) {
  return String(text).replace(/<\/(script)/gi, '<\\/$1').replace(/<!--/g, '<\\!--');
}

function jsonForScript(value) {
  const lineSeparator = new RegExp(String.fromCharCode(0x2028), 'g');
  const paragraphSeparator = new RegExp(String.fromCharCode(0x2029), 'g');
  return JSON.stringify(value)
    .replace(/</g, '\\u003c')
    .replace(lineSeparator, '\\u2028')
    .replace(paragraphSeparator, '\\u2029');
}

// 生成隔离框的 srcdoc。<head> 的第一个元素就是内容安全策略,必须早于任何样式与脚本。
export function buildInlineVisualSrcdoc({ theme, labels = {} } = {}) {
  const current = theme && theme.vars ? theme : readInlineVisualTheme(null);
  const mode = current.mode === 'dark' ? 'dark' : 'light';
  const config = {
    protocol: INLINE_VISUAL_PROTOCOL,
    mode,
    palette: Array.isArray(current.palette) ? current.palette.slice(0, 8) : [...INLINE_VISUAL_PALETTES[mode]],
    sendMaxChars: INLINE_VISUAL_SEND_MAX_CHARS,
    labels: {
      scriptFailed: typeof labels.scriptFailed === 'string' ? labels.scriptFailed : '',
    },
  };
  const runtime = `(${scriptSafe(inlineVisualRuntime.toString())})(${jsonForScript(config)});`;
  return '<!DOCTYPE html>'
    + `<html data-mode="${mode}"><head>`
    + `<meta http-equiv="Content-Security-Policy" content="${escapeAttribute(inlineVisualCsp())}">`
    + '<meta name="viewport" content="width=device-width, initial-scale=1">'
    + `<style>${inlineVisualBaseCss()}</style>`
    + `<style id="ace-theme">${inlineVisualThemeCss(current.vars)}</style>`
    + '</head><body>'
    + MARKER_DEFS
    + '<div id="ace-root"></div>'
    + `<script>${runtime}</script>`
    + '</body></html>';
}

const INBOUND_TYPES = new Set(['ready', 'size', 'send', 'link', 'error']);

// 框内发来的消息:带协议标记、类型已知才处理。来源校验(event.source)由组件做。
export function inlineVisualMessage(data) {
  if (!data || typeof data !== 'object' || data.ace_visual !== INLINE_VISUAL_PROTOCOL) return null;
  return INBOUND_TYPES.has(data.type) ? data : null;
}

export function clampInlineVisualHeight(value) {
  const height = Number(value);
  if (!Number.isFinite(height) || height <= 0) return null;
  return Math.min(INLINE_VISUAL_MAX_HEIGHT, Math.max(INLINE_VISUAL_MIN_HEIGHT, Math.ceil(height)));
}

// 追问文本:非字符串、去掉首尾空白后为空、超过 4000 字符的一律忽略。
export function inlineVisualSendText(value) {
  if (typeof value !== 'string') return null;
  const text = value.trim();
  if (!text || text.length > INLINE_VISUAL_SEND_MAX_CHARS) return null;
  return text;
}

// 每个隔离框 1 秒内最多一次追问;被拒的调用不刷新计时。
export function createInlineVisualSendLimiter(intervalMs = INLINE_VISUAL_SEND_INTERVAL_MS) {
  let last = -Infinity;
  return (now = Date.now()) => {
    if (now - last < intervalMs) return false;
    last = now;
    return true;
  };
}

// 框内链接只放行 http / https / mailto 的绝对地址,其余(javascript:、相对路径、file: …)忽略。
export function inlineVisualLinkUrl(value) {
  if (typeof value !== 'string' || value.length > 4096) return null;
  try {
    const url = new URL(value.trim());
    return ['http:', 'https:', 'mailto:'].includes(url.protocol) ? url.href : null;
  } catch {
    return null;
  }
}

// 高度记忆的键:同一段代码重新挂载(滚回历史、切回会话)时先用上次的高度,不跳版。
export function inlineVisualCodeKey(code) {
  const text = String(code || '');
  let hash = 0x811c9dc5;
  for (let index = 0; index < text.length; index += 1) {
    hash ^= text.charCodeAt(index);
    hash = Math.imul(hash, 0x01000193) >>> 0;
  }
  return `${text.length}:${hash.toString(36)}`;
}
