import { adaptMermaidBackgroundFills, mermaidTheme, mermaidThemeVariables } from './mermaidTheme.js';
import { registerMermaidExportTarget } from './mermaidExport.js';
import { dispatchMermaidPreview } from './mermaidPreview.js';
import { repairMermaidSource } from './mermaidRepair.js';
import {
  inspectMermaidModel,
  inspectMermaidSource,
  MAX_MERMAID_SOURCE_BYTES,
  mermaidErrorLine,
  prepareMermaidRenderSource,
} from './mermaidSource.js';

export { mermaidTheme } from './mermaidTheme.js';
export { repairMermaidSource } from './mermaidRepair.js';
export {
  dropMermaidInteractions,
  escapeMermaidSequenceText,
  inspectMermaidModel,
  inspectMermaidSource,
  MAX_MERMAID_SOURCE_BYTES,
  MAX_MERMAID_SOURCE_LINES,
  mermaidErrorLine,
  neutralizeMermaidMarkup,
  normalizeMermaidEntities,
  prepareMermaidRenderSource,
  stripMermaidPreamble,
} from './mermaidSource.js';

export const MAX_MERMAID_EDGES = 500;
export const MAX_MERMAID_SVG_BYTES = 4 * 1024 * 1024;
export const MAX_MERMAID_SVG_DIMENSION = 32768;

const SVG_NAMESPACE = 'http://www.w3.org/2000/svg';
const XLINK_NAMESPACE = 'http://www.w3.org/1999/xlink';
const DIAGRAM_SELECTOR = '[data-mermaid-diagram]';
const SOURCE_DIAGRAM_SELECTOR = '[data-mermaid-diagram="source"]';
const SOURCE_SELECTOR = '[data-mermaid-source="true"]';
const TARGET_SELECTOR = '[data-mermaid-render-target="true"]';

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

