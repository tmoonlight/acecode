import { useCallback, useEffect, useLayoutEffect, useRef, useState } from 'react';
import { clsx } from '../lib/format.js';
import {
  AGENT_BROWSER_STATE_EVENT,
  agentBrowserLayoutFromRect,
  getAgentBrowserConsoleLogs,
  getAgentBrowserState,
  normalizeAgentBrowserAddress,
  runAgentBrowserBridgeAction,
  selectAgentBrowserPage,
  setAgentBrowserShared,
  setAgentBrowserLayout,
  toggleAgentBrowserDevTools,
  toggleAgentBrowserElementSelection,
} from '../lib/agentBrowser.js';
import {
  createAgentBrowserConsoleContext,
  createAgentBrowserElementContext,
} from '../lib/agentBrowserChatContext.js';
import {
  agentBrowserShowsNativePage,
  agentBrowserSurfacePresentation,
} from '../lib/agentBrowserSurface.js';
import {
  NATIVE_SURFACE_OVERLAY_EVENT,
  allocateAgentBrowserLayoutRevision,
  nativeSurfaceOcclusionRectsFromClientRects,
  nativeSurfaceOverlayGeometryByDocument,
  nativeSurfaceShouldShow,
  nativeSurfaceSupportsLocalOcclusion,
  nativeSurfaceViewportRect,
  nextAgentBrowserLayoutRequest,
} from '../lib/agentBrowserSurfaceCoordinator.js';
import { NavigationArrowIcon, RefreshIcon, VsIcon } from './Icon.jsx';
import { toast } from './Toast.jsx';

const INITIAL_STATE = Object.freeze({
  supported: true,
  ready: false,
  loading: false,
  visible: false,
  can_go_back: false,
  can_go_forward: false,
  shared_with_agent: false,
  element_selection_active: false,
  element_selection_serial: 0,
  url: 'about:blank',
  title: '',
  favicon: '',
  content_state: 'empty',
  failure_kind: '',
  error: '',
});

