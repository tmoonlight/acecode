import { useCallback, useEffect, useLayoutEffect, useMemo, useRef, useState } from 'react';
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
  failedNativeSurfaceOcclusionSignature,
  nativeSurfaceOcclusionRectsFromClientRects,
  nativeSurfaceLayoutWithOcclusionFallback,
  nativeSurfaceInteractionBlockedByDocument,
  nativeSurfaceOverlayGeometryByDocument,
  nativeSurfaceShouldShow,
  nativeSurfaceSupportsLocalOcclusion,
  nativeSurfaceViewportRect,
  nextAgentBrowserLayoutRequest,
} from '../lib/agentBrowserSurfaceCoordinator.js';
import { AgentBrowserMoreMenu } from './AgentBrowserMoreMenu.jsx';
import { AgentBrowserNewTab } from './AgentBrowserNewTab.jsx';
import { PageAnnotationPopup, usePageAnnotations } from './PageAnnotations.jsx';
import { PageAnnotationSendBar } from './PageAnnotationSendBar.jsx';
import { annotationOutboxFor } from '../lib/annotationOutbox.js';
import { capturePageAnnotationScreenshot } from '../lib/pageAnnotations.js';
import { captureRectForRecord, cropAnnotationImages } from '../lib/annotationCapture.js';
import { registerAnnotationEditor } from '../lib/annotationEdit.js';
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
  diagnostic: '',
});

