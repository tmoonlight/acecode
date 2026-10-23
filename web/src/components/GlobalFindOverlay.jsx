import { isDismissShortcut } from '../lib/shortcutBindings.js';
import {
  Component,
  Fragment,
  useCallback,
  useEffect,
  useLayoutEffect,
  useRef,
  useState,
} from 'react';
import { VsIcon } from './Icon.jsx';
import {
  applyFindHighlights,
  clearFindHighlights,
  clearFindSelection,
  collectFindMatches,
  findMatchesNeedRecollect,
  isComposingInputEvent,
  isFindShortcut,
  requestConversationFindContent,
  scrollFindMatchIntoView,
  selectFindMatch,
} from '../lib/globalFind.js';
import { notifyNativeSurfaceOverlayChange } from '../lib/agentBrowserSurfaceCoordinator.js';

export const CONVERSATION_FIND_ROOT_SELECTOR = '[data-conversation-find-root="true"]';

// 同一会话内自动重建浮层的次数上限;超过后保持关闭,直到切换会话。
const MAX_FIND_OVERLAY_RECOVERIES = 3;

function describeFindError(error) {
  if (error?.stack) return String(error.stack);
  if (error?.message) return String(error.message);
  return String(error);
}

// 查找浮层挂在 App 最外层。它直接改写对话区 DOM,一旦抛错又没有边界,
// React 18 会卸载整棵树(长会话里 Ctrl+F 导致整页空白就是这样)。这里接住
// 错误、清掉残留高亮,再挂一个关闭状态的新实例,让 Ctrl+F 继续可用。
class GlobalFindBoundary extends Component {
  state = { failed: false, generation: 0, recoveries: 0 };

  static getDerivedStateFromError() {
    return { failed: true };
  }

  componentDidCatch(error) {
    clearFindHighlights(document);
    clearFindSelection(document);
    console.error(`[global-find] overlay crashed and was reset: ${describeFindError(error)}`);
    this.setState((state) => (state.recoveries < MAX_FIND_OVERLAY_RECOVERIES
      ? { failed: false, generation: state.generation + 1, recoveries: state.recoveries + 1 }
      : null));
  }

  componentDidUpdate(prevProps) {
    if (prevProps.resetKey === this.props.resetKey) return;
    if (!this.state.failed && this.state.recoveries === 0) return;
    this.setState((state) => ({ failed: false, generation: state.generation + 1, recoveries: 0 }));
  }

  render() {
    if (this.state.failed) return null;
    return <Fragment key={this.state.generation}>{this.props.children}</Fragment>;
  }
}

export function GlobalFindOverlay(props) {
  return (
    <GlobalFindBoundary resetKey={props.scopeKey || ''}>
      <GlobalFindOverlayContent {...props} />
    </GlobalFindBoundary>
  );
}

function selectedTextForFind(root) {
  const selection = window.getSelection?.();
  if (root && selection?.anchorNode && !root.contains(selection.anchorNode)) return '';
  const text = selection?.toString?.().trim() || '';
  if (!text || text.length > 80 || /[\r\n]/.test(text)) return '';
  return text;
}

