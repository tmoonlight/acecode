// HTML 文件可视化编辑的源码定位与精确替换(docs/html-preview.md)。
//
// 原则:文件里只改动过的那一小段,其余字节原样保留。
//   - 用 parse5(带源码位置)解析草稿,给 body 里每个有源码位置的元素一个编号;
//   - 编号只插进送给隔离框显示的那份副本(data-ace-sid),绝不写进文件;
//   - 每次修改都是一次「替换某个区间」:改样式只重写那个标签的 style 属性,改文字只替换
//     开始标签与结束标签之间的内容,删除只删那个元素自己那一段;
//   - 替换后整份重新解析,按「替换前的起始位置 → 平移后的起始位置」把编号接回新记录,
//     落在被替换区间里的元素失去编号(之后只能交给 AI 编辑或重新渲染)。
// 脚本运行时生成的元素没有编号:它们在源码里不存在,不能直接改。
//
// 这里全是纯函数,parse 由调用方传入(组件里是 parse5 的 parse,Node 测试同样传 parse5)。

export const HTML_SID_ATTR = 'data-ace-sid';

// 这些元素和它们的子树不编号:不显示、内容不是普通 DOM,或者编号会被脚本克隆成重复。
const SKIP_SUBTREE = new Set([
  'head', 'script', 'style', 'template', 'noscript', 'link', 'meta', 'base', 'title',
]);
// 结构容器本身不编号(选中它们没有意义),子元素照常编号。
const SKIP_SELF = new Set(['html', 'body']);
// 文字叶子里允许出现的行内元素。
export const HTML_INLINE_TAGS = new Set([
  'a', 'abbr', 'b', 'bdi', 'bdo', 'br', 'cite', 'code', 'data', 'del', 'dfn', 'em', 'font',
  'i', 'ins', 'kbd', 'mark', 'q', 's', 'samp', 'small', 'span', 'strike', 'strong', 'sub',
  'sup', 'time', 'u', 'var', 'wbr',
]);
const VOID_TAGS = new Set([
  'area', 'base', 'br', 'col', 'embed', 'hr', 'img', 'input', 'link', 'meta', 'source',
  'track', 'wbr',
]);
const RAW_TEXT_TAGS = new Set(['script', 'style', 'textarea', 'title', 'xmp', 'iframe', 'select']);
const HTML_NAMESPACE = 'http://www.w3.org/1999/xhtml';

function isElement(node) {
  return !!node && typeof node.tagName === 'string';
}

function hasVisibleText(node) {
  if (!node) return false;
  if (node.nodeName === '#text') return /\S/.test(node.value || '');
  return (node.childNodes || []).some(hasVisibleText);
}

function onlyInlineDescendants(node) {
  return (node.childNodes || []).every((child) => {
    if (!isElement(child)) return true;
    if (child.namespaceURI !== HTML_NAMESPACE) return false;
    if (!HTML_INLINE_TAGS.has(child.tagName)) return false;
    return onlyInlineDescendants(child);
  });
}

function tagNameEnd(text, start) {
  let index = start + 1;
  while (index < text.length && !/[\s/>]/.test(text[index])) index += 1;
  return index;
}

function recordFor(text, node, id) {
  const loc = node.sourceCodeLocation;
  const startTag = loc.startTag;
  const endTag = loc.endTag || null;
  const attrs = {};
  for (const [name, attrLoc] of Object.entries(startTag.attrs || loc.attrs || {})) {
    attrs[name] = { start: attrLoc.startOffset, end: attrLoc.endOffset };
  }
  const tag = node.tagName;
  const html = node.namespaceURI === HTML_NAMESPACE;
  return {
    id,
    tag,
    start: loc.startOffset,
    end: loc.endOffset,
    startTagEnd: startTag.endOffset,
    nameEnd: tagNameEnd(text, loc.startOffset),
    endTagStart: endTag ? endTag.startOffset : -1,
    hasEndTag: !!endTag,
    attrs,
    textLeaf: html && !!endTag && !VOID_TAGS.has(tag) && !RAW_TEXT_TAGS.has(tag)
      && hasVisibleText(node) && onlyInlineDescendants(node),
  };
}

