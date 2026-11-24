// 文件详情里的 HTML 页面预览与可视化编辑(docs/html-preview.md)。
//
// - 显示:页面原文进只有 allow-scripts + allow-modals 的 srcdoc 隔离框(不透明源,调不到本机
//   /api/*),<base href> 指向 daemon 的页面资源站点,相对路径的 CSS / 图片 / 脚本照常加载。
// - 编辑:只在桌面版、文件可编辑时开放。元素编号只插进隔离框里的副本;每次修改都算成一次源码区间
//   替换写进草稿(edit.text),同时让框内直接改 DOM,不重新加载页面;撤销 / 重做也在框内原地重放,
//   只有外部改动才重新渲染。改字时拖选的一段文字由框内套格式,离开改字时整段内容一次写回。
// - 保存仍走文件详情原有的草稿通道(Ctrl+S、未保存圆点、磁盘冲突提示)。
import { useCallback, useEffect, useLayoutEffect, useMemo, useRef, useState } from 'react';
import { parse } from 'parse5';
import '../styles/html-page-preview.css';
import {
  DESKTOP_CONTEXT_ACTION_EVENT,
  DESKTOP_CONTEXT_ACTIONS,
  openDesktopContextMenu,
} from '../lib/desktopContextMenu.js';
import { editingHistoryAction } from '../lib/editingHistoryShortcuts.js';
import { openExternalUrl } from '../lib/externalUrl.js';
import {
  HTML_PAGE_PROTOCOL,
  HTML_PAGE_SANDBOX,
  buildHtmlPageSrcdoc,
  createHtmlPageNonce,
  htmlElementCapabilities,
  htmlPageMessage,
  htmlSiteBaseHref,
  htmlSiteLinkToPath,
  htmlSiteRootUrl,
  isWindowsRedoKey,
  normalizeTextCaret,
  normalizeTextRange,
  splitRangeStyles,
} from '../lib/htmlPageFrame.js';
import {
  applyHtmlSplice,
  assignNewHtmlIds,
  attributeSplice,
  collectHtmlElementRecords,
  deleteElementSplice,
  indexHtmlSource,
  innerContentSplice,
  innerSourceText,
  instrumentHtmlRange,
  lineRangeForOffsets,
  outerSourceText,
  readSourceAttribute,
  remapHtmlIndex,
  sourceStyleValues,
  styleSplice,
  wrapLinkSplices,
} from '../lib/htmlSourceMap.js';
import { createHtmlRegionContext, normalizeHtmlRegion } from '../lib/htmlRegionContext.js';
import { createSelectionContext } from '../lib/selectionChatContext.js';
import { isMacShortcutHost, isShortcut } from '../lib/shortcutBindings.js';
import { copyTextToSystemClipboard } from '../lib/systemClipboard.js';
import { HtmlEditToolbar } from './HtmlEditToolbar.jsx';
import { toast } from './Toast.jsx';

const HISTORY_LIMIT = 100;
// 选区的工具栏只有「AI 编辑」。
const REGION_CAPS = Object.freeze({});
const HISTORY_MERGE_MS = 1000;

// 框内的点击会让本页面获得用户激活,框内脚本伪造不了;打开链接 / 文件必须有用户操作。
function parentUserActive(iframe) {
  const activation = globalThis.navigator?.userActivation;
  if (activation && typeof activation.isActive === 'boolean') return activation.isActive;
  return !!iframe && globalThis.document?.activeElement === iframe;
}

