import { useEffect, useLayoutEffect, useRef, useState } from 'react';
import { createPortal } from 'react-dom';
import {
  createPageAnnotationController,
  pageAnnotationPopupDragPosition,
  pageAnnotationPopupPosition,
} from '../lib/pageAnnotations.js';
import {
  localizedAnnotationSendError, requestAnnotationSubmit,
} from '../lib/annotationSubmission.js';
import { notifyNativeSurfaceOverlayChange } from '../lib/agentBrowserSurfaceCoordinator.js';
import {
  setAnnotationChromeSuspended,
} from '../lib/annotationChromeSuspension.js';
import { dialogEnterAction, resolveDialogTabTarget } from '../lib/dialogKeyboard.js';
import { isDismissShortcut, isShortcutComposing } from '../lib/shortcutBindings.js';
import { focusAnnotationDescription } from '../lib/annotationFocus.js';
import { PageAnnotationTextField } from './PageAnnotationTextField.jsx';
import { toast } from './Toast.jsx';

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
      // 一次读取失败（断网、页面正在导航）不能让轮询停下：停一次，页面上的
      // 点击就再也开不出浮窗。失败退避重试，成功即清掉错误提示。
      if (result?.ok === false) {
        setError(result.error || '无法读取网页批注，正在重试');
        timer = setTimeout(poll, 1000);
        return;
      }
      setError(current => (current ? '' : current));
      timer = setTimeout(poll, 200);
    };
    timer = setTimeout(poll, 200);
    return () => { cancelled = true; clearTimeout(timer); };
  }, [pageId, snapshot.active, snapshot.document_id]);
  // Any WebUI menu floated over the page (tab context menu, toolbar overflow,
  // page context menu forwarded from the iframe) covers the annotation chrome.
  // Suspend the in-page outline and hover card while one is up instead of
  // letting them punch through the menu.
  useEffect(() => {
    if (!snapshot.active) return undefined;
    const controller = controllerRef.current;
    if (!controller) return undefined;
    let suspended = false;
    let frame = 0;
    let disposed = false;
    const emit = (next) => { void controller.command('set_modal_suspended', { enabled: next }); };
    const apply = (next) => {
      if (disposed || next === suspended) return;
      suspended = next;
      emit(next);
    };
    const menuOpen = () => Array.from(document.querySelectorAll('[role="menu"]')).some((menu) => (
      menu.getAttribute('aria-hidden') !== 'true' && (menu.getClientRects?.().length || 0) > 0
    ));
    const sync = () => {
      frame = 0;
      if (disposed) return;
      setAnnotationChromeSuspended('menu', menuOpen(), apply);
    };
    const schedule = () => {
      if (frame || disposed) return;
      frame = requestAnimationFrame(sync);
    };
    const observer = new MutationObserver(schedule);
    observer.observe(document.body, {
      childList: true,
      subtree: true,
      attributes: true,
      attributeFilter: ['aria-hidden', 'hidden', 'role', 'style'],
    });
    schedule();
    return () => {
      disposed = true;
      observer.disconnect();
      if (frame) cancelAnimationFrame(frame);
      setAnnotationChromeSuspended('menu', false, emit);
    };
  }, [pageId, snapshot.active, snapshot.document_id]);
  const command = async (action, fields) => {
    const controller = controllerRef.current;
    if (!controller) return { ok: false };
    const preview = action === 'preview_text' || action === 'preview_style';
    // Chrome suppression is a pure feedback toggle: it must not mark the
    // session busy, otherwise opening a menu would grey out the toolbar.
    const quiet = preview || action === 'set_modal_suspended';
    if (!quiet) { pendingRef.current += 1; setWorking(true); }
    setError('');
    const result = await controller.command(action, fields);
    if (controllerRef.current === controller) {
      if (!quiet) { pendingRef.current -= 1; setWorking(pendingRef.current > 0); }
      if (result?.ok === false) setError(result.error || '网页批注操作失败');
    }
    return result;
  };
  return {
    snapshot: snapshot.pageId === pageId ? snapshot : { active: false, pending_count: 0 },
    command, working, error,
  };
}

const styleRows = [
  ['color', '文字颜色', 'color'],
  ['background-color', '背景颜色', 'color'],
  ['font-family', '字体', 'font'],
  ['font-size', '字号', 'px'],
  ['font-weight', '字重', 'weight'],
  ['border-radius', '圆角', 'px'],
  ['border-color', '边框颜色', 'color'],
  ['border-width', '边框宽度', 'px'],
  ['opacity', '不透明度', 'opacity'],
  ['letter-spacing', '字间距', 'px'],
  ['word-spacing', '词间距', 'px'],
];

const fontFamilies = [
  ['Inter', '"Inter Variable", Arial, sans-serif'],
  ['System', 'system-ui, sans-serif'],
  ['Arial', 'Arial, sans-serif'],
  ['Serif', 'Georgia, "Times New Roman", serif'],
  ['Mono', 'ui-monospace, SFMono-Regular, "SF Mono", Menlo, Consolas, monospace'],
];

const fontWeights = ['100', '200', '300', '400', '500', '600', '700', '800', '900'];
const spacingRows = [
  ['padding', '内边距', [
    ['padding-top', '上内边距'],
    ['padding-right', '右内边距'],
    ['padding-bottom', '下内边距'],
    ['padding-left', '左内边距'],
  ], [
    ['vertical', 'padding-top', '上', 'padding-bottom', '底部'],
    ['horizontal', 'padding-left', '左', 'padding-right', '右'],
  ]],
  ['margin', '外边距', [
    ['margin-top', '上外边距'],
    ['margin-right', '右外边距'],
    ['margin-bottom', '下外边距'],
    ['margin-left', '左外边距'],
  ], [
    ['vertical', 'margin-top', '上', 'margin-bottom', '底部'],
    ['horizontal', 'margin-left', '左', 'margin-right', '右'],
  ]],
];

