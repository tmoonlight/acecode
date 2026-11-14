import { adaptMermaidBackgroundFills, mermaidTheme, mermaidThemeVariables } from './mermaidTheme.js';
import { registerMermaidExportTarget } from './mermaidExport.js';
import { dispatchMermaidPreview } from './mermaidPreview.js';

export { mermaidTheme } from './mermaidTheme.js';

export const MAX_MERMAID_SOURCE_BYTES = 64 * 1024;
export const MAX_MERMAID_SOURCE_LINES = 1000;
export const MAX_MERMAID_EDGES = 500;
export const MAX_MERMAID_SVG_BYTES = 4 * 1024 * 1024;
export const MAX_MERMAID_SVG_DIMENSION = 32768;

const SVG_NAMESPACE = 'http://www.w3.org/2000/svg';
const XLINK_NAMESPACE = 'http://www.w3.org/1999/xlink';
const DIAGRAM_SELECTOR = '[data-mermaid-diagram]';
const SOURCE_DIAGRAM_SELECTOR = '[data-mermaid-diagram="source"]';
const SOURCE_SELECTOR = '[data-mermaid-source="true"]';
const TARGET_SELECTOR = '[data-mermaid-render-target="true"]';
const SAFE_MERMAID_LABEL_BREAK = /<br[ \t]*\/?>/gi;

// 标签里像 HTML 标签的文字(`<cachePath>`、`List<String>`、`<img src=x>`)交给 Mermaid 之前改写成
// 它自己的实体码 `#lt;` / `#gt;`:Mermaid 拿到的源码里除了 `<br>` 不再有能被当成标签的 `<`,图上
// 照原样显示尖括号。以前这类图整张退回源码,而讲代码的图几乎都带这种文字。能配成一对的 `<...>`
// 连右尖括号一起改(flowchart 会吞掉标签里裸的 `>`);右尖括号前是 `-` `=` `.` 的不算一对,那多半是
// 连线箭头 `-->`。构建会拒绝 lookbehind,这里只能用前瞻写。
const MERMAID_MARKUP_TOKEN = /<br[ \t]*\/?>|<(\/?[A-Za-z](?:[^<>[\](){}|\r\n]*[^-=.<>[\](){}|\r\n])?)>|<(?=\/?[A-Za-z])/gi;

export function neutralizeMermaidMarkup(source) {
  return String(source || '').replace(MERMAID_MARKUP_TOKEN, (match, inner) => {
    if (inner !== undefined) return `#lt;${inner}#gt;`;
    return match === '<' ? '#lt;' : match;
  });
}

// 时序图的消息、注释和 loop / alt 这类分组标题里,`;` 被 Mermaid 当成换行(后半句成了一条语法错误
// 的新语句,整张图画不出来),`#` 之后的内容被当成注释悄悄截掉。这几处文字里的 `;` `#` 改写成实体码,
// 已经是实体码的(`#59;`、`#lt;`)原样保留。
const SEQUENCE_BLOCK_STATEMENT = /^(\s*(?:loop|alt|else|opt|par|and|critical|option|break)\b)(.*)$/i;
const SEQUENCE_TEXT_HEAD = /^\s*note\b|--?[>x)|\\/]/i;
// Mermaid 预处理会把同一行里「style…:…#…;」(classDef 同理)的最后一个分号删掉,给样式里的颜色值
// 让路,行里插进去的实体码会被它吃掉分号。时序图行尾多一个 `;` 只是一条空语句,先垫一个给它删。
const MERMAID_STYLE_SEMICOLON_STRIP = /(?:style|classDef).*:\S*#.*;/;

function escapeSequenceText(text) {
  return text.replace(/#\w+;|[#;]/g, (match) => {
    if (match === '#') return '#35;';
    if (match === ';') return '#59;';
    return match;
  });
}

