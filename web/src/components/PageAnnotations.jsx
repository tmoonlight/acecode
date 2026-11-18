import { useEffect, useLayoutEffect, useRef, useState } from 'react';
import { createPageAnnotationController, pageAnnotationPopupPosition } from '../lib/pageAnnotations.js';
import { notifyNativeSurfaceOverlayChange } from '../lib/agentBrowserSurfaceCoordinator.js';
import { PageAnnotationTextField } from './PageAnnotationTextField.jsx';

export function usePageAnnotations(pageId, nativeState) {
  const [snapshot, setSnapshot] = useState({ active: false, pending_count: 0 });
  const [error, setError] = useState('');
  const [working, setWorking] = useState(false);
  const controllerRef = useRef(null);
  useEffect(() => {
    const controller = createPageAnnotationController({ pageId, onChange: next => setSnapshot({ ...next, pageId }) });
    controllerRef.current = controller;
    setSnapshot({ ...controller.getSnapshot(), pageId });
    setError('');
    setWorking(false);
    return () => {
      controller.dispose();
      controllerRef.current = null;
    };
  }, [pageId]);
  useEffect(() => { controllerRef.current?.observe(nativeState); }, [nativeState, pageId]);
  useEffect(() => {
    if (!snapshot.active) return undefined;
    let cancelled = false;
    let timer;
    const controller = controllerRef.current;
    const poll = async () => {
      const result = await controller.command('read');
      if (cancelled) return;
      if (result?.ok === false) {
        setError(result.error || '无法读取网页批注，请退出后重试');
        return;
      }
      timer = setTimeout(poll, 200);
    };
    timer = setTimeout(poll, 200);
    return () => { cancelled = true; clearTimeout(timer); };
  }, [pageId, snapshot.active, snapshot.document_id]);
  const command = async (action, fields) => {
    const controller = controllerRef.current;
    const preview = action === 'preview_text';
    if (!controller || (working && !preview)) return { ok: false };
    if (!preview) setWorking(true);
    setError('');
    const result = await controller.command(action, fields);
    if (controllerRef.current === controller) {
      if (!preview) setWorking(false);
      if (result?.ok === false) setError(result.error || '网页批注操作失败');
    }
    return result;
  };
  return {
    snapshot: snapshot.pageId === pageId ? snapshot : { active: false, pending_count: 0 },
    command, working, error,
  };
}

export function PageAnnotationPopup({ snapshot, command, working, viewportRef }) {
  const popupRef = useRef(null);
  const inputRef = useRef(null);
  const [description, setDescription] = useState('');
  useLayoutEffect(() => {
    const popup = popupRef.current;
    const viewport = viewportRef.current;
    if (!popup || !viewport) return undefined;
    const position = () => {
      const bounds = viewport.getBoundingClientRect();
      const popupBounds = popup.getBoundingClientRect();
      const next = pageAnnotationPopupPosition({
        width: bounds.width, height: bounds.height,
        popupWidth: popupBounds.width, popupHeight: popupBounds.height,
        target: snapshot.target, viewport: snapshot.viewport,
      });
      popup.style.left = `${next.left}px`;
      popup.style.top = `${next.top}px`;
      notifyNativeSurfaceOverlayChange();
    };
    position();
    const observer = new ResizeObserver(position);
    observer.observe(viewport);
    observer.observe(popup);
    return () => { observer.disconnect(); notifyNativeSurfaceOverlayChange(); };
  }, [snapshot.target, snapshot.viewport, viewportRef]);
  useEffect(() => {
    const previousFocus = document.activeElement;
    inputRef.current?.focus();
    return () => {
      notifyNativeSurfaceOverlayChange();
      if (previousFocus?.isConnected && document.activeElement === document.body) previousFocus.focus();
    };
  }, []);
  const close = () => { if (!working) void command('cancel'); };
  const onKeyDown = (event) => {
    if (event.key === 'Escape') {
      event.preventDefault();
      event.stopPropagation();
      close();
    }
    if (event.key === 'Tab') {
      const controls = [...popupRef.current.querySelectorAll('button:not(:disabled), textarea:not(:disabled)')];
      const first = controls[0];
      const last = controls[controls.length - 1];
      if (event.shiftKey && document.activeElement === first) {
        event.preventDefault(); last?.focus();
      } else if (!event.shiftKey && document.activeElement === last) {
        event.preventDefault(); first?.focus();
      }
    }
  };
  return (
    <form
      ref={popupRef}
      role="dialog"
      aria-label="网页批注"
      data-ace-native-overlay="overlap"
      className="absolute z-20 flex w-80 max-w-[calc(100%-16px)] max-h-[calc(100%-16px)] flex-col gap-3 overflow-auto rounded-lg border border-border bg-surface p-3 text-[12px] text-fg"
      onKeyDown={onKeyDown}
      onSubmit={(event) => { event.preventDefault(); if (!working) void command('save', { description }); }}
    >
      <div className="flex items-center justify-between gap-2">
        <span className="truncate">{snapshot.target.tag}</span>
        <button type="button" disabled={working} aria-label="关闭批注" onClick={close} className="rounded px-2 py-1 hover:bg-surface-hi">关闭</button>
      </div>
      <button type="button" disabled={working} onClick={() => void command('original', { enabled: !snapshot.original })} className="self-start rounded px-2 py-1 hover:bg-surface-hi">{snapshot.original ? '恢复预览' : '查看原图'}</button>
      <PageAnnotationTextField target={snapshot.target} command={command} working={working} />
      <textarea
        ref={inputRef}
        aria-label="批注描述"
        placeholder="描述修改要求"
        maxLength={4096}
        rows={4}
        value={description}
        disabled={working}
        onChange={(event) => setDescription(event.target.value)}
        className="min-h-16 resize-y rounded-md border border-border bg-bg px-2 py-1.5 text-fg outline-none focus:border-accent"
      />
      <button type="submit" disabled={working} className="self-end rounded-md bg-accent px-3 py-1.5 text-white disabled:opacity-50">保存批注</button>
    </form>
  );
}