function sameNonZeroLength(first, second) {
  const firstValue = String(first ?? '').trim();
  const secondValue = String(second ?? '').trim();
  if (!firstValue || !secondValue || firstValue === '0' || secondValue === '0') return false;
  if (firstValue === secondValue) return true;
  const firstNumber = Number.parseFloat(firstValue);
  const secondNumber = Number.parseFloat(secondValue);
  return Number.isFinite(firstNumber) && Number.isFinite(secondNumber) &&
    firstNumber !== 0 && firstNumber === secondNumber;
}

function spacingLockState(values = {}) {
  return {
    'padding-vertical': sameNonZeroLength(values['padding-top'], values['padding-bottom']),
    'padding-horizontal': sameNonZeroLength(values['padding-left'], values['padding-right']),
    'margin-vertical': sameNonZeroLength(values['margin-top'], values['margin-bottom']),
    'margin-horizontal': sameNonZeroLength(values['margin-left'], values['margin-right']),
  };
}

function colorSwatchValue(value) {
  const text = String(value || '').trim();
  const match = /^#(?:[0-9a-f]{3}|[0-9a-f]{6})$/i.exec(text);
  if (match) {
    const hex = match[0].slice(1);
    return `#${hex.length === 3 ? hex.replace(/./g, digit => digit + digit) : hex}`;
  }
  const rgb = /^rgba?\(\s*(\d{1,3})\s*,\s*(\d{1,3})\s*,\s*(\d{1,3})/i.exec(text);
  if (!rgb) return '#000000';
  return `#${rgb.slice(1, 4).map(channel => Math.min(255, Number(channel)).toString(16).padStart(2, '0')).join('')}`;
}

function pxNumberOf(value, fallback = '') {
  const text = String(value ?? '').trim();
  const match = /^(-?\d+(?:\.\d+)?)px$/i.exec(text);
  if (match) return match[1];
  return fallback;
}

function numberFromStyle(value) {
  const text = String(value ?? '').trim();
  if (!text) return '';
  const number = Number(text);
  return Number.isFinite(number) ? String(number) : '';
}

function cssValuesEqual(first, second) {
  const a = String(first ?? '').trim();
  const b = String(second ?? '').trim();
  if (a === b) return true;
  const firstNumber = Number.parseFloat(a);
  const secondNumber = Number.parseFloat(b);
  return Number.isFinite(firstNumber) && Number.isFinite(secondNumber)
    && /^(-?\d+(?:\.\d+)?)(px|em|rem|%)?$/i.test(a)
    && /^(-?\d+(?:\.\d+)?)(px|em|rem|%)?$/i.test(b)
    && firstNumber === secondNumber;
}

function editorValueFor(field, kind, raw) {
  const text = String(raw ?? '');
  if (!text.trim()) return '';
  if (kind === 'px') {
    const trimmed = text.trim();
    return /^-?(?:\d+\.?\d*|\.\d+)$/.test(trimmed) ? `${trimmed}px` : trimmed;
  }
  return text;
}

function controlValueFor(field, kind, value) {
  if (kind === 'px') return pxNumberOf(value);
  if (kind === 'opacity') return opacityPercentFromCss(value);
  return String(value ?? '');
}

function controlPlaceholderFor(field, kind, value) {
  if (kind === 'px') {
    const number = pxNumberOf(value);
    return number || (String(value ?? '').trim() === 'normal' ? '0' : '');
  }
  if (kind === 'opacity') return opacityPercentFromCss(value);
  return String(value ?? '');
}

function opacityPercentFromCss(value) {
  const text = String(value ?? '').trim();
  if (!text) return '';
  const number = Number(text);
  if (!Number.isFinite(number)) return '';
  return String(Math.round(Math.max(0, Math.min(1, number)) * 100));
}

function opacityCssFromPercent(value) {
  const text = String(value ?? '').trim();
  if (!text) return '';
  const number = Number(text);
  if (!Number.isFinite(number)) return '';
  return String(Math.max(0, Math.min(100, number)) / 100);
}

function fontSuggestions(value, previousValue) {
  const seen = new Set(fontFamilies.map(([, family]) => family));
  const custom = [value, previousValue]
    .map(item => String(item ?? '').trim())
    .filter((item, index, list) => item && list.indexOf(item) === index && !seen.has(item))
    .map(item => [item, item]);
  return [...custom, ...fontFamilies];
}

export function PageAnnotationPopup({
  snapshot, command, working, viewportRef, sessionId, onAnnotationsSaved,
}) {
  const popupRef = useRef(null);
  const inputRef = useRef(null);
  const [description, setDescription] = useState('');
  // 从待发卡片点铅笔回来时（#217），原生已经把该条的 locator 定位成草稿，
  // 这里把描述回填进输入框；新开一条时 draft_description 为空，等同清空。
  useEffect(() => {
    if (!snapshot.popup) return;
    setDescription(String(snapshot.draft_description || ''));
  }, [snapshot.document_id, snapshot.draft_description, snapshot.popup]);
  const [styleInputs, setStyleInputs] = useState({});
  const [saving, setSaving] = useState(false);
  const [adjusting, setAdjusting] = useState(false);
  const [openSpacing, setOpenSpacing] = useState({});
  const [spacingLocks, setSpacingLocks] = useState({});
  const [selectMenu, setSelectMenu] = useState(null);
  const [tooltip, setTooltip] = useState(null);
  const [adjustBounds, setAdjustBounds] = useState(null);
  const dirtyStyles = useRef({});
  // Ctrl+Enter = 先保存这一条，再走和输入框同一个提交（Q35 的第三个触发点）。
  const sendAfterSaveRef = useRef(false);
  const styleFlight = useRef(null);
  const styleTimer = useRef(null);
  const dragRef = useRef(null);
  const manualPositionRef = useRef(null);
  const lastPositionRef = useRef({ left: null, top: null, width: null, height: null });
  const schedulePositionRef = useRef(() => {});
  const positionNowRef = useRef(() => {});
  const liveRef = useRef(true);
  const selectMenuRef = useRef(null);
  // The native surface re-reads annotation state every 200ms. Those snapshots
  // are new objects even when nothing moved, so positioning must key off the
  // geometry itself instead of object identity or the overlay hole is rebuilt
  // continuously and the card visibly jitters.
  const snapshotRef = useRef(snapshot);
  snapshotRef.current = snapshot;
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
  useEffect(() => {
    dirtyStyles.current = {};
    setStyleInputs(preview?.styles || {});
    setOpenSpacing({});
    setSpacingLocks(spacingLockState(preview?.values));
    setSelectMenu(null);
    setTooltip(null);
    clearTimeout(styleTimer.current);
  }, [snapshot.target?.id]);
  useEffect(() => {
    if (!selectMenu) return undefined;
    const dismiss = event => {
      if (selectMenuRef.current?.contains(event.target)) return;
      if (event.target?.closest?.('.ace-annotation-select-trigger')) return;
      setSelectMenu(null);
    };
    const onKey = event => {
      if (event.key !== 'Escape') return;
      event.preventDefault();
      event.stopPropagation();
      setSelectMenu(null);
    };
    const onViewport = () => setSelectMenu(null);
    document.addEventListener('mousedown', dismiss);
    document.addEventListener('keydown', onKey, true);
    window.addEventListener('resize', onViewport);
    window.addEventListener('scroll', onViewport, true);
    return () => {
      document.removeEventListener('mousedown', dismiss);
      document.removeEventListener('keydown', onKey, true);
      window.removeEventListener('resize', onViewport);
      window.removeEventListener('scroll', onViewport, true);
    };
  }, [selectMenu]);
  useEffect(() => {
    liveRef.current = true;
    return () => { liveRef.current = false; clearTimeout(styleTimer.current); };
  }, []);
  useLayoutEffect(() => {
    dragRef.current = null;
    manualPositionRef.current = null;
    setAdjustBounds(current => (current ? null : current));
  }, [snapshot.target?.id, snapshot.region?.x, snapshot.region?.y, snapshot.region?.width, snapshot.region?.height]);
  useLayoutEffect(() => {
    const popup = popupRef.current;
    const viewport = viewportRef.current;
    if (!popup || !viewport) return undefined;
    let frame = 0;
    const position = () => {
      frame = 0;
      const current = snapshotRef.current;
      const bounds = viewport.getBoundingClientRect();
      const popupBounds = popup.getBoundingClientRect();
      const manual = manualPositionRef.current;
      const raw = manual
        ? pageAnnotationPopupDragPosition({
          ...manual,
          width: bounds.width,
          height: bounds.height,
          popupWidth: popupBounds.width,
          popupHeight: popupBounds.height,
        })
        : pageAnnotationPopupPosition({
          width: bounds.width, height: bounds.height,
          popupWidth: popupBounds.width, popupHeight: popupBounds.height,
          target: current.target || (current.region ? { bounds: {
            ...current.region,
            x: current.region.x - (current.viewport?.scroll_x || 0),
            y: current.region.y - (current.viewport?.scroll_y || 0),
          } } : null), viewport: current.viewport,
        });
      // The native overlay hole is built from integer pixels. Keep CSS and the
      // hole on the same grid so focus changes cannot make the card shimmer.
      const next = { left: Math.round(raw.left), top: Math.round(raw.top) };
      const last = lastPositionRef.current;
      const moved = Math.abs((last.left ?? NaN) - next.left) > 0.5 ||
        Math.abs((last.top ?? NaN) - next.top) > 0.5;
      const resized = Math.abs((last.width ?? NaN) - popupBounds.width) > 0.5 ||
        Math.abs((last.height ?? NaN) - popupBounds.height) > 0.5;
      popup.style.left = `${next.left}px`;
      popup.style.top = `${next.top}px`;
      if (moved || resized) notifyNativeSurfaceOverlayChange();
      last.left = next.left;
      last.top = next.top;
      last.width = popupBounds.width;
      last.height = popupBounds.height;
    };
    const schedule = () => {
      if (!frame) frame = requestAnimationFrame(position);
    };
    schedulePositionRef.current = schedule;
    positionNowRef.current = position;
    position();
    const observer = new ResizeObserver(schedule);
    observer.observe(viewport);
    observer.observe(popup);
    return () => {
      observer.disconnect();
      if (frame) cancelAnimationFrame(frame);
      schedulePositionRef.current = () => {};
      positionNowRef.current = () => {};
    };
  }, [viewportRef]);
  // ResizeObserver callbacks arrive after paint. Re-measure in a layout effect
  // so expanding the editor or swapping targets never leaves the card one
  // frame at its previous position before snapping into place.
  useLayoutEffect(() => {
    positionNowRef.current();
  });
  useLayoutEffect(() => {
    schedulePositionRef.current();
  }, [
    snapshot.selection_mode,
    snapshot.target?.id,
    snapshot.target?.bounds?.x,
    snapshot.target?.bounds?.y,
    snapshot.target?.bounds?.width,
    snapshot.target?.bounds?.height,
    snapshot.region?.x,
    snapshot.region?.y,
    snapshot.region?.width,
    snapshot.region?.height,
    snapshot.viewport?.width,
    snapshot.viewport?.height,
    snapshot.viewport?.scroll_x,
    snapshot.viewport?.scroll_y,
  ]);
  useEffect(() => {
    const previousFocus = document.activeElement;
    return () => {
      notifyNativeSurfaceOverlayChange();
      if (previousFocus?.isConnected && document.activeElement === document.body) previousFocus.focus();
    };
  }, []);
  useLayoutEffect(() => {
    if (!snapshot.popup || snapshot.selection_mode) return undefined;
    return focusAnnotationDescription(inputRef.current);
  }, [
    snapshot.popup,
    snapshot.selection_mode,
    snapshot.target?.id,
    snapshot.region?.x,
    snapshot.region?.y,
    snapshot.region?.width,
    snapshot.region?.height,
  ]);
  useEffect(() => {
    if (!snapshot.selection_mode) inputRef.current?.focus();
  }, [snapshot.selection_mode]);
  const close = () => { if (!working) void command('cancel'); };
  const showTooltip = (event, label, shortcut = '') => {
    const rect = event?.currentTarget?.getBoundingClientRect?.();
    if (!rect) return;
    setTooltip({
      label,
      shortcut,
      left: Math.max(56, Math.min(window.innerWidth - 56, rect.left + rect.width / 2)),
      top: rect.bottom + 8,
    });
  };
  const hideTooltip = () => setTooltip(null);
  const onKeyDown = (event) => {
    if (isShortcutComposing(event)) return;
    if (isDismissShortcut(event)) {
      event.preventDefault();
      event.stopPropagation();
      close();
      return;
    }
    if (event.key === 'Enter' && (event.ctrlKey || event.metaKey)) {
      event.preventDefault();
      sendAfterSaveRef.current = true;
      popupRef.current?.requestSubmit?.();
      return;
    }
    if (event.key === 'Enter') {
      const action = dialogEnterAction(event, popupRef.current);
      if (action.type === 'block') event.preventDefault();
      else if (action.type === 'activate') {
        event.preventDefault();
        action.element.click();
      }
      return;
    }
    if (event.key === 'Tab') {
      const target = resolveDialogTabTarget(popupRef.current, document.activeElement, event.shiftKey);
      if (!target) return;
      event.preventDefault();
      target.focus?.();
    }
  };
  const writeStyles = values => {
    for (const [field, value] of Object.entries(values)) dirtyStyles.current[field] = value;
    setStyleInputs(previous => ({ ...previous, ...values }));
    clearTimeout(styleTimer.current);
    if (Object.entries(values).every(([field, value]) => !value || CSS.supports(field, value))) {
      styleTimer.current = setTimeout(() => { void flushStyles(); }, 20);
    }
  };
  const writeStyle = (field, value) => writeStyles({ [field]: value });
  const styleKeyDown = event => {
    if (event.key === 'Enter' && !event.repeat && !isShortcutComposing(event)) {
      event.preventDefault();
      event.currentTarget.blur();
    }
  };
  const styleValue = field => styleInputs[field] ?? preview?.values?.[field] ?? '';
  const previousStyleValue = field => preview?.previous_values?.[field] ?? preview?.values?.[field] ?? '';
  const isStyleEdited = field => !cssValuesEqual(styleValue(field), previousStyleValue(field));
  const resetStyle = field => {
    writeStyles({ [field]: '' });
    setStyleInputs(previous => ({ ...previous, [field]: previousStyleValue(field) }));
  };
  const toggleStyleMenu = (field, label, options, button) => {
    if (selectMenu?.field === field) {
      setSelectMenu(null);
      return;
    }
    const rect = button?.getBoundingClientRect?.();
    if (!rect) return;
    const width = 220;
    setSelectMenu({
      field,
      label,
      options,
      left: Math.max(8, Math.min(rect.right - width, window.innerWidth - width - 8)),
      top: Math.max(8, Math.min(rect.bottom + 4, window.innerHeight - 228)),
      width,
    });
  };
  const styleControl = (field, label, kind, commit) => {
    const emit = commit || (kind === 'opacity'
      ? next => writeStyle(field, opacityCssFromPercent(next))
      : next => writeStyle(field, editorValueFor(field, kind, next)));
    if (kind === 'color') {
      return (
        <>
          <input
            type="color"
            aria-label={`${label}色板`}
            value={colorSwatchValue(styleValue(field))}
            onChange={event => emit(event.target.value)}
            className="ace-annotation-color-swatch"
          />
          <span className="ace-annotation-value-box">
            <input
              aria-label={label}
              dir="ltr"
              value={styleValue(field)}
              placeholder={previousStyleValue(field)}
              onChange={event => emit(event.target.value)}
              onBlur={() => { void flushStyles(); }}
              onKeyDown={styleKeyDown}
            />
          </span>
        </>
      );
    }
    if (kind === 'font' || kind === 'weight') {
      const options = kind === 'font'
        ? fontSuggestions(styleValue(field), previousStyleValue(field))
        : fontWeights.map(weight => [weight, weight]);
      return (
        <>
          <span className="ace-annotation-value-box has-select">
            <input
              aria-label={label}
              dir="ltr"
            aria-haspopup="listbox"
            aria-expanded={selectMenu?.field === field}
              value={styleValue(field)}
              placeholder={previousStyleValue(field)}
              onChange={event => emit(event.target.value)}
              onClick={event => toggleStyleMenu(field, label, options, event.currentTarget)}
              onBlur={() => { void flushStyles(); }}
              onKeyDown={styleKeyDown}
            />
            <button
              type="button"
              className="ace-annotation-select-trigger"
              aria-label={`${label}选项`}
              title={`${label}选项`}
              aria-haspopup="listbox"
              aria-expanded={selectMenu?.field === field}
              onClick={event => toggleStyleMenu(field, label, options, event.currentTarget)}
            >
              <SelectChevron />
            </button>
          </span>
        </>
      );
    }
    const numeric = kind === 'px' || kind === 'opacity';
    const unit = kind === 'opacity' ? '%' : 'px';
    return (
      <span className="ace-annotation-value-box">
        <input
          aria-label={label}
          dir="ltr"
          type={numeric ? 'number' : 'text'}
          step={numeric ? (kind === 'opacity' ? 1 : 'any') : undefined}
          min={kind === 'opacity' ? 0 : undefined}
          max={kind === 'opacity' ? 100 : undefined}
          inputMode={numeric ? 'decimal' : undefined}
          value={controlValueFor(field, kind, styleValue(field))}
          placeholder={controlPlaceholderFor(field, kind, previousStyleValue(field))}
          onChange={event => emit(event.target.value)}
          onBlur={() => { void flushStyles(); }}
          onKeyDown={styleKeyDown}
        />
        {numeric && <span className="ace-annotation-value-unit">{unit}</span>}
      </span>
    );
  };
  const styleValueCell = (field, label, kind, className = '') => (
    <span className={`ace-annotation-value-cell${kind === 'color' ? ' has-swatch' : ''}${isStyleEdited(field) ? ' has-reset' : ''}${className ? ` ${className}` : ''}`}>
      <button
        type="button"
        className="ace-annotation-reset"
        aria-label={`恢复${label}`}
        title={`恢复${label}`}
        tabIndex={isStyleEdited(field) ? 0 : -1}
        onClick={() => resetStyle(field)}
      >
        <ResetIcon />
      </button>
      {styleControl(field, label, kind)}
    </span>
  );
  const compactValueCell = (field, label) => (
    <span className={`ace-annotation-value-cell is-compact${isStyleEdited(field) ? ' has-reset' : ''}`}>
      <button
        type="button"
        className="ace-annotation-reset"
        aria-label={`恢复${label}`}
        title={`恢复${label}`}
        tabIndex={isStyleEdited(field) ? 0 : -1}
        onClick={() => resetStyle(field)}
      >
        <ResetIcon />
      </button>
      <span className="ace-annotation-value-box is-compact">
        <input
          aria-label={label}
          type="number"
          step="any"
          value={pxNumberOf(styleValue(field))}
          placeholder={controlPlaceholderFor(field, 'px', previousStyleValue(field))}
          onChange={event => writeStyle(field, editorValueFor(field, 'px', event.target.value))}
          onBlur={() => { void flushStyles(); }}
          onKeyDown={styleKeyDown}
        />
        <span className="ace-annotation-value-unit is-compact">px</span>
      </span>
    </span>
  );
  const lockPopupPosition = () => {
    const popup = popupRef.current;
    const viewport = viewportRef.current;
    if (!popup || !viewport) return;
    const popupBounds = popup.getBoundingClientRect();
    const viewportBounds = viewport.getBoundingClientRect();
    // Anchor the expanded card to the collapsed card's top-left corner and
    // shrink its ceiling to the room actually available. Reserving a fixed
    // 348x460 box made viewport clamping pull the card around on expand.
    const minWidth = Math.min(300, Math.round(viewportBounds.width - 16));
    const minHeight = Math.min(260, Math.round(viewportBounds.height - 16));
    const startLeft = Math.round(Math.max(8, Math.min(
      popupBounds.left - viewportBounds.left,
      Math.max(8, viewportBounds.width - minWidth - 8),
    )));
    const startTop = Math.round(Math.max(8, Math.min(
      popupBounds.top - viewportBounds.top,
      Math.max(8, viewportBounds.height - minHeight - 8),
    )));
    manualPositionRef.current = { startLeft, startTop };
    setAdjustBounds({
      width: Math.max(minWidth, Math.min(380, Math.round(viewportBounds.width - startLeft - 8))),
      height: Math.max(minHeight, Math.min(460, Math.round(viewportBounds.height - startTop - 8))),
    });
  };
  const spacingValue = field => styleInputs[field] ?? preview?.values?.[field] ?? '';
  const writeSpacing = (field, axis, first, second, value) => {
    if (spacingLocks[`${field}-${axis}`]) writeStyles({ [first]: value, [second]: value });
    else writeStyle(first, value);
  };
  const toggleSpacingLock = (field, axis, first, second) => {
    const key = `${field}-${axis}`;
    const next = !spacingLocks[key];
    setSpacingLocks(current => ({ ...current, [key]: next }));
    if (next) {
      const value = spacingValue(first);
      writeStyles({ [first]: value, [second]: value });
    }
  };
  const lockablePair = ({ field, axis, first, second, firstLabel, secondLabel, firstAria, secondAria }) => {
    const lockKey = `${field}-${axis}`;
    const locked = !!spacingLocks[lockKey];
    const kindLabel = field === 'padding' ? '内边距' : field === 'margin' ? '外边距' : '宽高';
    const axisLabel = axis === 'vertical' ? '垂直值' : '水平值';
    return (
      <div key={lockKey} className="ace-annotation-pair">
        <div className="ace-annotation-row">
          <span>{firstLabel}</span>
          <span className={`ace-annotation-value-cell${isStyleEdited(first) ? ' has-reset' : ''}`}>
            <button type="button" className="ace-annotation-reset" aria-label={`恢复${firstAria}`} title={`恢复${firstAria}`}
              tabIndex={isStyleEdited(first) ? 0 : -1}
              onClick={() => {
                if (spacingLocks[lockKey]) writeStyles({ [first]: '', [second]: '' });
                else resetStyle(first);
              }}>
              <ResetIcon />
            </button>
            {styleControl(first, firstAria, 'px',
              next => writeSpacing(field, axis, first, second, editorValueFor(first, 'px', next)))}
          </span>
        </div>
        <div className="ace-annotation-row">
          <span>{secondLabel}</span>
          <span className={`ace-annotation-value-cell${isStyleEdited(second) ? ' has-reset' : ''}`}>
            <button type="button" className="ace-annotation-reset" aria-label={`恢复${secondAria}`} title={`恢复${secondAria}`}
              tabIndex={isStyleEdited(second) ? 0 : -1}
              onClick={() => {
                if (spacingLocks[lockKey]) writeStyles({ [first]: '', [second]: '' });
                else resetStyle(second);
              }}>
              <ResetIcon />
            </button>
            {styleControl(second, secondAria, 'px',
              next => writeSpacing(field, axis, second, first, editorValueFor(second, 'px', next)))}
          </span>
        </div>
        <span className="ace-annotation-pair-lock">
          <svg className="ace-annotation-pair-bracket" viewBox="0 0 26 62" width="26" height="62" aria-hidden="true">
            <path d="M4 3H23V21M23 41V59H4" fill="none" stroke="currentColor" strokeWidth="1.6"
              strokeLinecap="round" strokeLinejoin="round" />
          </svg>
          <button
            type="button"
            className="ace-annotation-pair-lock-button"
            aria-label={`${locked ? '解锁' : '锁定'}${kindLabel}${axisLabel}`}
            title={locked ? `解锁${kindLabel}${axisLabel}` : `锁定${kindLabel}${axisLabel}`}
            aria-pressed={locked}
            onClick={() => toggleSpacingLock(field, axis, first, second)}
          >
            <LinkIcon locked={locked} />
          </button>
        </span>
      </div>
    );
  };
  const startPopupDrag = event => {
    const popup = popupRef.current;
    const viewport = viewportRef.current;
    if (working || event.button !== 0 || !popup || !viewport) return;
    const popupBounds = popup.getBoundingClientRect();
    const viewportBounds = viewport.getBoundingClientRect();
    dragRef.current = {
      pointerId: event.pointerId,
      startX: event.clientX,
      startY: event.clientY,
      left: popupBounds.left - viewportBounds.left,
      top: popupBounds.top - viewportBounds.top,
    };
    event.currentTarget.setPointerCapture?.(event.pointerId);
    event.preventDefault();
    event.stopPropagation();
  };
  const movePopup = event => {
    const drag = dragRef.current;
    const popup = popupRef.current;
    const viewport = viewportRef.current;
    if (!drag || drag.pointerId !== event.pointerId || !popup || !viewport) return;
    const bounds = viewport.getBoundingClientRect();
    const popupBounds = popup.getBoundingClientRect();
    const raw = pageAnnotationPopupDragPosition({
      startLeft: drag.left,
      startTop: drag.top,
      deltaX: event.clientX - drag.startX,
      deltaY: event.clientY - drag.startY,
      width: bounds.width,
      height: bounds.height,
      popupWidth: popupBounds.width,
      popupHeight: popupBounds.height,
    });
    const next = { left: Math.round(raw.left), top: Math.round(raw.top) };
    manualPositionRef.current = next;
    popup.style.left = `${next.left}px`;
    popup.style.top = `${next.top}px`;
    notifyNativeSurfaceOverlayChange();
    event.preventDefault();
  };
  const endPopupDrag = event => {
    if (dragRef.current?.pointerId !== event.pointerId) return;
    dragRef.current = null;
    if (event.currentTarget.hasPointerCapture?.(event.pointerId)) {
      event.currentTarget.releasePointerCapture(event.pointerId);
    }
  };
  const hasDraftNow = () => description.trim().length > 0 ||
    (snapshot.differences?.length || 0) > 0 ||
    Object.keys(dirtyStyles.current).length > 0;
  const hasDraft = hasDraftNow();
  const pageBackground = typeof snapshot.page_background === 'string' &&
    snapshot.page_background.trim() &&
    CSS.supports('color', snapshot.page_background)
    ? snapshot.page_background
    : undefined;
  return (
    <form
      ref={popupRef}
      role="dialog"
      tabIndex={-1}
      aria-label="网页批注"
      data-ace-native-overlay={snapshot.selection_mode ? undefined : 'overlap'}
      data-ace-native-overlay-square={snapshot.selection_mode ? undefined : 'true'}
      style={{
        ...(snapshot.selection_mode ? { display: 'none' } : {}),
        ...(pageBackground ? { '--ace-annotation-page-background': pageBackground } : {}),
        ...(adjustBounds ? {
          '--ace-annotation-adjust-width': `${adjustBounds.width}px`,
          '--ace-annotation-adjust-height': `${adjustBounds.height}px`,
        } : {}),
      }}
      className={adjusting ? 'ace-annotation-card is-adjusting' : 'ace-annotation-card'}
      onKeyDown={onKeyDown}
      onSubmit={async (event) => {
        event.preventDefault();
        if (working || saving || snapshot.original || !hasDraftNow()) return;
        setSaving(true);
        try {
          if ((await flushStyles())?.ok === true && liveRef.current) {
            const result = await command('save', { description });
            if (result?.ok === true && sendAfterSaveRef.current) {
              sendAfterSaveRef.current = false;
              onAnnotationsSaved?.(result.annotations);
              void requestAnnotationSubmit(sessionId)
                .then((sent) => {
                  if (sent?.ok === false) {
                    toast({ kind: 'err', text: localizedAnnotationSendError(sent.error) });
                  }
                })
                .catch((error) => {
                  toast({
                    kind: 'err',
                    text: localizedAnnotationSendError(error?.message || error),
                  });
                });
            } else if (result?.ok !== true) {
              sendAfterSaveRef.current = false;
            }
          } else {
            sendAfterSaveRef.current = false;
          }
        } finally { if (liveRef.current) setSaving(false); }
      }}
    >
      <div className="ace-annotation-card-header">
        <button
          type="button"
          className="ace-annotation-adjust"
          aria-label="调整"
          aria-expanded={adjusting}
          onPointerEnter={event => showTooltip(event, '调整')}
          onPointerLeave={hideTooltip}
          onFocus={event => showTooltip(event, '调整')}
          onBlur={hideTooltip}
          onClick={() => {
          if (!adjusting) lockPopupPosition();
            setAdjusting(value => !value);
          }}
        >
          <svg viewBox="0 0 16 16" width="14" height="14" aria-hidden="true">
            <path d="M2 4.5h12M2 8h12M2 11.5h12" fill="none" stroke="currentColor" strokeWidth="1.4" strokeLinecap="round" />
            <circle cx="5.5" cy="4.5" r="1.5" fill="currentColor" />
            <circle cx="10.5" cy="8" r="1.5" fill="currentColor" />
            <circle cx="7" cy="11.5" r="1.5" fill="currentColor" />
          </svg>
        </button>
        <textarea
          ref={inputRef}
          aria-label="批注描述"
          placeholder={adjusting ? '描述这些更改...' : '添加评论...'}
          maxLength={4096}
          rows={1}
          value={description}
          disabled={working}
          onChange={(event) => setDescription(event.target.value)}
          onKeyDown={(event) => {
            // Q36：描述框 Enter 添加，Shift+Enter 才换行；Ctrl/Cmd+Enter 由
            // 卡片级处理器先接管为「保存并发送」。
            if (event.key !== 'Enter' || event.shiftKey || event.ctrlKey || event.metaKey ||
                event.altKey || event.repeat || isShortcutComposing(event)) return;
            event.preventDefault();
            if (hasDraftNow() && !working && !saving && !snapshot.original) popupRef.current?.requestSubmit?.();
          }}
          className="ace-annotation-comment"
        />
      </div>
      {adjusting && (
        <>
          <div className="ace-annotation-design-handle">
            <div className="ace-annotation-target-tag" title="目标标签">
              {snapshot.target?.tag || '区域'}
            </div>
            <button
              type="button"
              className="ace-annotation-design-grip"
              title="拖动批注框"
              aria-label="拖动批注框"
              onPointerDown={startPopupDrag}
              onPointerMove={movePopup}
              onPointerUp={endPopupDrag}
              onPointerCancel={endPopupDrag}
            >
              <svg viewBox="0 0 16 16" width="16" height="16" aria-hidden="true">
                {[4.5, 8, 11.5].flatMap(y => [5.25, 10.75].map(x => (
                  <circle key={`${x}-${y}`} cx={x} cy={y} r="1.15" fill="currentColor" />
                )))}
              </svg>
            </button>
          </div>
          <div className="ace-annotation-editor">
            {preview && (
            <fieldset disabled={saving || snapshot.original}>
              <div className="ace-annotation-design">
                {styleRows.map(([field, label, kind]) => (
                  <div key={field} className="ace-annotation-row">
                    <span>{label}</span>
                    {styleValueCell(field, label, kind)}
                  </div>
                ))}
                <div className="ace-annotation-dimensions">
                  <div className="ace-annotation-row">
                    <span>宽度</span>
                    {styleValueCell('width', '宽度', 'px', 'is-width')}
                  </div>
                  <div className="ace-annotation-row">
                    <span>高度</span>
                    {styleValueCell('height', '高度', 'px', 'is-height')}
                  </div>
                  <span className="ace-annotation-pair-lock">
                    <svg className="ace-annotation-pair-bracket" viewBox="0 0 26 62" width="26" height="62"
                      aria-hidden="true">
                      <path d="M4 3H23V21M23 41V59H4" fill="none" stroke="currentColor" strokeWidth="1.6"
                        strokeLinecap="round" strokeLinejoin="round" />
                    </svg>
                    <button
                      type="button"
                      className="ace-annotation-pair-lock-button"
                      title={preview.ratio_locked ? '解锁宽高比例' : '锁定宽高比例'}
                      aria-label={preview.ratio_locked ? '解锁宽高比例' : '锁定宽高比例'}
                      aria-pressed={!!preview.ratio_locked}
                      disabled={working}
                      onClick={async () => {
                        const ratio_lock = !preview.ratio_locked;
                        if ((await flushStyles())?.ok === true && liveRef.current) {
                          await command('preview_style', { styles: {}, ratio_lock });
                        }
                      }}
                    >
                      <LinkIcon locked={!!preview.ratio_locked} />
                    </button>
                  </span>
                </div>
                <div className="ace-annotation-row">
                  <span>间隙</span>
                  <span className="ace-annotation-value-pair">
                    {styleValueCell('row-gap', '行间距', 'px')}
                    {styleValueCell('column-gap', '列间距', 'px')}
                  </span>
                </div>
                {spacingRows.map(([field, label, sides, pairs]) => (
                  <div key={field} className="ace-annotation-spacing">
                    <div className="ace-annotation-row">
                      <button
                        type="button"
                        className="ace-annotation-expand"
                        aria-label={openSpacing[field] ? `收起${label}` : `展开${label}`}
                        aria-expanded={!!openSpacing[field]}
                        onClick={() => setOpenSpacing(current => ({ ...current, [field]: !current[field] }))}
                      >
                        <span>{label}</span>
                        <ChevronIcon open={!!openSpacing[field]} />
                      </button>
                      {!openSpacing[field] && (
                        <span className="ace-annotation-spacing-values">
                          {sides.map(([side, sideLabel]) => compactValueCell(side, sideLabel))}
                        </span>
                      )}
                    </div>
                    {openSpacing[field] && pairs.map(([axis, first, firstLabel, second, secondLabel]) => lockablePair({
                      field,
                      axis,
                      first,
                      second,
                      firstLabel,
                      secondLabel,
                      firstAria: sides.find(([side]) => side === first)[1],
                      secondAria: sides.find(([side]) => side === second)[1],
                    }))}
                  </div>
                ))}
              </div>
            </fieldset>
            )}
            <PageAnnotationTextField target={snapshot.target} command={command} working={working || saving || snapshot.original} />
          </div>
        </>
      )}
      <div className="ace-annotation-actions">
        {adjusting && <button type="button" className="ace-annotation-cancel" disabled={working} onClick={close}>取消</button>}
        <button type="submit" aria-label="保存批注" data-ace-dialog-primary="true"
          onPointerEnter={event => showTooltip(event, '添加批注', 'Enter 添加 · Ctrl+Enter 发送')}
          onPointerLeave={hideTooltip}
          onFocus={event => showTooltip(event, '添加批注', 'Enter 添加 · Ctrl+Enter 发送')}
          onBlur={hideTooltip}
          disabled={working || saving || snapshot.original || !hasDraft} className="ace-annotation-submit">
          <svg viewBox="0 0 16 16" width="16" height="16" aria-hidden="true">
            <path d="M8 12.5V3.5M4.5 7 8 3.5 11.5 7" fill="none" stroke="currentColor" strokeWidth="1.6" strokeLinecap="round" strokeLinejoin="round" />
          </svg>
        </button>
      </div>
      {selectMenu && createPortal(
        <div
          ref={selectMenuRef}
          role="listbox"
          aria-label={`${selectMenu.label}选项`}
          data-ace-native-overlay="overlap"
          data-ace-native-overlay-square="true"
          className="ace-annotation-select-menu"
          style={{ left: `${selectMenu.left}px`, top: `${selectMenu.top}px`, width: `${selectMenu.width}px` }}
        >
          {selectMenu.options.map(([optionLabel, optionValue]) => (
            <button
              key={`${optionLabel}-${optionValue}`}
              type="button"
              role="option"
              aria-selected={optionValue === styleValue(selectMenu.field)}
              className="ace-annotation-select-option"
              onClick={() => {
                writeStyle(selectMenu.field, optionValue);
                setSelectMenu(null);
              }}
            >
              <span>{optionLabel}</span>
              {optionValue === styleValue(selectMenu.field) && <CheckIcon />}
            </button>
          ))}
        </div>,
        document.body,
      )}
      {tooltip && createPortal(
        <div
          role="tooltip"
          data-ace-native-overlay="overlap"
          data-ace-native-overlay-square="true"
          className="ace-annotation-tooltip"
          style={{ left: `${tooltip.left}px`, top: `${tooltip.top}px` }}
        >
          <span>{tooltip.label}</span>
          {tooltip.shortcut ? <kbd>{tooltip.shortcut}</kbd> : null}
        </div>,
        document.body,
      )}
    </form>
  );
}

function ResetIcon() {
  return (
    <svg viewBox="0 0 16 16" width="13" height="13" aria-hidden="true">
      <path d="M3.2 8a4.8 4.8 0 1 0 1.5-3.5M3.1 2.9v2.9h2.9" fill="none" stroke="currentColor"
        strokeWidth="1.4" strokeLinecap="round" strokeLinejoin="round" />
    </svg>
  );
}

function SelectChevron() {
  return (
    <svg viewBox="0 0 16 16" width="12" height="12" aria-hidden="true">
      <path d="M4.5 6.2 8 9.7l3.5-3.5" fill="none" stroke="currentColor" strokeWidth="1.4"
        strokeLinecap="round" strokeLinejoin="round" />
    </svg>
  );
}

function CheckIcon() {
  return (
    <svg viewBox="0 0 16 16" width="13" height="13" aria-hidden="true">
      <path d="M3.4 8.6 6.3 11.5l6.3-7" fill="none" stroke="currentColor" strokeWidth="1.5"
        strokeLinecap="round" strokeLinejoin="round" />
    </svg>
  );
}

function LinkIcon({ locked }) {
  const common = {
    viewBox: '0 0 24 24', width: 12, height: 12, 'aria-hidden': true,
  };
  if (locked) {
    return (
      <svg {...common}>
        <path d="M9 17H7A5 5 0 0 1 5.9 7" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round" />
        <path d="M15 7h2a5 5 0 0 1 4.1 7.9" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round" />
        <path d="M8 12h8" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round" />
        <path d="M3 3l18 18" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round" />
      </svg>
    );
  }
  return (
    <svg {...common}>
      <path d="M9 17H7A5 5 0 0 1 7 7h2" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round" strokeLinejoin="round" />
      <path d="M15 7h2a5 5 0 1 1 0 10h-2" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round" strokeLinejoin="round" />
      <path d="M8 12h8" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round" />
    </svg>
  );
}

function ChevronIcon({ open }) {
  return (
    <svg className={open ? 'is-open' : undefined} viewBox="0 0 16 16" width="12" height="12" aria-hidden="true">
      <path d="M6 4.2 9.8 8 6 11.8" fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round" />
    </svg>
  );
}