export function escapeMermaidSequenceText(source) {
  return String(source || '').split('\n').map((line) => {
    let head;
    let text;
    const block = line.match(SEQUENCE_BLOCK_STATEMENT);
    if (block) {
      [, head, text] = block;
    } else {
      const colon = line.indexOf(':');
      if (colon < 0 || !SEQUENCE_TEXT_HEAD.test(line.slice(0, colon))) return line;
      head = line.slice(0, colon + 1);
      text = line.slice(colon + 1);
    }
    const escaped = head + escapeSequenceText(text);
    return MERMAID_STYLE_SEMICOLON_STRIP.test(escaped) ? `${escaped};` : escaped;
  }).join('\n');
}

// 真正交给 Mermaid 的文本。复制按钮、退回显示和导出仍用原始源码。
export function prepareMermaidRenderSource(source, family) {
  const neutral = neutralizeMermaidMarkup(source);
  return family === 'sequence' ? escapeMermaidSequenceText(neutral) : neutral;
}

const ALLOWED_SVG_ELEMENTS = new Set([
  'circle',
  'clippath',
  'defs',
  'desc',
  'ellipse',
  'fecolormatrix',
  'fecomposite',
  'fedropshadow',
  'feflood',
  'fegaussianblur',
  'femerge',
  'femergenode',
  'feoffset',
  'filter',
  'g',
  'line',
  'lineargradient',
  'marker',
  'mask',
  'path',
  'pattern',
  'polygon',
  'polyline',
  'radialgradient',
  'rect',
  'stop',
  'style',
  'svg',
  'symbol',
  'text',
  'textpath',
  'title',
  'tspan',
]);

const FAMILY_HEADERS = [
  { family: 'flowchart', pattern: /^(?:flowchart|graph)\s+(?:TB|TD|BT|RL|LR)\s*;?(?:\s+%%.*)?$/ },
  { family: 'state', pattern: /^stateDiagram(?:-v2)?\s*;?(?:\s+%%.*)?$/ },
  { family: 'class', pattern: /^classDiagram\s*;?(?:\s+%%.*)?$/ },
  { family: 'er', pattern: /^erDiagram\s*;?(?:\s+%%.*)?$/ },
  { family: 'sequence', pattern: /^sequenceDiagram\s*;?(?:\s+%%.*)?$/ },
];

function utf8ByteLength(value) {
  return new TextEncoder().encode(String(value || '')).byteLength;
}

function rejected(reason) {
  return { ok: false, reason };
}