// 按文档先序收集可编号元素的源码位置。返回数组,元素顺序即先序。
export function collectHtmlElementRecords(text, parse) {
  const source = String(text ?? '');
  const document = parse(source, { sourceCodeLocationInfo: true });
  const records = [];
  let doctypeEnd = -1;
  const visit = (node, insideForeign) => {
    for (const child of node.childNodes || []) {
      if (child.nodeName === '#documentType' && child.sourceCodeLocation) {
        doctypeEnd = child.sourceCodeLocation.endOffset;
        continue;
      }
      if (!isElement(child)) continue;
      if (SKIP_SUBTREE.has(child.tagName) && child.namespaceURI === HTML_NAMESPACE) continue;
      const loc = child.sourceCodeLocation;
      const foreign = child.namespaceURI !== HTML_NAMESPACE;
      if (!SKIP_SELF.has(child.tagName) && loc?.startTag && !insideForeign) {
        records.push(recordFor(source, child, 0));
      }
      // SVG / MathML 只给根元素编号,内部图形不单独编辑。
      visit(child, insideForeign || foreign);
    }
  };
  visit(document, false);
  return { records, doctypeEnd };
}

// 首次渲染:给每条记录分配从 1 开始的编号。
export function indexHtmlSource(text, parse) {
  const { records, doctypeEnd } = collectHtmlElementRecords(text, parse);
  const byId = new Map();
  records.forEach((record, index) => {
    record.id = index + 1;
    byId.set(record.id, record);
  });
  return { byId, nextId: records.length + 1, doctypeEnd };
}

// 生成送进隔离框的副本:在每个记录的标签名后面插入 data-ace-sid。
export function instrumentHtmlSource(text, index) {
  const source = String(text ?? '');
  return instrumentHtmlRange(source, index, 0, source.length);
}

// 只取 [start, end) 这一段,并给落在其中的记录插上编号。撤销 / 重做改字时用它在框内原地
// 换回某个元素的内容,编号与当时的编号表一致,不用重新加载页面。
export function instrumentHtmlRange(text, index, start, end) {
  const source = String(text ?? '');
  const inserts = [...(index?.byId?.values() || [])]
    .filter((record) => record.nameEnd > start && record.nameEnd < end)
    .map((record) => ({ at: record.nameEnd, text: ` ${HTML_SID_ATTR}="${record.id}"` }))
    .sort((a, b) => a.at - b.at);
  const parts = [];
  let cursor = start;
  for (const insert of inserts) {
    parts.push(source.slice(cursor, insert.at), insert.text);
    cursor = insert.at;
  }
  parts.push(source.slice(cursor, end));
  return parts.join('');
}

// 某个元素的 style 属性里,这几个声明当前的值(没有就是 null)。撤销改样式时据此还原。
export function sourceStyleValues(text, record, props) {
  const current = readSourceAttribute(text, record, 'style');
  const values = new Map(parseStyleDeclarations(current?.value || '')
    .filter((decl) => decl.prop)
    .map((decl) => [decl.prop, decl.value]));
  const out = {};
  for (const prop of props) {
    const key = String(prop).toLowerCase();
    out[key] = values.has(key) ? values.get(key) : null;
  }
  return out;
}

export function applyHtmlSplice(text, splice) {
  const source = String(text ?? '');
  return source.slice(0, splice.start) + String(splice.insert ?? '') + source.slice(splice.end);
}