function GlobalFindOverlayContent({
  enabled = false,
  openRequest = 0,
  scopeKey = '',
  rootSelector = CONVERSATION_FIND_ROOT_SELECTOR,
}) {
  const [open, setOpen] = useState(false);
  const [query, setQuery] = useState('');
  const [matches, setMatches] = useState([]);
  const [activeIndex, setActiveIndex] = useState(-1);
  const [focusNonce, setFocusNonce] = useState(0);
  const [navigationNonce, setNavigationNonce] = useState(0);
  const [contentLoading, setContentLoading] = useState(false);
  const [searchScope, setSearchScope] = useState(null);
  const overlayRef = useRef(null);
  const inputRef = useRef(null);
  const composingRef = useRef(false);
  const queryRef = useRef(query);
  const lastOpenRequestRef = useRef(openRequest);
  const previousScopeKeyRef = useRef(scopeKey);
  const selectOnFocusRef = useRef(false);
  const syncValueOnFocusRef = useRef(false);
  const contentRequestRef = useRef(0);
  const latestMatchesRef = useRef(matches);
  const recollectedMatchesRef = useRef(null);

  queryRef.current = query;

  const focusFindInput = useCallback(() => {
    const input = inputRef.current;
    if (!input) return;
    if (syncValueOnFocusRef.current && !composingRef.current && input.value !== queryRef.current) {
      input.value = queryRef.current;
    }
    input.focus({ preventScroll: true });
    if (selectOnFocusRef.current && !composingRef.current) {
      input.select();
    }
    selectOnFocusRef.current = false;
    syncValueOnFocusRef.current = false;
  }, []);

  const requestInputFocus = useCallback(({ select = false, syncValue = false } = {}) => {
    selectOnFocusRef.current = select;
    syncValueOnFocusRef.current = syncValue;
    setFocusNonce((value) => value + 1);
  }, []);

  const resolveFindRoot = useCallback(
    () => document.querySelector(rootSelector),
    [rootSelector],
  );

  const close = useCallback(() => {
    contentRequestRef.current += 1;
    setOpen(false);
    setContentLoading(false);
    setNavigationNonce(0);
    clearFindHighlights(document);
    clearFindSelection(document);
  }, []);

  const openFind = useCallback(() => {
    if (!enabled) return;
    // 查找在 DOM 文本上执行,通知 ChatView 先把 transcript 全部加载并展开,
    // 否则窗口外的旧消息搜不到。长会话要逐页拉历史,期间每页都会重绘对话区,
    // 所以等加载完成再收集匹配。
    const selected = selectedTextForFind(resolveFindRoot());
    const request = requestConversationFindContent(window);
    const requestId = contentRequestRef.current + 1;
    contentRequestRef.current = requestId;
    setContentLoading(request.loading);
    setSearchScope(null);
    if (request.loading) {
      request.ready.then((scope) => {
        if (contentRequestRef.current !== requestId) return;
        setSearchScope(scope);
        setContentLoading(false);
      });
    }
    if (selected) setQuery(selected);
    setOpen(true);
    setNavigationNonce(0);
    requestInputFocus({ select: true, syncValue: true });
  }, [enabled, requestInputFocus, resolveFindRoot]);

  useLayoutEffect(() => {
    if (!open) return undefined;
    focusFindInput();
    const frame = requestAnimationFrame(focusFindInput);
    const timer = window.setTimeout(focusFindInput, 80);
    return () => {
      cancelAnimationFrame(frame);
      window.clearTimeout(timer);
    };
  }, [focusFindInput, focusNonce, open]);

  useLayoutEffect(() => {
    notifyNativeSurfaceOverlayChange();
    return () => notifyNativeSurfaceOverlayChange();
  }, [open]);

  useEffect(() => {
    const onKeyDown = (event) => {
      if (!enabled || !isFindShortcut(event)) return;
      event.preventDefault();
      event.stopPropagation();
      openFind();
    };
    window.addEventListener('keydown', onKeyDown, true);
    return () => window.removeEventListener('keydown', onKeyDown, true);
  }, [enabled, openFind]);

  useEffect(() => {
    if (!enabled) {
      lastOpenRequestRef.current = openRequest;
      close();
      return;
    }
    if (lastOpenRequestRef.current === openRequest) return;
    lastOpenRequestRef.current = openRequest;
    openFind();
  }, [close, enabled, openFind, openRequest]);

  useEffect(() => {
    if (previousScopeKeyRef.current === scopeKey) return;
    previousScopeKeyRef.current = scopeKey;
    close();
  }, [close, scopeKey]);

  useEffect(() => {
    if (!open) return undefined;
    const onPointerDown = (event) => {
      if (overlayRef.current?.contains(event.target)) return;
      close();
    };
    document.addEventListener('pointerdown', onPointerDown, true);
    return () => document.removeEventListener('pointerdown', onPointerDown, true);
  }, [close, open]);

  useEffect(() => {
    if (!open) return;
    clearFindHighlights(document);
    const nextMatches = contentLoading ? [] : collectFindMatches(resolveFindRoot(), query);
    latestMatchesRef.current = nextMatches;
    setMatches(nextMatches);
    setActiveIndex(nextMatches.length > 0 ? 0 : -1);
    setNavigationNonce(0);
  }, [contentLoading, open, query, resolveFindRoot]);

  useEffect(() => {
    if (!open) return undefined;
    // 同一次提交里上面的收集已经换上新的一组匹配,本帧这组作废,等下一次渲染。
    if (latestMatchesRef.current !== matches) return undefined;
    if (!query || activeIndex < 0 || matches.length === 0) {
      clearFindHighlights(document);
      if (document.activeElement !== inputRef.current) {
        clearFindSelection(document);
      }
      return undefined;
    }
    // 收集之后对话区又重绘过(新消息、流式输出、历史加载),记下的节点和偏移
    // 已经作废:就地重新收集,保留当前序号。刚重收、还没高亮过的那一组不再重收,
    // 防止循环;高亮过之后再失效(之后又重绘)仍会重收。
    if (recollectedMatchesRef.current !== matches && findMatchesNeedRecollect(matches)) {
      clearFindHighlights(document);
      const nextMatches = collectFindMatches(resolveFindRoot(), query);
      recollectedMatchesRef.current = nextMatches;
      latestMatchesRef.current = nextMatches;
      setMatches(nextMatches);
      setActiveIndex(nextMatches.length > 0 ? Math.min(activeIndex, nextMatches.length - 1) : -1);
      return undefined;
    }
    recollectedMatchesRef.current = null;
    const highlighted = applyFindHighlights(matches, activeIndex, document);
    const activeMatch = matches[activeIndex];
    if (navigationNonce > 0 && !composingRef.current) {
      scrollFindMatchIntoView(activeMatch, document);
      if (!highlighted) selectFindMatch(activeMatch, document);
    }
    return undefined;
  }, [activeIndex, matches, navigationNonce, open, query, resolveFindRoot]);

  useEffect(() => () => {
    clearFindHighlights(document);
    clearFindSelection(document);
  }, []);

  const move = useCallback((direction) => {
    if (!matches.length) return;
    setNavigationNonce((value) => value + 1);
    setActiveIndex((current) => {
      const base = current >= 0 ? current : 0;
      return (base + direction + matches.length) % matches.length;
    });
  }, [matches.length]);

  const onInputKeyDown = (event) => {
    if (isComposingInputEvent(event, composingRef.current)) return;
    if (isDismissShortcut(event)) {
      event.preventDefault();
      close();
    } else if (event.key === 'Enter') {
      event.preventDefault();
      move(event.shiftKey ? -1 : 1);
    }
  };

  const keepInputFocus = (event) => {
    event.preventDefault();
  };

  const onInputChange = (event) => {
    if (isComposingInputEvent(event, composingRef.current)) return;
    setQuery(event.currentTarget.value);
  };

  if (!open) return null;

  const current = activeIndex >= 0 ? activeIndex + 1 : 0;
  const disabled = matches.length === 0;
  const partialScope = !contentLoading && searchScope?.partial ? searchScope : null;

  return (
    <>
      <div
        ref={overlayRef}
        className="ace-global-find"
        data-ace-native-overlay="overlap"
        role="search"
        aria-label="当前对话内容查找"
      >
        <VsIcon name="search" size={15} className="ace-global-find-search" />
        <input
          ref={inputRef}
          defaultValue={query}
          onChange={onInputChange}
          onCompositionStart={() => {
            composingRef.current = true;
          }}
          onCompositionEnd={(event) => {
            composingRef.current = false;
            setQuery(event.currentTarget.value);
          }}
          onKeyDown={onInputKeyDown}
          className="ace-global-find-input"
          aria-label="查找"
          placeholder="搜索当前对话内容"
          spellCheck={false}
        />
        <span className="ace-global-find-count" aria-live="polite">
          {contentLoading ? '加载中…' : `${current}/${matches.length}`}
        </span>
        <span className="ace-global-find-divider" aria-hidden="true" />
        <button
          type="button"
          className="ace-global-find-button"
          onMouseDown={keepInputFocus}
          onClick={() => move(-1)}
          disabled={disabled}
          title="上一个"
          aria-label="上一个"
        >
          <VsIcon name="glyphUp" size={13} />
        </button>
        <button
          type="button"
          className="ace-global-find-button"
          onMouseDown={keepInputFocus}
          onClick={() => move(1)}
          disabled={disabled}
          title="下一个"
          aria-label="下一个"
        >
          <VsIcon name="glyphDown" size={13} />
        </button>
        <button
          type="button"
          className="ace-global-find-close"
          onClick={close}
          title="关闭"
          aria-label="关闭"
        >
          <VsIcon name="close" size={14} />
        </button>
      </div>
      {partialScope && (
        <div className="ace-global-find-note" data-ace-native-overlay="overlap" role="status">
          {`只搜索了最近 ${partialScope.messageCount} 条消息`}
        </div>
      )}
    </>
  );
}
