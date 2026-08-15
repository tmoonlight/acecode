// 输入框:富文本 composer 自动撑高(最多 8 行) + Enter 发 / Shift+Enter 换行 +
// 空输入或未编辑的历史项用上下键翻 history。
//
// 底部工具栏单独占一行,提交按钮在右侧(只在有内容时变蓝),空内容时灰色不可点。
//
// 斜杠命令:value 以 / 开头且无空白时,SlashDropdown 浮层显示在输入框上方。
// 选中后插入 `/<name> ` 到输入框,不立即发送(builtin 与 skill 行为统一)。
// 已识别的首段命令以原子 token 样式在同一 editable layout 内渲染。

import { forwardRef, useCallback, useEffect, useImperativeHandle, useLayoutEffect, useMemo, useRef, useState } from 'react';
import { createPortal } from 'react-dom';
import { useTranslation } from 'react-i18next';
import { clsx } from '../lib/format.js';
import { getGoalStopControlState } from '../lib/goalControl.js';
import { getInputBarActionState } from '../lib/inputBarState.js';
import { FileTypeIcon, VsIcon } from './Icon.jsx';
import { SelectionAnnotationBadge } from './SelectionAnnotationBadge.jsx';
import { ComposerSessionControls } from './ComposerSessionControls.jsx';
import { ExpertAvatar, compactExpertSummary } from './ExpertCatalog.jsx';
import { GoalStatusBar } from './GoalStatusBar.jsx';
import { ImageLightbox } from './ImageLightbox.jsx';
import { SwarmModeIcon } from './SwarmModeIcon.jsx';
import { RichComposer } from './RichComposer.jsx';
import { PathReferenceDropdown } from './PathReferenceDropdown.jsx';
import { SlashDropdown } from './SlashDropdown.jsx';
import { toast } from './Toast.jsx';
import { useSlashCommands } from './SlashCommandsContext.jsx';
import { getNextInputHistoryPointer, isUserComposerEdit, shouldNavigateInputHistory } from '../lib/inputHistoryNavigation.js';
import { filesFromTransfer, hasFileTransfer } from '../lib/composerFileTransfer.js';
import {
  captureComposerTextareaSelection,
  requestDesktopFileDragActivation,
  requestDesktopWindowFocus,
  restoreComposerTextareaCaret,
} from '../lib/composerCaretRestore.js';
import {
  DESKTOP_CONTEXT_ACTION_EVENT,
  DESKTOP_CONTEXT_ACTIONS,
} from '../lib/desktopContextMenu.js';
import {
  SELECTION_CONTEXT_TYPE,
  contextPresentation,
} from '../lib/selectionChatContext.js';
import {
  insertPathReferenceAtCaret,
  normalizePathReferenceCandidates,
  pathReferenceSignature,
  pathReferenceTokenAtCursor,
  replacePathReferenceToken,
  splitPathReferenceQuery,
  unsafeReferencePath,
} from '../lib/pathReference.js';
import {
  loadSessionReferenceData,
  rankSessionReferenceCandidates,
  replaceQueryWithSessionReference,
} from '../lib/sessionReference.js';
import {
  hasNativeContextPicker,
  nativeFolderReferencePath,
  nativePickedFileToFile,
  parseNativeContextPickerResult,
} from '../lib/desktopContextPicker.js';
import {
  desktopHostOs,
  hasNativeFilesystemClipboard,
  hasNativeFilesystemMaterializer,
  insertAbsoluteFolderReferences,
  localPathsFromDropPayload,
  localPathsFromUriList,
  materializeNativeFilesystemPaths,
  nativeFileDropEnabled,
  readNativeClipboardFilesystemItems,
  uriListFromTransfer,
} from '../lib/desktopFilesystemTransfer.js';
import { postWindowsNativeFilesystemDrop } from '../lib/desktopNativeFilesystemDrop.js';
import {
  nextExpertMenuItemIndex,
  placeExpertSubmenu,
} from '../lib/expertMenuPosition.js';

const MAX_ROWS = 8;
const LINE_HEIGHT = 20; // 与 leading-[20px] 对齐
const HOST_OS = desktopHostOs();
const NATIVE_FILE_DROP = nativeFileDropEnabled();

function composerAttachmentKey(item, index = 0) {
  return String(item?.local_id || item?.id || item?.name || index);
}

function composerAttachmentContext(item, index = 0) {
  const key = composerAttachmentKey(item, index);
  return {
    key,
    id: `composer:${key}`,
    name: item?.name || 'attachment',
    url: item?.preview_url || item?.blob_url || item?.url || '',
    path: item?.path || '',
    sourcePath: item?.source_path || item?.metadata?.source_path || '',
  };
}

function composerContextKey(item, index = 0) {
  return String(item?.local_id || item?.id || item?.type || index);
}

function ComposerSelectionCard({
  item,
  annotationPresentations = null,
  pinned = false,
  onPin,
  onRemove,
}) {
  const presentation = contextPresentation(item, annotationPresentations);
  const sourcePath = item?.source?.path || item?.path || presentation.label;
  const actionTitle = pinned ? '移除引用' : '固定引用';
  const actionLabel = pinned ? '移除引用上下文' : '固定引用上下文';
  return (
    <div
      className={[
        'group h-6 max-w-[260px] shrink-0 rounded-md border px-1.5 flex items-center gap-1 text-[11px] font-sans leading-none',
        pinned ? 'border-border bg-surface text-fg' : 'border-accent-soft bg-accent-bg text-fg',
      ].join(' ')}
      title={presentation.title}
    >
      <button
        type="button"
        className="w-[14px] h-[14px] shrink-0 rounded-full flex items-center justify-center hover:bg-surface-hi text-fg-mute hover:text-fg"
        onMouseDown={(event) => event.preventDefault()}
        onClick={() => {
          if (pinned) onRemove?.();
          else onPin?.();
        }}
        title={actionTitle}
        aria-label={actionLabel}
      >
        <VsIcon name={pinned ? 'close' : 'pin'} size={11} className="ace-selection-context-icon" />
      </button>
      <FileTypeIcon path={sourcePath} size={11} className="ace-selection-context-icon opacity-90" />
      <span className={['truncate text-fg', pinned ? '' : 'opacity-80'].filter(Boolean).join(' ')}>
        {presentation.label}
      </span>
      <SelectionAnnotationBadge
        number={presentation.annotationNumber}
        annotations={presentation.annotations}
        compact
      />
    </div>
  );
}

function ComposerBrowserContextCard({ item, onRemove }) {
  const presentation = contextPresentation(item);
  return (
    <div
      className="group h-6 max-w-[260px] shrink-0 rounded-md border border-border bg-surface px-1.5 flex items-center gap-1 text-[11px] font-sans leading-none text-fg"
      title={presentation.title}
    >
      <button
        type="button"
        className="w-[14px] h-[14px] shrink-0 rounded-full flex items-center justify-center hover:bg-surface-hi text-fg-mute hover:text-fg"
        onMouseDown={(event) => event.preventDefault()}
        onClick={onRemove}
        title="移除引用"
        aria-label={presentation.removeLabel}
      >
        <VsIcon name="close" size={11} className="ace-selection-context-icon" />
      </button>
      <VsIcon name={presentation.icon} size={11} className="ace-selection-context-icon" />
      <span className="truncate text-fg">{presentation.label}</span>
    </div>
  );
}

