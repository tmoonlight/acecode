// 输入框:富文本 composer 自动撑高(最多 8 行) + Enter 发 / Shift+Enter 换行 +
// 空输入或未编辑的历史项用上下键翻 history。
//
// 底部工具栏单独占一行,提交按钮在右侧;空内容仅在可重试末尾用户消息时允许发送。
//
// 斜杠命令:value 以 / 开头且无空白时,SlashDropdown 浮层显示在输入框上方。
// 目标指令显示在加号旁的标签中；其他已确认命令仍在正文中显示原子 token。
// 选择命令不立即发送，草稿与发送继续使用原始命令协议。

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
import { normalizeSwarmMode, pickSwarmMode, swarmModeLabel } from '../lib/swarmMode.js';
import { RichComposer } from './RichComposer.jsx';
import { PastedTextCard } from './PastedTextCard.jsx';
import { PastedTextDialog } from './PastedTextDialog.jsx';
import { PathReferenceDropdown } from './PathReferenceDropdown.jsx';
import { SlashDropdown } from './SlashDropdown.jsx';
import { toast } from './Toast.jsx';
import { withAppShortcutHint } from '../lib/appShortcuts.js';
import { useSlashCommands } from './SlashCommandsContext.jsx';
import { getNextInputHistoryPointer, isUserComposerEdit, shouldNavigateInputHistory } from '../lib/inputHistoryNavigation.js';
import { filesFromTransfer, hasFileTransfer } from '../lib/composerFileTransfer.js';
import { composerContentWithoutImages, isComposerThumbnailAttachment, withComposerImageAttachments } from '../lib/composerImagePresentation.js';
import { composerDraftEditFingerprint, removeComposerAttachmentReference } from '../lib/composerDraft.js';
import { projectComposerGoal, serializeComposerGoal } from '../lib/composerGoal.js';
import { isComposerCompletionSelectionCollapsed } from '../lib/composerDropdownKeyboard.js';
import { commandQueryAtCursor } from '../lib/slashCommands.js';
import { normalizeComposerContent, composerContentSignature, composerContentText, composerContentAttachments, composerContentFromText } from '../lib/composerContent.js';
import {
  editorAttachmentResources,
  legacyTextNeedsFold,
  pasteBlockTextSource,
  pasteBlocksOf,
  pastedTextTitle,
  removePastedTextPart,
} from '../lib/pastedText.js';
import {
  captureComposerTextareaSelection,
  isComposerEditorFocused,
  preserveComposerFocusOnPointerDown,
  requestDesktopFileDragActivation,
  requestDesktopFileDropFocus,
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
  parseNativeContextPickerResult,
} from '../lib/desktopContextPicker.js';
import {
  desktopHostOs,
  hasNativeFilesystemMaterializer,
  localPathsFromDropPayload,
  localPathsFromUriList,
  nativeFileDropEnabled,
  uriListFromTransfer,
} from '../lib/desktopFilesystemTransfer.js';
import { resolveComposerFileIntake } from '../lib/composerFileIntake.js';
import { postWindowsNativeFilesystemDrop } from '../lib/desktopNativeFilesystemDrop.js';
import { fileDropDiagnostic, registerNativeComposerFileDrop } from '../lib/macNativeFileDrag.js';
import {
  nextExpertMenuItemIndex,
  placeExpertSubmenu,
} from '../lib/expertMenuPosition.js';

const MAX_ROWS = 8;
const LINE_HEIGHT = 20; // 与 leading-[20px] 对齐
const HOST_OS = desktopHostOs();
const NATIVE_FILE_DROP = nativeFileDropEnabled();
const EMPTY_COMPOSER_ATTACHMENTS = Object.freeze([]);

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

// 粘贴块卡片的状态:由块对应的资源(上传状态、内存 File)推导。
//   uploading 正在上传;failed 上传失败(点卡片重试);deferred 来自旧输入历史、
//   尚未上传;lost 没有附件 id 也没有内存 File(刷新前没传完),只能删除。
function pasteBlockCardState(block, resource) {
  if (block.kind === 'inline') return 'ready';
  if (resource?.uploading) return 'uploading';
  if (resource?.upload_error) return 'failed';
  if (resource?.upload_deferred && resource?.pending_upload) return 'deferred';
  if (!block.part?.id && !resource?.id && !resource?.file) return 'lost';
  return 'ready';
}

