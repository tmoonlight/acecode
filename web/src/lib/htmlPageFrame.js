// HTML 文件预览隔离框的外层纯逻辑(docs/html-preview.md):
//   - srcdoc:在 doctype 之后插入 <base href>(指向页面资源站点)与框内运行时,编辑时再插元素编号;
//   - 框内消息过滤(协议号 + 每次渲染一个随机 nonce,丢掉上一次渲染的旧消息)。nonce 不是鉴权:
//     它写在 srcdoc 的脚本里,页面脚本读得到;有后果的消息由 HtmlPagePreview 另外要求用户激活;
//   - 站内链接 → 本地文件路径;计算样式 → 工具栏的取值。
//
// 注意:这里生成的字符串会原样进入隔离框,不要写中文字符串字面量(构建期本地化插件会替换它们)。

import { htmlPageRuntime } from './htmlPageRuntime.js';
import { HTML_INLINE_TAGS, instrumentHtmlSource } from './htmlSourceMap.js';

export const HTML_PAGE_PROTOCOL = 1;
// 只给 allow-scripts 与 allow-modals:不透明源,读不到外层页面,也调不到本机 /api/*。
// 绝不能加 allow-same-origin(本机回环免 token,同源的页面脚本能调全部 daemon 接口)。
export const HTML_PAGE_SANDBOX = 'allow-scripts allow-modals';

const INBOUND_TYPES = new Set([
  'ready', 'scroll', 'link', 'openFile', 'contextmenu', 'key', 'select', 'rect',
  'textEditRequest', 'textEditRejected', 'textEditStarted', 'textEditEnded', 'textCommit', 'textSelection',
  'region', 'regionRect', 'marquee',
]);

export function htmlPageMessage(data, nonce) {
  if (!data || typeof data !== 'object' || data.ace_html !== HTML_PAGE_PROTOCOL) return null;
  if (!nonce || data.nonce !== nonce) return null;
  return INBOUND_TYPES.has(data.type) ? data : null;
}

export function createHtmlPageNonce() {
  const bytes = new Uint8Array(16);
  globalThis.crypto.getRandomValues(bytes);
  return Array.from(bytes, (byte) => byte.toString(16).padStart(2, '0')).join('');
}

// 没有解析器结果时(浏览模式)用的 doctype 位置:允许前面有空白与注释。
export function findDoctypeEnd(text) {
  const match = /^\s*(?:<!--[\s\S]*?-->\s*)*<!doctype[^>]*>/i.exec(String(text ?? ''));
  return match ? match[0].length : -1;
}

