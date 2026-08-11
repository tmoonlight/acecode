import {
  useCallback,
  useEffect,
  useLayoutEffect,
  useMemo,
  useRef,
  useState,
} from 'react';
import { useTranslation } from 'react-i18next';
import {
  computeAnchoredDropdownLayout,
  DROPDOWN_GAP_PX,
} from '../lib/dropdownPlacement.js';
import { clsx } from '../lib/format.js';
import {
  PATH_REFERENCE_KEY_ACTION,
  pathReferenceKeyboardAction,
} from '../lib/pathReference.js';
import { FileTypeIcon, VsIcon } from './Icon.jsx';

const VISIBLE_ROWS = 9;
const ROW_HEIGHT = 36;
const MAX_LIST_HEIGHT = VISIBLE_ROWS * ROW_HEIGHT;

function sameLayout(left, right) {
  return left.placement === right.placement
    && left.constrained === right.constrained
    && Math.abs((left.maxHeight ?? 0) - (right.maxHeight ?? 0)) < 0.5;
}

export function PathReferenceDropdown({
  items = [],
  fileItems = null,
  sessionItems = [],
  loading = false,
  fileLoading = null,
  sessionLoading = false,
  error = '',
  fileError = null,
  sessionError = '',
  onReference,
  onReferenceSession,
  onEnterDirectory,
  onClose,
}) {
  const { t } = useTranslation();
  const [selected, setSelected] = useState(0);
  const [layout, setLayout] = useState({ placement: 'above', maxHeight: null });
  const popupRef = useRef(null);
  const listRef = useRef(null);
  const rowRefs = useRef(new Map());
  const measuredPlacementRef = useRef(null);
  const measureFrameRef = useRef(null);
  const files = Array.isArray(fileItems) ? fileItems : items;
  const sessions = Array.isArray(sessionItems) ? sessionItems : [];
  const filesLoading = fileLoading == null ? loading : !!fileLoading;
  const filesError = fileError == null ? error : fileError;
  const showFileGroup = filesLoading || !!filesError || files.length > 0;
  const showSessionGroup = sessionLoading || !!sessionError || sessions.length > 0;
  const showEmptyReferenceState = !showFileGroup && !showSessionGroup;
  const selectableItems = useMemo(() => [
    ...files.map((item) => ({ type: 'file', item })),
    ...sessions.map((item) => ({ type: 'session', item })),
  ], [files, sessions]);

  useEffect(() => { setSelected(0); }, [files, sessions]);
  useEffect(() => {
    rowRefs.current.get(selected)?.scrollIntoView?.({ block: 'nearest' });
  }, [selected]);

  const onKey = useCallback((event) => {
    if (event.key === 'Escape') {
      event.preventDefault();
      event.stopPropagation();
      onClose?.();
      return;
    }
    const selection = selectableItems[selected];
    const item = selection?.item;
    const action = pathReferenceKeyboardAction(event.key, selection);
    if (action) {
      event.preventDefault();
      event.stopPropagation();
      if (action === PATH_REFERENCE_KEY_ACTION.ENTER_DIRECTORY) {
        onEnterDirectory?.(item);
      } else if (action === PATH_REFERENCE_KEY_ACTION.REFERENCE && item) {
        if (selection.type === 'session') onReferenceSession?.(item);
        else onReference?.(item);
      }
      return;
    }
    if (selectableItems.length === 0) return;
    let next = selected;
    if (event.key === 'ArrowDown') next = Math.min(selectableItems.length - 1, selected + 1);
    else if (event.key === 'ArrowUp') next = Math.max(0, selected - 1);
    else if (event.key === 'PageDown') next = Math.min(selectableItems.length - 1, selected + VISIBLE_ROWS);
    else if (event.key === 'PageUp') next = Math.max(0, selected - VISIBLE_ROWS);
    else if (event.key === 'Home') next = 0;
    else if (event.key === 'End') next = selectableItems.length - 1;
    else return;
    event.preventDefault();
    event.stopPropagation();
    setSelected(next);
  }, [onClose, onEnterDirectory, onReference, onReferenceSession, selectableItems, selected]);

  useEffect(() => {
    window.addEventListener('keydown', onKey, true);
    return () => window.removeEventListener('keydown', onKey, true);
  }, [onKey]);

  const measureLayout = useCallback(() => {
    const popup = popupRef.current;
    const list = listRef.current;
    const anchor = popup?.parentElement;
    if (!popup || !list || !anchor) return;

    const anchorRect = anchor.getBoundingClientRect();
    const visualViewport = window.visualViewport;
    const viewportTop = Number.isFinite(visualViewport?.offsetTop)
      ? visualViewport.offsetTop
      : 0;
    const viewportHeight = Number.isFinite(visualViewport?.height)
      ? visualViewport.height
      : window.innerHeight;
    const footer = popup.querySelector('[data-path-reference-footer]');
    const borderHeight = Math.max(0, popup.offsetHeight - popup.clientHeight);
    const preferredHeight = borderHeight
      + Math.min(list.scrollHeight, MAX_LIST_HEIGHT)
      + (footer?.offsetHeight || 0);
    const next = computeAnchoredDropdownLayout({
      anchorTop: anchorRect.top,
      anchorBottom: anchorRect.bottom,
      viewportTop,
      viewportHeight,
      preferredHeight,
      previousPlacement: measuredPlacementRef.current,
    });

    measuredPlacementRef.current = next.placement;
    setLayout((previous) => (sameLayout(previous, next) ? previous : next));
  }, [
    files.length,
    filesError,
    filesLoading,
    sessionError,
    sessionLoading,
    sessions.length,
  ]);

  const scheduleMeasureLayout = useCallback(() => {
    if (measureFrameRef.current != null) return;
    measureFrameRef.current = window.requestAnimationFrame(() => {
      measureFrameRef.current = null;
      measureLayout();
    });
  }, [measureLayout]);

  useLayoutEffect(() => {
    measureLayout();

    const visualViewport = window.visualViewport;
    const handleCapturedScroll = (event) => {
      if (event.target === listRef.current) return;
      scheduleMeasureLayout();
    };
    window.addEventListener('resize', scheduleMeasureLayout);
    window.addEventListener('scroll', handleCapturedScroll, true);
    visualViewport?.addEventListener?.('resize', scheduleMeasureLayout);
    visualViewport?.addEventListener?.('scroll', scheduleMeasureLayout);

    let resizeObserver = null;
    if (typeof ResizeObserver !== 'undefined') {
      resizeObserver = new ResizeObserver(scheduleMeasureLayout);
      const anchor = popupRef.current?.parentElement;
      if (anchor) resizeObserver.observe(anchor);
    }

    return () => {
      window.removeEventListener('resize', scheduleMeasureLayout);
      window.removeEventListener('scroll', handleCapturedScroll, true);
      visualViewport?.removeEventListener?.('resize', scheduleMeasureLayout);
      visualViewport?.removeEventListener?.('scroll', scheduleMeasureLayout);
      resizeObserver?.disconnect();
      if (measureFrameRef.current != null) {
        window.cancelAnimationFrame(measureFrameRef.current);
        measureFrameRef.current = null;
      }
    };
  }, [measureLayout, scheduleMeasureLayout]);

  const opensBelow = layout.placement === 'below';

  return (
    <div
      ref={popupRef}
      role="listbox"
      data-ace-native-overlay="overlap"
      data-placement={layout.placement}
      data-constrained={layout.constrained ? 'true' : 'false'}
      aria-label={t('pathReference.ariaLabel')}
      className="absolute left-0 w-[400px] max-w-full flex flex-col bg-surface border border-border rounded-lg ace-shadow-lg overflow-hidden font-sans"
      style={{
        zIndex: 62,
        top: opensBelow ? `calc(100% + ${DROPDOWN_GAP_PX}px)` : 'auto',
        bottom: opensBelow ? 'auto' : `calc(100% + ${DROPDOWN_GAP_PX}px)`,
        maxHeight: layout.maxHeight == null ? undefined : `${layout.maxHeight}px`,
      }}
      onMouseDown={(event) => event.preventDefault()}
    >
      <div
        ref={listRef}
        className="min-h-0 flex-1 overflow-y-auto"
        style={{ maxHeight: MAX_LIST_HEIGHT }}
      >
        {showFileGroup && (
          <div role="group" aria-label={t('pathReference.files')}>
            <div className="border-y border-border bg-surface-alt px-3 py-1.5 text-[12px] font-semibold text-fg-2">
              {t('pathReference.files')}
            </div>
            {filesLoading && files.length === 0 ? (
              <div className="px-3 py-3 text-center text-fg-mute text-[12px]">
                {t('pathReference.loadingFiles')}
              </div>
            ) : filesError ? (
              <div className="px-3 py-3 text-center text-danger text-[12px]">{filesError}</div>
            ) : files.map((item, index) => {
              const isDirectory = item.kind === 'dir';
              const active = index === selected;
              return (
                <div
                  key={`${item.kind}:${item.path}`}
                  ref={(element) => {
                    if (element) rowRefs.current.set(index, element);
                    else rowRefs.current.delete(index);
                  }}
                  role="option"
                  aria-selected={active}
                  aria-label={`${isDirectory ? t('pathReference.folder') : t('pathReference.file')} ${item.path}`}
                  className={clsx(
                    'flex items-center gap-2 px-2 text-[13px]',
                    active ? 'bg-surface-hi text-fg' : 'text-fg hover:bg-surface-hi/60',
                  )}
                  style={{ height: ROW_HEIGHT }}
                  onMouseEnter={() => setSelected(index)}
                >
                  <button
                    type="button"
                    className="min-w-0 flex-1 h-full flex items-center gap-2 text-left"
                    onMouseDown={(event) => { event.preventDefault(); onReference?.(item); }}
                    title={isDirectory ? t('pathReference.referenceFolder') : t('pathReference.referenceFile')}
                  >
                    {isDirectory ? <VsIcon name="folder" size={14} /> : <FileTypeIcon path={item.path} size={14} />}
                    <span className="truncate">{item.path}{isDirectory ? '/' : ''}</span>
                  </button>
                  {isDirectory && (
                    <button
                      type="button"
                      className="shrink-0 h-6 px-2 rounded text-[11px] text-fg-mute hover:bg-bg hover:text-fg"
                      aria-label={t('pathReference.enterFolderLabel', { path: item.path })}
                      onMouseDown={(event) => { event.preventDefault(); event.stopPropagation(); onEnterDirectory?.(item); }}
                    >
                      {t('pathReference.enter')}
                    </button>
                  )}
                </div>
              );
            })}
          </div>
        )}

        {showSessionGroup && (
          <div role="group" aria-label={t('pathReference.sessions')}>
            <div className="border-y border-border bg-surface-alt px-3 py-1.5 text-[12px] font-semibold text-fg-2">
              {t('pathReference.sessions')}
            </div>
            {sessionLoading && sessions.length === 0 ? (
              <div className="px-3 py-3 text-center text-fg-mute text-[12px]">
                {t('pathReference.loadingSessions')}
              </div>
            ) : sessionError ? (
              <div className="px-3 py-3 text-center text-danger text-[12px]">{sessionError}</div>
            ) : sessions.map((item, index) => {
              const selectableIndex = files.length + index;
              const active = selectableIndex === selected;
              return (
                <div
                  key={`session:${item.workspace_hash || (item.no_workspace ? 'task' : '')}:${item.id}`}
                  ref={(element) => {
                    if (element) rowRefs.current.set(selectableIndex, element);
                    else rowRefs.current.delete(selectableIndex);
                  }}
                  role="option"
                  aria-selected={active}
                  aria-label={`${t('pathReference.session')} ${item.title}`}
                  className={clsx(
                    'flex items-center gap-2 px-2 text-[13px]',
                    active ? 'bg-surface-hi text-fg' : 'text-fg hover:bg-surface-hi/60',
                  )}
                  style={{ height: ROW_HEIGHT }}
                  onMouseEnter={() => setSelected(selectableIndex)}
                >
                  <button
                    type="button"
                    className="min-w-0 flex-1 h-full flex items-center gap-2 text-left"
                    onMouseDown={(event) => { event.preventDefault(); onReferenceSession?.(item); }}
                    title={item.title}
                  >
                    <VsIcon name="newSession" size={14} />
                    <span className="min-w-0 flex-1 flex items-baseline gap-2">
                      <span className="min-w-0 truncate">{item.title}</span>
                      <span className="max-w-[40%] shrink-0 truncate text-fg-mute">
                        {item.workspaceName}
                      </span>
                    </span>
                  </button>
                </div>
              );
            })}
          </div>
        )}

        {showEmptyReferenceState && (
          <div className="px-3 py-3 text-center text-fg-mute text-[12px]">
            {t('pathReference.noReferences')}
          </div>
        )}
      </div>
      <div
        data-path-reference-footer
        className="shrink-0 px-2 py-1 border-t border-border text-[10px] text-fg-mute bg-surface-alt"
      >
        {t('pathReference.hint')}
      </div>
    </div>
  );
}
