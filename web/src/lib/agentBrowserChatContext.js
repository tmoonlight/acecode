const MAX_BROWSER_CONTEXT_TEXT = 120000;
let browserContextSequence = 0;

function asText(value) {
  return value == null ? '' : String(value);
}

function clip(value, limit = MAX_BROWSER_CONTEXT_TEXT) {
  const text = asText(value);
  return text.length > limit ? `${text.slice(0, limit)}\n[truncated]` : text;
}

function nextBrowserContextId(kind, now = Date.now()) {
  browserContextSequence = (browserContextSequence + 1) % 0x100000;
  return `browser-${kind}-${now}-${browserContextSequence.toString(36)}`;
}

function pageNote({ title = '', url = '' } = {}) {
  const safeTitle = asText(title).trim();
  if (safeTitle && safeTitle !== '新标签页') return safeTitle;
  try {
    return new URL(asText(url)).host || asText(url);
  } catch {
    return asText(url);
  }
}

function formatElementPath(ancestors = []) {
  return (Array.isArray(ancestors) ? ancestors : [])
    .map((ancestor) => {
      const tag = asText(ancestor?.tagName || 'element');
      const id = ancestor?.id ? `#${ancestor.id}` : '';
      const classes = Array.isArray(ancestor?.classNames)
        ? ancestor.classNames.filter(Boolean).map((name) => `.${name}`).join('')
        : '';
      return `${tag}${id}${classes}`;
    })
    .join(' > ');
}

export function createAgentBrowserElementContext(element = {}, page = {}, now = Date.now()) {
  if (!element || typeof element !== 'object' || !asText(element.outerHTML).trim()) return null;
  const id = nextBrowserContextId('element', now);
  const label = asText(element.name || element.tagName || '网页元素').trim() || '网页元素';
  const url = asText(element.url || page.url);
  const path = formatElementPath(element.ancestors);
  const dimensions = element.dimensions || element.bounds || {};
  const sections = [
    'Attached Element Context from ACECode Agent Browser',
    `Element: ${label}`,
    url ? `URL: ${url}` : '',
    path ? `HTML Path: ${path}` : '',
    `Outer HTML:\n\`\`\`html\n${clip(element.outerHTML, 40000)}\n\`\`\``,
    `Dimensions:\n- top: ${Math.round(Number(dimensions.top ?? dimensions.y) || 0)}px\n- left: ${Math.round(Number(dimensions.left ?? dimensions.x) || 0)}px\n- width: ${Math.round(Number(dimensions.width) || 0)}px\n- height: ${Math.round(Number(dimensions.height) || 0)}px`,
    element.computedStyle
      ? `Computed CSS:\n\`\`\`css\n${clip(element.computedStyle, 40000)}\n\`\`\``
      : '',
    element.innerText ? `Visible text:\n${clip(element.innerText, 20000)}` : '',
  ].filter(Boolean);
  return {
    type: 'browser',
    kind: 'element',
    id,
    local_id: id,
    label,
    note: pageNote({ title: element.title || page.title, url }),
    content: clip(sections.join('\n\n')),
    source: {
      page_id: asText(page.page_id || page.pageId),
      url,
      title: asText(element.title || page.title),
    },
  };
}

export function createAgentBrowserConsoleContext(payload = {}, now = Date.now()) {
  const logs = clip(payload.logs, MAX_BROWSER_CONTEXT_TEXT).trim();
  if (!logs) return null;
  const id = nextBrowserContextId('console', now);
  const url = asText(payload.url);
  const title = asText(payload.title);
  return {
    type: 'browser',
    kind: 'console',
    id,
    local_id: id,
    label: '控制台日志',
    note: pageNote({ title, url }),
    content: [
      'Console logs captured from ACECode Agent Browser',
      url ? `URL: ${url}` : '',
      `Logs:\n\`\`\`text\n${logs}\n\`\`\``,
    ].filter(Boolean).join('\n\n'),
    source: {
      page_id: asText(payload.page_id || payload.pageId),
      url,
      title,
    },
  };
}