function pasteBlockCardTitle(block) {
  if (block.kind === 'inline') return pastedTextTitle(block.part.text);
  return String(block.part?.paste?.title || block.part?.name || '');
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
  disabled, submitting = false, canRetryLastUserMessage = false,
  queuePaused = false, onResumeQueue,
  placeholder = '输入消息或 / 命令…', onSubmit, onAbort, busy, stopping = false, goal = null, mainComposer = false,
  onGoalEdit, onGoalStatusChange, onGoalClear,
  history = [], historyEntries = [], variant = 'default', attentionRequest = 0,
  value: controlledValue, onChange,
  composerContent: controlledComposerContent, onComposerContentChange,
  attachments = EMPTY_COMPOSER_ATTACHMENTS, contexts = [], annotationPresentations = null,
  onMediaFiles, onRemoveAttachment, onRemoveContext,
  onLargeTextPaste, onReplacePasteBlock, onRetryPasteUpload, onCommitDeferredPastes,
  attachmentTextLoader,
  swarmMode = 'off', onSwarmModeChange,
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
  const [internalContent, setInternalContent] = useState(null);
  const draftContent = controlledComposerContent !== undefined ? controlledComposerContent : internalContent;
  const draftValue = isControlled ? String(controlledValue || '') : internalValue;
  const { goalMode, text: value, content: composerContent } = useMemo(
    () => projectComposerGoal(draftValue, draftContent), [draftValue, draftContent],
  );
  const goalModeRef = useRef(goalMode);
  goalModeRef.current = goalMode;
  const contentRef = useRef(composerContent);
  contentRef.current = composerContent;
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
  // 打开的粘贴块对话框:打开那一刻固定文本来源,上传中途资源从 File 换成服务端记录
  // 时不重新装载(否则会覆盖用户正在编辑的内容)。
  const [openPaste, setOpenPaste] = useState(null);
  const ta = useRef(null);
  const rootRef = useRef(null);
  const attentionRingRef = useRef(null);
  const lastAttentionRequestRef = useRef(attentionRequest);
  const fileInputRef = useRef(null);
  const fileIntakeQueueRef = useRef(Promise.resolve());
  const fileIntakeScope = JSON.stringify([cwd, currentSessionId]);
  const fileIntakeScopeRef = useRef(fileIntakeScope);
  if (fileIntakeScopeRef.current !== fileIntakeScope) {
    fileIntakeScopeRef.current = fileIntakeScope;
    fileIntakeQueueRef.current = Promise.resolve();
  }
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
  useLayoutEffect(() => {
    // A request is transient: mounting a composer must not replay an old click.
    if (lastAttentionRequestRef.current === attentionRequest) return;
    lastAttentionRequestRef.current = attentionRequest;
    const ring = attentionRingRef.current;
    if (!ring) return;
    if (!isComposerEditorFocused(rootRef.current)) ta.current?.focus();
    const reduceMotion = window.matchMedia('(prefers-reduced-motion: reduce)').matches;
    const peakOpacity = reduceMotion ? 0.35 : 1;
    const animation = ring.animate([
      { opacity: 0 },
      { opacity: peakOpacity, offset: 0.125 },
      { opacity: peakOpacity, offset: 0.375 },
      { opacity: 0, offset: 0.625 },
      { opacity: 0 },
    ], {
      duration: reduceMotion ? 180 : 160,
      iterations: 3,
      easing: 'linear',
    });
    return () => animation.cancel();
  }, [attentionRequest]);
  const textareaVerticalPadding = isHero ? 16 : 12;
  // 状态控制已经收进 composer，空输入区统一保留两行高度，整体比例与
  // WorkBuddy 式输入框一致；最大高度仍保持原有 8 行上限。
  const textareaBaseHeight = LINE_HEIGHT * 2 + textareaVerticalPadding;
  const textareaMaxHeight = LINE_HEIGHT * MAX_ROWS + textareaVerticalPadding;
  const attachmentItems = Array.isArray(attachments) ? attachments : EMPTY_COMPOSER_ATTACHMENTS;
  const attachmentItemsRef = useRef(attachmentItems);
  attachmentItemsRef.current = attachmentItems;
  // 交给 RichComposer 的附件剥掉图片(缩略图条)与粘贴资源(卡片条):否则 RichComposer
  // 会把上传完成后丢了 paste 标记的粘贴资源当成新附件插进编辑器。
  const editorAttachmentItems = useMemo(
    () => editorAttachmentResources(attachmentItems, composerContent),
    [attachmentItems, composerContent],
  );
  const pasteBlocks = useMemo(() => pasteBlocksOf(composerContent), [composerContent]);
  const editorContent = useMemo(() => composerContentWithoutImages(composerContent), [composerContent]);
  const mergeEditorContent = useCallback((content) => withComposerImageAttachments(
    content, contentRef.current || composerContentFromText(valueRef.current, attachmentItemsRef.current),
  ), []);
  const activeAttachmentItems = useMemo(() => (
    normalizeComposerContent(composerContent)
      ? composerContentAttachments(composerContent, attachmentItems)
      : attachmentItems
  ), [composerContent, attachmentItems]);
  const imageAttachments = useMemo(() => activeAttachmentItems.filter(isComposerThumbnailAttachment), [activeAttachmentItems]);
  const contextItems = Array.isArray(contexts) ? contexts : [];
  const recentExpertItems = Array.isArray(expertOptions) ? expertOptions.slice(0, 5) : [];
  const selectionContextItems = contextItems.filter((item) => item?.type === SELECTION_CONTEXT_TYPE);
  const browserContextItems = contextItems.filter((item) => item?.type === 'browser');
  const otherContextItems = contextItems.filter((item) => (
    item?.type !== SELECTION_CONTEXT_TYPE && item?.type !== 'browser'
  ));
  // 文件块是附件,已计入 activeAttachmentItems;内联块不是附件,单独计入。
  const hasExtras = activeAttachmentItems.length > 0 || contextItems.length > 0
    || pasteBlocks.some((block) => block.kind === 'inline');
  const pasteCards = useMemo(() => pasteBlocks.map((block) => {
    const resource = block.kind === 'file'
      ? activeAttachmentItems.find((item) => item?.local_id === block.part.key
        || (block.part.id && item?.id === block.part.id)) || null
      : null;
    return {
      block,
      resource,
      title: pasteBlockCardTitle(block),
      sizeBytes: resource?.size_bytes,
      status: pasteBlockCardState(block, resource),
    };
  }), [activeAttachmentItems, pasteBlocks]);
  const openPasteCard = openPaste
    ? pasteCards.find((card) => card.block.id === openPaste.id) || null
    : null;
  const nativeContextPickerAvailable = hasNativeContextPicker();
  const nativeFilesystemMaterializerAvailable = hasNativeFilesystemMaterializer();
  const canChooseLocalContext = !!onMediaFiles || nativeContextPickerAvailable;
  const hasExpertHandlers = !!onSelectExpert || !!onOpenExpertComponents;
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
    ...pasteBlocks.map((block) => `paste:${block.id}`),
  ].join('\n'), [attachmentItems, contextItems, pasteBlocks]);

  const previewComposerAttachment = useCallback((item) => {
    const src = String(item?.url || item?.preview_url || item?.blob_url || '');
    if (!src) return;
    setAttachmentPreview({ src, alt: String(item?.name || 'attachment') });
  }, []);

  const updateValue = useCallback((next, content, replacementRange, options = {}) => {
    const text = String(next || '');
    const nextContent = content === undefined
      ? mergeEditorContent(ta.current?.replaceTextPreservingReferences?.(text, replacementRange) || composerContentFromText(text))
      : normalizeComposerContent(content);
    const nextGoalMode = options.goalMode ?? goalModeRef.current;
    if (goalModeRef.current === nextGoalMode && valueRef.current === text
        && composerContentSignature(contentRef.current) === composerContentSignature(nextContent)) return;
    const draft = serializeComposerGoal(text, nextContent, nextGoalMode);
    const editor = projectComposerGoal(draft.text, draft.content);
    const newlyConfirmedGoal = options.goalMode === undefined && !goalModeRef.current && editor.goalMode;
    const selection = newlyConfirmedGoal ? captureComposerTextareaSelection(ta.current) : null;
    valueRef.current = editor.text;
    contentRef.current = editor.content;
    goalModeRef.current = editor.goalMode;
    if (!isControlled) setInternalValue(draft.text);
    if (controlledComposerContent === undefined) setInternalContent(draft.content);
    onChange?.(draft.text, draft.content);
    onComposerContentChange?.(draft.content);
    if (newlyConfirmedGoal) {
      const cursor = Math.max(0, (selection?.end ?? text.length) - editor.prefixLength);
      const scope = fileIntakeScopeRef.current;
      // The empty body can match an earlier Slate local echo. Remove the
      // confirmed token explicitly before another input event can reuse it.
      queueMicrotask(() => {
        if (fileIntakeScopeRef.current !== scope || !goalModeRef.current || valueRef.current !== editor.text) return;
        if (ta.current?.getEditorStateText?.() !== editor.text) {
          ta.current?.replaceTextPreservingReferences?.(editor.text, { begin: 0, end: editor.prefixLength });
        }
        ta.current?.setSelectionRange(cursor, cursor);
      });
    }
  }, [isControlled, controlledComposerContent, onChange, onComposerContentChange, mergeEditorContent]);

  const removePasteBlock = useCallback((id) => {
    const current = contentRef.current || composerContentFromText(valueRef.current, attachmentItemsRef.current);
    updateValue(valueRef.current, removePastedTextPart(current, id));
  }, [updateValue]);

  const openPasteBlock = useCallback((card) => {
    setOpenPaste({
      id: card.block.id,
      source: pasteBlockTextSource({ part: card.block.part, resource: card.resource }, { sessionId: currentSessionId }),
    });
  }, [currentSessionId]);

  const removeAttachment = useCallback((key) => {
    const current = contentRef.current || composerContentFromText(valueRef.current, attachmentItemsRef.current);
    const image = current.parts?.find((part) => part.type === 'attachment'
      && (part.key === key || part.id === key) && isComposerThumbnailAttachment(part));
    if (image) updateValue(valueRef.current, removeComposerAttachmentReference(current, key));
    else if (ta.current?.removeAttachment) ta.current.removeAttachment(key);
    else onRemoveAttachment?.(key);
  }, [onRemoveAttachment, updateValue]);

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
        removeAttachment(match.context.key);
      }
    };
    window.addEventListener(DESKTOP_CONTEXT_ACTION_EVENT, handler);
    return () => window.removeEventListener(DESKTOP_CONTEXT_ACTION_EVENT, handler);
  }, [attachmentItems, removeAttachment]);

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
  const composerSelectionCollapsed = isComposerCompletionSelectionCollapsed(composerSelection);
  const commandQuery = composerSelectionCollapsed ? commandQueryAtCursor(value, composerSelection.end) : null;
  const commandItems = commandQuery?.leading ? commands : commands.filter((item) => item.kind === 'skill');
  const showDropdownRaw = !!commandQuery;
  const showDropdown = showDropdownRaw && !dropdownClosed && !composerComposing && commandItems.length > 0;

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
    if (!item || !commandQuery) return;
    let cursor;
    if (item.kind === 'skill') {
      const content = ta.current?.insertSkill?.(item, commandQuery.begin, commandQuery.end);
      if (content) updateValue(composerContentText(content), mergeEditorContent(content));
      cursor = commandQuery.begin + String(item.mention || `$${item.name}`).length + 1;
    } else {
      if (!commandQuery.leading) return;
      const selectedGoal = item.kind === 'builtin' && item.name === 'goal';
      const next = (selectedGoal ? '' : '/' + item.name + ' ') + value.slice(commandQuery.end);
      updateValue(next, undefined, commandQuery, { goalMode: selectedGoal });
      cursor = selectedGoal ? 0 : item.name.length + 2;
    }
    setEditedSinceHistory(true);
    setDropdownClosed(true);
    requestAnimationFrame(() => {
      const el = ta.current;
      if (el) {
        el.focus();
        el.setSelectionRange(cursor, cursor);
      }
    });
  };

  const submit = () => {
    if (!actionState.canSubmit) return;
    // 队列暂停 + 空输入框:这一下是「继续」,不是发送 —— 空内容本来也没什么可发。
    if (actionState.mode === 'resume') {
      onResumeQueue?.();
      requestAnimationFrame(() => ta.current?.focus());
      return;
    }
    onSubmit?.(serializeComposerGoal(valueRef.current, contentRef.current, goalModeRef.current).text);
    if (!isControlled) updateValue('', composerContentFromText(''), undefined, { goalMode: false });
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

  const changeGoalMode = (enabled) => {
    updateValue(valueRef.current, contentRef.current, undefined, { goalMode: enabled });
    setEditedSinceHistory(true);
    setCapabilityOpen(false);
    requestAnimationFrame(() => ta.current?.focus());
  };

  // `@` reference menu: files keep the existing visible-path behavior, while
  // sessions use a stable inline token that is expanded only when submitted.
  // Directory contents and session transcripts are never preloaded here.
  useEffect(() => {
    const cursor = composerSelection.end;
    const token = pathReferenceTokenAtCursor(value, cursor);
    const signature = pathReferenceSignature(token, cursor, cwd);
    const unavailable = disabled || !pathReferenceApi || composerComposing || !composerSelectionCollapsed || showDropdown || !token;
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
    composerSelectionCollapsed,
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
    updateValue(replacement.text, undefined, { ...pathMention.token, plainText: enterDirectory });
    setEditedSinceHistory(true);
    restorePathCaret(replacement.cursor);
  }, [pathMention?.token, restorePathCaret, updateValue, value]);

  const applySessionMentionItem = useCallback((item) => {
    if (!pathMention?.token || !item) return;
    const replacement = replaceQueryWithSessionReference(value, pathMention.token, item);
    mentionGenerationRef.current += 1;
    setPathMention(null);
    updateValue(replacement.text, undefined, pathMention.token);
    setEditedSinceHistory(true);
    restorePathCaret(replacement.cursor);
  }, [pathMention?.token, restorePathCaret, updateValue, value]);

  const activePathDropdown = composerSelectionCollapsed ? pathMention : null;

  const handleFileIntake = useCallback((payload, transfer) => {
    if (disabled || !transfer?.isActive()) return Promise.resolve(true);
    setCapabilityOpen(false);
    // Start acquisition immediately (the clipboard can change), but commit
    // batches in gesture order even when native requests finish out of order.
    const request = resolveComposerFileIntake(payload).then(
      result => ({ result }), error => ({ error }),
    );
    const completion = fileIntakeQueueRef.current.then(async () => {
      const { result, error } = await request;
      if (!transfer.isActive()) return true;
      if (error) {
        toast({ kind: 'err', text: `添加文件或文件夹失败:${error.message || '原生文件系统不可用'}` });
        return true;
      }
      if (result.kind === 'none') return false;
      if (result.kind === 'paths') transfer.insertPaths(result.items);
      // Raster images (a pure upload, or those that came with path references)
      // become thumbnail attachments rather than @path tags.
      if (result.files?.length && onMediaFiles && transfer.reserveAttachments()) onMediaFiles(result.files);
      setEditedSinceHistory(true);
      return true;
    }).catch(error => {
      if (transfer.isActive()) toast({ kind: 'err', text: `添加文件或文件夹失败:${error?.message || '原生文件系统不可用'}` });
      return true;
    });
    fileIntakeQueueRef.current = completion;
    return completion;
  }, [disabled, onMediaFiles]);

  const acceptFileIntake = useCallback((payload) => {
    const transfer = ta.current?.beginFileTransfer();
    if (!transfer) return;
    ta.current?.focus();
    handleFileIntake(payload, transfer).finally(() => transfer.dispose());
  }, [handleFileIntake]);

  const handleFilesystemPaste = useCallback((payload, transfer) => (
    handleFileIntake({ ...payload, source: 'paste' }, transfer)
  ), [handleFileIntake]);

  const chooseLocalContext = useCallback(async () => {
    setCapabilityOpen(false);
    if (!nativeContextPickerAvailable) {
      fileInputRef.current?.click();
      return;
    }

    const transfer = ta.current?.beginFileTransfer();
    if (!transfer) return;
    try {
      const raw = await window.aceDesktop_pickContextItems({ cwd });
      const picked = parseNativeContextPickerResult(raw);
      if (!transfer.isActive()) return;
      ta.current?.focus();
      if (!picked.cancelled) await handleFileIntake({
        source: 'picker', items: picked.folder ? [picked.folder] : picked.files,
      }, transfer);
    } catch (error) {
      if (transfer.isActive()) toast({ kind: 'err', text: `添加文件或文件夹失败:${error?.message || '选择器不可用'}` });
    } finally {
      transfer.dispose();
    }
  }, [
    handleFileIntake,
    cwd,
    nativeContextPickerAvailable,
  ]);

  const handleFiles = (e) => {
    const files = Array.from(e.target.files || []);
    e.target.value = '';
    acceptFileIntake({ source: 'picker', files });
  };

  const restoreCapabilityMenuFocus = useCallback(() => {
    if (!isComposerEditorFocused(rootRef.current)) capabilityButtonRef.current?.focus();
  }, []);

  const selectExpert = (expert) => {
    if (!expert?.id || !onSelectExpert) return;
    setCapabilityOpen(false);
    setExpertSubmenuOpen(false);
    setExpertSubmenuPosition(null);
    restoreCapabilityMenuFocus();
    onSelectExpert(expert);
  };

  const openMoreExperts = () => {
    setCapabilityOpen(false);
    setExpertSubmenuOpen(false);
    setExpertSubmenuPosition(null);
    restoreCapabilityMenuFocus();
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
    if (disabled || !onMediaFiles) return;
    const files = filesFromTransfer(event.dataTransfer, { source: 'drop' });
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
      requestDesktopFileDropFocus();
      acceptFileIntake({ source: 'drop', paths: uriPaths, files });
    }
    resetDragState();
  }, [
    acceptFileIntake,
    disabled,
    markNativeDropHover,
    nativeFilesystemMaterializerAvailable,
    onMediaFiles,
    resetDragState,
  ]);

  useImperativeHandle(ref, () => ({
    focus: () => ta.current?.focus(),
    getElement: () => rootRef.current,
    getComposerContent: () => serializeComposerGoal(
      valueRef.current,
      mergeEditorContent(ta.current?.getComposerContent?.() || contentRef.current),
      goalModeRef.current,
    ).content,
    setComposerContent: (content, options) => {
      const normalized = normalizeComposerContent(content) || composerContentFromText('');
      const editor = projectComposerGoal(composerContentText(normalized), normalized);
      updateValue(editor.text, editor.content, undefined, { goalMode: editor.goalMode });
      ta.current?.setComposerContent?.(composerContentWithoutImages(editor.content), options);
    },
    replaceText: (text) => {
      const editor = projectComposerGoal(text, composerContentFromText(text));
      updateValue(editor.text, editor.content, undefined, { goalMode: editor.goalMode });
      ta.current?.setComposerContent?.(editor.content);
    },
    clear: () => {
      const content = composerContentFromText('');
      updateValue('', content, undefined, { goalMode: false });
      ta.current?.setComposerContent?.(content);
      setHistPtr(-1);
      setEditedSinceHistory(false);
    },
    insertPathReference: (path, { directory = false } = {}) => {
      const insertion = insertPathReferenceAtCaret(value, composerSelection.end, path, {
        directory,
      });
      updateValue(insertion.text, undefined, { begin: composerSelection.end, end: composerSelection.end });
      setEditedSinceHistory(true);
      restorePathCaret(insertion.cursor);
      return insertion;
    },
    insertDirectoryReference: (relativePath) => {
      const insertion = insertPathReferenceAtCaret(value, composerSelection.end, relativePath, {
        directory: true,
      });
      updateValue(insertion.text, undefined, { begin: composerSelection.end, end: composerSelection.end });
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
    mergeEditorContent,
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
      const coordinateAuthorized = payload?.nativeLocation === true;
      let rawPaths = coordinateAuthorized ? payload.paths : payload;
      if (typeof rawPaths === 'string') {
        try { rawPaths = JSON.parse(rawPaths); } catch {
          fileDropDiagnostic('composer-rejected', {
            disabled: !!disabled, hover: false, ageMs: -1, count: 0, invalid: true,
          });
          return;
        }
      }
      const hover = nativeDropHoverRef.current;
      const ageMs = hover.ts > 0 ? Math.max(0, Date.now() - hover.ts) : -1;
      const count = Array.isArray(rawPaths) ? rawPaths.length : 0;
      if (!Array.isArray(rawPaths) || count === 0 || disabled || !onMediaFiles ||
          (!coordinateAuthorized && (!hover.active || ageMs > 1500))) {
        fileDropDiagnostic('composer-rejected', {
          disabled: !!disabled,
          hover: !!hover.active,
          ageMs,
          count,
          coordinateAuthorized,
        });
        return;
      }

      nativeDropHoverRef.current = { active: false, ts: 0 };
      resetDragState();
      const paths = localPathsFromDropPayload(rawPaths, HOST_OS);
      if (paths.length === 0) {
        fileDropDiagnostic('composer-rejected', {
          disabled: !!disabled,
          hover: !!hover.active,
          ageMs,
          count,
          normalizedCount: 0,
          coordinateAuthorized,
        });
        return;
      }
      // Drag-enter activation is best effort: the source window can still
      // own keyboard focus while Slate shows a caret. Retry at acceptance,
      // before async materialization; later completions must not foreground us.
      requestDesktopFileDropFocus();
      acceptFileIntake({ source: 'drop', paths });
    };
    const unregister = registerNativeComposerFileDrop(rootRef.current, handler);
    window.__aceComposerAcceptFileDrop = handler;
    return () => {
      unregister();
      if (window.__aceComposerAcceptFileDrop !== handler) return;
      try { delete window.__aceComposerAcceptFileDrop; }
      catch { window.__aceComposerAcceptFileDrop = undefined; }
    };
  }, [
    acceptFileIntake,
    disabled,
    nativeFilesystemMaterializerAvailable,
    onMediaFiles,
    resetDragState,
  ]);

  useEffect(() => {
    if (disabled && capabilityOpen) setCapabilityOpen(false);
  }, [capabilityOpen, disabled]);

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

  // 蜂群模式（星型）/（网状）两项互斥;再点已选中的一项即关闭。
  const activeSwarmMode = normalizeSwarmMode(swarmMode);
  const chooseSwarmMode = useCallback((mode) => {
    setCapabilityOpen(false);
    closeExpertSubmenu(false);
    onSwarmModeChange?.(pickSwarmMode(activeSwarmMode, mode));
    requestComposerCaretRestore();
  }, [
    activeSwarmMode,
    closeExpertSubmenu,
    onSwarmModeChange,
    requestComposerCaretRestore,
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
      if (event.key !== 'Escape') return;
      if (expertSubmenuOpen) {
        // Mouse-opened submenus leave keyboard events with the editor.
        // Keyboard-opened submenus handle Escape on their focused menu item.
        if (isComposerEditorFocused(rootRef.current)) {
          event.preventDefault();
          event.stopPropagation();
          closeExpertSubmenu(false);
        }
        return;
      }
      closeCapabilityMenu();
      window.requestAnimationFrame(restoreCapabilityMenuFocus);
    };

    document.addEventListener('click', closeFromPointer, true);
    document.addEventListener('keydown', onKeyDown, true);
    window.addEventListener('blur', closeCapabilityMenu);
    return () => {
      document.removeEventListener('click', closeFromPointer, true);
      document.removeEventListener('keydown', onKeyDown, true);
      window.removeEventListener('blur', closeCapabilityMenu);
    };
  }, [capabilityOpen, closeExpertSubmenu, expertSubmenuOpen, restoreCapabilityMenuFocus]);

  const handleComposerChange = (next, editorContent) => {
    const content = mergeEditorContent(editorContent);
    const textChanged = isUserComposerEdit({ nextValue: next, currentValue: valueRef.current });
    const contentChanged = composerContentSignature(content) !== composerContentSignature(contentRef.current);
    if (!textChanged && !contentChanged) return;
    const edited = composerDraftEditFingerprint(next, content)
      !== composerDraftEditFingerprint(valueRef.current, contentRef.current);
    if (edited) {
      // A picker/drag focus request belongs to the old draft selection. Once
      // editing resumes, a later upload/layout update must not replay it.
      caretRestoreUntilRef.current = 0;
      caretRestoreSelectionRef.current = null;
      clearCaretRestoreSchedule();
    }
    updateValue(next, content);
    if (edited) setEditedSinceHistory(next.length > 0 || !!content?.parts?.length);
  };

  // 用户粘贴 / 拖放的超长文本经父组件变成粘贴块,编辑器文本不变,上面的
  // handleComposerChange 不会被调用。粘贴本身就是编辑:折叠成功即置 editedSinceHistory,
  // 否则翻历史途中粘贴一块后再按上箭头,历史条目会把刚粘贴的块整体替换掉。
  // 上箭头翻旧历史的折叠(deferUpload)直接调 onLargeTextPaste,不经这里。
  const onLargeTextPasteRef = useRef(onLargeTextPaste);
  onLargeTextPasteRef.current = onLargeTextPaste;
  const handleEditorLargeTextPaste = useCallback((text) => {
    const handled = onLargeTextPasteRef.current?.(text);
    if (handled !== false) setEditedSinceHistory(true);
    return handled;
  }, []);

  // 翻到旧超长历史时暂存的粘贴文件块(「待上传」):用户在该条目上开始编辑才上传,
  // 即 editedSinceHistory 从 false 变 true 的那一刻。没有暂存块时父组件什么也不做。
  const onCommitDeferredPastesRef = useRef(onCommitDeferredPastes);
  onCommitDeferredPastesRef.current = onCommitDeferredPastes;
  useEffect(() => {
    if (editedSinceHistory) onCommitDeferredPastesRef.current?.();
  }, [editedSinceHistory]);

  const handleComposerSelection = useCallback((selection) => {
    setComposerSelection(selection);
    if (caretRestoreUntilRef.current) caretRestoreSelectionRef.current = selection;
  }, []);

  const onKey = (e) => {
    // 下拉打开时,Enter / Tab / Esc / 方向键 由 SlashDropdown 在捕获阶段处理。
    // 这里只处理常规情况。
    if (shouldNavigateInputHistory({
      key: e.key,
      value: draftValue,
      editedSinceHistory,
      historyLength: history.length,
      historyPointer: histPtr,
      // 输入框里有粘贴块 / 附件时编辑器为空不代表输入框为空,不能被历史条目替换。
      hasNonTextContent: pasteBlocks.length > 0 || activeAttachmentItems.length > 0,
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
        updateValue('', composerContentFromText(''), undefined, { goalMode: false });
      } else {
        setHistPtr(next);
        const entry = historyEntries[next];
        const content = normalizeComposerContent(entry?.composer_content || entry) || composerContentFromText(history[next] || '');
        const entryText = composerContentText(content);
        if (onLargeTextPaste && legacyTextNeedsFold(entryText, content)) {
          // 本版之前的超长历史(如 f300 那条 24 MB):编辑器置空,正文走粘贴块分类。
          // 落文件时只在内存暂存、显示「待上传」,不上传 —— 否则每按一次上箭头就重传一次。
          updateValue('', composerContentFromText(''), undefined, { goalMode: false });
          onLargeTextPaste(entryText, { deferUpload: true });
        } else {
          updateValue(entryText, content, undefined, { goalMode: false });
        }
      }
      setEditedSinceHistory(false);
      return;
    }
  };

  const actionState = getInputBarActionState({ value: draftValue, disabled, busy, hasExtras, submitting, canRetryLastUserMessage, queuePaused });
  const stopControl = getGoalStopControlState({ busy, stopping });
  const composerSpacingClass = isHero ? 'px-4 pt-3 pb-1 text-[14px]' : 'px-3 pt-2 pb-1 text-[13px]';
  const hasInlineContexts = otherContextItems.length > 0;
  const capabilityControl = (
    <div ref={capabilityMenuRef} className="relative shrink-0 flex items-center">
      <button
        ref={capabilityButtonRef}
        type="button"
        disabled={disabled}
        className="w-7 h-7 rounded-full flex items-center justify-center text-fg-mute hover:bg-surface-hi hover:text-fg disabled:opacity-50"
        onClick={() => setCapabilityOpen((open) => !open)}
        title="添加能力或上下文"
        aria-label="添加能力或上下文"
      >
        <VsIcon name="add" size={15} />
      </button>
      {capabilityOpen && (
        <div
          data-composer-capability-menu="true"
          data-ace-native-overlay="overlap"
          role="menu"
          className="absolute left-0 bottom-8 z-50 w-52 py-1 rounded-lg border border-border bg-surface ace-shadow"
        >
          <button
            type="button"
            role="menuitemcheckbox"
            aria-checked={goalMode}
            className={clsx(
              'w-full h-8 px-2 flex items-center gap-2 text-left text-[13px] hover:bg-surface-hi',
              goalMode ? 'bg-accent-bg text-accent' : 'text-fg',
            )}
            onPointerEnter={() => closeExpertSubmenu(false)}
            onClick={() => changeGoalMode(!goalMode)}
          >
            <VsIcon name="Goal" size={15} className="shrink-0" />
            <span className="min-w-0 flex-1 truncate">目标</span>
          </button>
          {['star', 'mesh'].map((mode) => (
            <button
              key={mode}
              type="button"
              role="menuitemradio"
              data-swarm-mode={mode}
              aria-checked={activeSwarmMode === mode}
              className={clsx(
                'w-full h-8 px-2 flex items-center gap-2 text-left text-[13px] hover:bg-surface-hi',
                activeSwarmMode === mode ? 'bg-accent-bg text-accent' : 'text-fg',
              )}
              onPointerEnter={() => closeExpertSubmenu(false)}
              onClick={() => chooseSwarmMode(mode)}
            >
              <SwarmModeIcon size={15} className="shrink-0" />
              <span className="min-w-0 flex-1 truncate">{swarmModeLabel(mode)}</span>
              {activeSwarmMode === mode && <VsIcon name="check" size={12} className="shrink-0 opacity-70" />}
            </button>
          ))}

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
              onClick={(event) => {
                if (expertSubmenuOpen) closeExpertSubmenu(false);
                else openExpertSubmenu(event.detail === 0);
              }}
            >
              <VsIcon name="expert" size={14} />
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
          title={mainComposer ? withAppShortcutHint(stopControl.title, 'stop') : stopControl.title}
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
            'ace-composer-send px-2 h-7 rounded-md text-[11px] transition flex items-center gap-1',
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
          data-composer-action={actionState.mode}
          aria-label={actionState.submitLabel}
          className={clsx(
            'ace-composer-send w-7 h-7 rounded-full flex items-center justify-center transition',
            actionState.canSubmit
              ? 'bg-accent text-white hover:opacity-90'
              : 'bg-surface-hi text-fg-mute cursor-default',
          )}
          title={actionState.submitTitle}
        >
          {/* 队列暂停 + 空输入框:按钮语义是「继续」,图标换成播放三角与横幅上的一致 */}
          <VsIcon
            name={actionState.mode === 'resume' ? 'run' : 'send'}
            size={14}
            mono={false}
            className={actionState.canSubmit ? 'ace-icon-on-accent' : ''}
          />
        </button>
      )}
    </>
  );

  return (
    <div className={clsx(
      'ace-inputbar-layer',
      isHero ? 'ace-inputbar-hero' : 'px-2.5 py-2 bg-surface shrink-0',
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
        'ace-composer-card relative bg-surface transition',
        isHero ? 'ace-inputbar-hero-card rounded-2xl' : 'rounded-xl',
        dragActive && 'is-drag-active',
      )}
      ref={rootRef}
      data-main-composer={mainComposer ? 'true' : undefined}
      data-native-file-drop-disabled={disabled ? 'true' : undefined}
      onPointerDownCapture={(event) => preserveComposerFocusOnPointerDown(event, rootRef.current)}
      onDragEnter={fileDropManagedExternally ? undefined : handleDragEnter}
      onDragOver={fileDropManagedExternally ? undefined : handleDragOver}
      onDragLeave={fileDropManagedExternally ? undefined : handleDragLeave}
      onDrop={fileDropManagedExternally ? undefined : handleDrop}
      >
        {isHero && <span ref={attentionRingRef} className="ace-composer-attention-ring" aria-hidden="true" />}
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
            items={commandItems}
            query={commandQuery?.query || ''}
            onSelect={handleSelectCommand}
            onClose={() => setDropdownClosed(true)}
          />
        )}
        {imageAttachments.length > 0 && (
          <div className={clsx(
            'px-3 pt-3 flex flex-wrap items-start gap-2',
            isHero && 'px-4',
          )}>
            {imageAttachments.map((item, index) => {
              const context = composerAttachmentContext(item, index);
              const linkPath = context.sourcePath || context.path;
              const mimeType = String(item?.mime_type || item?.mimeType || '');
              return (
                <div
                  key={context.key}
                  data-composer-image-preview="true"
                  data-desktop-attachment-id={context.id}
                  data-desktop-attachment-name={context.name}
                  data-desktop-attachment-url={context.url || undefined}
                  data-desktop-attachment-path={linkPath || undefined}
                  data-desktop-attachment-preview-url={context.url || undefined}
                  data-desktop-attachment-copy-image-url={context.url || undefined}
                  data-desktop-attachment-mime-type={mimeType || undefined}
                  data-desktop-attachment-kind="image"
                  data-desktop-attachment-mutable="true"
                  aria-busy={item?.uploading || undefined}
                  className={clsx(
                    'group relative w-[86px] h-[86px] sm:w-24 sm:h-24 shrink-0 overflow-hidden rounded-lg border border-border bg-bg',
                    context.url && 'cursor-zoom-in hover:border-accent-soft',
                  )}
                  title={context.sourcePath || context.name}
                  role={context.url ? 'button' : undefined}
                  tabIndex={context.url ? 0 : undefined}
                  onClick={context.url
                    ? () => setAttachmentPreview({ src: context.url, alt: context.name })
                    : undefined}
                  onKeyDown={(event) => {
                    if (event.target !== event.currentTarget || !context.url) return;
                    if (event.key !== 'Enter' && event.key !== ' ') return;
                    event.preventDefault();
                    setAttachmentPreview({ src: context.url, alt: context.name });
                  }}
                >
                  {context.url ? (
                    <img
                      src={context.url}
                      alt={context.name}
                      draggable={false}
                      className="block w-full h-full object-cover bg-bg"
                    />
                  ) : (
                    <div className="w-full h-full flex items-center justify-center text-fg-mute bg-bg">
                      <FileTypeIcon path={context.name} size={24} />
                    </div>
                  )}
                  <button
                    type="button"
                    className="absolute right-[5px] top-[5px] w-[17px] h-[17px] rounded-full bg-black/75 hover:bg-black/85 text-white flex items-center justify-center"
                    onClick={(event) => {
                      event.stopPropagation();
                      removeAttachment(context.key);
                    }}
                    aria-label="移除附件"
                  >
                    <VsIcon name="close" size={8} />
                  </button>
                </div>
              );
            })}
          </div>
        )}
        {pasteCards.length > 0 && (
          <div
            data-composer-pasted-text-strip="true"
            className={clsx(
              'px-3 pt-3 flex flex-wrap items-start gap-2',
              isHero && 'px-4',
            )}
          >
            {pasteCards.map((card) => (
              <PastedTextCard
                key={card.block.id}
                title={card.title}
                sizeBytes={card.sizeBytes}
                status={card.status}
                removable={!disabled}
                onOpen={() => openPasteBlock(card)}
                onRemove={() => removePasteBlock(card.block.id)}
                onRetry={() => onRetryPasteUpload?.(card.block.part.key || card.block.id)}
              />
            ))}
          </div>
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
            syncKey={fileIntakeScope}
            commands={commands}
            composerContent={editorContent}
            attachments={editorAttachmentItems}
            onChange={handleComposerChange}
            onKeyDown={onKey}
            onCompositionStart={handleCompositionStart}
            onCompositionEnd={handleCompositionEnd}
            onSelectionChange={handleComposerSelection}
            isComposingKeyEvent={isComposingKeyEvent}
            onSubmit={submit}
            onPreviewAttachment={previewComposerAttachment}
            onRemoveAttachment={removeAttachment}
            onPasteFilesystemItems={handleFilesystemPaste}
            onLargeTextPaste={onLargeTextPaste ? handleEditorLargeTextPaste : undefined}
            allowNativeFilesystemDrop={NATIVE_FILE_DROP}
            disabled={disabled}
            placeholder={placeholder}
            className={clsx(
              'ace-rich-composer-input relative w-full bg-transparent border-0 outline-none leading-[20px] font-sans text-fg whitespace-pre-wrap break-words',
              'aria-disabled:opacity-50 aria-disabled:cursor-not-allowed',
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
          goalMode={goalMode}
          goalDisabled={disabled}
          onDisableGoal={() => changeGoalMode(false)}
          contexts={inlineContextControls}
          actions={submitControls}
          onCaptureComposerSelection={() => {
            if (!isComposerEditorFocused(rootRef.current)) return null;
            const selection = captureComposerTextareaSelection(ta.current);
            return () => restoreComposerTextareaCaret({
              textareaElement: ta.current,
              rootElement: rootRef.current,
              selection,
            });
          }}
          expertId={selectedExpertId}
          expertName={selectedExpertName}
          expertType={selectedExpertType}
          swarmMode={activeSwarmMode}
          onDisableSwarm={() => onSwarmModeChange?.('off')}
          expertRemoving={expertRemoving}
          onRemoveExpert={onRemoveExpert}
          pendingExpertName={pendingExpertName}
          pendingExpertType={pendingExpertType}
        />
      </div>
      <ImageLightbox preview={attachmentPreview} onClose={() => setAttachmentPreview(null)} />
      {openPasteCard ? (
        <PastedTextDialog
          key={openPasteCard.block.id}
          title={openPasteCard.title}
          source={openPaste.source}
          readOnly={disabled || typeof onReplacePasteBlock !== 'function'}
          loader={attachmentTextLoader}
          onSave={(next) => onReplacePasteBlock?.(openPasteCard.block.id, next)}
          onReplace={(next) => onReplacePasteBlock?.(openPasteCard.block.id, next)}
          onClose={() => setOpenPaste(null)}
        />
      ) : null}
    </div>
  );
});
