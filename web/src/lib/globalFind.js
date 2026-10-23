import { markTextMatches, unwrapMarks } from './domTextMarks.js';
import { isCustomShortcut, isShortcut, isShortcutRecording } from './shortcutBindings.js';

const FIND_HIGHLIGHT = 'ace-global-find-match';
const FIND_ACTIVE_HIGHLIGHT = 'ace-global-find-active';

export const CONVERSATION_FIND_OPEN_EVENT = 'acecode:conversation-find-open';

// 查找时最多加载到这么多条对话记录(transcript 条目)就停止拉更早的历史。
// 实测 6 万条记录的会话:不设上限逐页拉 8 分钟到约 3 万行时 JS 堆 1.5 GB、
// 渲染进程 4.5 GB 仍未拉完;上限 5000 条(24 页)约 6 秒加载完,JS 堆约 250 MB。
export const FIND_HISTORY_ITEM_LIMIT = 5000;

const SKIP_TAGS = new Set(['SCRIPT', 'STYLE', 'NOSCRIPT', 'TEMPLATE']);

// 查找实际覆盖的范围:还有更早的历史没加载时为部分搜索,并给出已加载的消息条数。
export function conversationFindScope(state) {
  const items = Array.isArray(state?.items) ? state.items : [];
  return {
    partial: state?.historyHasMore === true,
    messageCount: items.reduce((count, item) => count + (item?.kind === 'msg' ? 1 : 0), 0),
  };
}

function mergeFindScopes(results) {
  for (const result of results) {
    const scope = result.status === 'fulfilled' ? result.value : null;
    if (scope?.partial) return { partial: true, messageCount: Number(scope.messageCount) || 0 };
  }
  return { partial: false, messageCount: 0 };
}

// 广播「打开查找」,并收集监听方经 detail.waitUntil 交回的任务(ChatView 借此
// 交回「加载全部历史」的 Promise,兑现值为 conversationFindScope)。查找在 DOM
// 文本上执行,内容还在分页加载时收集到的匹配会被随后的重绘作废,所以调用方要等
// ready 之后再收集;ready 兑现为合并后的搜索范围。
export function requestConversationFindContent(target = globalThis.window) {
  const pending = [];
  const EventCtor = target?.CustomEvent || globalThis.CustomEvent;
  if (target?.dispatchEvent && EventCtor) {
    target.dispatchEvent(new EventCtor(CONVERSATION_FIND_OPEN_EVENT, {
      detail: {
        waitUntil(task) {
          if (task && typeof task.then === 'function') pending.push(task);
        },
      },
    }));
  }
  return {
    loading: pending.length > 0,
    ready: Promise.allSettled(pending).then(mergeFindScopes),
  };
}

function hasFindModifier(event) {
  return !!(event && (event.ctrlKey || event.metaKey));
}

export function isFindShortcut(event) {
  if (!event || isShortcutRecording(event)) return false;
  if (isCustomShortcut('find')) return isShortcut(event, 'find');
  if (event.altKey) return false;
  const key = typeof event.key === 'string' ? event.key.toLowerCase() : '';
  const code = typeof event.code === 'string' ? event.code.toLowerCase() : '';
  if ((key === 'f3' || code === 'f3') && !event.ctrlKey && !event.metaKey) {
    return true;
  }
  if (!hasFindModifier(event)) return false;
  return key === 'f';
}

export function isComposingInputEvent(event, composing = false) {
  return !!(
    composing ||
    event?.isComposing ||
    event?.nativeEvent?.isComposing
  );
}

export function canOpenConversationFind({
  view = '',
  activeSessionId = '',
  loop = false,
  showSettings = false,
  searchOpen = false,
  updateDialogOpen = false,
  permissionOpen = false,
  questionOpen = false,
  guidedTourPreparing = false,
  guidedTourRun = false,
} = {}) {
  return view === 'single'
    && !!activeSessionId
    && !loop
    && !showSettings
    && !searchOpen
    && !updateDialogOpen
    && !permissionOpen
    && !questionOpen
    && !guidedTourPreparing
    && !guidedTourRun;
}

export function findMatchesInText(text, query) {
  const source = String(text || '');
  const needle = String(query || '');
  if (!source || !needle) return [];
  const haystack = source.toLocaleLowerCase();
  const normalizedNeedle = needle.toLocaleLowerCase();
  const matches = [];
  let index = 0;
  while (index <= haystack.length - normalizedNeedle.length) {
    const found = haystack.indexOf(normalizedNeedle, index);
    if (found < 0) break;
    matches.push({ start: found, end: found + normalizedNeedle.length });
    index = found + Math.max(1, normalizedNeedle.length);
  }
  return matches;
}

function nodeWindow(node) {
  return node?.ownerDocument?.defaultView || globalThis.window || null;
}