// 一次替换之后把编号接回新文本的记录。fresh 是新文本的 collectHtmlElementRecords 结果。
export function remapHtmlIndex(index, splice, fresh) {
  const delta = String(splice.insert ?? '').length - (splice.end - splice.start);
  const freshByStart = new Map(fresh.records.map((record) => [record.start, record]));
  const byId = new Map();
  const insertOnly = splice.start === splice.end;
  for (const [id, record] of index.byId) {
    let start = record.start;
    if (insertOnly) {
      // 纯插入:插入点及其之后开始的元素整体后移(包链接时紧贴开始标签的子元素也在其中)。
      if (start >= splice.start) start += delta;
    } else if (start >= splice.end) {
      start += delta;
    } else if (start >= splice.start) {
      continue;  // 落在被替换的区间里
    }
    const next = freshByStart.get(start);
    if (!next || next.tag !== record.tag) continue;
    byId.set(id, { ...next, id });
  }
  return { byId, nextId: index.nextId, doctypeEnd: fresh.doctypeEnd };
}

// 新出现的记录(例如替换后的文字里带了 <b>)按先序分配新编号,返回 {index, assigned}。
export function assignNewHtmlIds(index, fresh, withinStart, withinEnd) {
  const known = new Set([...index.byId.values()].map((record) => record.start));
  const byId = new Map(index.byId);
  let nextId = index.nextId;
  const assigned = [];
  for (const record of fresh.records) {
    if (record.start < withinStart || record.start >= withinEnd || known.has(record.start)) continue;
    const id = nextId;
    nextId += 1;
    byId.set(id, { ...record, id });
    assigned.push(id);
  }
  return { index: { ...index, byId, nextId }, assigned };
}

// ---- style 属性 -------------------------------------------------------------

// 按顶层分号切分声明,引号与括号里的分号不算。保留每段原文,只改动用到的那几段。
export function parseStyleDeclarations(styleText) {
  const text = String(styleText ?? '');
  const segments = [];
  let depth = 0;
  let quote = '';
  let start = 0;
  for (let index = 0; index <= text.length; index += 1) {
    const ch = text[index];
    if (index < text.length) {
      if (quote) {
        if (ch === '\\') index += 1;
        else if (ch === quote) quote = '';
        continue;
      }
      if (ch === '"' || ch === "'") { quote = ch; continue; }
      if (ch === '(') { depth += 1; continue; }
      if (ch === ')') { depth = Math.max(0, depth - 1); continue; }
      if (ch !== ';' || depth > 0) continue;
    }
    const raw = text.slice(start, index);
    const colon = raw.indexOf(':');
    segments.push({
      raw,
      prop: colon >= 0 ? raw.slice(0, colon).trim().toLowerCase() : '',
      value: colon >= 0 ? raw.slice(colon + 1).trim() : '',
    });
    start = index + 1;
  }
  return segments;
}

// changes: { 'font-size': '30px', 'font-weight': null(删除) }
export function updateStyleText(styleText, changes = {}) {
  const segments = parseStyleDeclarations(styleText);
  const pending = new Map(Object.entries(changes).map(([prop, value]) => [prop.toLowerCase(), value]));
  const out = [];
  for (const segment of segments) {
    if (!segment.prop || !pending.has(segment.prop)) {
      out.push(segment.raw);
      continue;
    }
    const value = pending.get(segment.prop);
    pending.delete(segment.prop);
    if (value === null || value === undefined || value === '') continue;
    const leading = /^\s*/.exec(segment.raw)[0];
    out.push(`${leading}${segment.prop}: ${value}`);
  }
  while (out.length && !out[out.length - 1].trim()) out.pop();
  for (const [prop, value] of pending) {
    if (value === null || value === undefined || value === '') continue;
    out.push(`${out.length ? ' ' : ''}${prop}: ${value}`);
  }
  const joined = out.join(';');
  return joined.trim() ? joined.trim() : '';
}