function utf8ByteLength(value) {
  return new TextEncoder().encode(String(value || '')).byteLength;
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

// 标签用 SVG 文字画(htmlLabels 关着)时,流程图 / 类图 / 状态图 / ER 图只把 `#lt;` `#gt;` `#amp;`
// 还原成字符,`#quot;`、`#35;` 这类在图上原样显示成 `&quot;`、`&#35;`(时序图没有这个问题)。
// 这些正是 Mermaid 文档教人在标签里写引号、井号的办法,这里替它补上最后一步。
const LEFTOVER_ENTITY = /&(quot|apos|#\d{1,7}|#x[0-9a-f]{1,6});/gi;

function decodeLeftoverEntities(elements) {
  for (const element of elements) {
    const name = String(element.localName || element.nodeName || '').toLowerCase();
    if (name !== 'text' && name !== 'tspan') continue;
    for (let node = element.firstChild; node; node = node.nextSibling) {
      if (node.nodeType !== 3 || !node.data || !node.data.includes('&')) continue;
      const decoded = node.data.replace(LEFTOVER_ENTITY, (match, body) => {
        const entity = body.toLowerCase();
        if (entity === 'quot') return '"';
        if (entity === 'apos') return "'";
        const code = entity[1] === 'x' ? Number.parseInt(entity.slice(2), 16) : Number.parseInt(entity.slice(1), 10);
        // 控制字符、代理区和超出范围的码点不是合法的 XML 文字,原样留着。
        if (!(code >= 0x20 && code <= 0x10FFFF) || (code >= 0xD800 && code <= 0xDFFF)) return match;
        return String.fromCodePoint(code);
      });
      if (decoded !== node.data) node.data = decoded;
    }
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

  // 安全检查全部通过后才改内容。先把没解开的实体码还原成字符(只改文字节点的文本,不产生标记)。
  decodeLeftoverEntities(elements);
  // 再改颜色;写回的只有 rgb()/rgba() 字面量。
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

function errorText(error) {
  if (!error) return '';
  try {
    return String(typeof error.message === 'string' ? error.message : error).slice(0, 400);
  } catch {
    return '';
  }
}

// 画不出来时的返回值:stage 是卡在哪一层(gate 闸门 / load 组件没加载到 / parse 语法 /
// render 绘制 / sanitize 结果没过检查),reason 是该层给的原因,line 是能对回原文时的行号。
export function mermaidFailure(stage, reason = '', { line = 0 } = {}) {
  return { failure: { stage, reason: String(reason || ''), line: Number(line) || 0 } };
}

// 显示在源码下方的一行说明。问题反馈过来时往往只有一张照片或截图,从图上要能看出卡在哪一层。
export function mermaidFailureNote(failure) {
  const stage = failure?.stage;
  if (stage === 'load') return '图表组件加载失败';
  if (stage === 'parse') return failure.line > 0 ? `第 ${failure.line} 行语法有误` : '图表语法有误';
  if (stage === 'sanitize') return '图表绘制结果未通过检查';
  if (stage === 'gate') {
    if (failure.reason === 'unsupported-family') return '暂不支持这种图表类型';
    if (failure.reason === 'too-large' || failure.reason === 'too-many-lines') return '图表过大';
    if (failure.reason === 'empty') return '';
    return '图表含有不支持的写法';
  }
  return '图表绘制失败';
}

export function createMermaidRenderAdapter({
  runtime = null,
  DOMParserType = globalThis.DOMParser,
  XMLSerializerType = globalThis.XMLSerializer,
} = {}) {
  let queue = Promise.resolve();

  // 解析 → 检查解析出来的模型 → 绘制。失败返回 { stage, error },不抛。
  const draw = async (engine, renderSource) => {
    try {
      if (await engine.parse(renderSource) === false) return { stage: 'parse', error: null };
    } catch (error) {
      return { stage: 'parse', error };
    }
    // 真正的 mermaid 一定带 mermaidAPI;带了却取不到模型就不画(测试用的替身没有 mermaidAPI)。
    if (engine.mermaidAPI) {
      let reason;
      try {
        reason = inspectMermaidModel((await engine.mermaidAPI.getDiagramFromText(renderSource))?.db);
      } catch (error) {
        return { stage: 'render', error };
      }
      if (reason) return { stage: 'gate', error: new Error(reason) };
    }
    try {
      const rendered = await engine.render(`ace-mermaid-${nextRenderId++}`, renderSource);
      return { svg: rendered?.svg };
    } catch (error) {
      return { stage: 'render', error };
    }
  };

  const run = async (source, theme) => {
    const inspected = inspectMermaidSource(source);
    if (!inspected.ok) return mermaidFailure('gate', inspected.reason);
    let engine;
    try {
      engine = runtime || await loadMermaidRuntime();
    } catch (error) {
      return mermaidFailure('load', errorText(error));
    }
    try {
      engine.initialize(mermaidConfig(theme));
      const renderSource = prepareMermaidRenderSource(inspected.source, inspected.family);
      let drawn = await draw(engine, renderSource);
      let repaired = false;
      if (!drawn.svg) {
        // 模型常见的语法小错修一遍再试;修过的文本要重新过一遍闸门。
        const candidate = repairMermaidSource(renderSource, inspected.family, errorText(drawn.error));
        if (candidate !== renderSource && inspectMermaidSource(candidate).ok) {
          const second = await draw(engine, candidate);
          if (second.svg) {
            drawn = second;
            repaired = true;
          }
        }
      }
      if (!drawn.svg) {
        // 报错文字里的行号从 1 起(error.hash.line 从 0 起,不用它)。
        const reported = /\bon line (\d+)/i.exec(errorText(drawn.error))?.[1];
        return mermaidFailure(drawn.stage, errorText(drawn.error), {
          line: drawn.stage === 'parse' ? mermaidErrorLine(renderSource, Number(reported), inspected.family) : 0,
        });
      }
      const clean = sanitizeMermaidSvg(drawn.svg, { DOMParserType, XMLSerializerType, theme });
      if (!clean) return mermaidFailure('sanitize', '');
      return repaired ? { ...clean, repaired: true } : clean;
    } catch (error) {
      return mermaidFailure('render', errorText(error));
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
    frame.removeAttribute('data-mermaid-error');
    setState(frame, 'source');
  };
  // 画不出来:源码照旧显示,下方加一行说明,卡在哪一层记在框上。
  const showFailure = (frame, target, failure) => {
    const note = mermaidFailureNote(failure);
    const children = [];
    if (note) {
      const owner = frame.ownerDocument || doc;
      const line = owner.createElement('div');
      line.className = 'ace-mermaid-error-note';
      line.textContent = note;
      // 组件没取到多半是页面开着的时候前端换了新版本,重新加载页面就好。给一个按钮:桌面版里
      // 按 F5 不管用。
      if (failure.stage === 'load') {
        const reload = owner.createElement('button');
        reload.type = 'button';
        reload.className = 'ace-mermaid-error-action';
        reload.textContent = '重新加载';
        reload.addEventListener('click', () => win?.location?.reload?.());
        line.append(reload);
      }
      children.push(line);
    }
    target.replaceChildren(...children);
    target.setAttribute('aria-busy', 'false');
    frame.setAttribute('data-mermaid-error', [failure.stage, failure.reason].filter(Boolean).join(': ').slice(0, 400));
    setState(frame, 'error');
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
    if (!result?.svg) {
      showFailure(frame, target, result?.failure || { stage: 'render', reason: '' });
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
      showFailure(frame, target, { stage: 'image', reason: '' });
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