export const InputBar = forwardRef(function InputBar({
  disabled, placeholder = '输入消息或 / 命令…', onSubmit, onAbort, busy, goal = null,
  onGoalEdit, onGoalStatusChange, onGoalClear,
  history = [], variant = 'default',
  value: controlledValue, onChange,
  attachments = [], contexts = [], annotationPresentations = null,
  onMediaFiles, onRemoveAttachment, onRemoveContext,
  swarmMode = false, onSwarmModeChange,
  expertOptions = [],
  selectedExpertId = '',
  selectedExpertName = '',
  selectedExpertType = 'agent',
  expertRemoving = false,
  pendingExpertName = '',
  pendingExpertType = 'agent',
  onSelectExpert,
  onRemoveExpert,
  onOpenExpertComponents,
  selectionPreview = null, onPinSelectionPreview,
  pathReferenceApi = null, cwd = '', currentSessionId = '',
  fileDropManagedExternally = false, onFileDragActiveChange,
  sessionControls = null,
}, ref) {
  const { t } = useTranslation();
  const isControlled = controlledValue != null;
  const [internalValue, setInternalValue] = useState('');
  const value = isControlled ? String(controlledValue || '') : internalValue;
  const valueRef = useRef(value);
  valueRef.current = value;
  const [histPtr, setHistPtr] = useState(-1);
  const [editedSinceHistory, setEditedSinceHistory] = useState(false);
  const [dropdownClosed, setDropdownClosed] = useState(false); // Esc 关闭后,直到首段变化或重新输入 / 才重开
  const [capabilityOpen, setCapabilityOpen] = useState(false);
  const [expertSubmenuOpen, setExpertSubmenuOpen] = useState(false);
  const [expertSubmenuPosition, setExpertSubmenuPosition] = useState(null);
  const [composerSelection, setComposerSelection] = useState({ start: 0, end: 0, direction: 'none' });
  const [composerComposing, setComposerComposing] = useState(false);
  const [pathMention, setPathMention] = useState(null);
  const [dragActive, setDragActive] = useState(false);
  const [attachmentPreview, setAttachmentPreview] = useState(null);
  const ta = useRef(null);
  const rootRef = useRef(null);
  const fileInputRef = useRef(null);
  const dismissedPathSignatureRef = useRef('');
  const mentionGenerationRef = useRef(0);
  const capabilityMenuRef = useRef(null);
  const capabilityButtonRef = useRef(null);
  const expertMenuParentRef = useRef(null);
  const fileMenuItemRef = useRef(null);
  const expertSubmenuRef = useRef(null);
  const dragDepthRef = useRef(0);
  const dragActiveRef = useRef(false);
  const nativeDropHoverRef = useRef({ active: false, ts: 0 });
  const composingRef = useRef(false);
  const justFinishedCompositionRef = useRef(false);
  const compositionGuardTimerRef = useRef(0);
  const caretRestoreUntilRef = useRef(0);
  const caretRestoreSelectionRef = useRef(null);
  const caretRestoreScheduleRef = useRef({ firstRaf: 0, secondRaf: 0, timeout: 0 });
  const isHero = variant === 'hero';
  const textareaVerticalPadding = isHero ? 16 : 12;
  // 状态控制已经收进 composer，空输入区统一保留两行高度，整体比例与
  // WorkBuddy 式输入框一致；最大高度仍保持原有 8 行上限。
  const textareaBaseHeight = LINE_HEIGHT * 2 + textareaVerticalPadding;
  const textareaMaxHeight = LINE_HEIGHT * MAX_ROWS + textareaVerticalPadding;
  const attachmentItems = Array.isArray(attachments) ? attachments : [];
  const contextItems = Array.isArray(contexts) ? contexts : [];
  const recentExpertItems = Array.isArray(expertOptions) ? expertOptions.slice(0, 5) : [];
  const selectionContextItems = contextItems.filter((item) => item?.type === SELECTION_CONTEXT_TYPE);
  const browserContextItems = contextItems.filter((item) => item?.type === 'browser');
  const otherContextItems = contextItems.filter((item) => (
    item?.type !== SELECTION_CONTEXT_TYPE && item?.type !== 'browser'
  ));
  const hasExtras = attachmentItems.length > 0 || contextItems.length > 0;
  const nativeContextPickerAvailable = hasNativeContextPicker();
  const nativeFilesystemMaterializerAvailable = hasNativeFilesystemMaterializer();
  const nativeFilesystemClipboardAvailable = hasNativeFilesystemClipboard();
  const canChooseLocalContext = !!onMediaFiles || nativeContextPickerAvailable;
  const hasExpertHandlers = !!onSelectExpert || !!onOpenExpertComponents;
  const hasCapabilityHandlers = !!onSwarmModeChange || canChooseLocalContext || hasExpertHandlers;
  const composerLayoutSignature = useMemo(() => [
    ...attachmentItems.map((item, index) => [
      composerAttachmentKey(item, index),
      item?.id || '',
      item?.uploading ? 'uploading' : 'ready',
      item?.preview_url || '',
    ].join(':')),
    ...contextItems.map((item, index) => [
      composerContextKey(item, index),
      item?.type || '',
      item?.id || '',
    ].join(':')),
  ].join('\n'), [attachmentItems, contextItems]);

  useEffect(() => {
    const handler = (event) => {
      const detail = event.detail || {};
      const { action, target } = detail;
      if (target?.type !== 'attachment' || !target.id || !target.id.startsWith('composer:')) return;
      const match = attachmentItems
        .map((item, index) => ({ item, context: composerAttachmentContext(item, index) }))
        .find(({ context }) => context.id === target.id);
      if (!match) return;
      if (action === DESKTOP_CONTEXT_ACTIONS.PREVIEW_ATTACHMENT) {
        if (!match.context.url) return;
        detail.handled = true;
        setAttachmentPreview({ src: match.context.url, alt: match.context.name });
      } else if (action === DESKTOP_CONTEXT_ACTIONS.REMOVE_ATTACHMENT) {
        detail.handled = true;
        onRemoveAttachment?.(match.context.key);
      }
    };
    window.addEventListener(DESKTOP_CONTEXT_ACTION_EVENT, handler);
    return () => window.removeEventListener(DESKTOP_CONTEXT_ACTION_EVENT, handler);
  }, [attachmentItems, onRemoveAttachment]);

  const previewComposerAttachment = useCallback((item) => {
    const src = String(item?.url || item?.preview_url || item?.blob_url || '');
    if (!src) return;
    setAttachmentPreview({ src, alt: String(item?.name || 'attachment') });
  }, []);

  const updateValue = useCallback((next) => {
    const text = String(next || '');
    if (!isControlled) setInternalValue(text);
    onChange?.(text);
  }, [isControlled, onChange]);

  const slashCtx = useSlashCommands();
  const commands = slashCtx?.commands || [];

  useEffect(() => () => {
    if (compositionGuardTimerRef.current) {
      window.clearTimeout(compositionGuardTimerRef.current);
    }
  }, []);

  const clearCaretRestoreSchedule = useCallback(() => {
    const schedule = caretRestoreScheduleRef.current;
    if (schedule.firstRaf) window.cancelAnimationFrame(schedule.firstRaf);
    if (schedule.secondRaf) window.cancelAnimationFrame(schedule.secondRaf);
    if (schedule.timeout) window.clearTimeout(schedule.timeout);
    caretRestoreScheduleRef.current = { firstRaf: 0, secondRaf: 0, timeout: 0 };
  }, []);

  useEffect(() => () => {
    clearCaretRestoreSchedule();
  }, [clearCaretRestoreSchedule]);

  const restoreComposerCaretIfPending = useCallback(() => {
    const until = caretRestoreUntilRef.current;
    if (!until || Date.now() > until) return false;
    return restoreComposerTextareaCaret({
      textareaElement: ta.current,
      rootElement: rootRef.current,
      selection: caretRestoreSelectionRef.current,
      documentRef: typeof document === 'undefined' ? null : document,
    });
  }, []);

  const scheduleComposerCaretRestore = useCallback(() => {
    if (!caretRestoreUntilRef.current) return;
    clearCaretRestoreSchedule();
    caretRestoreScheduleRef.current.firstRaf = window.requestAnimationFrame(() => {
      caretRestoreScheduleRef.current.firstRaf = 0;
      restoreComposerCaretIfPending();
      caretRestoreScheduleRef.current.secondRaf = window.requestAnimationFrame(() => {
        caretRestoreScheduleRef.current.secondRaf = 0;
        restoreComposerCaretIfPending();
      });
      caretRestoreScheduleRef.current.timeout = window.setTimeout(() => {
        caretRestoreScheduleRef.current.timeout = 0;
        restoreComposerCaretIfPending();
      }, 80);
    });
  }, [clearCaretRestoreSchedule, restoreComposerCaretIfPending]);

  const requestComposerCaretRestore = useCallback(({ requestNativeFocus = true } = {}) => {
    caretRestoreUntilRef.current = Date.now() + 1500;
    caretRestoreSelectionRef.current = captureComposerTextareaSelection(ta.current);
    if (requestNativeFocus) requestDesktopWindowFocus();
    restoreComposerTextareaCaret({
      textareaElement: ta.current,
      rootElement: rootRef.current,
      selection: caretRestoreSelectionRef.current,
      documentRef: typeof document === 'undefined' ? null : document,
      allowExternalFocus: true,
    });
    scheduleComposerCaretRestore();
  }, [scheduleComposerCaretRestore]);

  useLayoutEffect(() => {
    if (!caretRestoreUntilRef.current) return;
    scheduleComposerCaretRestore();
  }, [composerLayoutSignature, scheduleComposerCaretRestore]);

  const clearCompositionEndGuard = () => {
    if (compositionGuardTimerRef.current) {
      window.clearTimeout(compositionGuardTimerRef.current);
      compositionGuardTimerRef.current = 0;
    }
  };

  const handleCompositionStart = () => {
    clearCompositionEndGuard();
    composingRef.current = true;
    justFinishedCompositionRef.current = false;
    setComposerComposing(true);
  };

  const handleCompositionEnd = () => {
    composingRef.current = false;
    setComposerComposing(false);
    justFinishedCompositionRef.current = true;
    clearCompositionEndGuard();
    compositionGuardTimerRef.current = window.setTimeout(() => {
      justFinishedCompositionRef.current = false;
      compositionGuardTimerRef.current = 0;
    }, 0);
  };

  const isComposingKeyEvent = (event) => (
    composingRef.current ||
    justFinishedCompositionRef.current ||
    !!event.isComposing ||
    !!event.nativeEvent?.isComposing ||
    event.keyCode === 229 ||
    event.which === 229
  );

  // 触发条件:value 非空、首字符 /、整段无空白
  const showDropdownRaw = value.length > 0 && value[0] === '/' && !/\s/.test(value);
  const showDropdown = showDropdownRaw && !dropdownClosed && commands.length > 0;

  // value 变化:首段不再是 / 时复位 dropdownClosed,允许下次重新出现
  useEffect(() => {
    if (!showDropdownRaw) setDropdownClosed(false);
  }, [showDropdownRaw]);

  // 斜杠菜单从关闭→打开时重新拉一次 /api/commands:后端按 session cwd 当场扫盘,
  // 这样会话进行中(比如 agent 用 skill-creator)新写到磁盘的 skill 不必刷新页面
  // 就能出现在下拉里。只在进入命令态的那一刻触发,不是每次按键。
  useEffect(() => {
    if (showDropdownRaw) slashCtx?.invalidate?.();
  }, [showDropdownRaw]);

  const handleSelectCommand = (item) => {
    if (!item) return;
    const next = '/' + item.name + ' ';
    updateValue(next);
    setEditedSinceHistory(true);
    setDropdownClosed(true);
    requestAnimationFrame(() => {
      const el = ta.current;
      if (el) {
        el.focus();
        el.setSelectionRange(next.length, next.length);
      }
    });
  };

  const submit = () => {
    const v = value.trim();
    if ((!v && !hasExtras) || disabled) return;
    onSubmit?.(value);
    if (!isControlled) updateValue('');
    setHistPtr(-1);
    setEditedSinceHistory(false);
    setDropdownClosed(false);
    mentionGenerationRef.current += 1;
    setPathMention(null);
    requestAnimationFrame(() => ta.current?.focus());
  };

  const restorePathCaret = useCallback((cursor) => {
    requestAnimationFrame(() => {
      const editor = ta.current;
      editor?.focus?.();
      editor?.setSelectionRange?.(cursor, cursor);
    });
  }, []);

  // `@` reference menu: files keep the existing visible-path behavior, while
  // sessions use a stable inline token that is expanded only when submitted.
  // Directory contents and session transcripts are never preloaded here.
  useEffect(() => {
    const cursor = composerSelection.end;
    const token = pathReferenceTokenAtCursor(value, cursor);
    const signature = pathReferenceSignature(token, cursor, cwd);
    const unavailable = disabled || !pathReferenceApi || composerComposing || showDropdown || !token;
    if (unavailable || dismissedPathSignatureRef.current === signature) {
      mentionGenerationRef.current += 1;
      setPathMention(null);
      return undefined;
    }
    dismissedPathSignatureRef.current = '';
    const { directory, filter } = splitPathReferenceQuery(token.path);
    const pathUnsafe = unsafeReferencePath(token.path);
    const canLoadFiles = !!cwd && !pathUnsafe && typeof pathReferenceApi.listFiles === 'function';
    const canLoadSessions = typeof pathReferenceApi.listAllWorkspaceSessions === 'function';
    const generation = ++mentionGenerationRef.current;
    setPathMention({
      token,
      signature,
      fileItems: [],
      sessionItems: [],
      fileLoading: canLoadFiles,
      sessionLoading: canLoadSessions,
      fileError: pathUnsafe ? t('pathReference.pathOutside') : '',
      sessionError: '',
    });

    const updateMention = (patch) => {
      if (mentionGenerationRef.current !== generation) return;
      setPathMention((current) => (
        current?.signature === signature ? { ...current, ...patch } : current
      ));
    };

    if (canLoadFiles) {
      Promise.resolve(pathReferenceApi.listFiles(cwd, directory, true, true))
        .then((entries) => {
          updateMention({
            fileItems: normalizePathReferenceCandidates(entries, filter),
            fileLoading: false,
            fileError: '',
          });
        })
        .catch((error) => {
          updateMention({
            fileItems: [],
            fileLoading: false,
            fileError: error?.message || t('pathReference.loadFilesFailed'),
          });
        });
    }

    let sessionTimer = 0;
    if (canLoadSessions) {
      const query = String(token.path || '').trim();
      sessionTimer = window.setTimeout(() => {
        const listPromise = loadSessionReferenceData(pathReferenceApi);
        const contentPromise = query && typeof pathReferenceApi.searchSessionUserMessages === 'function'
          ? Promise.resolve(pathReferenceApi.searchSessionUserMessages(query, 100))
          : Promise.resolve({ matches: [] });
        Promise.all([listPromise, contentPromise])
          .then(([data, content]) => {
            updateMention({
              sessionItems: rankSessionReferenceCandidates({
                sessions: data?.sessions || [],
                contentMatches: Array.isArray(content?.matches) ? content.matches : [],
                query,
                currentSessionId,
                noWorkspaceLabel: t('pathReference.task'),
              }),
              sessionLoading: false,
              sessionError: '',
            });
          })
          .catch((error) => {
            updateMention({
              sessionItems: [],
              sessionLoading: false,
              sessionError: error?.message || t('pathReference.loadSessionsFailed'),
            });
          });
      }, query ? 120 : 0);
    }

    return () => {
      if (sessionTimer) window.clearTimeout(sessionTimer);
      mentionGenerationRef.current += 1;
    };
  }, [
    composerComposing,
    composerSelection.end,
    currentSessionId,
    cwd,
    disabled,
    pathReferenceApi,
    showDropdown,
    t,
    value,
  ]);

  useEffect(() => {
    if (!disabled && pathReferenceApi) return;
    mentionGenerationRef.current += 1;
    setPathMention(null);
  }, [disabled, pathReferenceApi]);

  const closePathDropdown = useCallback(() => {
    if (pathMention?.signature) dismissedPathSignatureRef.current = pathMention.signature;
    mentionGenerationRef.current += 1;
    setPathMention(null);
  }, [pathMention?.signature]);

  const applyMentionItem = useCallback((item, enterDirectory = false) => {
    if (!pathMention?.token || !item) return;
    const replacement = replacePathReferenceToken(value, pathMention.token, item.path, {
      directory: item.kind === 'dir',
      enterDirectory,
    });
    mentionGenerationRef.current += 1;
    setPathMention(null);
    updateValue(replacement.text);
    setEditedSinceHistory(true);
    restorePathCaret(replacement.cursor);
  }, [pathMention?.token, restorePathCaret, updateValue, value]);

  const applySessionMentionItem = useCallback((item) => {
    if (!pathMention?.token || !item) return;
    const replacement = replaceQueryWithSessionReference(value, pathMention.token, item);
    mentionGenerationRef.current += 1;
    setPathMention(null);
    updateValue(replacement.text);
    setEditedSinceHistory(true);
    restorePathCaret(replacement.cursor);
  }, [pathMention?.token, restorePathCaret, updateValue, value]);

  const activePathDropdown = pathMention;

  const addMediaFiles = useCallback((files, { requestNativeFocus = true } = {}) => {
    const fileList = Array.from(files || []).filter(Boolean);
    if (disabled || !onMediaFiles || fileList.length === 0) return false;
    setCapabilityOpen(false);
    requestComposerCaretRestore({ requestNativeFocus });
    onMediaFiles(fileList);
    return true;
  }, [disabled, onMediaFiles, requestComposerCaretRestore]);

  const addNativeFilesystemItems = useCallback((
    items,
    savedCursor = composerSelection.end,
    { requestNativeFocus = true } = {},
  ) => {
    const list = Array.from(items || []);
    if (list.length === 0) return false;

    const folders = list.filter((item) => item?.kind === 'folder' && item.path);
    const files = list
      .filter((item) => item?.kind === 'file')
      .map((item) => nativePickedFileToFile(item));

    if (folders.length > 0) {
      const currentValue = valueRef.current;
      const insertion = insertAbsoluteFolderReferences(currentValue, savedCursor, folders);
      if (insertion.text !== currentValue) {
        valueRef.current = insertion.text;
        updateValue(insertion.text);
        setEditedSinceHistory(true);
        restorePathCaret(insertion.cursor);
      }
    }
    if (files.length > 0) addMediaFiles(files, { requestNativeFocus });
    return folders.length > 0 || files.length > 0;
  }, [addMediaFiles, composerSelection.end, restorePathCaret, updateValue]);

  const addMaterializedPaths = useCallback(async (paths, savedCursor, options) => {
    const result = await materializeNativeFilesystemPaths(paths);
    return addNativeFilesystemItems(result.items, savedCursor, options);
  }, [addNativeFilesystemItems]);

  const handleFilesystemPaste = useCallback(({ files = [], uriList = '' } = {}) => {
    const fallbackFiles = Array.from(files || []);
    const savedCursor = composerSelection.end;
    const uriPaths = nativeFilesystemMaterializerAvailable
      ? localPathsFromUriList(uriList, HOST_OS)
      : [];

    let request = null;
    if (uriPaths.length > 0) {
      request = materializeNativeFilesystemPaths(uriPaths);
    } else if (nativeFilesystemClipboardAvailable) {
      request = readNativeClipboardFilesystemItems();
    }

    if (!request) {
      addMediaFiles(fallbackFiles);
      return;
    }

    Promise.resolve(request)
      .then((result) => {
        if (result.items.length > 0) {
          addNativeFilesystemItems(result.items, savedCursor);
        } else {
          addMediaFiles(fallbackFiles);
        }
      })
      .catch((error) => {
        toast({ kind: 'err', text: `粘贴文件或文件夹失败:${error?.message || '原生文件系统不可用'}` });
      });
  }, [
    addMediaFiles,
    addNativeFilesystemItems,
    composerSelection.end,
    nativeFilesystemClipboardAvailable,
    nativeFilesystemMaterializerAvailable,
  ]);

  const chooseLocalContext = useCallback(async () => {
    setCapabilityOpen(false);
    if (!nativeContextPickerAvailable) {
      fileInputRef.current?.click();
      return;
    }

    const savedCursor = composerSelection.end;
    try {
      const raw = await window.aceDesktop_pickContextItems({ cwd });
      const picked = parseNativeContextPickerResult(raw);
      if (picked.cancelled) {
        restorePathCaret(savedCursor);
        return;
      }
      if (picked.folder) {
        const referencePath = nativeFolderReferencePath(cwd, picked.folder);
        const insertion = insertPathReferenceAtCaret(value, savedCursor, referencePath);
        updateValue(insertion.text);
        setEditedSinceHistory(true);
        restorePathCaret(insertion.cursor);
        return;
      }

      const files = picked.files.map((item) => nativePickedFileToFile(item));
      if (!addMediaFiles(files)) restorePathCaret(savedCursor);
    } catch (error) {
      toast({ kind: 'err', text: `添加文件或文件夹失败:${error?.message || '选择器不可用'}` });
      restorePathCaret(savedCursor);
    }
  }, [
    addMediaFiles,
    composerSelection.end,
    cwd,
    nativeContextPickerAvailable,
    restorePathCaret,
    updateValue,
    value,
  ]);

  const handleFiles = (e) => {
    const files = Array.from(e.target.files || []);
    e.target.value = '';
    addMediaFiles(files);
  };

  const selectExpert = (expert) => {
    if (!expert?.id || !onSelectExpert) return;
    setCapabilityOpen(false);
    setExpertSubmenuOpen(false);
    setExpertSubmenuPosition(null);
    capabilityButtonRef.current?.focus();
    onSelectExpert(expert);
  };

  const openMoreExperts = () => {
    setCapabilityOpen(false);
    setExpertSubmenuOpen(false);
    setExpertSubmenuPosition(null);
    capabilityButtonRef.current?.focus();
    onOpenExpertComponents?.();
  };

  const setFileDragActive = useCallback((active) => {
    const next = !!active;
    if (dragActiveRef.current === next) return;
    dragActiveRef.current = next;
    setDragActive(next);
    onFileDragActiveChange?.(next);
  }, [onFileDragActiveChange]);

  const resetDragState = useCallback(() => {
    dragDepthRef.current = 0;
    setFileDragActive(false);
  }, [setFileDragActive]);

  const markNativeDropHover = useCallback(() => {
    nativeDropHoverRef.current = { active: true, ts: Date.now() };
  }, []);

  const handleDragEnter = useCallback((event) => {
    if (disabled || !onMediaFiles || !hasFileTransfer(event.dataTransfer)) return;
    // Windows 的 native 路径由 DOM drop → WebView2 additional objects 桥接，
    // 必须先取消 dragenter/dragover 默认行为才能收到 drop。macOS 仍让事件
    // 下沉给 WKWebView 原生 performDragOperation。
    if (!NATIVE_FILE_DROP || HOST_OS === 'windows') event.preventDefault();
    if (NATIVE_FILE_DROP) markNativeDropHover();
    if (dragDepthRef.current === 0) requestDesktopFileDragActivation();
    dragDepthRef.current += 1;
    setFileDragActive(true);
  }, [disabled, markNativeDropHover, onMediaFiles, setFileDragActive]);

  const handleDragOver = useCallback((event) => {
    if (disabled || !onMediaFiles || !hasFileTransfer(event.dataTransfer)) return;
    if (!NATIVE_FILE_DROP || HOST_OS === 'windows') {
      event.preventDefault();
      event.dataTransfer.dropEffect = 'copy';
    }
    if (NATIVE_FILE_DROP) markNativeDropHover();
    setFileDragActive(true);
  }, [disabled, markNativeDropHover, onMediaFiles, setFileDragActive]);

  const handleDragLeave = useCallback((event) => {
    if (!dragActiveRef.current) return;
    dragDepthRef.current = Math.max(0, dragDepthRef.current - 1);
    if (dragDepthRef.current === 0) {
      setFileDragActive(false);
      nativeDropHoverRef.current = { active: false, ts: 0 };
    }
  }, [setFileDragActive]);

  const handleDrop = useCallback((event) => {
    const files = disabled || !onMediaFiles ? [] : filesFromTransfer(event.dataTransfer, { source: 'drop' });
    if (NATIVE_FILE_DROP) {
      markNativeDropHover();
      if (HOST_OS === 'windows' && postWindowsNativeFilesystemDrop(event.dataTransfer)) {
        event.preventDefault();
        event.stopPropagation();
        resetDragState();
      }
      return;
    }
    const uriPaths = nativeFilesystemMaterializerAvailable
      ? localPathsFromUriList(uriListFromTransfer(event.dataTransfer), HOST_OS)
      : [];
    if (files.length > 0 || uriPaths.length > 0) {
      event.preventDefault();
      event.stopPropagation();
      if (uriPaths.length > 0) {
        const savedCursor = composerSelection.end;
        addMaterializedPaths(uriPaths, savedCursor, { requestNativeFocus: false })
          .catch((error) => toast({
            kind: 'err',
            text: `拖入文件或文件夹失败:${error?.message || '原生文件系统不可用'}`,
          }));
      } else {
        addMediaFiles(files, { requestNativeFocus: false });
      }
    }
    resetDragState();
  }, [
    addMaterializedPaths,
    addMediaFiles,
    composerSelection.end,
    disabled,
    markNativeDropHover,
    nativeFilesystemMaterializerAvailable,
    onMediaFiles,
    resetDragState,
  ]);

  useImperativeHandle(ref, () => ({
    focus: () => ta.current?.focus(),
    clear: () => {
      updateValue('');
      setHistPtr(-1);
      setEditedSinceHistory(false);
    },
    insertDirectoryReference: (relativePath) => {
      const insertion = insertPathReferenceAtCaret(value, composerSelection.end, relativePath);
      updateValue(insertion.text);
      setEditedSinceHistory(true);
      restorePathCaret(insertion.cursor);
      return insertion;
    },
    handleFileDragEnter: handleDragEnter,
    handleFileDragOver: handleDragOver,
    handleFileDragLeave: handleDragLeave,
    handleFileDrop: handleDrop,
  }), [
    composerSelection.end,
    handleDragEnter,
    handleDragLeave,
    handleDragOver,
    handleDrop,
    restorePathCaret,
    updateValue,
    value,
  ]);

  useEffect(() => () => {
    if (dragActiveRef.current) onFileDragActiveChange?.(false);
  }, [onFileDragActiveChange]);

  useEffect(() => {
    if (!dragActive) return undefined;
    window.addEventListener('dragend', resetDragState);
    window.addEventListener('drop', resetDragState);
    window.addEventListener('blur', resetDragState);
    return () => {
      window.removeEventListener('dragend', resetDragState);
      window.removeEventListener('drop', resetDragState);
      window.removeEventListener('blur', resetDragState);
    };
  }, [dragActive, resetDragState]);

  useEffect(() => {
    if (!NATIVE_FILE_DROP || !nativeFilesystemMaterializerAvailable) return undefined;
    const handler = (payload) => {
      let rawPaths = payload;
      if (typeof rawPaths === 'string') {
        try { rawPaths = JSON.parse(rawPaths); } catch { return; }
      }
      const hover = nativeDropHoverRef.current;
      if (!Array.isArray(rawPaths) || rawPaths.length === 0 ||
          !hover.active || Date.now() - hover.ts > 1500) return;

      nativeDropHoverRef.current = { active: false, ts: 0 };
      resetDragState();
      const paths = localPathsFromDropPayload(rawPaths, HOST_OS);
      if (paths.length === 0) return;
      const savedCursor = composerSelection.end;
      addMaterializedPaths(paths, savedCursor, { requestNativeFocus: false })
        .catch((error) => toast({
          kind: 'err',
          text: `拖入文件或文件夹失败:${error?.message || '原生文件系统不可用'}`,
        }));
    };
    window.__aceComposerAcceptFileDrop = handler;
    return () => {
      if (window.__aceComposerAcceptFileDrop !== handler) return;
      try { delete window.__aceComposerAcceptFileDrop; }
      catch { window.__aceComposerAcceptFileDrop = undefined; }
    };
  }, [
    addMaterializedPaths,
    composerSelection.end,
    nativeFilesystemMaterializerAvailable,
    resetDragState,
  ]);

  useEffect(() => {
    if (!hasCapabilityHandlers && capabilityOpen) setCapabilityOpen(false);
  }, [capabilityOpen, hasCapabilityHandlers]);

  useEffect(() => {
    if (!capabilityOpen) {
      setExpertSubmenuOpen(false);
      setExpertSubmenuPosition(null);
    }
  }, [capabilityOpen]);

  useLayoutEffect(() => {
    if (!expertSubmenuOpen) return undefined;
    let frame = 0;
    const updatePosition = () => {
      const parent = expertMenuParentRef.current;
      const menu = expertSubmenuRef.current;
      if (!parent || !menu) return;
      setExpertSubmenuPosition(placeExpertSubmenu({
        anchorRect: parent.getBoundingClientRect(),
        menuRect: menu.getBoundingClientRect(),
        viewportWidth: window.innerWidth,
        viewportHeight: window.innerHeight,
      }));
    };
    updatePosition();
    frame = window.requestAnimationFrame(updatePosition);
    window.addEventListener('resize', updatePosition);
    document.addEventListener('scroll', updatePosition, true);
    return () => {
      if (frame) window.cancelAnimationFrame(frame);
      window.removeEventListener('resize', updatePosition);
      document.removeEventListener('scroll', updatePosition, true);
    };
  }, [expertSubmenuOpen, recentExpertItems.length]);

  const focusExpertSubmenuItem = useCallback((index = 0) => {
    window.requestAnimationFrame(() => {
      const items = [...(expertSubmenuRef.current?.querySelectorAll('[role="menuitem"]:not(:disabled)') || [])];
      items[Math.min(Math.max(index, 0), Math.max(items.length - 1, 0))]?.focus();
    });
  }, []);

  const openExpertSubmenu = useCallback((focusFirst = false) => {
    setExpertSubmenuOpen(true);
    if (focusFirst) focusExpertSubmenuItem(0);
  }, [focusExpertSubmenuItem]);

  const closeExpertSubmenu = useCallback((restoreFocus = false) => {
    setExpertSubmenuOpen(false);
    setExpertSubmenuPosition(null);
    if (restoreFocus) {
      window.requestAnimationFrame(() => expertMenuParentRef.current?.focus());
    }
  }, []);

  const toggleSwarmMode = useCallback(() => {
    setCapabilityOpen(false);
    closeExpertSubmenu(false);
    onSwarmModeChange?.(!swarmMode);
    requestComposerCaretRestore();
  }, [
    closeExpertSubmenu,
    onSwarmModeChange,
    requestComposerCaretRestore,
    swarmMode,
  ]);

  const handleExpertSubmenuKeyDown = useCallback((event) => {
    const items = [...(expertSubmenuRef.current?.querySelectorAll('[role="menuitem"]:not(:disabled)') || [])];
    const currentIndex = Math.max(0, items.indexOf(document.activeElement));
    if (['ArrowDown', 'ArrowUp', 'Home', 'End'].includes(event.key)) {
      event.preventDefault();
      items[nextExpertMenuItemIndex(event.key, currentIndex, items.length)]?.focus();
      return;
    }
    if (event.key === 'ArrowLeft' || event.key === 'Escape') {
      event.preventDefault();
      event.stopPropagation();
      closeExpertSubmenu(true);
    } else if (event.key === 'Tab') {
      event.preventDefault();
      closeExpertSubmenu(false);
      window.requestAnimationFrame(() => {
        if (event.shiftKey) expertMenuParentRef.current?.focus();
        else fileMenuItemRef.current?.focus();
      });
    }
  }, [closeExpertSubmenu]);

  useEffect(() => {
    if (!capabilityOpen) return undefined;

    const closeCapabilityMenu = () => setCapabilityOpen(false);
    const closeFromPointer = (event) => {
      const menu = capabilityMenuRef.current;
      if (menu && event.target instanceof Node && menu.contains(event.target)) return;
      const submenu = expertSubmenuRef.current;
      if (submenu && event.target instanceof Node && submenu.contains(event.target)) return;
      closeCapabilityMenu();
    };
    const onKeyDown = (event) => {
      if (event.key === 'Escape' && !expertSubmenuOpen) {
        closeCapabilityMenu();
        window.requestAnimationFrame(() => capabilityButtonRef.current?.focus());
      }
    };

    document.addEventListener('click', closeFromPointer, true);
    document.addEventListener('keydown', onKeyDown, true);
    window.addEventListener('blur', closeCapabilityMenu);
    return () => {
      document.removeEventListener('click', closeFromPointer, true);
      document.removeEventListener('keydown', onKeyDown, true);
      window.removeEventListener('blur', closeCapabilityMenu);
    };
  }, [capabilityOpen, expertSubmenuOpen]);

  const handleComposerChange = (next) => {
    // 编辑器的 onChange 回声(程序化设值同步 / 光标移动)文本与当前 value 相同,
    // 不算用户编辑 —— 否则历史导航刚填入的文本会被误标为已编辑,上下键随即失效。
    if (!isUserComposerEdit({ nextValue: next, currentValue: value })) return;
    updateValue(next);
    setEditedSinceHistory(next.length > 0);
  };

  const onKey = (e) => {
    // 下拉打开时,Enter / Tab / Esc / 方向键 由 SlashDropdown 在捕获阶段处理。
    // 这里只处理常规情况。
    if (shouldNavigateInputHistory({
      key: e.key,
      value,
      editedSinceHistory,
      historyLength: history.length,
      historyPointer: histPtr,
      altKey: e.altKey,
      ctrlKey: e.ctrlKey,
      metaKey: e.metaKey,
      shiftKey: e.shiftKey,
    })) {
      e.preventDefault();
      const next = getNextInputHistoryPointer({
        key: e.key,
        historyLength: history.length,
        historyPointer: histPtr,
      });
      if (next === -1) {
        setHistPtr(-1);
        updateValue('');
      } else {
        setHistPtr(next);
        updateValue(history[next] || '');
      }
      setEditedSinceHistory(false);
      return;
    }
  };

  const actionState = getInputBarActionState({ value, disabled, busy, hasExtras });
  const stopControl = getGoalStopControlState({ busy });
  const composerSpacingClass = isHero ? 'px-4 pt-3 pb-1 text-[14px]' : 'px-3 pt-2 pb-1 text-[13px]';
  const hasInlineContexts = otherContextItems.length > 0;
  const capabilityControl = (
    <div ref={capabilityMenuRef} className="relative shrink-0 flex items-center">
      <button
        ref={capabilityButtonRef}
        type="button"
        disabled={disabled || !hasCapabilityHandlers}
        className="w-7 h-7 rounded-full flex items-center justify-center text-fg-mute hover:bg-surface-hi hover:text-fg disabled:opacity-50"
        onClick={() => setCapabilityOpen((open) => !open)}
        title="添加能力或上下文"
        aria-label="添加能力或上下文"
      >
        <VsIcon name="add" size={15} />
      </button>
      {capabilityOpen && hasCapabilityHandlers && (
        <div
          data-composer-capability-menu="true"
          data-ace-native-overlay="overlap"
          role="menu"
          className="absolute left-0 bottom-8 z-50 w-52 py-1 rounded-lg border border-border bg-surface ace-shadow"
        >
          <button
            type="button"
            role="menuitemcheckbox"
            aria-checked={swarmMode}
            className={clsx(
              'w-full h-8 px-2 flex items-center gap-2 text-left text-[13px] hover:bg-surface-hi',
              swarmMode ? 'bg-accent-bg text-accent' : 'text-fg',
            )}
            onPointerEnter={() => closeExpertSubmenu(false)}
            onClick={toggleSwarmMode}
          >
            <SwarmModeIcon size={15} className="shrink-0" />
            <span className="min-w-0 flex-1 truncate">蜂群模式</span>
            {swarmMode && <VsIcon name="check" size={12} className="shrink-0 opacity-70" />}
          </button>

          <div className="my-1 border-t border-border" aria-hidden="true" />

          <div className="relative" data-expert-menu-parent="true">
            <button
              ref={expertMenuParentRef}
              type="button"
              role="menuitem"
              aria-haspopup="menu"
              aria-expanded={expertSubmenuOpen}
              disabled={!hasExpertHandlers}
              title={selectedExpertName ? `当前专家组件：${selectedExpertName}` : '选择专家组件'}
              className="w-full h-8 px-2 flex items-center gap-2 text-left text-[13px] text-fg hover:bg-surface-hi disabled:opacity-50"
              onPointerEnter={() => openExpertSubmenu(false)}
              onKeyDown={(event) => {
                if (event.key === 'ArrowRight' || event.key === 'ArrowDown') {
                  event.preventDefault();
                  openExpertSubmenu(true);
                }
              }}
              onClick={() => {
                if (expertSubmenuOpen) closeExpertSubmenu(false);
                else openExpertSubmenu(true);
              }}
            >
              <VsIcon name="brain" size={14} />
              <span className="min-w-0 flex-1 truncate">专家组件</span>
              <VsIcon name="expandRight" size={13} className="shrink-0 text-fg-mute" />
            </button>

            {expertSubmenuOpen && hasExpertHandlers && typeof document !== 'undefined' && createPortal(
              <div
                ref={expertSubmenuRef}
                data-expert-components-submenu="true"
                data-ace-native-overlay="overlap"
                role="menu"
                aria-label="最近使用的专家组件"
                onKeyDown={handleExpertSubmenuKeyDown}
                className="fixed z-[100] overflow-y-auto rounded-lg border border-border bg-surface ace-shadow"
                style={{
                  top: expertSubmenuPosition?.top ?? 0,
                  left: expertSubmenuPosition?.left ?? 0,
                  width: expertSubmenuPosition?.width ?? Math.max(0, Math.min(440, window.innerWidth - 24)),
                  maxHeight: expertSubmenuPosition?.maxHeight ?? Math.max(0, window.innerHeight - 24),
                  visibility: expertSubmenuPosition ? 'visible' : 'hidden',
                }}
              >
                {recentExpertItems.length > 0 && (
                  <div className="py-1">
                    {recentExpertItems.map((expert) => {
                      const selected = expert.id === selectedExpertId;
                      return (
                        <button
                          key={expert.id}
                          type="button"
                          role="menuitem"
                          data-expert-menu-item={expert.id}
                          onClick={() => selectExpert(expert)}
                          className={clsx(
                            'grid h-9 w-full grid-cols-[24px_minmax(88px,142px)_minmax(0,1fr)_auto] items-center gap-2 px-3 text-left transition-colors',
                            selected ? 'bg-accent-bg text-accent' : 'text-fg hover:bg-surface-hi',
                          )}
                        >
                          <ExpertAvatar expert={expert} size={22} className="rounded-md" />
                          <span className="truncate text-[12px] font-medium">
                            {expert.display_name || expert.id}
                          </span>
                          <span className="truncate text-[11px] text-fg-mute">
                            {compactExpertSummary(expert) || '尚未填写擅长领域'}
                          </span>
                          <span className="rounded border border-border px-1 py-0.5 text-[9px] text-fg-mute">
                            {expert.type === 'team' ? '专家团' : '专家'}
                          </span>
                        </button>
                      );
                    })}
                  </div>
                )}
                <div className={clsx('py-1', recentExpertItems.length > 0 && 'border-t border-border')}>
                  <button
                    type="button"
                    role="menuitem"
                    onClick={openMoreExperts}
                    className="flex h-9 w-full items-center gap-2 px-3 text-left text-[13px] text-fg-2 hover:bg-surface-hi"
                  >
                    <VsIcon name="extension" size={15} className="shrink-0" />
                    <span>更多专家</span>
                  </button>
                </div>
              </div>,
              document.body,
            )}
          </div>

          <div className="my-1 border-t border-border" aria-hidden="true" />

          <button
            ref={fileMenuItemRef}
            type="button"
            role="menuitem"
            className="w-full h-8 px-2 flex items-center gap-2 text-left text-[13px] text-fg hover:bg-surface-hi disabled:opacity-50"
            onPointerEnter={() => closeExpertSubmenu(false)}
            onClick={chooseLocalContext}
            disabled={!canChooseLocalContext}
          >
            <VsIcon name="folderOpen" size={14} />
            <span>文件或文件夹</span>
          </button>
        </div>
      )}
    </div>
  );
  const inlineContextControls = hasInlineContexts ? (
    <>
      {otherContextItems.map((item, index) => {
        const key = composerContextKey(item, index);
        const presentation = contextPresentation(item);
        return (
          <div
            key={key}
            className="group h-7 max-w-[112px] shrink-0 rounded-md px-1.5 flex items-center gap-1 text-[12px] text-fg-mute hover:bg-surface-hi"
            title={presentation.title}
          >
            <VsIcon name={presentation.icon} size={13} />
            <span className="truncate">{presentation.label}</span>
            <button
              type="button"
              className="w-4 h-4 rounded-full flex items-center justify-center hover:bg-bg text-fg-mute opacity-0 group-hover:opacity-100 focus:opacity-100"
              onClick={() => onRemoveContext?.(key)}
              aria-label={presentation.removeLabel}
            >
              <VsIcon name="close" size={9} />
            </button>
          </div>
        );
      })}
    </>
  ) : null;
  const submitControls = (
    <>
      {stopControl.visible && (
        <button
          type="button"
          onClick={onAbort}
          disabled={stopControl.disabled}
          className="px-2 h-7 rounded-md text-[11px] text-danger border border-danger/40 hover:bg-danger-bg transition flex items-center gap-1 disabled:opacity-50 disabled:cursor-wait"
          title={stopControl.title}
        >
          <VsIcon name="stop" size={12} mono={false} />
          <span>{stopControl.label}</span>
        </button>
      )}
      {busy ? (
        <button
          type="button"
          onClick={submit}
          disabled={!actionState.canSubmit}
          className={clsx(
            'px-2 h-7 rounded-md text-[11px] transition flex items-center gap-1',
            actionState.canSubmit
              ? 'bg-accent text-white hover:opacity-90'
              : 'bg-surface-hi text-fg-mute cursor-default',
          )}
          title={actionState.submitTitle}
        >
          <VsIcon name="send" size={12} mono={false} className={actionState.canSubmit ? 'ace-icon-on-accent' : ''} />
          <span>{actionState.submitLabel}</span>
        </button>
      ) : (
        <button
          type="button"
          onClick={submit}
          disabled={!actionState.canSubmit}
          className={clsx(
            'w-7 h-7 rounded-full flex items-center justify-center transition',
            actionState.canSubmit
              ? 'bg-accent text-white hover:opacity-90'
              : 'bg-surface-hi text-fg-mute cursor-default',
          )}
          title={actionState.submitTitle}
        >
          <VsIcon name="send" size={14} mono={false} className={actionState.canSubmit ? 'ace-icon-on-accent' : ''} />
        </button>
      )}
    </>
  );

  return (
    <div className={clsx(
      'ace-inputbar-layer',
      isHero ? 'ace-inputbar-hero' : 'border-t border-border px-2.5 py-2 bg-surface shrink-0',
    )}>
      <input
        ref={fileInputRef}
        type="file"
        multiple
        className="hidden"
        onChange={handleFiles}
      />
      {!isHero && goal && (
        <GoalStatusBar
          goal={goal}
          onEdit={onGoalEdit}
          onStatusChange={onGoalStatusChange}
          onClear={onGoalClear}
        />
      )}
      <div className={clsx(
        'ace-composer-card relative bg-surface border-[1.5px] border-border focus-within:border-accent focus-within:ring-2 focus-within:ring-accent/15 transition',
        isHero ? 'ace-inputbar-hero-card rounded-2xl' : 'rounded-xl',
        dragActive && 'border-accent ring-2 ring-accent/20',
      )}
      ref={rootRef}
      onDragEnter={fileDropManagedExternally ? undefined : handleDragEnter}
      onDragOver={fileDropManagedExternally ? undefined : handleDragOver}
      onDragLeave={fileDropManagedExternally ? undefined : handleDragLeave}
      onDrop={fileDropManagedExternally ? undefined : handleDrop}
      >
        {activePathDropdown && (
          <PathReferenceDropdown
            fileItems={activePathDropdown.fileItems || []}
            sessionItems={activePathDropdown.sessionItems || []}
            fileLoading={!!activePathDropdown.fileLoading}
            sessionLoading={!!activePathDropdown.sessionLoading}
            fileError={activePathDropdown.fileError || ''}
            sessionError={activePathDropdown.sessionError || ''}
            onReference={(item) => applyMentionItem(item, false)}
            onReferenceSession={applySessionMentionItem}
            onEnterDirectory={(item) => applyMentionItem(item, true)}
            onClose={closePathDropdown}
          />
        )}
        {showDropdown && !activePathDropdown && (
          <SlashDropdown
            items={commands}
            query={value.slice(1)}
            onSelect={handleSelectCommand}
            onClose={() => setDropdownClosed(true)}
          />
        )}
        {(selectionPreview || selectionContextItems.length > 0 || browserContextItems.length > 0) && (
          <div className={clsx(
            'px-3 pt-2 flex flex-wrap items-center gap-1.5',
            isHero && 'px-4',
          )}>
            {selectionPreview ? (
              <ComposerSelectionCard
                item={selectionPreview}
                annotationPresentations={annotationPresentations}
                onPin={() => onPinSelectionPreview?.(selectionPreview)}
              />
            ) : null}
            {selectionContextItems.map((item, index) => {
              const key = composerContextKey(item, index);
              return (
                <ComposerSelectionCard
                  key={key}
                  item={item}
                  annotationPresentations={annotationPresentations}
                  pinned
                  onRemove={() => onRemoveContext?.(key)}
                />
              );
            })}
            {browserContextItems.map((item, index) => {
              const key = composerContextKey(item, index);
              return (
                <ComposerBrowserContextCard
                  key={key}
                  item={item}
                  onRemove={() => onRemoveContext?.(key)}
                />
              );
            })}
          </div>
        )}
        <div className={clsx('relative', isHero && 'ace-inputbar-hero-editor')}>
          <RichComposer
            ref={ta}
            value={value}
            commands={commands}
            attachments={attachmentItems}
            onChange={handleComposerChange}
            onKeyDown={onKey}
            onCompositionStart={handleCompositionStart}
            onCompositionEnd={handleCompositionEnd}
            onSelectionChange={setComposerSelection}
            isComposingKeyEvent={isComposingKeyEvent}
            onSubmit={submit}
            onPreviewAttachment={previewComposerAttachment}
            onRemoveAttachment={onRemoveAttachment}
            onPasteFiles={addMediaFiles}
            onPasteFilesystemItems={
              nativeFilesystemClipboardAvailable || nativeFilesystemMaterializerAvailable
                ? handleFilesystemPaste
                : undefined
            }
            allowNativeFilesystemDrop={NATIVE_FILE_DROP}
            disabled={disabled}
            placeholder={placeholder}
            className={clsx(
              'ace-rich-composer-input relative w-full bg-transparent border-0 outline-none leading-[20px] font-sans text-fg disabled:opacity-50 whitespace-pre-wrap break-words',
              composerSpacingClass,
            )}
            placeholderClassName={composerSpacingClass}
            style={{
              minHeight: textareaBaseHeight,
              maxHeight: textareaMaxHeight,
              overflowY: 'auto',
            }}
          />
        </div>
        <ComposerSessionControls
          {...(sessionControls || {})}
          className={isHero ? 'px-2.5 pb-2.5' : 'px-1.5 pb-1'}
          addControl={capabilityControl}
          contexts={inlineContextControls}
          actions={submitControls}
          expertId={selectedExpertId}
          expertName={selectedExpertName}
          expertType={selectedExpertType}
          swarmMode={swarmMode}
          onDisableSwarm={() => onSwarmModeChange?.(false)}
          expertRemoving={expertRemoving}
          onRemoveExpert={onRemoveExpert}
          pendingExpertName={pendingExpertName}
          pendingExpertType={pendingExpertType}
        />
      </div>
      <ImageLightbox preview={attachmentPreview} onClose={() => setAttachmentPreview(null)} />
    </div>
  );
});
