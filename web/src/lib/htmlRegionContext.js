// HTML 预览编辑模式里拖出的一块选区 → 输入框里的「选区」引用(docs/html-preview.md)。
//
// - 框里报来的选区是不可信输入(页面脚本能伪造任何消息):只留已知字段,数字取有限值,字符串截短,
//   元素最多 30 个。它只决定引用里写什么,不会改文件。
// - 引用走「浏览器上下文」那条通道(type: 'browser', kind: 'region'),正文给模型看:文件路径、选区在
//   页面里的位置,以及选区里每个元素的源码行号与源码片段;脚本运行时生成、源码里没有的元素带选择器
//   和运行时的 HTML。模型用普通文件工具去改文件。
import { lineRangeForOffsets, outerSourceText } from './htmlSourceMap.js';
import { basenameForPath } from './selectionChatContext.js';

export const HTML_REGION_ELEMENT_LIMIT = 30;
const MAX_SNIPPET_CHARS = 4000;
const MAX_CONTENT_CHARS = 60000;
// 元素和选区的关系:整个在选区里 / 只有一部分在选区里 / 选区落在它里面。
const RELATIONS = new Set(['inside', 'partial', 'container']);
const RELATION_NOTES = {
  inside: '',
  partial: ' (only partly inside the region)',
  container: ' (the region lies inside this element)',
};
let regionSequence = 0;

function finite(value) {
  const number = Number(value);
  return Number.isFinite(number) ? Math.round(number) : 0;
}

function clipped(value, limit) {
  return typeof value === 'string' ? value.slice(0, limit) : '';
}

function box(value, keys) {
  const source = value && typeof value === 'object' ? value : {};
  return Object.fromEntries(keys.map((key) => [key, finite(source[key])]));
}

function normalizeElement(value) {
  if (!value || typeof value !== 'object') return null;
  const tag = clipped(value.tag, 40).toLowerCase();
  if (!/^[a-z][a-z0-9-]*$/.test(tag)) return null;
  const sid = finite(value.sid);
  const anchorSid = finite(value.anchorSid);
  return {
    sid: sid > 0 ? sid : 0,
    anchorSid: anchorSid > 0 ? anchorSid : 0,
    duplicate: value.duplicate === true,
    tag,
    selector: clipped(value.selector, 400),
    text: clipped(value.text, 200),
    html: clipped(value.html, 3000),
    relation: RELATIONS.has(value.relation) ? value.relation : 'inside',
  };
}

export function normalizeHtmlRegion(value) {
  if (!value || typeof value !== 'object') return null;
  const rect = box(value.rect, ['x', 'y', 'w', 'h', 'vw', 'vh']);
  if (rect.w <= 0 || rect.h <= 0) return null;
  const elements = (Array.isArray(value.elements) ? value.elements : [])
    .slice(0, HTML_REGION_ELEMENT_LIMIT)
    .map(normalizeElement)
    .filter(Boolean);
  return {
    rect,
    page: box(value.page, ['x', 'y', 'w', 'h']),
    scroll: box(value.scroll, ['x', 'y']),
    elements,
  };
}

function snippet(text) {
  return text.length > MAX_SNIPPET_CHARS ? `${text.slice(0, MAX_SNIPPET_CHARS)}\n<!-- truncated -->` : text;
}

function lineLabel(source, record) {
  const range = lineRangeForOffsets(source, record.start, record.end);
  return range.startLine === range.endLine ? `line ${range.startLine}` : `lines ${range.startLine}-${range.endLine}`;
}

function describeElement(element, number, { source, index }) {
  const record = element.sid && !element.duplicate ? index?.byId?.get(element.sid) : null;
  const partial = RELATION_NOTES[element.relation] || '';
  const lines = [];
  if (record && record.tag === element.tag) {
    lines.push(`${number}. <${element.tag}> at ${lineLabel(source, record)}${partial}`);
    if (element.text) lines.push(`   Text: ${element.text}`);
    lines.push('```html', snippet(outerSourceText(source, record)), '```');
  } else {
    lines.push(`${number}. <${element.tag}> rendered at runtime by the page's script, not present in the source${partial}`);
    if (element.selector) lines.push(`   Selector: ${element.selector}`);
    const anchor = element.anchorSid ? index?.byId?.get(element.anchorSid) : null;
    if (anchor) lines.push(`   Nearest element in the source: <${anchor.tag}> at ${lineLabel(source, anchor)}`);
    if (element.text) lines.push(`   Text: ${element.text}`);
    if (element.html) lines.push('```html', snippet(element.html), '```');
  }
  return lines.join('\n');
}

export function createHtmlRegionContext({
  path = '',
  region = null,
  source = '',
  index = null,
  now = Date.now(),
} = {}) {
  const normalized = normalizeHtmlRegion(region);
  if (!normalized || !path) return null;
  const { rect, page, scroll, elements } = normalized;
  const sections = [
    'Region selected in the ACECode HTML preview (visual edit mode)',
    `File: ${path}`,
    `Region (CSS px, page coordinates): left ${page.x}, top ${page.y}, width ${page.w}, height ${page.h}`
      + ` (viewport ${rect.vw}x${rect.vh}, scrolled to ${scroll.x},${scroll.y})`,
  ];
  if (elements.length) {
    sections.push('Elements in the region:');
    elements.forEach((element, offset) => {
      sections.push(describeElement(element, offset + 1, { source: String(source ?? ''), index }));
    });
  } else {
    sections.push('No element lies in the region; it covers empty page space.');
  }
  let content = sections.join('\n\n');
  if (content.length > MAX_CONTENT_CHARS) content = `${content.slice(0, MAX_CONTENT_CHARS)}\n[truncated]`;
  regionSequence = (regionSequence + 1) % 0x100000;
  const id = `html-region-${now}-${regionSequence.toString(36)}`;
  const name = basenameForPath(path);
  return {
    type: 'browser',
    kind: 'region',
    id,
    local_id: id,
    label: '选区',
    note: name,
    content,
    source: { page_id: '', url: '', title: name },
  };
}