export function AgentBrowserPanel({
  pageId,
  agentActive = false,
  surfaceEnabled = true,
  onAddContext,
}) {
  const viewportRef = useRef(null);
  const addressFocusedRef = useRef(false);
  const layoutFrameRef = useRef(0);
  const layoutStateRef = useRef({ signature: '', revision: 0 });
  const lastSurfaceRectRef = useRef({ left: 0, top: 0, width: 0, height: 0 });
  const [state, setState] = useState(INITIAL_STATE);
  const [address, setAddress] = useState('');
  const nativePageVisible = agentBrowserShowsNativePage(state);
  const surface = agentBrowserSurfacePresentation(state);

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
      const { selected_element: selectedElement, ...nextState } = event.detail;
      setState((previous) => ({ ...previous, ...nextState }));
      if (selectedElement && typeof selectedElement === 'object') {
        const context = createAgentBrowserElementContext(selectedElement, event.detail);
        if (context && typeof onAddContext === 'function' && onAddContext(context) !== false) {
          toast({ kind: 'ok', text: `已将 ${context.label} 添加到聊天` });
        }
      }
    };
    window.addEventListener(AGENT_BROWSER_STATE_EVENT, onState);
    return () => {
      cancelled = true;
      window.removeEventListener(AGENT_BROWSER_STATE_EVENT, onState);
    };
  }, [onAddContext, pageId]);

  useEffect(() => {
    if (addressFocusedRef.current) return;
    const nextUrl = state.url === 'about:blank' ? '' : String(state.url || '');
    setAddress(nextUrl);
  }, [state.url]);

  const syncNativeSurface = useCallback(({ forceHidden = false, force = false } = {}) => {
    const viewport = viewportRef.current;
    const surfaceRect = viewport?.getBoundingClientRect?.() || lastSurfaceRectRef.current;
    if (viewport) lastSurfaceRectRef.current = surfaceRect;
    const documentVisible = document.visibilityState !== 'hidden' && !document.hidden;
    const overlayGeometry = forceHidden
      ? { blocking: false, occlusionRects: [] }
      : nativeSurfaceOverlayGeometryByDocument(surfaceRect, document, window);
    const supportsLocalOcclusion = nativeSurfaceSupportsLocalOcclusion(
      window.__ACECODE_OS__,
    );
    const overlayBlocked = overlayGeometry.blocking
      || (!supportsLocalOcclusion && overlayGeometry.occlusionRects.length > 0);
    const visible = !forceHidden && nativeSurfaceShouldShow({
      applicationVisible: surfaceEnabled,
      detailsVisible: true,
      tabActive: true,
      pageLive: nativePageVisible,
      documentVisible,
      surfaceRect,
      viewportRect: nativeSurfaceViewportRect(window),
      overlayBlocked,
    });
    const scale = window.__ACECODE_OS__ === 'macos' ? 1 : (window.devicePixelRatio || 1);
    const layout = agentBrowserLayoutFromRect(
      surfaceRect,
      scale,
      visible,
    );
    layout.occlusion_rects = visible && supportsLocalOcclusion
      ? nativeSurfaceOcclusionRectsFromClientRects(
          surfaceRect,
          overlayGeometry.occlusionRects,
          scale,
        )
      : [];
    const next = nextAgentBrowserLayoutRequest(layout, layoutStateRef.current, {
      force,
      allocateRevision: allocateAgentBrowserLayoutRevision,
    });
    if (!next.changed) return;
    layoutStateRef.current = { signature: next.signature, revision: next.revision };
    void setAgentBrowserLayout(pageId, next.request);
  }, [nativePageVisible, pageId, surfaceEnabled]);

  useLayoutEffect(() => {
    const viewport = viewportRef.current;
    if (!viewport) return undefined;
    let disposed = false;
    const scheduleLayout = () => {
      if (disposed || layoutFrameRef.current) return;
      layoutFrameRef.current = requestAnimationFrame(sampleLayout);
    };
    const sampleLayout = () => {
      layoutFrameRef.current = 0;
      if (disposed) return;
      syncNativeSurface();
      if (surfaceEnabled
          && nativePageVisible
          && document.visibilityState !== 'hidden'
          && !document.hidden) {
        scheduleLayout();
      }
    };

    scheduleLayout();
    const resizeObserver = typeof ResizeObserver === 'function'
      ? new ResizeObserver(scheduleLayout)
      : null;
    resizeObserver?.observe(viewport);
    const intersectionObserver = typeof IntersectionObserver === 'function'
      ? new IntersectionObserver(scheduleLayout)
      : null;
    intersectionObserver?.observe(viewport);
    const mutationObserver = typeof MutationObserver === 'function'
      ? new MutationObserver(scheduleLayout)
      : null;
    mutationObserver?.observe(document.body, {
      childList: true,
      subtree: true,
      attributes: true,
      attributeFilter: [
        'aria-hidden',
        'class',
        'data-ace-native-overlay',
        'hidden',
        'style',
      ],
    });

    const visualViewport = window.visualViewport;
    const documentEvents = [
      'animationcancel',
      'animationend',
      'animationstart',
      'transitioncancel',
      'transitionend',
      'transitionrun',
      'visibilitychange',
    ];
    window.addEventListener('resize', scheduleLayout);
    window.addEventListener('scroll', scheduleLayout, true);
    window.addEventListener(NATIVE_SURFACE_OVERLAY_EVENT, scheduleLayout);
    visualViewport?.addEventListener('resize', scheduleLayout);
    visualViewport?.addEventListener('scroll', scheduleLayout);
    documentEvents.forEach((name) => document.addEventListener(name, scheduleLayout, true));

    return () => {
      disposed = true;
      resizeObserver?.disconnect();
      intersectionObserver?.disconnect();
      mutationObserver?.disconnect();
      window.removeEventListener('resize', scheduleLayout);
      window.removeEventListener('scroll', scheduleLayout, true);
      window.removeEventListener(NATIVE_SURFACE_OVERLAY_EVENT, scheduleLayout);
      visualViewport?.removeEventListener('resize', scheduleLayout);
      visualViewport?.removeEventListener('scroll', scheduleLayout);
      documentEvents.forEach((name) => document.removeEventListener(name, scheduleLayout, true));
      if (layoutFrameRef.current) cancelAnimationFrame(layoutFrameRef.current);
      layoutFrameRef.current = 0;
      syncNativeSurface({ forceHidden: true, force: true });
    };
  }, [nativePageVisible, surfaceEnabled, syncNativeSurface]);

  const runAction = useCallback(async (name) => {
    const result = await runAgentBrowserBridgeAction(name, pageId);
    if (result?.ok === false) {
      toast({ kind: 'err', text: result.error || '浏览器操作失败' });
    }
    return result;
  }, [pageId]);

  const submitAddress = useCallback(async (event) => {
    event.preventDefault();
    const normalized = normalizeAgentBrowserAddress(address);
    if (!normalized) {
      toast({ kind: 'err', text: '请输入 http 或 https 地址' });
      return;
    }
    const result = await runAgentBrowserBridgeAction(
      'aceDesktop_agentBrowserNavigate',
      { page_id: pageId, url: normalized },
    );
    if (result?.ok === false) {
      toast({ kind: 'err', text: result.error || '无法打开该地址' });
    }
  }, [address, pageId]);

  const toggleSharing = useCallback(async () => {
    const shared = !state.shared_with_agent;
    const result = await setAgentBrowserShared(pageId, shared);
    if (result?.ok === false) {
      toast({ kind: 'err', text: result.error || '无法更改共享状态' });
    }
  }, [pageId, state.shared_with_agent]);

  const toggleElementSelection = useCallback(async () => {
    const result = await toggleAgentBrowserElementSelection(pageId);
    if (result?.ok === false) {
      toast({ kind: 'err', text: result.error || '无法选择网页元素' });
    }
  }, [pageId]);

  const addConsoleLogs = useCallback(async () => {
    const result = await getAgentBrowserConsoleLogs(pageId);
    if (result?.ok === false) {
      toast({ kind: 'err', text: result.error || '无法读取控制台日志' });
      return;
    }
    const context = createAgentBrowserConsoleContext(result);
    if (!context) {
      toast({ kind: 'info', text: '当前页面还没有控制台日志' });
      return;
    }
    if (typeof onAddContext === 'function' && onAddContext(context) !== false) {
      toast({ kind: 'ok', text: '已将控制台日志添加到聊天' });
    }
  }, [onAddContext, pageId]);

  const openDevTools = useCallback(async () => {
    const result = await toggleAgentBrowserDevTools(pageId);
    if (result?.ok === false) {
      toast({ kind: 'err', text: result.error || '无法打开开发者工具' });
    }
  }, [pageId]);

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
          disabled={!state.ready || surface.kind === 'empty'}
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
          <button
            type="button"
            className={clsx(
              'ace-agent-browser-share-toggle',
              state.shared_with_agent && 'is-shared',
            )}
            disabled={!state.ready}
            aria-pressed={state.shared_with_agent ? 'true' : 'false'}
            aria-label={state.shared_with_agent ? '停止与智能体共享' : '与智能体共享'}
            title={state.shared_with_agent ? '停止与智能体共享' : '与智能体共享'}
            onClick={toggleSharing}
          >
            {state.shared_with_agent && <span>正在与智能体共享</span>}
            <VsIcon name="ShareWindow" size={16} />
          </button>
        </form>
        <div className="ace-agent-browser-toolbar-actions">
          <button
            type="button"
            className={clsx(
              'ace-agent-browser-toolbar-button',
              state.element_selection_active && 'is-checked',
            )}
            disabled={!nativePageVisible}
            aria-pressed={state.element_selection_active ? 'true' : 'false'}
            title={state.element_selection_active ? '停止选择元素 (Esc)' : '将元素添加到聊天'}
            aria-label="将元素添加到聊天"
            onClick={toggleElementSelection}
          >
            <VsIcon name="Inspect" size={17} />
          </button>
          <button
            type="button"
            className="ace-agent-browser-toolbar-button"
            disabled={!state.ready}
            title="将控制台日志添加到聊天"
            aria-label="将控制台日志添加到聊天"
            onClick={addConsoleLogs}
          >
            <VsIcon name="Output" size={17} />
          </button>
          <button
            type="button"
            className="ace-agent-browser-toolbar-button"
            disabled={!state.ready}
            title="切换开发者工具"
            aria-label="切换开发者工具"
            onClick={openDevTools}
          >
            <VsIcon name="DeveloperTools" size={17} />
          </button>
        </div>
      </div>
      <div className={clsx('ace-agent-browser-frame', agentActive && 'is-agent-active')}>
        <div ref={viewportRef} className="ace-agent-browser-native-viewport">
          {!nativePageVisible && (
            <div
              className="ace-agent-browser-status"
              data-browser-surface={surface.kind}
              data-tone={surface.tone}
              role={surface.role}
              aria-live={surface.role === 'alert' ? 'assertive' : 'polite'}
            >
              <VsIcon
                name={surface.icon}
                size={50}
                className={clsx(
                  'ace-agent-browser-status-icon',
                  surface.kind === 'loading' && 'is-loading',
                )}
              />
              <div className="ace-agent-browser-status-title">
                {surface.title}
              </div>
              <div className="ace-agent-browser-status-detail">
                {surface.detail}
              </div>
              {surface.canRetry && (
                <div className="ace-agent-browser-status-actions">
                  <button
                    type="button"
                    className="inline-flex h-8 items-center gap-1.5 rounded-md bg-accent px-3 text-[12px] font-medium text-white transition hover:opacity-90"
                    onClick={() => runAction('aceDesktop_agentBrowserReload')}
                  >
                    <RefreshIcon size={14} />
                    重试
                  </button>
                  {surface.canGoBack && (
                    <button
                      type="button"
                      className="inline-flex h-8 items-center gap-1.5 rounded-md border border-border bg-surface px-3 text-[12px] font-medium text-fg-2 transition hover:bg-surface-hi hover:text-fg"
                      onClick={() => runAction('aceDesktop_agentBrowserGoBack')}
                    >
                      <NavigationArrowIcon direction="back" size={14} />
                      返回上一页
                    </button>
                  )}
                </div>
              )}
            </div>
          )}
        </div>
      </div>
    </div>
  );
}