export function inspectMermaidSource(value) {
  const source = String(value || '').replace(/\r\n?/g, '\n');
  if (!source.trim()) return rejected('empty');
  if (utf8ByteLength(source) > MAX_MERMAID_SOURCE_BYTES) return rejected('too-large');

  const lines = source.split('\n');
  if (lines.length > MAX_MERMAID_SOURCE_LINES) return rejected('too-many-lines');
  if (/[\u0000-\u0008\u000b\u000c\u000e-\u001f\u007f]/.test(source)) {
    return rejected('control-character');
  }

  const firstMeaningful = lines.find((line) => {
    const trimmed = line.trim();
    return trimmed && !/^%%(?!\{)/.test(trimmed);
  })?.trim() || '';
  if (firstMeaningful === '---') return rejected('frontmatter');

  const family = FAMILY_HEADERS.find(({ pattern }) => pattern.test(firstMeaningful))?.family;
  if (!family) return rejected('unsupported-family');

  if (/%%\s*\{/.test(source)) return rejected('directive');
  if (/@\s*\{/.test(source)) return rejected('generalized-shape');
  // 标签形状的文字不再拒绝,渲染前由 neutralizeMermaidMarkup 改写成实体码;这里按改写后的文本
  // 自检一遍,保证交给 Mermaid 的内容里除了 `<br>` 没有标签。
  const htmlSafetySource = neutralizeMermaidMarkup(source).replace(SAFE_MERMAID_LABEL_BREAK, '');
  if (/<\/?[A-Za-z][^>\r\n]*>/i.test(htmlSafetySource)
      || /&(?:lt|gt|#0*60|#x0*3c);/i.test(source)) {
    return rejected('html');
  }
  if (/!\s*\[/.test(source) || /\][ \t]*\(/.test(source)) return rejected('markdown-link');
  if (/\$\$/.test(source)) return rejected('math-label');
  if (/\bfa[blrs]?:fa-[\w-]+/i.test(source)) return rejected('icon-label');
  // 标签文字里的 `file://`、`javascript:`、`https://` 只是文字:能生成链接 / 外部资源的写法(click、
  // href、link、Markdown 链接、`@{ img }`、图标)上下各有专门的拒绝,渲染结果还要过 sanitizeMermaidSvg
  // (属性值里带协议或 `//` 整张作废)并且只以 <img> 显示。这里只拦样式语句里的 url();标签里的
  // `getUrl()`、`saveUrl (...)` 是代码文字,不算。
  if (/^\s*(?:style|classDef|linkStyle)\b[^\n]*url\s*\(/im.test(source)) {
    return rejected('external-resource');
  }
  if (/(?:expression\s*\(|-moz-binding\b|behavior\s*:|@(?:import|font-face|namespace|supports|media|document|page)\b)/i.test(source)) {
    return rejected('unsafe-style');
  }
  if (/(?:^|[\r\n;])\s*(?:click|href|links?|callback|call)\b/im.test(source)) {
    return rejected('interactive');
  }

  return { ok: true, family, source };
}

export function mermaidConfig(theme) {
  return {
    startOnLoad: false,
    securityLevel: 'strict',
    suppressErrorRendering: true,
    htmlLabels: false,
    theme: theme === 'dark' ? 'dark' : 'default',
    themeVariables: mermaidThemeVariables(theme),
    look: 'classic',
    layout: 'dagre',
    arrowMarkerAbsolute: false,
    deterministicIds: true,
    deterministicIDSeed: 'acecode',
    maxTextSize: MAX_MERMAID_SOURCE_BYTES,
    maxEdges: MAX_MERMAID_EDGES,
    logLevel: 'fatal',
  };
}

function isLocalFragmentUrl(value) {
  const text = String(value || '').trim();
  const unquoted = ((text.startsWith('"') && text.endsWith('"'))
    || (text.startsWith("'") && text.endsWith("'")))
    ? text.slice(1, -1).trim()
    : text;
  return /^#[A-Za-z_][\w:.-]*$/.test(unquoted);
}

export function isSafeMermaidResourceValue(value, { css = false } = {}) {
  const text = String(value || '');
  if (/\b(?:javascript|vbscript|data|blob|mailto|tel|about|chrome|resource|https?|ftp|file)\s*:/i.test(text)
      || /(?:^|[^:])\/\//.test(text)
      || /expression\s*\(|-moz-binding|behavior\s*:/i.test(text)) {
    return false;
  }

  let urlCount = 0;
  for (const match of text.matchAll(/url\s*\(([^)]*)\)/gi)) {
    urlCount += 1;
    if (!isLocalFragmentUrl(match[1])) return false;
  }
  const urlTokens = text.match(/url\s*\(/gi)?.length || 0;
  if (urlTokens !== urlCount) return false;

  if (css) {
    if (/@(?:import|font-face|namespace|supports|media|document|page)\b/i.test(text)) return false;
    const withoutKeyframes = text.replace(/@(?:-webkit-)?keyframes\s+[-\w]+/gi, '');
    if (withoutKeyframes.includes('@')) return false;
  }
  return true;
}

function numericDimension(value) {
  const match = String(value || '').trim().match(/^(?:\d+(?:\.\d+)?|\.\d+)(?:px)?$/i);
  if (!match) return null;
  const parsed = Number.parseFloat(match[0]);
  return Number.isFinite(parsed) && parsed > 0 ? parsed : null;
}

export function readMermaidSvgDimensions(root) {
  const viewBox = String(root?.getAttribute?.('viewBox') || '').trim();
  if (viewBox) {
    const values = viewBox.split(/[\s,]+/).map(Number);
    if (values.length !== 4 || values.some((value) => !Number.isFinite(value))
        || values[2] <= 0 || values[3] <= 0) {
      return null;
    }
    return { width: values[2], height: values[3] };
  }

  const width = numericDimension(root?.getAttribute?.('width'));
  const height = numericDimension(root?.getAttribute?.('height'));
  return width && height ? { width, height } : null;
}

function hasParserError(documentNode) {
  if (!documentNode?.documentElement) return true;
  try {
    return documentNode.getElementsByTagName('parsererror').length > 0;
  } catch {
    return true;
  }
}

export function sanitizeMermaidSvg(svg, {
  DOMParserType = globalThis.DOMParser,
  XMLSerializerType = globalThis.XMLSerializer,
  theme = null,
  resolveColor,
} = {}) {
  const source = String(svg || '').trim();
  if (!source.startsWith('<svg') || utf8ByteLength(source) > MAX_MERMAID_SVG_BYTES
      || /<!DOCTYPE|<!ENTITY|<\?xml-stylesheet/i.test(source)
      || typeof DOMParserType !== 'function' || typeof XMLSerializerType !== 'function') {
    return null;
  }

  let documentNode;
  try {
    documentNode = new DOMParserType().parseFromString(source, 'image/svg+xml');
  } catch {
    return null;
  }
  if (hasParserError(documentNode)) return null;

  const root = documentNode.documentElement;
  if (String(root.localName || root.nodeName).toLowerCase() !== 'svg'
      || root.namespaceURI !== SVG_NAMESPACE) {
    return null;
  }

  let elements;
  try {
    elements = Array.from(documentNode.getElementsByTagName('*'));
  } catch {
    return null;
  }
  for (const element of elements) {
    const localName = String(element.localName || element.nodeName || '').toLowerCase();
    if (!ALLOWED_SVG_ELEMENTS.has(localName) || element.namespaceURI !== SVG_NAMESPACE) {
      return null;
    }

    if (localName === 'style'
        && !isSafeMermaidResourceValue(element.textContent, { css: true })) {
      return null;
    }

    for (const attribute of Array.from(element.attributes || [])) {
      const name = String(attribute.name || '').toLowerCase();
      const local = String(attribute.localName || name).toLowerCase();
      const value = String(attribute.value || '');
      if (name.startsWith('on') || local === 'href' || local === 'src'
          || local === 'base' || local === 'target') {
        return null;
      }
      if (name === 'xmlns') {
        if (value !== SVG_NAMESPACE) return null;
        continue;
      }
      if (name === 'xmlns:xlink') {
        if (value !== XLINK_NAMESPACE) return null;
        continue;
      }
      if (!isSafeMermaidResourceValue(value, { css: name === 'style' })) return null;
    }
  }

  const dimensions = readMermaidSvgDimensions(root);
  if (!dimensions || dimensions.width > MAX_MERMAID_SVG_DIMENSION
      || dimensions.height > MAX_MERMAID_SVG_DIMENSION) {
    return null;
  }

  // 安全检查全部通过后才改颜色;写回的只有 rgb()/rgba() 字面量。
  if (theme) adaptMermaidBackgroundFills(root, theme, resolveColor ? { resolveColor } : undefined);

  let serialized;
  try {
    serialized = new XMLSerializerType().serializeToString(root);
  } catch {
    return null;
  }
  if (!serialized || utf8ByteLength(serialized) > MAX_MERMAID_SVG_BYTES) return null;
  return { svg: serialized, ...dimensions };
}

let nextRenderId = 1;

// mermaid 连同它的图布局依赖约 3 MB,是整个前端最大的一块;按需加载,只在正文里
// 第一次出现通过了 inspectMermaidSource 的图表时才拉取。加载失败不缓存失败结果,
// 下一张图重试。
let mermaidRuntimePromise = null;
export function loadMermaidRuntime() {
  if (!mermaidRuntimePromise) {
    mermaidRuntimePromise = import('mermaid')
      .then((module) => module.default || module)
      .catch((error) => {
        mermaidRuntimePromise = null;
        throw error;
      });
  }
  return mermaidRuntimePromise;
}

export function createMermaidRenderAdapter({
  runtime = null,
  DOMParserType = globalThis.DOMParser,
  XMLSerializerType = globalThis.XMLSerializer,
} = {}) {
  let queue = Promise.resolve();

  const run = async (source, theme) => {
    const inspected = inspectMermaidSource(source);
    if (!inspected.ok) return null;
    try {
      const engine = runtime || await loadMermaidRuntime();
      engine.initialize(mermaidConfig(theme));
      const renderSource = prepareMermaidRenderSource(inspected.source, inspected.family);
      const parsed = await engine.parse(renderSource, { suppressErrors: true });
      if (parsed === false) return null;
      const id = `ace-mermaid-${nextRenderId++}`;
      const rendered = await engine.render(id, renderSource);
      return sanitizeMermaidSvg(rendered?.svg, { DOMParserType, XMLSerializerType, theme });
    } catch {
      return null;
    }
  };

  return (source, theme) => {
    const pending = queue.then(() => run(source, theme), () => run(source, theme));
    queue = pending.then(() => undefined, () => undefined);
    return pending;
  };
}

export function installMermaidRenderer(
  win = globalThis.window,
  doc = globalThis.document,
  render = null,
) {
  const Observer = win?.MutationObserver || globalThis.MutationObserver;
  const BlobType = win?.Blob || globalThis.Blob;
  const urlApi = win?.URL || globalThis.URL;
  const DOMParserType = win?.DOMParser || globalThis.DOMParser;
  const XMLSerializerType = win?.XMLSerializer || globalThis.XMLSerializer;
  if (!doc?.documentElement || typeof Observer !== 'function'
      || typeof BlobType !== 'function' || typeof urlApi?.createObjectURL !== 'function'
      || typeof DOMParserType !== 'function' || typeof XMLSerializerType !== 'function') {
    return () => {};
  }

  const requestRender = typeof render === 'function'
    ? render
    : createMermaidRenderAdapter({ DOMParserType, XMLSerializerType });
  const jobs = new WeakMap();
  let stopped = false;
  let scheduled = false;

  const setState = (frame, state) => frame.setAttribute('data-mermaid-diagram', state);
  const invalidate = (frame) => {
    const active = jobs.get(frame);
    active?.release?.();
    jobs.set(frame, { token: {} });
  };
  const restoreSource = (frame) => {
    invalidate(frame);
    const target = frame.querySelector(TARGET_SELECTOR);
    target?.replaceChildren();
    target?.setAttribute('aria-busy', 'false');
    frame.removeAttribute('data-mermaid-theme');
    setState(frame, 'source');
  };

  const renderFrame = async (frame) => {
    if (stopped || frame.getAttribute('data-mermaid-diagram') !== 'source') return;
    const sourceNode = frame.querySelector(SOURCE_SELECTOR);
    const target = frame.querySelector(TARGET_SELECTOR);
    const source = String(sourceNode?.textContent || '');
    if (!target || !source.trim()) return;

    const token = {};
    const theme = mermaidTheme(doc, frame);
    jobs.set(frame, { token, source, theme });
    setState(frame, 'loading');
    target.setAttribute('aria-busy', 'true');
    const result = await requestRender(source, theme);
    const active = jobs.get(frame);
    if (stopped || active?.token !== token || frame.isConnected === false) {
      return;
    }
    if (String(sourceNode.textContent || '') !== source || mermaidTheme(doc, frame) !== theme) {
      restoreSource(frame);
      schedule();
      return;
    }
    if (!result) {
      target.setAttribute('aria-busy', 'false');
      setState(frame, 'error');
      return;
    }

    const owner = frame.ownerDocument || doc;
    const previewTrigger = owner.createElement('button');
    const image = owner.createElement('img');
    const objectUrl = urlApi.createObjectURL(
      new BlobType([result.svg], { type: 'image/svg+xml;charset=utf-8' }),
    );
    let released = false;
    const release = () => {
      if (released) return;
      released = true;
      try { urlApi.revokeObjectURL(objectUrl); } catch { /* best effort */ }
    };
    jobs.set(frame, { token, source, theme, release });
    previewTrigger.type = 'button';
    previewTrigger.className = 'ace-mermaid-preview-trigger';
    previewTrigger.title = '放大预览 Mermaid 图表';
    previewTrigger.setAttribute('aria-label', '放大预览 Mermaid 图表');
    registerMermaidExportTarget(previewTrigger, {
      source,
      svg: result.svg,
      width: result.width,
      height: result.height,
      theme,
    });
    previewTrigger.addEventListener('click', () => {
      dispatchMermaidPreview(win, {
        source,
        svg: result.svg,
        width: result.width,
        height: result.height,
        alt: image.alt,
        theme,
      });
    });
    image.className = 'ace-mermaid-svg';
    image.alt = 'Mermaid diagram';
    // 宽度用 SVG viewBox 的原始尺寸(SVG 的百分比宽度不是图片固有尺寸,不写会被放大到栏宽);
    // 高度交给 CSS 按比例算:比对话栏宽的图整体等比缩到栏宽,不出横向滚动条,细节点开预览再放大。
    image.style.width = `${result.width}px`;
    image.width = Math.ceil(result.width);
    image.height = Math.ceil(result.height);
    image.decoding = 'async';
    image.draggable = false;
    image.addEventListener('load', () => {
      release();
      if (stopped || jobs.get(frame)?.token !== token || frame.isConnected === false) return;
      if (String(sourceNode.textContent || '') !== source || mermaidTheme(doc, frame) !== theme) {
        restoreSource(frame);
        schedule();
        return;
      }
      target.setAttribute('aria-busy', 'false');
      frame.setAttribute('data-mermaid-theme', theme);
      setState(frame, 'ready');
    }, { once: true });
    image.addEventListener('error', () => {
      release();
      if (stopped || jobs.get(frame)?.token !== token || frame.isConnected === false) return;
      if (String(sourceNode.textContent || '') !== source || mermaidTheme(doc, frame) !== theme) {
        restoreSource(frame);
        schedule();
        return;
      }
      target.replaceChildren();
      target.setAttribute('aria-busy', 'false');
      setState(frame, 'error');
    }, { once: true });
    image.src = objectUrl;
    previewTrigger.append(image);
    target.replaceChildren(previewTrigger);
  };

  const scan = () => {
    scheduled = false;
    if (stopped) return;
    for (const frame of doc.querySelectorAll(SOURCE_DIAGRAM_SELECTOR)) renderFrame(frame);
  };
  const schedule = () => {
    if (stopped || scheduled) return;
    scheduled = true;
    const enqueue = win?.queueMicrotask || globalThis.queueMicrotask;
    if (typeof enqueue === 'function') enqueue(scan);
    else Promise.resolve().then(scan);
  };
  const refreshThemes = (changedAncestors) => {
    if (stopped || !changedAncestors.length) return;
    for (const frame of doc.querySelectorAll(DIAGRAM_SELECTOR)) {
      const theme = jobs.get(frame)?.theme;
      // Ignore mutations inside the diagram (including our own image styles).
      if (theme && changedAncestors.some((ancestor) => ancestor.contains(frame))
          && mermaidTheme(doc, frame) !== theme) restoreSource(frame);
    }
    schedule();
  };
  const onBackgroundTransition = (event) => {
    // Global theme transitions still expose the previous color when attributes
    // first change. Recheck the painted color when the transition settles.
    if (event.propertyName === 'background-color') refreshThemes([event.target]);
  };
  doc.addEventListener('transitionend', onBackgroundTransition);
  doc.addEventListener('transitioncancel', onBackgroundTransition);

  const observer = new Observer((records) => {
    refreshThemes(records.filter((record) => record.type === 'attributes').map((record) => record.target));
    schedule();
  });
  observer.observe(doc.documentElement, {
    attributes: true,
    attributeFilter: ['data-theme', 'data-color-theme', 'data-theme-session-background', 'style', 'class'],
    childList: true,
    subtree: true,
  });
  schedule();

  return () => {
    stopped = true;
    observer.disconnect();
    doc.removeEventListener('transitionend', onBackgroundTransition);
    doc.removeEventListener('transitioncancel', onBackgroundTransition);
    for (const frame of doc.querySelectorAll(DIAGRAM_SELECTOR)) invalidate(frame);
  };
}