function escapeAttributeValue(value, quote) {
  const text = String(value ?? '').replace(/&/g, '&amp;');
  return quote === "'" ? text.replace(/'/g, '&#39;') : text.replace(/"/g, '&quot;');
}

function decodeAttributeValue(raw) {
  return String(raw ?? '')
    .replace(/&quot;/g, '"')
    .replace(/&#39;|&apos;/g, "'")
    .replace(/&lt;/g, '<')
    .replace(/&gt;/g, '>')
    .replace(/&amp;/g, '&');
}

// 读源码里某个属性:返回 {value, quote, start, end};不存在返回 null。
export function readSourceAttribute(text, record, name) {
  const loc = record?.attrs?.[name];
  if (!loc) return null;
  const raw = String(text ?? '').slice(loc.start, loc.end);
  const eq = raw.indexOf('=');
  if (eq < 0) return { value: '', quote: '"', start: loc.start, end: loc.end };
  let rawValue = raw.slice(eq + 1).trim();
  let quote = '';
  if (rawValue.startsWith('"') || rawValue.startsWith("'")) {
    quote = rawValue[0];
    rawValue = rawValue.slice(1, rawValue.endsWith(quote) ? -1 : undefined);
  }
  return { value: decodeAttributeValue(rawValue), quote: quote || '"', start: loc.start, end: loc.end };
}

// 设置 / 删除一个属性。已有就原地替换(沿用原来的引号),没有就插在标签名后面
// (避开自闭合斜杠与无引号属性值的边界问题)。
export function attributeSplice(text, record, name, value) {
  const existing = readSourceAttribute(text, record, name);
  const source = String(text ?? '');
  if (value === null || value === undefined) {
    if (!existing) return null;
    let start = existing.start;
    while (start > record.nameEnd && /\s/.test(source[start - 1])) start -= 1;
    return { start, end: existing.end, insert: '' };
  }
  const quote = existing?.quote || '"';
  const attr = `${name}=${quote}${escapeAttributeValue(value, quote)}${quote}`;
  if (existing) return { start: existing.start, end: existing.end, insert: attr };
  return { start: record.nameEnd, end: record.nameEnd, insert: ` ${attr}` };
}

export function styleSplice(text, record, changes) {
  const current = readSourceAttribute(text, record, 'style');
  const next = updateStyleText(current?.value || '', changes);
  if (next === (current?.value || '')) return null;
  return attributeSplice(text, record, 'style', next ? next : null);
}

// 改文字:替换开始标签与结束标签之间的内容。没有结束标签的元素不支持。
export function innerContentSplice(record, html) {
  if (!record?.hasEndTag) return null;
  return { start: record.startTagEnd, end: record.endTagStart, insert: String(html ?? '') };
}

export function innerSourceText(text, record) {
  if (!record?.hasEndTag) return '';
  return String(text ?? '').slice(record.startTagEnd, record.endTagStart);
}

export function outerSourceText(text, record) {
  return String(text ?? '').slice(record.start, record.end);
}

// 删除元素;它独占一行时连同这一行一起删掉,不留空行。
export function deleteElementSplice(text, record) {
  const source = String(text ?? '');
  let start = record.start;
  let end = record.end;
  const lineStart = source.lastIndexOf('\n', start - 1) + 1;
  let lineEnd = source.indexOf('\n', end);
  if (lineEnd < 0) lineEnd = source.length;
  if (!source.slice(lineStart, start).trim() && !source.slice(end, lineEnd).trim()) {
    start = lineStart;
    end = lineEnd < source.length ? lineEnd + 1 : lineEnd;
  }
  return { start, end, insert: '' };
}

// 给一个元素的全部内容包一层链接:两次纯插入,内部元素只平移、不丢编号。
export function wrapLinkSplices(record, href) {
  if (!record?.hasEndTag) return null;
  const open = `<a href="${escapeAttributeValue(href, '"')}">`;
  return [
    { start: record.endTagStart, end: record.endTagStart, insert: '</a>' },
    { start: record.startTagEnd, end: record.startTagEnd, insert: open },
  ];
}

export function lineRangeForOffsets(text, start, end) {
  const source = String(text ?? '');
  const count = (until) => {
    let lines = 1;
    for (let index = source.indexOf('\n'); index >= 0 && index < until; index = source.indexOf('\n', index + 1)) {
      lines += 1;
    }
    return lines;
  };
  const startLine = count(start);
  const endLine = count(Math.max(start, end - 1));
  return { startLine, endLine, lineCount: endLine - startLine + 1 };
}
