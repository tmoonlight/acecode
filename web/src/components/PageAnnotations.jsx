import { useEffect, useLayoutEffect, useRef, useState } from 'react';
import { createPageAnnotationController, pageAnnotationPopupPosition } from '../lib/pageAnnotations.js';
import { notifyNativeSurfaceOverlayChange } from '../lib/agentBrowserSurfaceCoordinator.js';
import { PageAnnotationTextField } from './PageAnnotationTextField.jsx';
import { PageAnnotationTargets } from './PageAnnotationTargets.jsx';

export function usePageAnnotations(pageId, nativeState) {
  const [snapshot, setSnapshot] = useState({ active: false, pending_count: 0 });
  const [error, setError] = useState('');
  const [working, setWorking] = useState(false);
  const controllerRef = useRef(null);
  const pendingRef = useRef(0);
  useEffect(() => {
    const controller = createPageAnnotationController({ pageId, onChange: next => setSnapshot({ ...next, pageId }) });
    controllerRef.current = controller;
    setSnapshot({ ...controller.getSnapshot(), pageId });
    setError('');
    setWorking(false);
    pendingRef.current = 0;
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
    if (!controller) return { ok: false };
    const preview = action === 'preview_text' || action === 'preview_style';
    if (!preview) { pendingRef.current += 1; setWorking(true); }
    setError('');
    const result = await controller.command(action, fields);
    if (controllerRef.current === controller) {
      if (!preview) { pendingRef.current -= 1; setWorking(pendingRef.current > 0); }
      if (result?.ok === false) setError(result.error || '网页批注操作失败');
    }
    return result;
  };
  return {
    snapshot: snapshot.pageId === pageId ? snapshot : { active: false, pending_count: 0 },
    command, working, error,
  };
}

const styleGroups = [
  [['color', '文字颜色'], ['background-color', '背景颜色'], ['opacity', '不透明度']],
  [['font-family', '字体'], ['font-size', '字号'], ['font-weight', '字重']],
  [['border-radius', '圆角'], ['border-color', '边框颜色'], ['border-width', '边框宽度']],
  [['width', '宽度'], ['height', '高度']],
  [['letter-spacing', '字间距'], ['word-spacing', '词间距'], ['gap', '间隙']],
  [['padding', '内边距'], ['padding-top', '上内边距'], ['padding-right', '右内边距'], ['padding-bottom', '下内边距'], ['padding-left', '左内边距']],
  [['margin', '外边距'], ['margin-top', '上外边距'], ['margin-right', '右外边距'], ['margin-bottom', '下外边距'], ['margin-left', '左外边距']],
];

export function PageAnnotationPopup({ snapshot, command, working, viewportRef }) {
  const popupRef = useRef(null);
  const inputRef = useRef(null);
  const [description, setDescription] = useState('');
  const [styleInputs, setStyleInputs] = useState({});
  const [saving, setSaving] = useState(false);
  const dirtyStyles = useRef({});
  const styleFlight = useRef(null);
  const styleTimer = useRef(null);
  const liveRef = useRef(true);
  const preview = snapshot.target?.style_preview;
  const flushStyles = async () => {
    clearTimeout(styleTimer.current);
    if (styleFlight.current) {
      const result = await styleFlight.current;
      if (result?.ok !== true) return result;
    }
    if (!liveRef.current) return { ok: false, cancelled: true };
    const styles = { ...dirtyStyles.current };
    if (!Object.keys(styles).length) return { ok: true };
    const flight = command('preview_style', { target_id: snapshot.target?.id, styles });
    styleFlight.current = flight;
    const result = await flight;
    if (styleFlight.current === flight) styleFlight.current = null;
    if (result?.ok === true) {
      for (const [field, value] of Object.entries(styles)) {
        if (dirtyStyles.current[field] === value) delete dirtyStyles.current[field];
      }
    }
    return result;
  };
  const selectCommand = async (action, fields) => {
    if ((await flushStyles())?.ok === true && liveRef.current) return command(action, fields);
    return { ok: false };
  };
  useEffect(() => {
    dirtyStyles.current = {};
    setStyleInputs(preview?.styles || {});
    clearTimeout(styleTimer.current);
  }, [snapshot.target?.id]);
  useEffect(() => {
    liveRef.current = true;
    return () => { liveRef.current = false; clearTimeout(styleTimer.current); };
  }, []);
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
        target: snapshot.target || (snapshot.region ? { bounds: {
          ...snapshot.region,
          x: snapshot.region.x - snapshot.viewport.scroll_x,
          y: snapshot.region.y - snapshot.viewport.scroll_y,
        } } : null), viewport: snapshot.viewport,
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
  }, [snapshot.target, snapshot.region, snapshot.viewport, snapshot.selection_mode, viewportRef]);
  useEffect(() => {
    const previousFocus = document.activeElement;
    inputRef.current?.focus();
    return () => {
      notifyNativeSurfaceOverlayChange();
      if (previousFocus?.isConnected && document.activeElement === document.body) previousFocus.focus();
    };
  }, []);
  useEffect(() => {
    if (!snapshot.selection_mode) inputRef.current?.focus();
  }, [snapshot.selection_mode]);
  const close = () => { if (!working) void command('cancel'); };
  const onKeyDown = (event) => {
    if (event.key === 'Escape') {
      event.preventDefault();
      event.stopPropagation();
      close();
    }
    if (event.key === 'Tab') {
      const controls = [...popupRef.current.querySelectorAll('button:not(:disabled), textarea:not(:disabled), input:not(:disabled), summary')]
        .filter(control => control.getClientRects().length > 0);
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
      data-ace-native-overlay={snapshot.selection_mode ? undefined : 'overlap'}
      style={snapshot.selection_mode ? { display: 'none' } : undefined}
      className="absolute z-20 flex w-80 max-w-[calc(100%-16px)] max-h-[calc(100%-16px)] flex-col gap-3 overflow-auto rounded-lg border border-border bg-surface p-3 text-[12px] text-fg"
      onKeyDown={onKeyDown}
      onSubmit={async (event) => {
        event.preventDefault();
        if (working || snapshot.original) return;
        setSaving(true);
        try {
          if ((await flushStyles())?.ok === true && liveRef.current) await command('save', { description });
        } finally { if (liveRef.current) setSaving(false); }
      }}
    >
      <div className="flex items-center justify-between gap-2">
        <span className="truncate">{snapshot.target?.tag || '区域'}</span>
        <button type="button" disabled={working} aria-label="关闭批注" onClick={close} className="rounded px-2 py-1 hover:bg-surface-hi">关闭</button>
      </div>
      <button type="button" disabled={working || saving} onClick={() => void selectCommand('original', { enabled: !snapshot.original })} className="self-start rounded px-2 py-1 hover:bg-surface-hi">{snapshot.original ? '恢复预览' : '查看原图'}</button>
      <PageAnnotationTextField target={snapshot.target} command={command} working={working || saving || snapshot.original} />
      <PageAnnotationTargets snapshot={snapshot} command={selectCommand} working={working || saving} />
      {preview && (
        <details>
          <summary className="cursor-pointer py-1">样式</summary>
          <fieldset disabled={saving || snapshot.original} className="mt-2 flex flex-col gap-3 disabled:opacity-60">
            {styleGroups.map((group, index) => (
              <div key={index} className="flex flex-col gap-1.5 border-b border-border pb-3 last:border-0 last:pb-0">
                {group.map(([field, label]) => (
                  <label key={field} className="flex items-center justify-between gap-2">
                    <span className="text-fg-mute">{label}</span>
                    <input
                      aria-label={label}
                      value={styleInputs[field] ?? ''}
                      placeholder={preview.values[field]}
                      onChange={event => {
                        const value = event.target.value;
                        dirtyStyles.current[field] = value;
                        setStyleInputs(previous => ({ ...previous, [field]: value }));
                        clearTimeout(styleTimer.current);
                        if (!value || CSS.supports(field, value)) styleTimer.current = setTimeout(() => { void flushStyles(); }, 150);
                      }}
                      onBlur={() => { void flushStyles(); }}
                      onKeyDown={event => {
                        if (event.key === 'Enter') { event.preventDefault(); event.currentTarget.blur(); }
                      }}
                      className="h-7 w-40 min-w-0 rounded-md border border-border bg-surface-alt px-2 text-[12px] text-fg outline-none focus:border-accent"
                    />
                  </label>
                ))}
                {index === 3 && (
                  <label className="flex items-center gap-2">
                    <input type="checkbox" disabled={working} checked={!!preview.ratio_locked} onChange={async event => {
                      const ratio_lock = event.target.checked;
                      if ((await flushStyles())?.ok === true && liveRef.current) await command('preview_style', { styles: {}, ratio_lock });
                    }} />
                    锁定当前比例
                  </label>
                )}
              </div>
            ))}
          </fieldset>
          {preview.errors?.length > 0 && <p role="alert" className="mt-2 text-danger">原内联 !important 阻止预览: {preview.errors.join(', ')}</p>}
          {snapshot.differences?.length > 0 && (
            <dl className="mt-3 space-y-1 overflow-wrap-anywhere font-mono text-[11px]">
              {snapshot.differences.map(change => (
                <div key={`${change.target_id}:${change.property}`}>
                  <dt>{change.property}</dt>
                  <dd><del>{change.old_value}</del> → <ins>{change.new_value}</ins></dd>
                </div>
              ))}
            </dl>
          )}
        </details>
      )}
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
      <button type="submit" disabled={working || snapshot.original} className="self-end rounded-md bg-accent px-3 py-1.5 text-white disabled:opacity-50">保存批注</button>
    </form>
  );
}
