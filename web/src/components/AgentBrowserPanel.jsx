import { useCallback, useEffect, useLayoutEffect, useRef, useState } from 'react';
import { clsx } from '../lib/format.js';
import {
  AGENT_BROWSER_STATE_EVENT,
  agentBrowserErrorPresentation,
  agentBrowserLayoutFromRect,
  getAgentBrowserState,
  normalizeAgentBrowserAddress,
  runAgentBrowserBridgeAction,
  selectAgentBrowserPage,
  setAgentBrowserLayout,
} from '../lib/agentBrowser.js';
import { NavigationArrowIcon, RefreshIcon, VsIcon } from './Icon.jsx';

const INITIAL_STATE = Object.freeze({
  supported: true,
  ready: false,
  loading: false,
  visible: false,
  can_go_back: false,
  can_go_forward: false,
  url: 'about:blank',
  title: '',
  error: '',
});

function modalIsOpen() {
  return !!document.querySelector(
    '[data-ace-modal-dialog="true"], .ace-desktop-context-menu',
  );
}

export function AgentBrowserPanel({ pageId, agentActive = false }) {
  const viewportRef = useRef(null);
  const addressFocusedRef = useRef(false);
  const layoutFrameRef = useRef(0);
  const [state, setState] = useState(INITIAL_STATE);
  const [address, setAddress] = useState('');
  const [localError, setLocalError] = useState('');
  const [overlayBlocked, setOverlayBlocked] = useState(false);

  useEffect(() => {
    let cancelled = false;
    setState({ ...INITIAL_STATE, page_id: pageId });
    void selectAgentBrowserPage(pageId);
    getAgentBrowserState(pageId).then((next) => {
      if (cancelled) return;
      setState((previous) => ({ ...previous, ...next }));
    });
    const onState = (event) => {
      if (!event?.detail || typeof event.detail !== 'object') return;
      if (event.detail.page_id !== pageId) return;
      setState((previous) => ({ ...previous, ...event.detail }));
    };
    window.addEventListener(AGENT_BROWSER_STATE_EVENT, onState);
    return () => {
      cancelled = true;
      window.removeEventListener(AGENT_BROWSER_STATE_EVENT, onState);
    };
  }, [pageId]);

  useEffect(() => {
    if (addressFocusedRef.current) return;
    const nextUrl = state.url === 'about:blank' ? '' : String(state.url || '');
    setAddress(nextUrl);
  }, [state.url]);

  useEffect(() => {
    const update = () => setOverlayBlocked(modalIsOpen() || document.hidden);
    update();
    const observer = new MutationObserver(update);
    observer.observe(document.body, { childList: true, subtree: true });
    document.addEventListener('visibilitychange', update);
    return () => {
      observer.disconnect();
      document.removeEventListener('visibilitychange', update);
    };
  }, []);

  const syncLayout = useCallback((visible = true) => {
    const viewport = viewportRef.current;
    if (!viewport) return;
    const layout = agentBrowserLayoutFromRect(
      viewport.getBoundingClientRect(),
      window.devicePixelRatio || 1,
      visible && !overlayBlocked,
    );
    void setAgentBrowserLayout(pageId, layout);
  }, [overlayBlocked, pageId]);

  const scheduleLayout = useCallback(() => {
    if (layoutFrameRef.current) cancelAnimationFrame(layoutFrameRef.current);
    layoutFrameRef.current = requestAnimationFrame(() => {
      layoutFrameRef.current = 0;
      syncLayout(true);
    });
  }, [syncLayout]);

  useLayoutEffect(() => {
    const viewport = viewportRef.current;
    if (!viewport) return undefined;
    scheduleLayout();
    const resizeObserver = new ResizeObserver(scheduleLayout);
    resizeObserver.observe(viewport);
    window.addEventListener('resize', scheduleLayout);
    window.addEventListener('scroll', scheduleLayout, true);
    return () => {
      resizeObserver.disconnect();
      window.removeEventListener('resize', scheduleLayout);
      window.removeEventListener('scroll', scheduleLayout, true);
      if (layoutFrameRef.current) cancelAnimationFrame(layoutFrameRef.current);
      layoutFrameRef.current = 0;
      void runAgentBrowserBridgeAction('aceDesktop_agentBrowserHide', pageId);
    };
  }, [pageId, scheduleLayout]);

  useEffect(() => {
    syncLayout(!overlayBlocked);
  }, [overlayBlocked, syncLayout]);

  const runAction = useCallback(async (name) => {
    setLocalError('');
    const result = await runAgentBrowserBridgeAction(name, pageId);
    if (result?.ok === false) setLocalError(result.error || '浏览器操作失败');
  }, [pageId]);

  const submitAddress = useCallback(async (event) => {
    event.preventDefault();
    const normalized = normalizeAgentBrowserAddress(address);
    if (!normalized) {
      setLocalError('请输入 http 或 https 地址');
      return;
    }
    setLocalError('');
    const result = await runAgentBrowserBridgeAction(
      'aceDesktop_agentBrowserNavigate',
      { page_id: pageId, url: normalized },
    );
    if (result?.ok === false) setLocalError(result.error || '无法打开该地址');
  }, [address, pageId]);

  const effectiveError = localError || state.error || '';
  const errorPresentation = agentBrowserErrorPresentation(effectiveError);
  const unavailable = state.supported === false;

  return (
    <div
      className="ace-agent-browser-panel"
      data-agent-active={agentActive ? 'true' : 'false'}
      data-browser-ready={state.ready ? 'true' : 'false'}
    >
      <div className="ace-agent-browser-toolbar">
        <button
          type="button"
          className="ace-agent-browser-toolbar-button"
          disabled={!state.ready || !state.can_go_back}
          title="后退"
          aria-label="后退"
          onClick={() => runAction('aceDesktop_agentBrowserGoBack')}
        >
          <NavigationArrowIcon direction="back" size={17} />
        </button>
        <button
          type="button"
          className="ace-agent-browser-toolbar-button"
          disabled={!state.ready || !state.can_go_forward}
          title="前进"
          aria-label="前进"
          onClick={() => runAction('aceDesktop_agentBrowserGoForward')}
        >
          <NavigationArrowIcon direction="forward" size={17} />
        </button>
        <button
          type="button"
          className="ace-agent-browser-toolbar-button"
          disabled={!state.ready}
          title="刷新"
          aria-label="刷新"
          onClick={() => runAction('aceDesktop_agentBrowserReload')}
        >
          <RefreshIcon size={17} className={state.loading ? 'is-spinning' : ''} />
        </button>
        <form className="ace-agent-browser-address-form" onSubmit={submitAddress}>
          <VsIcon name="globe" size={15} className="ace-agent-browser-address-icon" />
          <input
            type="text"
            className="ace-agent-browser-address"
            value={address}
            spellCheck="false"
            autoCapitalize="off"
            autoCorrect="off"
            placeholder="输入 URL"
            aria-label="浏览器地址"
            title={state.title || state.url || '浏览器地址'}
            onFocus={() => { addressFocusedRef.current = true; }}
            onBlur={() => {
              addressFocusedRef.current = false;
              setAddress(state.url === 'about:blank' ? '' : String(state.url || ''));
            }}
            onChange={(event) => setAddress(event.target.value)}
          />
        </form>
        {agentActive && (
          <span className="ace-agent-browser-shared-badge" title="AI 正在使用此浏览器">
            正在与智能体共享
          </span>
        )}
      </div>
      {effectiveError && (
        <div className="ace-agent-browser-error" role="status">
          <VsIcon name="warning" size={14} mono={false} />
          <span className="ace-agent-browser-error-message">{errorPresentation.message}</span>
          {errorPresentation.retryable && (
            <button
              type="button"
              className="ace-agent-browser-error-retry"
              onClick={() => runAction('aceDesktop_agentBrowserReload')}
            >
              重试
            </button>
          )}
        </div>
      )}
      <div className={clsx('ace-agent-browser-frame', agentActive && 'is-agent-active')}>
        <div ref={viewportRef} className="ace-agent-browser-native-viewport">
          {(unavailable || !state.ready) && (
            <div className="ace-agent-browser-placeholder">
              <VsIcon name={unavailable ? 'warning' : 'globe'} size={46} />
              <div className="ace-agent-browser-placeholder-title">
                {unavailable ? '浏览器不可用' : '正在启动浏览器'}
              </div>
              <div className="ace-agent-browser-placeholder-detail">
                {unavailable
                  ? (state.error || '此功能仅支持 ACECode Windows Desktop')
                  : '正在连接 Windows WebView2'}
              </div>
            </div>
          )}
        </div>
      </div>
    </div>
  );
}