function escapeAttribute(value) {
  return String(value).replace(/&/g, '&amp;').replace(/"/g, '&quot;').replace(/</g, '&lt;');
}

function scriptSafe(text) {
  return String(text).replace(/<\/(script)/gi, '<\\/$1').replace(/<!--/g, '<\\!--');
}

function jsonForScript(value) {
  return JSON.stringify(value)
    .replace(/</g, '\\u003c')
    .replace(new RegExp(String.fromCharCode(0x2028), 'g'), '\\u2028')
    .replace(new RegExp(String.fromCharCode(0x2029), 'g'), '\\u2029');
}

// 站点根(<base href> 去掉入口所在目录之前的部分),用来把站内链接换回文件路径。
export function htmlSiteRootUrl(entryUrl, entry) {
  const url = String(entryUrl || '');
  const suffix = String(entry || '');
  if (!url || !suffix || !url.endsWith(suffix)) return '';
  return url.slice(0, url.length - suffix.length);
}

export function htmlSiteBaseHref(entryUrl) {
  const url = String(entryUrl || '');
  const slash = url.lastIndexOf('/');
  return slash >= 0 ? url.slice(0, slash + 1) : '';
}

const DOCUMENT_HAS_BASE = /<base\b[^>]*\bhref\s*=/i;

// text:要显示的页面原文(草稿);index:编辑模式下的编号表(null = 浏览模式不插编号);
// doctypeEnd:原文里 doctype 的结束位置(编号都在它之后,插入编号不影响这个位置)。
export function buildHtmlPageSrcdoc({ text, index = null, doctypeEnd, baseHref = '', config = {} } = {}) {
  const source = String(text ?? '');
  const body = index ? instrumentHtmlSource(source, index) : source;
  const at = Number.isInteger(doctypeEnd) ? Math.max(0, doctypeEnd) : Math.max(0, findDoctypeEnd(source));
  const runtimeConfig = {
    ...config,
    inlineTags: [...HTML_INLINE_TAGS],
  };
  const runtime = `(${scriptSafe(htmlPageRuntime.toString())})(${jsonForScript(runtimeConfig)});`;
  const base = baseHref && !DOCUMENT_HAS_BASE.test(source)
    ? `<base href="${escapeAttribute(baseHref)}">`
    : '';
  return `${body.slice(0, at)}${base}<script data-ace-html-runtime>${runtime}</script>${body.slice(at)}`;
}

// 站内链接(框内运行时报来的、相对站点根的已编码路径)→ 本地绝对路径。
// absoluteFile 是当前 HTML 的绝对路径,entry 是它相对站点根的路径。
export function htmlSiteLinkToPath({ absoluteFile = '', entry = '', rest = '' } = {}) {
  let decodedEntry;
  let decodedRest;
  try {
    decodedEntry = decodeURIComponent(String(entry));
    decodedRest = decodeURIComponent(String(rest));
  } catch {
    return '';
  }
  if (!decodedRest || decodedRest.split('/').some((part) => !part || part === '.' || part === '..')) return '';
  // 文件详情里的路径可能是 N:\a\b.html,也可能是 N:/a/b.html:统一成正斜杠比较,
  // 结果沿用原路径的分隔符风格;Windows 盘符路径不区分大小写。
  const file = String(absoluteFile);
  const forward = file.replace(/\\/g, '/');
  const windows = /^[a-zA-Z]:\//.test(forward) || forward.startsWith('//');
  const comparable = (value) => (windows ? value.toLowerCase() : value);
  if (!comparable(forward).endsWith(comparable(decodedEntry))) return '';
  const root = forward.slice(0, forward.length - decodedEntry.length);
  if (root && !root.endsWith('/')) return '';
  const target = root + decodedRest;
  return file.includes('\\') ? target.replace(/\//g, '\\') : target;
}

// 程序里的输入框与富文本编辑器在 Windows 上都认 Ctrl+Y 重做(浏览器与 Slate 的默认行为),
// HTML 编辑保持一致;配置的重做键(默认 Ctrl+Shift+Z)另由 editingHistoryAction 判定。
export function isWindowsRedoKey(event, mac = false) {
  if (mac || !event?.ctrlKey || event.metaKey || event.shiftKey || event.altKey) return false;
  return event.code === 'KeyY' || String(event.key || '').toLowerCase() === 'y';
}

// ---- 工具栏下拉 ------------------------------------------------------------------

export const HTML_TOOLBAR_FONT_SIZES = Object.freeze([10, 12, 14, 16, 18, 20, 24, 28, 32, 36, 40, 48, 56, 64, 72]);
export const HTML_TOOLBAR_RADII = Object.freeze([0, 2, 4, 6, 8, 12, 16, 20, 24, 32]);
export const HTML_TOOLBAR_BORDER_WIDTHS = Object.freeze([0, 1, 2, 3, 4]);
// 两行色板:中性色一行、常用彩色一行(Tailwind 调色板,MIT)。小写,与 cssColorToHex 的输出可直接比较。
export const HTML_TOOLBAR_COLORS = Object.freeze([
  '#000000', '#404040', '#737373', '#a3a3a3', '#d4d4d4', '#f5f5f5', '#ffffff', '#7c2d12',
  '#dc2626', '#ea580c', '#ca8a04', '#16a34a', '#0d9488', '#2563eb', '#7c3aed', '#db2777',
]);

const MENU_GAP = 6;
const MENU_MARGIN = 6;
const MENU_MIN_HEIGHT = 96;
// 列表最多露出约 8 行(字号列表其余部分滚动),和参考设计一致。
const MENU_MAX_HEIGHT = 320;

// 下拉弹层的位置:默认贴在按钮正下方、左边对齐;右边放不下改右对齐,下方放不下且上方更宽敞
// 时向上展开;高度限制在可用空间内(预览区域会裁掉溢出的部分)。trigger / bounds 是视口矩形。
export function toolbarMenuPlacement({ trigger, menu, bounds } = {}) {
  if (!trigger || !menu || !bounds) return { alignRight: false, above: false, maxHeight: 0 };
  const below = bounds.bottom - trigger.bottom - MENU_GAP - MENU_MARGIN;
  const aboveSpace = trigger.top - bounds.top - MENU_GAP - MENU_MARGIN;
  const above = menu.height > below && aboveSpace > below;
  const alignRight = trigger.left + menu.width > bounds.right - MENU_MARGIN
    && trigger.right - menu.width >= bounds.left + MENU_MARGIN;
  return {
    alignRight,
    above,
    maxHeight: Math.min(MENU_MAX_HEIGHT, Math.max(MENU_MIN_HEIGHT, Math.floor(above ? aboveSpace : below))),
  };
}

// ---- 工具栏取值 ------------------------------------------------------------------

export function cssColorToHex(value) {
  const text = String(value || '').trim();
  let match = /^#([0-9a-f]{6})$/i.exec(text);
  if (match) return `#${match[1].toLowerCase()}`;
  match = /^#([0-9a-f]{3})$/i.exec(text);
  if (match) return `#${match[1].split('').map((digit) => digit + digit).join('').toLowerCase()}`;
  match = /^rgba?\(\s*(\d+(?:\.\d+)?)[\s,]+(\d+(?:\.\d+)?)[\s,]+(\d+(?:\.\d+)?)(?:[\s,/]+(\d*\.?\d+%?))?\s*\)$/i.exec(text);
  if (!match) return '';
  if (match[4] !== undefined) {
    const alpha = match[4].endsWith('%') ? Number(match[4].slice(0, -1)) / 100 : Number(match[4]);
    if (alpha === 0) return '';
  }
  return `#${match.slice(1, 4).map((part) => Math.max(0, Math.min(255, Math.round(Number(part))))
    .toString(16).padStart(2, '0')).join('')}`;
}

export function cssPixels(value) {
  const number = Number.parseFloat(String(value || ''));
  return Number.isFinite(number) ? Math.round(number * 10) / 10 : 0;
}

export function isBoldWeight(weight) {
  const text = String(weight || '').trim().toLowerCase();
  if (text === 'bold' || text === 'bolder') return true;
  const number = Number(text);
  return Number.isFinite(number) && number >= 600;
}

// 下划线 / 删除线开关:在当前 text-decoration-line 上加减一项,全部去掉时写 none。
export function toggledDecoration(current, line) {
  const parts = String(current || '').split(/\s+/).filter((part) => part && part !== 'none');
  const next = parts.includes(line) ? parts.filter((part) => part !== line) : [...parts, line];
  return next.length ? next.join(' ') : 'none';
}

// ---- 改字时选中的一段文字 ----------------------------------------------------------

// 能只作用在选中文字上的样式;其余(对齐是段落属性)仍改整个元素。
const RANGE_STYLE_PROPS = new Set(['font-weight', 'font-style', 'text-decoration-line', 'color', 'font-size']);
const RANGE_STYLE_KEYS = ['fontSize', 'color', 'fontWeight', 'fontStyle', 'textDecorationLine'];

export function splitRangeStyles(changes) {
  const range = {};
  const element = {};
  for (const [name, value] of Object.entries(changes || {})) {
    (RANGE_STYLE_PROPS.has(name) ? range : element)[name] = value;
  }
  return { range, element };
}

// 框里报来的选中文字(不可信输入,只用来显示工具栏的当前值):只留已知字段、全部转成短字符串。
export function normalizeTextRange(value) {
  if (!value || typeof value !== 'object' || !value.styles || typeof value.styles !== 'object') return null;
  const styles = {};
  for (const key of RANGE_STYLE_KEYS) {
    const item = value.styles[key];
    if (typeof item === 'string' && item.length <= 200) styles[key] = item;
  }
  return {
    text: typeof value.text === 'string' ? value.text.slice(0, 400) : '',
    href: typeof value.href === 'string' ? value.href.slice(0, 4000) : '',
    styles,
  };
}

// 改字交回时带的光标(按字符数算的起止位置,框里报来的不可信输入):只认非负整数,结束不早于开始。
const MAX_CARET_OFFSET = 10000000;
export function normalizeTextCaret(value) {
  if (!value || typeof value !== 'object') return null;
  const start = Number(value.start);
  const end = Number(value.end);
  if (!Number.isInteger(start) || start < 0 || start > MAX_CARET_OFFSET) return null;
  const safeEnd = Number.isInteger(end) && end >= start && end <= MAX_CARET_OFFSET ? end : start;
  return { start, end: safeEnd };
}

// 选中元素能做哪些直接修改:有源码编号且编号唯一(没被脚本克隆)才能改;
// 改字还要求源码里是有结束标签的文字叶子。
export function htmlElementCapabilities(info, record) {
  const mapped = !!(info && info.sid && !info.duplicate && record && record.tag === info.tag);
  return {
    mapped,
    style: mapped,
    text: mapped && !!record.textLeaf && info.textLeaf,
    link: mapped && (record.tag === 'a' || record.hasEndTag),
    image: mapped && record.tag === 'img',
    remove: mapped,
  };
}