export function AgentBrowserPanel({
  pageId,
  api,
  sessionId,
  agentActive = false,
  surfaceEnabled = true,
  onAddContext,
  recentFiles = [],
  onOpenRecentFile,
}) {
  const viewportRef = useRef(null);
  const addressFocusedRef = useRef(false);
  const layoutFrameRef = useRef(0);
  const layoutStateRef = useRef({ signature: '', revision: 0 });
  const occlusionFailureRef = useRef({ pageId: '', signature: '' });
  const lastSurfaceRectRef = useRef({ left: 0, top: 0, width: 0, height: 0 });
  const [state, setState] = useState(INITIAL_STATE);
  const [address, setAddress] = useState('');
  const nativePageVisible = agentBrowserShowsNativePage(state);
  const surface = agentBrowserSurfacePresentation(state);
  const annotations = usePageAnnotations(pageId, state.page_id === pageId ? state.annotations : null);
  const annotationPageBackground = useMemo(() => {
    if (!annotations.snapshot?.active) return undefined;
    const value = String(annotations.snapshot.page_background || '').trim();
    if (!value) return undefined;
    const supports = globalThis.CSS?.supports;
    if (typeof supports === 'function' && !supports.call(globalThis.CSS, 'color', value)) return undefined;
    return value;
  }, [annotations.snapshot?.active, annotations.snapshot?.page_background]);
  // 记录归会话（Q33）：页签只把快照上报到会话级待发箱，关掉时降级为 orphan 而不是删除。
  const annotationOutbox = useMemo(() => annotationOutboxFor(sessionId, api), [sessionId, api]);
  useEffect(() => {
    if (!annotationOutbox) return;
    const closed = state.closed === true;
    annotationOutbox.observe({
      pageId,
      url: state.url,
      title: state.title,
      closed,
      snapshot: closed ? null : annotations.snapshot,
    });
  }, [annotationOutbox, pageId, state.url, state.title, state.closed, annotations.snapshot]);
  // 保存后补图（#212）：记录数一涨就说明刚存了一条，截一次视口、裁两张图回填。
  // 放在面板而不是保存命令里，是因为捕获是异步的，且失败不能阻塞保存。
  // annotations 每次渲染都是新对象：把它放进依赖，任何一次重渲染（保存后紧跟
  // 的页面状态事件就是一次）都会作废在途的截图，而记录数没有再涨，不会重截，
  // 这条批注就永远没有图。所以只按「页面 + 记录数」触发，命令与快照从 ref 取，
  // 只有换页或卸载才作废在途的截图。
  const annotationsRef = useRef(annotations);
  annotationsRef.current = annotations;
  const capturePageRef = useRef('');
  useEffect(() => {
    capturePageRef.current = pageId;
    return () => { capturePageRef.current = ''; };
  }, [pageId]);
  const captureStateRef = useRef({ pageId: '', count: 0 });
  const annotationRecordCount = Array.isArray(annotations.snapshot?.records)
    ? annotations.snapshot.records.length
    : 0;
  useEffect(() => {
    const previous = captureStateRef.current;
    captureStateRef.current = { pageId, count: annotationRecordCount };
    if (previous.pageId !== pageId || annotationRecordCount <= previous.count) return;
    const snapshot = annotationsRef.current.snapshot;
    const record = snapshot.records[annotationRecordCount - 1];
    const current = () => capturePageRef.current === pageId;
    void (async () => {
      try {
        const result = await capturePageAnnotationScreenshot(pageId);
        if (!current() || result?.ok !== true) return;
        const rect = captureRectForRecord(record, snapshot.viewport);
        const images = await cropAnnotationImages(result.data, rect, snapshot.viewport);
        if (!current()) return;
        await annotationsRef.current.command('attach_screenshot', {
          record_id: record.id,
          thumbnail: images.thumbnail,
          page_shot: images.pageShot,
        });
      } catch {
        // 捕获失败只是该条无图，卡片退化为标签 + 文本片段。
      }
    })();
  }, [pageId, annotationRecordCount]);
  // 铅笔路径（#217）：切到这一页，按图钉编号重新选中那条记录。注册表只挂一次，
  // 命令同样从上面的 ref 取。
  useEffect(() => {
    if (!sessionId || !annotations.snapshot?.document_id) return undefined;
    return registerAnnotationEditor(pageId, async (record) => {
      const selected = await selectAgentBrowserPage(pageId);
      if (selected?.ok === false) return selected;
      return annotationsRef.current.command('edit_record', { record_id: record.id });
    });
  }, [pageId, sessionId, annotations.snapshot?.document_id]);

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
    const desiredLayout = agentBrowserLayoutFromRect(
      surfaceRect,
      scale,
      visible,
    );
    desiredLayout.interaction_blocked = !forceHidden &&
      nativeSurfaceInteractionBlockedByDocument(surfaceRect, document, window);
    desiredLayout.occlusion_rects = visible && supportsLocalOcclusion
      ? nativeSurfaceOcclusionRectsFromClientRects(
          surfaceRect,
          overlayGeometry.occlusionRects,
          scale,
        )
      : [];
    if (occlusionFailureRef.current.pageId !== pageId) {
      occlusionFailureRef.current = { pageId, signature: '' };
    }
    let delivery = nativeSurfaceLayoutWithOcclusionFallback(
      desiredLayout,
      occlusionFailureRef.current.signature,
    );
    if (occlusionFailureRef.current.signature
        && occlusionFailureRef.current.signature !== delivery.idealSignature) {
      occlusionFailureRef.current = { pageId, signature: '' };
      delivery = nativeSurfaceLayoutWithOcclusionFallback(desiredLayout);
    }
    const next = nextAgentBrowserLayoutRequest(delivery.layout, layoutStateRef.current, {
      force,
      allocateRevision: allocateAgentBrowserLayoutRevision,
    });
    if (!next.changed) return;
    layoutStateRef.current = { signature: next.signature, revision: next.revision };
    const request = next.request;
    void setAgentBrowserLayout(pageId, request).then((result) => {
      const failedSignature = failedNativeSurfaceOcclusionSignature({
        result,
        request,
        requestSignature: delivery.idealSignature,
        currentRevision: layoutStateRef.current.revision,
      });
      if (!failedSignature) {
        if (result?.ok !== true && request.layout_revision === layoutStateRef.current.revision) {
          console.warn('[agent-browser] native layout was not acknowledged', result?.error || '');
        }
        return;
      }

      occlusionFailureRef.current = { pageId, signature: failedSignature };
      console.warn(
        '[agent-browser] local overlay occlusion failed; hiding Browser until overlay changes',
        result?.error || '',
      );
      const fallback = nextAgentBrowserLayoutRequest(
        { ...request, visible: false, occlusion_rects: [] },
        layoutStateRef.current,
        { force: true, allocateRevision: allocateAgentBrowserLayoutRevision },
      );
      layoutStateRef.current = {
        signature: fallback.signature,
        revision: fallback.revision,
      };
      void setAgentBrowserLayout(pageId, fallback.request).then((fallbackResult) => {
        if (fallbackResult?.ok !== true) {
          console.warn(
            '[agent-browser] native overlay fallback was not acknowledged',
            fallbackResult?.error || '',
          );
        }
      }).catch((error) => {
        console.warn('[agent-browser] native overlay fallback failed', error);
      });
    }).catch((error) => {
      const failedSignature = failedNativeSurfaceOcclusionSignature({
        result: { ok: false },
        request,
        requestSignature: delivery.idealSignature,
        currentRevision: layoutStateRef.current.revision,
      });
      if (failedSignature) {
        occlusionFailureRef.current = { pageId, signature: failedSignature };
      }
      console.warn('[agent-browser] native layout failed', error);
    });
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
    const onNativeSurfaceOverlayChange = () => {
      if (disposed) return;
      // Explicit overlay notifications run from layout effects after the DOM
      // is committed. Submit the native hide/occlusion now so a sibling
      // WebView cannot paint over the overlay while waiting for the next frame.
      syncNativeSurface();
      scheduleLayout();
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
        'data-ace-native-interaction',
        'data-ace-native-overlay',
        'data-ace-native-overlay-bleed',
        'data-ace-native-overlay-square',
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
    window.addEventListener(NATIVE_SURFACE_OVERLAY_EVENT, onNativeSurfaceOverlayChange);
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
      window.removeEventListener(NATIVE_SURFACE_OVERLAY_EVENT, onNativeSurfaceOverlayChange);
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

  const navigateTo = useCallback(async (url) => {
    const result = await runAgentBrowserBridgeAction(
      'aceDesktop_agentBrowserNavigate',
      { page_id: pageId, url },
    );
    if (result?.ok === false) {
      toast({ kind: 'err', text: result.error || '无法打开该地址' });
    }
  }, [pageId]);

  const submitAddress = useCallback(async (event) => {
    event.preventDefault();
    const normalized = normalizeAgentBrowserAddress(address);
    if (!normalized) {
      toast({ kind: 'err', text: '请输入 http 或 https 地址' });
      return;
    }
    await navigateTo(normalized);
  }, [address, navigateTo]);

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

  const statusSurface = (
    <AgentBrowserStatusSurface
      surface={surface}
      onRetry={() => runAction('aceDesktop_agentBrowserReload')}
      onGoBack={() => runAction('aceDesktop_agentBrowserGoBack')}
    />
  );

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
            className={clsx('ace-agent-browser-annotation-toggle', annotations.snapshot.active && 'is-active')}
            disabled={!nativePageVisible || state.element_selection_active || annotations.working}
            aria-pressed={!!annotations.snapshot.active}
            aria-label={annotations.snapshot.active ? '退出网页批注' : '网页批注'}
            title={annotations.snapshot.active ? '退出网页批注' : '网页批注'}
            onClick={() => annotations.command(annotations.snapshot.active ? 'exit' : 'start')}
          >
            <VsIcon name="chat" size={16} />
            </button>
          <button
            type="button"
            className={clsx(
              'ace-agent-browser-toolbar-button',
              state.element_selection_active && 'is-checked',
            )}
            disabled={!nativePageVisible || annotations.snapshot.active || annotations.working}
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
            title="切换开发者工具"
            aria-label="切换开发者工具"
            onClick={openDevTools}
          >
            <VsIcon name="DeveloperTools" size={17} />
          </button>
          <AgentBrowserMoreMenu
            pageId={pageId}
            ready={state.ready}
            onAddConsoleLogs={addConsoleLogs}
          />
        </div>
      </div>
      <div className={clsx('ace-agent-browser-frame', agentActive && 'is-agent-active')}>
        <div
          ref={viewportRef}
          className="ace-agent-browser-native-viewport"
          style={annotationPageBackground
            ? { '--ace-annotation-page-background': annotationPageBackground }
            : undefined}
        >
          {nativePageVisible && annotations.snapshot.popup && (
            <PageAnnotationPopup
              key={`${pageId}:${annotations.snapshot.document_id}`}
              snapshot={annotations.snapshot}
              command={annotations.command}
              sessionId={sessionId}
              working={annotations.working}
              viewportRef={viewportRef}
              onAnnotationsSaved={(next) => {
                annotationOutbox?.observe({
                  pageId,
                  url: state.url,
                  title: state.title,
                  snapshot: next,
                });
              }}
            />
          )}
          <PageAnnotationSendBar pageId={pageId} snapshot={annotations.snapshot}
            api={api} sessionId={sessionId} page={state}
            command={annotations.command} working={annotations.working} />
          {surface.kind === 'empty' && (
            <AgentBrowserNewTab
              recentFiles={recentFiles}
              onNavigate={navigateTo}
              onOpenRecentFile={onOpenRecentFile}
              fallback={statusSurface}
            />
          )}
          {!nativePageVisible && surface.kind !== 'empty' && statusSurface}
        </div>
      </div>
      {annotations.error && (
        <div className="flex items-center gap-3 border-t border-border px-3 py-1.5 text-[12px] text-fg-2">
          {annotations.error && <span role="alert" className="text-err">{annotations.error}</span>}
        </div>
      )}
    </div>
  );
}

function AgentBrowserStatusSurface({ surface, onRetry, onGoBack }) {
  return (
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
      {surface.diagnostic && (
        <pre
          className="ace-agent-browser-status-diagnostic"
          aria-label="NSError"
        >
          {surface.diagnostic}
        </pre>
      )}
      {surface.canRetry && (
        <div className="ace-agent-browser-status-actions">
          <button
            type="button"
            className="inline-flex h-8 items-center gap-1.5 rounded-md bg-accent px-3 text-[12px] font-medium text-white transition hover:opacity-90"
            onClick={onRetry}
          >
            <RefreshIcon size={14} />
            重试
          </button>
          {surface.canGoBack && (
            <button
              type="button"
              className="inline-flex h-8 items-center gap-1.5 rounded-md border border-border bg-surface px-3 text-[12px] font-medium text-fg-2 transition hover:bg-surface-hi hover:text-fg"
              onClick={onGoBack}
            >
              <NavigationArrowIcon direction="back" size={14} />
              返回上一页
            </button>
          )}
        </div>
      )}
    </div>
  );
}