function readAccent() {
  try {
    const style = getComputedStyle(document.documentElement);
    const accent = style.getPropertyValue('--ace-accent').trim();
    if (/^#[0-9a-f]{3,8}$|^rgba?\([^;{}<>]*\)$/i.test(accent)) return accent;
  } catch {
    // 读不到主题变量时用默认蓝。
  }
  return '#2563eb';
}

function safeExternalUrl(value) {
  try {
    const url = new URL(String(value || ''));
    return ['http:', 'https:', 'mailto:'].includes(url.protocol) ? url.href : '';
  } catch {
    return '';
  }
}

function pinSelectionContext(selectionContext) {
  const detail = {
    action: DESKTOP_CONTEXT_ACTIONS.ADD_SELECTION_CONTEXT,
    target: null,
    handled: false,
    selectionContext,
  };
  window.dispatchEvent(new CustomEvent(DESKTOP_CONTEXT_ACTION_EVENT, { detail }));
  if (!detail.handled) toast({ kind: 'err', text: '输入框当前不可用' });
}

export default function HtmlPagePreview({
  api,
  cwd,
  path,
  sourcePath,
  text,
  contentRevision = '',
  editable = false,
  disabled = false,
  editing = false,
  onEditingChange,
  onChange,
  onSave,
  onOpenFile,
  onAddContext,
}) {
  const iframeRef = useRef(null);
  const hostRef = useRef(null);
  const [site, setSite] = useState({ status: 'loading', url: '', entry: '' });
  const [frame, setFrame] = useState(null);
  const [selection, setSelection] = useState(null);
  const [textEditing, setTextEditing] = useState(false);
  // 改字时框里拖选出的一段文字({ sid, text, href, styles }):有它时工具栏的文字格式只作用在这段上。
  const [textRange, setTextRange] = useState(null);
  // 编辑模式里拖出的一块选区(htmlRegionContext.js::normalizeHtmlRegion 的结果);和元素选中互斥。
  const [region, setRegion] = useState(null);
  const [bounds, setBounds] = useState(null);

  const textRef = useRef(String(text ?? ''));
  const ownTextRef = useRef(null);
  const indexRef = useRef(null);
  const nonceRef = useRef('');
  const scrollRef = useRef({ x: 0, y: 0 });
  const historyRef = useRef({ undo: [], redo: [] });
  const selectionRef = useRef(null);
  const editingRef = useRef(false);
  // 外层发起改字时记下的编号;框内交回的文字只认这一个(框内消息不可信,见下)。
  const pendingTextEditRef = useRef(0);
  const textRangeRef = useRef(null);
  const regionRef = useRef(null);
  const accent = useMemo(readAccent, []);
  // 两次外部变更之间以自己写出的草稿为准:页签状态回传晚一拍时,不能拿旧文本配新编号表。
  textRef.current = ownTextRef.current ?? String(text ?? '');
  selectionRef.current = selection;
  textRangeRef.current = textRange;
  regionRef.current = region;
  const editActive = editing && editable && !disabled;
  editingRef.current = editActive;

  const post = useCallback((message) => {
    const target = iframeRef.current?.contentWindow;
    if (!target || !nonceRef.current) return;
    target.postMessage({ ...message, ace_html: HTML_PAGE_PROTOCOL, nonce: nonceRef.current }, '*');
  }, []);

  // ---- 资源站点 ----------------------------------------------------------------
  useEffect(() => {
    let cancelled = false;
    setSite({ status: 'loading', url: '', entry: '' });
    if (!cwd || !path || typeof api?.htmlSite !== 'function') {
      setSite({ status: 'ok', url: '', entry: '' });
      return undefined;
    }
    api.htmlSite(cwd, path).then((result) => {
      if (!cancelled) setSite({ status: 'ok', url: result?.url || '', entry: result?.entry || '' });
    }).catch(() => {
      // 旧 daemon 没有这条路由:页面照常显示,只是相对路径的资源加载不到。
      if (!cancelled) setSite({ status: 'ok', url: '', entry: '' });
    });
    return () => { cancelled = true; };
  }, [api, cwd, path]);

  // ---- 渲染 --------------------------------------------------------------------
  const renderFrame = useCallback((nextText) => {
    const nonce = createHtmlPageNonce();
    let index = null;
    if (editable) {
      try {
        index = indexHtmlSource(nextText, parse);
      } catch {
        index = null;
      }
    }
    indexRef.current = index;
    nonceRef.current = nonce;
    pendingTextEditRef.current = 0;
    setSelection(null);
    setTextEditing(false);
    setTextRange(null);
    setRegion(null);
    setFrame({
      nonce,
      srcdoc: buildHtmlPageSrcdoc({
        text: nextText,
        index,
        doctypeEnd: index ? index.doctypeEnd : undefined,
        baseHref: htmlSiteBaseHref(site.url),
        config: {
          nonce,
          editing: editingRef.current && !!index,
          siteBase: htmlSiteRootUrl(site.url, site.entry),
          forwardContextMenu: true,
          scroll: scrollRef.current,
          accent,
        },
      }),
    });
  }, [accent, editable, site.entry, site.url]);

  useEffect(() => {
    if (site.status !== 'ok') return;
    const current = String(text ?? '');
    if (ownTextRef.current !== null && current === ownTextRef.current) return;
    ownTextRef.current = null;
    textRef.current = current;
    historyRef.current = { undo: [], redo: [] };
    renderFrame(current);
  }, [renderFrame, site.status, text]);

  useEffect(() => {
    post({ type: 'mode', editing: editActive && !!indexRef.current });
    if (!editActive) {
      setSelection(null);
      setTextRange(null);
      setRegion(null);
    }
  }, [editActive, post]);

  useLayoutEffect(() => {
    const host = hostRef.current;
    if (!host) return undefined;
    const update = () => {
      const next = { width: host.clientWidth, height: host.clientHeight };
      setBounds((previous) => (previous && previous.width === next.width && previous.height === next.height
        ? previous : next));
    };
    update();
    const observer = typeof ResizeObserver === 'function' ? new ResizeObserver(update) : null;
    observer?.observe(host);
    return () => observer?.disconnect();
  }, []);

  // ---- 写回草稿 ----------------------------------------------------------------
  // 每一步记下前后两份草稿与编号表,以及在框内正向 / 反向重放的 DOM 操作。撤销 / 重做只在框内
  // 原地还原,不重新加载页面(重新加载会闪屏,页面脚本的运行状态也会丢)。
  const pushHistory = useCallback((entry) => {
    const history = historyRef.current;
    const last = history.undo[history.undo.length - 1];
    if (entry.mergeKey && last && last.mergeKey === entry.mergeKey
        && entry.time - last.time < HISTORY_MERGE_MS) {
      // 连续改同一个属性(拖颜色、按住箭头改字号)合成一步:保留最早的「之前」,换成最新的「之后」。
      last.after = entry.after;
      last.afterIndex = entry.afterIndex;
      last.redo = entry.redo;
      last.time = entry.time;
    } else {
      history.undo.push(entry);
      if (history.undo.length > HISTORY_LIMIT) history.undo.shift();
    }
    history.redo = [];
    return history.undo[history.undo.length - 1];
  }, []);

  const publishText = useCallback((next) => {
    ownTextRef.current = next;
    textRef.current = next;
    onChange?.(next);
  }, [onChange]);

  // undo / redo 是框内反向 / 正向重放的操作;sid 是撤销 / 重做后要选中的元素。
  const commit = useCallback((splices, { mergeKey = '', undo = [], redo = [], sid = 0 } = {}) => {
    const index = indexRef.current;
    if (!index) return null;
    const before = textRef.current;
    let next = before;
    let nextIndex = index;
    for (const splice of splices) {
      if (!splice) continue;
      next = applyHtmlSplice(next, splice);
      nextIndex = remapHtmlIndex(nextIndex, splice, collectHtmlElementRecords(next, parse));
    }
    if (next === before) return null;
    indexRef.current = nextIndex;
    const entry = pushHistory({
      before, after: next, beforeIndex: index, afterIndex: nextIndex,
      undo, redo, sid, mergeKey, time: Date.now(),
    });
    publishText(next);
    return entry;
  }, [publishText, pushHistory]);

  const replay = useCallback((entry, direction) => {
    const forward = direction === 'redo';
    indexRef.current = forward ? entry.afterIndex : entry.beforeIndex;
    // 改字中撤销 / 重做不结束改字:框里原地换回内容后接着改,之后交回的文字照常收。框内按 Ctrl+Z
    // 之前已把打到一半的字交回来,这里不会和旧文字交错;改字真结束时框会发 textEditEnded。
    for (const op of forward ? entry.redo : [...entry.undo].reverse()) post(op);
    const sid = entry.sid && indexRef.current?.byId.has(entry.sid) ? entry.sid : 0;
    post({ type: 'select', sid });
    publishText(forward ? entry.after : entry.before);
  }, [post, publishText]);

  const undo = useCallback(() => {
    if (!editingRef.current) return;
    const entry = historyRef.current.undo.pop();
    if (!entry) return;
    historyRef.current.redo.push(entry);
    replay(entry, 'undo');
  }, [replay]);

  const redo = useCallback(() => {
    if (!editingRef.current) return;
    const entry = historyRef.current.redo.pop();
    if (!entry) return;
    historyRef.current.undo.push(entry);
    replay(entry, 'redo');
  }, [replay]);

  // 所有直接修改都只在编辑模式里生效;离开编辑模式后能力全部视为关闭。
  const selectedRecord = useCallback(() => {
    const info = selectionRef.current;
    const record = info?.sid ? indexRef.current?.byId.get(info.sid) : null;
    const caps = htmlElementCapabilities(info, record);
    if (!editingRef.current) {
      for (const key of Object.keys(caps)) if (key !== 'mapped') caps[key] = false;
    }
    return { info, record, caps };
  }, []);

  // 改字时在框里拖选了一段文字:文字格式只套在这段文字上(框内执行,随这次改字一起写回源码,
  // 改字期间 Ctrl+Z 由浏览器原生撤销);对齐这类段落属性仍改整个元素。
  const activeTextRange = useCallback(() => {
    const range = textRangeRef.current;
    const sid = pendingTextEditRef.current;
    return range && sid && range.sid === sid && selectionRef.current?.sid === sid ? range : null;
  }, []);

  const updateTextRange = useCallback((value) => {
    textRangeRef.current = value;
    setTextRange(value);
  }, []);

  const applyStyle = useCallback((requested) => {
    const { info, record, caps } = selectedRecord();
    if (!caps.style) return;
    const sid = info.sid;
    let changes = requested;
    if (activeTextRange()) {
      const split = splitRangeStyles(requested);
      if (Object.keys(split.range).length) post({ type: 'formatRange', sid, styles: split.range });
      changes = split.element;
      if (!Object.keys(changes).length) return;
    }
    const previous = sourceStyleValues(textRef.current, record, Object.keys(changes));
    const splice = styleSplice(textRef.current, record, changes);
    if (!splice) return;
    const forward = { type: 'patchStyle', sid, styles: changes };
    post(forward);
    commit([splice], {
      sid,
      mergeKey: `style:${sid}:${Object.keys(changes).sort().join(',')}`,
      undo: [{ type: 'patchStyle', sid, styles: previous }],
      redo: [forward],
    });
  }, [activeTextRange, commit, post, selectedRecord]);

  const endTextEdit = useCallback(() => post({ type: 'endTextEdit' }), [post]);

  const beginTextEdit = useCallback((sid) => {
    const info = selectionRef.current;
    if (!editingRef.current || !info || info.sid !== sid) return;
    const record = indexRef.current?.byId.get(sid);
    const caps = htmlElementCapabilities(info, record);
    if (!caps.text) {
      if (info.text) {
        toast({ kind: 'info', text: caps.mapped ? '这个元素不能直接改文字' : '这部分由脚本生成,请用 AI 编辑' });
      }
      return;
    }
    pendingTextEditRef.current = sid;
    post({ type: 'beginTextEdit', sid, expected: innerSourceText(textRef.current, record) });
  }, [post]);

  // 改字期间框里边打边交(停顿一下就交一次),每次都进同一条撤销栈:连续打字 1 秒内合成一步
  // (merge),选区格式单独一步。只接受外层自己发起的这次改字,框报 textEditEnded 才作废。
  // 合成一步时保留这一串最早的「之前」(撤销回到开始打字前,光标也回到那时),「之后」换成最新的。
  const commitText = useCallback((sid, html, { caretBefore = null, caretAfter = null, merge = false } = {}) => {
    if (!sid || sid !== pendingTextEditRef.current) return;
    const record = indexRef.current?.byId.get(sid);
    const splice = innerContentSplice(record, html);
    if (!splice) return;
    const undoHtml = instrumentHtmlRange(textRef.current, indexRef.current, record.startTagEnd, record.endTagStart);
    const entry = commit([splice], {
      sid,
      mergeKey: merge ? `text:${sid}` : '',
      undo: [{ type: 'setInner', sid, html: undoHtml, caret: caretBefore }],
    });
    if (!entry) return;
    const updated = indexRef.current.byId.get(sid);
    if (!updated?.hasEndTag) return;
    const { index, assigned } = assignNewHtmlIds(
      indexRef.current, collectHtmlElementRecords(entry.after, parse), updated.startTagEnd, updated.endTagStart,
    );
    indexRef.current = index;
    entry.afterIndex = index;
    entry.redo = [{
      type: 'setInner',
      sid,
      html: instrumentHtmlRange(entry.after, index, updated.startTagEnd, updated.endTagStart),
      caret: caretAfter,
    }];
    post({ type: 'assignSids', sid, ids: assigned });
  }, [commit, post]);

  const removeSelected = useCallback(() => {
    const { info, record, caps } = selectedRecord();
    if (!caps.remove) return;
    const sid = info.sid;
    // 正在改字的元素被删掉:框里交回的文字作废。
    if (pendingTextEditRef.current === sid) pendingTextEditRef.current = 0;
    post({ type: 'remove', sid });
    commit([deleteElementSplice(textRef.current, record)], {
      sid, undo: [{ type: 'restore', sid }], redo: [{ type: 'remove', sid }],
    });
    setSelection(null);
  }, [commit, post, selectedRecord]);

  const setAttributeWithHistory = useCallback((record, sid, name, value) => {
    const previous = readSourceAttribute(textRef.current, record, name);
    const forward = { type: 'setAttribute', sid, name, value };
    post(forward);
    commit([attributeSplice(textRef.current, record, name, value)], {
      sid, undo: [{ type: 'setAttribute', sid, name, value: previous ? previous.value : null }], redo: [forward],
    });
  }, [commit, post]);

  const applyLink = useCallback((href) => {
    const { info, record, caps } = selectedRecord();
    if (!caps.link) return;
    if (record.tag !== 'a' && activeTextRange()) {
      post({ type: 'formatRange', sid: info.sid, link: href || '' });
      // 地址输入框关掉后焦点会落到外层页面:放回框里,接着打字 / Ctrl+B 仍作用在这段文字上。
      iframeRef.current?.focus({ preventScroll: true });
      return;
    }
    if (record.tag === 'a') {
      setAttributeWithHistory(record, info.sid, 'href', href || null);
    } else if (href) {
      const splices = wrapLinkSplices(record, href);
      if (!splices) return;
      const forward = { type: 'wrapLink', sid: info.sid, href };
      post(forward);
      commit(splices, { sid: info.sid, undo: [{ type: 'unwrapLink', sid: info.sid }], redo: [forward] });
    }
  }, [activeTextRange, commit, post, selectedRecord, setAttributeWithHistory]);

  const applyImage = useCallback((src) => {
    const { info, record, caps } = selectedRecord();
    if (!caps.image) return;
    setAttributeWithHistory(record, info.sid, 'src', src);
  }, [selectedRecord, setAttributeWithHistory]);

  const aiEdit = useCallback(() => {
    const { info, record, caps } = selectedRecord();
    if (!info) return;
    const source = textRef.current;
    const picked = activeTextRange()?.text || info.text;
    let context;
    if (caps.mapped) {
      const range = lineRangeForOffsets(source, record.start, record.end);
      const outer = outerSourceText(source, record);
      context = createSelectionContext({
        text: outer,
        selectedText: picked || outer,
        path: sourcePath,
        kind: 'text',
        view: 'source',
        startLine: range.startLine,
        endLine: range.endLine,
        lineCount: range.lineCount,
        startOffset: record.start,
        endOffset: record.end,
        contentRevision,
      });
    } else {
      const note = `<!-- Rendered at runtime by the page's own script; not present in the source. Selector: ${info.selector} -->`;
      context = createSelectionContext({
        text: `${note}\n${info.outerHTML}`,
        selectedText: picked || info.outerHTML,
        path: sourcePath,
        kind: 'text',
      });
    }
    pinSelectionContext(context);
  }, [activeTextRange, contentRevision, selectedRecord, sourcePath]);

  // 拖出的选区:作为「选区」引用加进输入框,正文带选区里元素的源码行号与片段。
  const aiEditRegion = useCallback(() => {
    const context = createHtmlRegionContext({
      path: sourcePath, region: regionRef.current, source: textRef.current, index: indexRef.current,
    });
    if (!context) return;
    if (typeof onAddContext === 'function' && onAddContext(context) !== false) {
      toast({ kind: 'ok', text: '已将选区添加到聊天' });
    } else {
      toast({ kind: 'err', text: '输入框当前不可用' });
    }
  }, [onAddContext, sourcePath]);

  const quoteText = useCallback((selected) => {
    pinSelectionContext(createSelectionContext({
      text: selected,
      selectedText: selected,
      path: sourcePath,
      kind: 'text',
      view: 'rendered',
    }));
  }, [sourcePath]);

  // ---- 框内消息 ----------------------------------------------------------------
  // 框内消息一律当成不可信输入:nonce 写在 srcdoc 的脚本里,页面自己的脚本读得到,能伪造任何
  // 消息。nonce 只用来丢掉上一次渲染的旧消息,不是鉴权。会产生后果的消息(快捷键、右键菜单、
  // 打开链接 / 文件)都要求本页面此刻有用户激活 —— 真实按键与点击会沿父级传上来,脚本伪造不了;
  // 改文字只接受外层自己发起的那一次。
  const handleKey = (message) => {
    const keyEvent = {
      key: message.key, code: message.code, ctrlKey: !!message.ctrlKey, metaKey: !!message.metaKey,
      shiftKey: !!message.shiftKey, altKey: !!message.altKey, repeat: !!message.repeat,
    };
    const escape = String(keyEvent.key || '').toLowerCase() === 'escape';
    // Esc 不产生用户激活;它在这里只会取消选中或退出编辑,放行。
    if (!escape && !parentUserActive(iframeRef.current)) return;
    if (isShortcut(keyEvent, 'save')) {
      onSave?.();
      return;
    }
    if (editingRef.current) {
      const action = editingHistoryAction(keyEvent)
        || (isWindowsRedoKey(keyEvent, isMacShortcutHost()) ? 'redo' : null);
      if (action === 'undo' || action === 'redo') {
        if (!keyEvent.repeat) (action === 'undo' ? undo : redo)();
        return;
      }
      const key = String(keyEvent.key || '').toLowerCase();
      if (key === 'escape') {
        if (regionRef.current) post({ type: 'clearRegion' });
        else if (selectionRef.current) post({ type: 'select', sid: 0 });
        else onEditingChange?.(false);
        return;
      }
      if (key === 'delete') {
        removeSelected();
        return;
      }
    }
    if (escape) return;
    iframeRef.current?.dispatchEvent(new KeyboardEvent('keydown', { ...keyEvent, bubbles: true, cancelable: true }));
  };

  const openContextMenu = (message) => {
    const element = iframeRef.current;
    if (!element) return;
    const rect = element.getBoundingClientRect();
    const selected = String(message.selection || '');
    const leadingItems = [];
    if (selected.trim()) {
      leadingItems.push(
        { id: 'html_page_copy', label: '复制', group: 'html-page',
          onSelect: () => copyTextToSystemClipboard(selected, window, { allowEmpty: false }) },
        { id: 'html_page_quote', label: '引用到聊天', group: 'html-page', onSelect: () => quoteText(selected) },
      );
    }
    if (editingRef.current && regionRef.current) {
      leadingItems.push({ id: 'html_page_ai_edit', label: 'AI 编辑', group: 'html-page', onSelect: aiEditRegion });
    } else if (editingRef.current && selectionRef.current) {
      leadingItems.push({ id: 'html_page_ai_edit', label: 'AI 编辑', group: 'html-page', onSelect: aiEdit });
    }
    openDesktopContextMenu({
      target: element,
      x: rect.left + Number(message.x || 0),
      y: rect.top + Number(message.y || 0),
      placement: 'pointer',
      showIcons: false,
      leadingItems,
    });
  };

  // 框里正在画选区:拖出框后松开的事件落在本页面,在这里接住,让框收尾。
  const marqueeWatchRef = useRef(null);
  const watchMarqueeRelease = () => {
    marqueeWatchRef.current?.();
    const stop = () => {
      window.removeEventListener('pointerup', finish, true);
      window.removeEventListener('blur', finish);
      marqueeWatchRef.current = null;
    };
    const finish = () => {
      stop();
      post({ type: 'endMarquee' });
    };
    window.addEventListener('pointerup', finish, true);
    window.addEventListener('blur', finish);
    marqueeWatchRef.current = stop;
  };
  useEffect(() => () => marqueeWatchRef.current?.(), []);

  const handlerRef = useRef(null);
  handlerRef.current = (message) => {
    const iframe = iframeRef.current;
    switch (message.type) {
      case 'ready':
        post({ type: 'mode', editing: editingRef.current && !!indexRef.current });
        break;
      case 'scroll':
        scrollRef.current = { x: Number(message.x) || 0, y: Number(message.y) || 0 };
        break;
      case 'link': {
        const url = safeExternalUrl(message.url);
        if (url && parentUserActive(iframe)) Promise.resolve(openExternalUrl(url)).catch(() => {});
        break;
      }
      case 'openFile': {
        if (!parentUserActive(iframe)) break;
        const target = htmlSiteLinkToPath({ absoluteFile: sourcePath, entry: site.entry, rest: message.path });
        if (target) onOpenFile?.(target);
        break;
      }
      case 'contextmenu':
        if (parentUserActive(iframe)) openContextMenu(message);
        break;
      case 'key':
        handleKey(message);
        break;
      case 'select': {
        // 同步更新 ref:双击时框内紧接着就发改字请求,不能等到下一次渲染。
        const next = message.info && typeof message.info === 'object' ? message.info : null;
        selectionRef.current = next;
        setSelection(next);
        if (textRangeRef.current && next?.sid !== textRangeRef.current.sid) updateTextRange(null);
        if (next && regionRef.current) {
          regionRef.current = null;
          setRegion(null);
        }
        break;
      }
      case 'region': {
        marqueeWatchRef.current?.();
        const next = normalizeHtmlRegion(message.region);
        regionRef.current = next;
        setRegion(next);
        break;
      }
      case 'marquee':
        watchMarqueeRelease();
        break;
      case 'regionRect':
        setRegion((previous) => {
          const rect = normalizeHtmlRegion({ rect: message.rect })?.rect;
          if (!previous || !rect) return previous;
          const a = previous.rect;
          if (a.x === rect.x && a.y === rect.y && a.w === rect.w && a.h === rect.h) return previous;
          return { ...previous, rect };
        });
        break;
      case 'textSelection': {
        const sid = Number(message.sid) || 0;
        if (!sid || sid !== pendingTextEditRef.current) break;
        const range = normalizeTextRange(message.range);
        updateTextRange(range ? { ...range, sid } : null);
        break;
      }
      case 'rect':
        setSelection((previous) => {
          if (!previous || previous.sid !== message.sid || !message.rect) return previous;
          const a = previous.rect || {};
          const b = message.rect;
          if (a.x === b.x && a.y === b.y && a.w === b.w && a.h === b.h) return previous;
          return { ...previous, rect: b };
        });
        break;
      case 'textEditRequest':
        beginTextEdit(Number(message.sid) || 0);
        break;
      case 'textEditRejected':
        if (Number(message.sid) !== pendingTextEditRef.current) break;
        pendingTextEditRef.current = 0;
        toast({ kind: 'info', text: '这段文字已被页面脚本改动,请用 AI 编辑' });
        break;
      case 'textEditStarted':
        setTextEditing(true);
        break;
      case 'textEditEnded':
        if (Number(message.sid) === pendingTextEditRef.current) pendingTextEditRef.current = 0;
        setTextEditing(false);
        updateTextRange(null);
        break;
      case 'textCommit':
        if (typeof message.html === 'string') {
          commitText(Number(message.sid) || 0, message.html, {
            caretBefore: normalizeTextCaret(message.caretBefore),
            caretAfter: normalizeTextCaret(message.caretAfter),
            merge: message.merge === true,
          });
        }
        break;
      default:
        break;
    }
  };

  // 监听在提交阶段同步挂上:框内运行时启动即发 ready。
  useLayoutEffect(() => {
    const onMessage = (event) => {
      const iframe = iframeRef.current;
      if (!iframe || event.source !== iframe.contentWindow) return;
      const message = htmlPageMessage(event.data, nonceRef.current);
      if (message) handlerRef.current?.(message);
    };
    window.addEventListener('message', onMessage);
    return () => window.removeEventListener('message', onMessage);
  }, []);

  // 离开编辑模式或卸载前把正在改的文字交回来。
  useEffect(() => () => post({ type: 'endTextEdit' }), [post]);

  const { caps } = selection ? selectedRecord() : { caps: null };

  return (
    <div ref={hostRef} className="ace-html-page" data-html-page-editing={editActive ? 'true' : undefined}
      onKeyDown={(event) => {
        // 焦点在工具栏(外层页面)里时,保存键不经过框内脚本,这里接住。
        if (isShortcut(event, 'save')) {
          event.preventDefault();
          if (!event.repeat) onSave?.();
        }
      }}>
      {frame && (
        <iframe
          ref={iframeRef}
          className="ace-html-page-frame"
          title={path}
          sandbox={HTML_PAGE_SANDBOX}
          srcDoc={frame.srcdoc}
          referrerPolicy="no-referrer"
        />
      )}
      {editActive && region && !selection && (
        <HtmlEditToolbar info={{ kind: 'region', rect: region.rect }} caps={REGION_CAPS} bounds={bounds}
          onAiEdit={aiEditRegion} />
      )}
      {editActive && selection && caps && (
        <HtmlEditToolbar
          info={selection}
          caps={caps}
          bounds={bounds}
          textEditing={textEditing}
          range={textEditing && textRange?.sid === selection.sid ? textRange : null}
          onAiEdit={aiEdit}
          onStyle={applyStyle}
          onEditText={() => (textEditing ? endTextEdit() : beginTextEdit(selection.sid))}
          onLink={applyLink}
          onImage={applyImage}
          onDelete={removeSelected}
        />
      )}
    </div>
  );
}