function isHiddenElement(element, root) {
  const view = nodeWindow(element);
  for (let el = element; el && el !== root?.parentElement; el = el.parentElement) {
    if (el.nodeType !== 1) continue;
    if (el.classList?.contains('ace-global-find')) return true;
    if (el.classList?.contains(FIND_HIGHLIGHT)) return true;
    if (SKIP_TAGS.has(el.tagName)) return true;
    if (el.hidden || el.getAttribute?.('aria-hidden') === 'true') return true;
    if (!view?.getComputedStyle) continue;
    const style = view.getComputedStyle(el);
    if (style.display === 'none' || style.visibility === 'hidden') return true;
  }
  return false;
}

function isSearchableTextNode(node, root) {
  if (!node || node.nodeType !== 3) return false;
  if (!String(node.nodeValue || '').trim()) return false;
  const parent = node.parentElement;
  if (!parent) return false;
  return !isHiddenElement(parent, root);
}

export function collectFindMatches(root, query) {
  if (!root || !query) return [];
  const doc = root.ownerDocument || globalThis.document;
  if (!doc?.createTreeWalker) return [];
  const view = doc.defaultView || globalThis.window || {};
  const nodeFilter = view.NodeFilter || globalThis.NodeFilter || {};
  const walker = doc.createTreeWalker(
    root,
    nodeFilter.SHOW_TEXT || 4,
    {
      acceptNode(node) {
        return isSearchableTextNode(node, root)
          ? (nodeFilter.FILTER_ACCEPT || 1)
          : (nodeFilter.FILTER_REJECT || 2);
      },
    },
  );
  const matches = [];
  let node = walker.nextNode();
  while (node) {
    for (const match of findMatchesInText(node.nodeValue, query)) {
      matches.push({ node, start: match.start, end: match.end, query });
    }
    node = walker.nextNode();
  }
  return matches;
}

// 匹配记录的是「文字节点 + 字符偏移」。高亮会把节点切短(splitText),对话记录
// 重绘又会把 <mark> 连同节点一起换掉,所以偏移只在节点仍在文档里、长度够、
// 文字也还对得上时才可用。
function matchOffsetsUsable(match) {
  const node = match?.node;
  if (!node || node.nodeType !== 3 || node.isConnected === false) return false;
  const { start, end } = match;
  if (!Number.isInteger(start) || !Number.isInteger(end)) return false;
  const text = String(node.nodeValue || '');
  if (start < 0 || end <= start || end > text.length) return false;
  if (match.query == null) return true;
  return text.slice(start, end).toLocaleLowerCase()
    === String(match.query).toLocaleLowerCase();
}

export function isFindMatchStale(match) {
  if (match?.mark) return !match.mark.isConnected;
  return !matchOffsetsUsable(match);
}

export function findMatchesNeedRecollect(matches) {
  return Array.isArray(matches) && matches.some(isFindMatchStale);
}

function rangeForMatch(match, doc = globalThis.document) {
  if (!doc?.createRange || !matchOffsetsUsable(match)) return null;
  try {
    const range = doc.createRange();
    range.setStart(match.node, match.start);
    range.setEnd(match.node, match.end);
    return range;
  } catch {
    return null;
  }
}

export function clearFindHighlights(doc = globalThis.document) {
  unwrapMarks(doc?.body || doc, FIND_HIGHLIGHT);
}

export function applyFindHighlights(matches, activeIndex, doc = globalThis.document) {
  return markTextMatches(matches, {
    className: FIND_HIGHLIGHT,
    activeClassName: FIND_ACTIVE_HIGHLIGHT,
    activeIndex,
  });
}

export function scrollFindMatchIntoView(match, doc = globalThis.document) {
  if (match?.mark?.isConnected) {
    match.mark.scrollIntoView?.({ block: 'center', inline: 'nearest' });
    return;
  }
  const range = rangeForMatch(match, doc);
  if (!range) return;
  const rect = range.getBoundingClientRect();
  const view = doc.defaultView || globalThis.window;
  if (!view || !Number.isFinite(rect.top)) return;
  const margin = 88;
  const outside =
    rect.top < margin ||
    rect.bottom > view.innerHeight - margin ||
    rect.left < 0 ||
    rect.right > view.innerWidth;
  if (!outside) return;
  match.node.parentElement?.scrollIntoView?.({ block: 'center', inline: 'nearest' });
}

export function selectFindMatch(match, doc = globalThis.document) {
  const range = match?.mark?.isConnected && doc?.createRange
    ? doc.createRange()
    : rangeForMatch(match, doc);
  if (match?.mark?.isConnected && range) range.selectNodeContents(match.mark);
  const selection = doc?.defaultView?.getSelection?.();
  if (!range || !selection) return;
  selection.removeAllRanges();
  selection.addRange(range);
}

export function clearFindSelection(doc = globalThis.document) {
  doc?.defaultView?.getSelection?.()?.removeAllRanges?.();
}
