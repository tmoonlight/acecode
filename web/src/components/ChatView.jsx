// 主聊天视图:头部(会话名 + 状态 badge)+ 消息流 +
// 内嵌会话控制的 InputBar。
//
// 消息流是 items 数组,每个 item 形如:
//   { kind: 'msg' | 'tool' | 'task_complete', id, role?, content?, ts?, streaming?, tool? }
// 工具事件用 toolBlocks 单独的 Map 存进度态,完成时 tool 卡片切到 summary。
//
// 没有 sessionId 时显示 Codex 风格新任务主页(首条消息提交时才创建 session)。

import {
  Component,
  Suspense,
  lazy,
  useCallback,
  useEffect,
  useLayoutEffect,
  useMemo,
  useRef,
  useState,
  useSyncExternalStore,
} from 'react';
import { flushSync } from 'react-dom';
import { appendComposerImageAttachments, appendPasteFileAttachments } from '../lib/composerImagePresentation.js';
import {
  PASTED_TEXT_UPLOAD_TIMEOUT_MS,
  WORKSPACE_DRAFT_STORE,
  appendPastedTextPart,
  appendPastedTextToSubmission,
  homeDraftAttachmentScope,
  inputHistoryTextForPayload,
  legacyFoldUploadPending,
  legacyTextNeedsFold,
  normalizePastedText,
  pastedTextFileMeta,
  pastedTextFileName,
  pastedTextUploadBody,
  payloadWithImportedPastes,
  planPastedTextInsertion,
  reconcileHomeDraftUpload,
  registerPastedTextFile,
  removePastedTextPart,
  replacePasteBlock,
  replacePastedTextPart,
  sessionTitleSeedForPayload,
  workspaceDraftPasteRefs,
} from '../lib/pastedText.js';
import { createApi } from '../lib/api.js';
import { connection } from '../lib/connection.js';
import { tr } from '../i18n/index.js';
import {
  fileSourcePath,
  fileSourceReference,
} from '../lib/composerFileTransfer.js';
import {
  clearComposerAttachmentReservations,
  composerAttachmentFilesForLocalIds,
  createComposerAttachmentReservations,
  releaseComposerAttachmentFile,
  reserveComposerAttachmentFiles,
} from '../lib/composerAttachmentReservations.js';
import { ActivityLine } from './ActivityLine.jsx';
import { InputBar } from './InputBar.jsx';
import InteractiveHomeLogo from './InteractiveHomeLogo.jsx';
import { SelectionActionPopover } from './SelectionActionPopover.jsx';
import { AnchoredMenu } from './AnchoredMenu.jsx';
import { ExpertPickerDialog } from './ExpertCatalog.jsx';
import { QueueCardList } from './QueueCardList.jsx';
import { TaskSuggestionCards } from './TaskSuggestionCards.jsx';
import { SideChatWindow } from './SideChatWindow.jsx';
import { createSideChatController } from '../lib/sideChatController.js';
import '../styles/side-chat.css';
import { GitSessionPill } from './GitSessionPill.jsx';
import { SessionTitleBar } from './SessionTitleBar.jsx';
import { LspIndicator } from './LspIndicator.jsx';
import { QuestionPicker } from './QuestionPicker.jsx';
import { PermissionCard } from './PermissionCard.jsx';
import { StickyUserContext } from './StickyUserContext.jsx';
import { AttachmentTextLoaderContext } from './AttachmentTextLoaderContext.jsx';
import { SessionContentLoading } from './SessionContentLoading.jsx';
import { sessionContentLoadingPhase } from '../lib/sessionContentLoading.js';
import { SidePanel } from './SidePanel.jsx';
import { SubagentPanel } from './SubagentPanel.jsx';
import { TranscriptItems } from './TranscriptItems.jsx';
import { TranscriptHistoryBoundary } from './TranscriptHistoryBoundary.jsx';
import { useTranscriptHistory } from '../lib/useTranscriptHistory.js';
import { PreviewDetailsPanel } from './PreviewDetailsPanel.jsx';
import { Modal } from './Modal.jsx';
import { CreateProjectModal } from './CreateProjectModal.jsx';
import { ChangeGlassDock } from './ChangeReview.jsx';
import { TurnFileList } from './TurnFileList.jsx';
import { toast } from './Toast.jsx';
import { clsx, formatBytes } from '../lib/format.js';
import {
  aggregateHunksFromMessages,
  changeGroupsSignature,
  collectHunkMessagesFromItems,
  collectTurnChangeSetsFromItems,
  latestTurnSuccessfulChangedFiles,
  summarizeChangeGroups,
} from '../lib/sessionChanges.js';
import { forkRestoredPrompt } from '../lib/sessionFork.js';
import { stableBySignature } from '../lib/changeReviewStability.js';
import {
  acceptedQueuedInputEvent,
  acceptedUserInputEvent,
  beginQueuedGuidance,
  buildQueuedMessageItems,
  cancelQueuedInput,
  completeQueuedInputForMessage,
  createChatInputQueueState,
  enqueueQueuedInput,
  shouldDrainQueuedInput,
  shouldPauseQueuedInputAfterAbort,
  finishQueuedGuidance,
  hasSendingQueuedInput,
  isQueuedInputPaused,
  markQueuedGuidanceAccepted,
  markQueuedInputCompleted,
  markQueuedInputFailed,
  markQueuedInputSending,
  nextQueuedInput,
  pauseQueuedInput,
  QUEUED_INPUT_STATE,
  queuedInputPause,
  queuedInputRequestPayload,
  resumeQueuedInput,
  retryQueuedInput,
  updateQueuedInputContent,
} from '../lib/chatInputQueue.js';
import {
  effectiveSwarmMode,
  normalizeSwarmMode,
  reconcileSwarmChoice,
  swarmModeForSubmission,
} from '../lib/swarmMode.js';
import { findStickyUserContext, sameStickyUserContext, scrollTopForStickySourceRow } from '../lib/stickyUserContext.js';
import { fetchCompletedTurnHistory, loadTranscriptHistory, useSessionTranscript } from '../lib/sessionTranscript.js';
import { sessionJumpMessagePosition } from '../lib/sessionJump.js';
import { trailingUserMessageRetryId } from '../lib/trailingUserMessageRetry.js';
import { createSingleWriterStore } from '../lib/singleWriterStore.js';
import { projectCollapsedTranscriptItems } from '../lib/transcriptProjection.js';
import {
  reconcileTranscriptWindowAnchorKey,
  windowTranscriptItems,
} from '../lib/transcriptWindow.js';
import { buildComposerHistoryEntries } from '../lib/inputHistoryNavigation.js';
import {
  completedTurnSelfHealEnabled,
  createCompletedTurnSelfHealScheduler,
  reconcileLatestCompletedTurn,
} from '../lib/transcriptSelfHeal.js';
import { usePreference } from '../lib/usePreference.js';
import { pickExistingWorkspace } from '../lib/workspacePicker.js';
import { refreshWorkspaceGitInfo } from '../lib/gitInfoCache.js';
import { createPendingActionGuard } from '../lib/pendingActionGuard.js';
import { homeComposerDraft } from '../lib/homeComposerDrafts.js';
import {
  composerContentAttachments, composerContentFromText, composerContentSignature,
  normalizeComposerContent, reconcileComposerContentAttachments,
} from '../lib/composerContent.js';
import {
  composerDraftSnapshot, composerDraftFingerprint, composerDraftEditFingerprint, mergeComposerAttachmentResources,
  removeComposerAttachmentReference,
  completeDetachedComposerUpload,
  composerContentForGuidance,
} from '../lib/composerDraft.js';
import { homeComposerScopedWorkspace } from '../lib/aiThemeCreation.js';
import {
  createPendingNewSessionFirstUserMessage,
  withPendingNewSessionFirstUserMessage,
} from '../lib/newSessionFirstUserMessage.js';
import { extractSessionReferences } from '../lib/sessionReference.js';
import {
  DEFAULT_HOME_WORKSPACE_SELECTION,
  HOME_WORKSPACE_SELECTION_STORAGE_KEY,
  homeWorkspaceOptionForHash,
  noHomeWorkspaceOption,
  readDesktopHomeWorkspaceHash,
  resolveHomeWorkspaceHash,
  validateHomeWorkspaceSelection,
  writeDesktopHomeWorkspaceHash,
} from '../lib/homeWorkspaceSelection.js';
import { bindDesktopComposerAutoFocus } from '../lib/composerCaretRestore.js';
import { useSubagentTasks } from '../lib/useSubagentTasks.js';
import { taskDisplayTitle } from '../lib/subagentTasks.js';
import {
  CONVERSATION_ACTIVITY_KIND,
  selectConversationActivity,
} from '../lib/conversationActivity.js';
import { normalizeTokenBudget } from '../lib/tokenBudget.js';
import { pickModelLoad } from '../lib/modelLoad.js';
import {
  expertDispatchDraftFromRef,
  normalizeExperts,
  normalizeExpertSwitchReceipt,
  resolveCanonicalExpertSwitchPoll,
  shouldApplyExpertSwitchResponse,
  shouldRequestExpertSwitch,
} from '../lib/expertComponents.js';
import { resolveRecentExperts } from '../lib/recentExperts.js';
import {
  modelDisplayLabel,
  isEmptyModelState,
  modelSelectValue,
  normalizeModelOptions,
  normalizeModelState,
  resolveHomeModelName,
  selectedModelName,
  sessionModelReloadFeedback,
  withCreateSessionPreferences,
} from '../lib/sessionModel.js';
import { composerReasoningOptions, nextReasoningEffort } from '../lib/modelReasoning.js';
import { useAppShortcuts } from '../lib/useAppShortcuts.js';
import { requestSavedModelReasoningSync } from '../lib/modelReasoningSync.js';
import { normalizePermissionMode, permissionModeOption } from '../lib/permissionMode.js';
import { ATTACHMENT_HARD_LIMIT_BYTES, normalizeImageFile } from '../lib/imageNormalize.js';
import { VsIcon } from './Icon.jsx';
import { commandWorkspaceHashForInput } from '../lib/slashCommandWorkspace.js';
import { consoleCwdForContext } from '../lib/consoleDock.js';
import {
  inputRouteForPayload,
  remoteControlSessionRefreshForCommand,
  sessionCreateOptionsForText,
} from '../lib/builtinCommandRouting.js';
import { buildCurrentSessionDesktopFeedbackPayload } from '../lib/desktopFeedback.js';
import { buildWorktreeIntent } from '../lib/gitSessionPill.js';
import { fileTreeRefreshKeyFromItems } from '../lib/fileTreeRefresh.js';
import { buildAssistantRunDirectives } from '../lib/assistantRunDirectives.js';
import { notifySessionListChanged } from '../lib/sessionListEvents.js';
import {
  DEFAULT_SUBAGENT_PANEL_WIDTH,
  MIN_CHAT_WIDTH,
  normalizeSubagentPanelWidth,
  solveSingleContentLayout,
  subagentPanelWidthRange,
} from '../lib/singleLayout.js';
import {
  activeConversationTurnIndex as resolveActiveConversationTurnIndex,
  activatedConversationTurnIndex as resolveActivatedConversationTurnIndex,
  buildConversationTurnPreviews,
  shouldShowConversationTurnScrubber,
} from '../lib/conversationTurnScrubber.js';
import {
  PREVIEW_TAB_TYPES,
  activePreviewTab,
  activatePreviewTab,
  closeOtherPreviewTabs,
  closePreviewTab,
  closePreviewTabsToRight,
  closeVisiblePreviewTabs,
  defaultBrowserTabTitle,
  discardFileTabDraft,
  openFileTab,
  openBrowserTab,
  openGitChangesTab,
  openSessionChangesTab,
  previewFileLocation,
  previewScopeKey,
  previewTabContext,
  refreshPreviewTab,
  reorderPreviewTab,
  sessionWorkingCwd,
  syncBrowserTabsForSession,
  updateGitChangesTab,
  updateFileTabDraft,
  updateSessionChangesTab,
  visiblePreviewTabs,
} from '../lib/previewTabs.js';
import {
  pickPreviewFile,
} from '../lib/desktopPreviewFilePicker.js';
import {
  editableFileConflict,
  editableFileError,
  saveEditableFileDraftBatch,
} from '../lib/editableFileDraft.js';
import { createUnsavedFileGuard, runAfterFileApproval } from '../lib/unsavedFileGuard.js';
import { sessionWorkbench } from '../lib/sessionWorkbench.js';
import { useWorkbenchState } from '../lib/useWorkbenchState.js';
import { transferPreviewTabs } from '../lib/previewTabs.js';
import {
  AGENT_BROWSER_STATE_EVENT,
  agentBrowserActivityFromItems,
  agentBrowserOwnerForSession,
  closeAgentBrowserPage,
  createAgentBrowserPage,
  hasNativeAgentBrowser,
  runAgentBrowserBridgeAction,
  selectAgentBrowserPage,
  waitForAgentBrowserPageReady,
} from '../lib/agentBrowser.js';
import {
  agentBrowserPageStore,
  agentBrowserPageStoreSnapshot,
  agentBrowserPageStoreSubscribe,
  agentBrowserPagesForSession,
  agentBrowserSessionTargetPageId,
  claimUnownedAgentBrowserPage,
  reconcileAgentBrowserPageStore,
} from '../lib/agentBrowserPages.js';
import { nextAutoPreviewRefresh } from '../lib/previewRefresh.js';
import {
  CHAT_TAIL_FOLLOW_STATE,
  chatScrollMetrics,
  nextChatTailFollowState,
  observeChatTailContent,
  shouldAutoFollowChatTail,
  shouldShowChatScrollToBottom,
} from '../lib/chatScrollFollow.js';
import {
  activityAnchorViewportTop,
  matchesProgrammaticActivityScroll,
  scrollTopForPreservedActivityAnchor,
} from '../lib/activityExpansionAnchor.js';
import {
  closeDesktopContextMenu,
  openDesktopContextMenu,
  CONTEXT_MENU_DELEGATE_EVENT,
  DESKTOP_CONTEXT_ACTION_EVENT,
  DESKTOP_CONTEXT_ACTIONS,
  SESSION_HEADER_CONTEXT_MENU_DELEGATE,
} from '../lib/desktopContextMenu.js';
import { resolveSessionTitleRename, sessionTitleRenameWorkspaceHash } from '../lib/sessionTitleRename.js';
import { normalizeReferencePath } from '../lib/pathReference.js';
import {
  createSelectionAnnotation,
  mergeSelectionAnnotations,
  normalizeComposerContext,
  SELECTION_PREVIEW_SELECTOR,
  selectionAnnotationPresentationMap,
  selectionContextFingerprint,
  selectionContextFromWindowSelection,
  selectionContextLocationKey,
  selectionContextsFromTranscriptItems,
  upsertSelectionContext,
} from '../lib/selectionChatContext.js';
import {
  selectionPointerViewportRect,
  selectionRangeViewportRect,
  selectionTargetViewportRect,
} from '../lib/selectionActionPopover.js';
import { clearPreviewSelection } from '../lib/inactiveSelection.js';
import {
  CHANGE_DOCK_DISMISSALS_STORAGE_KEY,
  dismissChangeDockSignature,
  dismissedDockSignatureFor,
  dockDismissalKey,
  hasCompletedTurnResult,
  isTodoDockSuppressed,
  todoDockSignature,
  validateDockDismissals,
} from '../lib/changeDockDismissal.js';

const LazyConversationTurnScrubber = lazy(
  () => import('./ConversationTurnScrubber.jsx'),
);
const LazyTrajectoryView = lazy(
  () => import('./trajectory/TrajectoryView.jsx').then((module) => ({
    default: module.TrajectoryView,
  })),
);

class ConversationTurnScrubberBoundary extends Component {
  state = { failed: false };

  static getDerivedStateFromError() {
    return { failed: true };
  }

  render() {
    return this.state.failed ? null : this.props.children;
  }
}

function isEditableElement(el) {
  if (!el || el === document.body || el === document.documentElement) return false;
  const tag = String(el.tagName || '').toLowerCase();
  return tag === 'input' || tag === 'textarea' || tag === 'select' || !!el.isContentEditable;
}

function fileToBase64(file) {
  return new Promise((resolve, reject) => {
    const reader = new FileReader();
    reader.onerror = () => reject(reader.error || new Error('read failed'));
    reader.onload = () => {
      const text = String(reader.result || '');
      const comma = text.indexOf(',');
      resolve(comma >= 0 ? text.slice(comma + 1) : text);
    };
    reader.readAsDataURL(file);
  });
}

// swarmMode: 要随这条消息写给服务端的会话级蜂群模式(swarmModeForSubmission),null = 不写。
function normalizeComposerPayload(text, attachments = [], contexts = [], swarmMode = null, composerContent = null) {
  const sessionReferences = extractSessionReferences(String(text || ''));
  const content = reconcileComposerContentAttachments(composerContent, attachments);
  const payload = {
    // 消息正文 = 编辑器文本 + 内联粘贴块(以 "\n\n" 拼在编辑器内容之后);文件块不进
    // 正文,走 attachments → [Attached file reference]。
    text: appendPastedTextToSubmission(sessionReferences.displayText, content),
    // 首页草稿附件(粘贴的文件块)留在 attachments 里并带 store / store_scope:这样
    // payloadHasExtras 原样就把「只有文件块」「/compact + 文件块」算作 extras;发送前
    // 由 materializeWorkspaceDraftPastes 这一个导入点复制成会话附件。
    attachments: attachments
      .filter((item) => item && !item.uploading && item.id)
      .map((item) => (item.store === WORKSPACE_DRAFT_STORE && item.store_scope
        ? { id: item.id, store: item.store, store_scope: item.store_scope }
        : { id: item.id })),
    contexts: contexts.map(normalizeComposerContext).filter(Boolean),
  };
  if (sessionReferences.references.length > 0) {
    payload.session_references = sessionReferences.references;
  }
  if (swarmMode) payload.swarm_mode = swarmMode;
  if (content) payload.composer_content = content;
  return payload;
}

// 粘贴文本分类结果为文件块时,按字节区间造 File(一次 TextEncoder 编码,各段直接
// subarray,不再重复编码);描述挂在 File 上(WeakMap),任何持有 File 的路径都能取回。
function pastedTextFilesForPlan(plan, now = new Date()) {
  const multi = plan.chunks.length > 1;
  return plan.chunks.map((chunk, index) => registerPastedTextFile(
    new File(
      [plan.bytes.subarray(chunk.start, chunk.end)],
      pastedTextFileName(now, multi ? index + 1 : 0),
      { type: 'text/plain' },
    ),
    chunk.paste,
  ));
}

function largePasteNotice(plan) {
  return `粘贴内容较大（${formatBytes(plan.byteLength)}），将分 ${plan.chunks.length} 段上传`;
}

function payloadWithAttachmentIds(payload, attachments = []) {
  const nextAttachments = Array.from(payload?.attachments || []);
  const seen = new Set(nextAttachments.map((item) => String(item?.id || '')).filter(Boolean));
  for (const attachment of Array.from(attachments || [])) {
    const id = String(attachment?.id || '');
    if (!id || seen.has(id)) continue;
    seen.add(id);
    nextAttachments.push({ id });
  }
  return {
    ...payload, attachments: nextAttachments,
    ...(payload.composer_content ? {
      composer_content: reconcileComposerContentAttachments(payload.composer_content, attachments),
    } : {}),
  };
}

function payloadHasExtras(payload) {
  return (Array.isArray(payload?.attachments) && payload.attachments.length > 0) ||
    (Array.isArray(payload?.contexts) && payload.contexts.length > 0) ||
    (Array.isArray(payload?.session_references) && payload.session_references.length > 0);
}

function nextSelectionContextId() {
  return `selection-${Date.now()}-${Math.random().toString(16).slice(2)}`;
}

function finiteMessageOrdinal(value) {
  const n = Number(value);
  return Number.isInteger(n) && n >= 0 ? n : null;
}

function searchJumpOrdinalFromRef(ref) {
  const match = ref?.searchMatch || ref?.search_match || null;
  return finiteMessageOrdinal(
    match?.messageOrdinal ??
    match?.message_ordinal ??
    ref?.messageOrdinal ??
    ref?.message_ordinal,
  );
}

function scrollTopForCenteredRow(container, row) {
  if (!container || !row) return 0;
  const containerRect = container.getBoundingClientRect();
  const rowRect = row.getBoundingClientRect();
  const target = container.scrollTop +
    rowRect.top - containerRect.top -
    Math.max(0, (container.clientHeight - rowRect.height) / 2);
  return Math.max(0, target);
}

function searchJumpTargetRow(container, ordinal, position = null) {
  if (!container || (ordinal === null && position === null)) return null;
  return Array.from(container.querySelectorAll('[data-chat-row="true"][data-chat-user-message="true"]'))
    .find((row) => row.getAttribute(position !== null ? 'data-chat-message-position' : 'data-chat-message-ordinal') === String(position ?? ordinal)) || null;
}

function collectRowMetrics(container) {
  if (!container) return [];
  const containerRect = container.getBoundingClientRect();
  return Array.from(container.querySelectorAll('[data-chat-row="true"]')).map((node) => {
    const rect = node.getBoundingClientRect();
    return {
      id: node.getAttribute('data-chat-item-id') || '',
      top: rect.top - containerRect.top + container.scrollTop,
      bottom: rect.bottom - containerRect.top + container.scrollTop,
    };
  });
}

function currentTurnActivityId(items, activeTurnId, sessionId) {
  const list = Array.isArray(items) ? items : [];
  for (let i = list.length - 1; i >= 0; i -= 1) {
    const item = list[i];
    if (item?.kind !== 'msg' || item.role !== 'user') continue;
    const id = item.id ?? item.messageId;
    if (id !== undefined && id !== null && String(id).trim()) return `user:${id}`;
  }
  return String(activeTurnId || sessionId || 'pending');
}

function ChatFileDropOverlay({ active }) {
  if (!active) return null;
  return (
    <div
      className="ace-chat-file-drop-overlay"
      data-chat-file-drop-overlay="true"
    >
      <div
        className="ace-chat-file-drop-prompt"
        role="status"
        aria-live="polite"
        aria-atomic="true"
      >
        {tr('fileDrop.releaseToAdd')}
      </div>
    </div>
  );
}

function chatFileDropEventIsInsideScope(event) {
  const scope = event?.currentTarget;
  const target = event?.target;
  return !!scope && !!target && scope.contains(target);
}

function normalizeSessionRef(sessionRef, sessionId) {
  if (sessionRef && typeof sessionRef === 'object') return sessionRef;
  if (typeof sessionRef === 'string' && sessionRef) return { sessionId: sessionRef };
  if (sessionId) return { sessionId };
  return null;
}

function sidebarSessionContextTarget(sessionId = '', workspaceHash = '', fallbackTarget = null) {
  if (!sessionId || typeof document === 'undefined') return fallbackTarget;
  const rows = Array.from(document.querySelectorAll('.ace-sidebar-session-row[data-desktop-session-id]'));
  const exact = rows.find((row) => (
    row.getAttribute('data-desktop-session-id') === sessionId
    && (!workspaceHash || row.getAttribute('data-desktop-session-workspace') === workspaceHash)
  ));
  if (exact) return exact;
  return rows.find((row) => row.getAttribute('data-desktop-session-id') === sessionId) || fallbackTarget;
}

function newSessionRefFrom(ref, sessionId) {
  const next = { sessionId };
  if (!ref || typeof ref !== 'object') return next;
  for (const key of ['workspaceHash', 'workspaceName', 'contextId', 'port', 'token', 'cwd']) {
    if (ref[key] != null) next[key] = ref[key];
  }
  return next;
}

function hasDesktopBridge() {
  return typeof window.aceDesktop_listWorkspaces === 'function';
}

function parseDesktopResult(value) {
  if (value == null) return value;
  if (typeof value !== 'string') return value;
  const text = value.trim();
  if (!text || text === 'null') return null;
  return JSON.parse(text);
}

function pathBaseName(path = '') {
  const normalized = String(path || '').replace(/\\/g, '/').replace(/\/+$/, '');
  if (!normalized) return '';
  return normalized.split('/').filter(Boolean).pop() || normalized;
}

function normalizeWorkspaceOption(workspace, fallbackIndex = 0) {
  if (!workspace || typeof workspace !== 'object') return null;
  const hash = workspace.hash || workspace.workspaceHash || workspace.workspace_hash || '';
  const cwd = workspace.cwd || '';
  const name = workspace.name || workspace.workspaceName || pathBaseName(cwd) || hash || `项目 ${fallbackIndex + 1}`;
  return {
    hash,
    cwd,
    name,
    active: !!workspace.active,
    contextId: workspace.contextId || workspace.context_id || 'default',
    port: workspace.port,
    token: workspace.token,
  };
}

function fallbackWorkspaceOption(ref, health) {
  const hash = ref?.workspaceHash || ref?.workspace_hash || '';
  const cwd = ref?.cwd || health?.cwd || '';
  return {
    hash: hash || '__local__',
    cwd,
    name: ref?.workspaceName || ref?.name || pathBaseName(cwd) || '当前项目',
    active: true,
    contextId: ref?.contextId || 'default',
    port: ref?.port,
    token: ref?.token,
  };
}

function isRealWorkspaceHash(hash) {
  return !!hash && hash !== '__local__';
}

const EXPERT_SWITCH_CANONICAL_POLL_ATTEMPTS = 6;
const EXPERT_SWITCH_CANONICAL_POLL_INTERVAL_MS = 160;
const FORK_ACTION_KEY = 'fork-session';

export function ChatView({ titleTarget, actionsTarget, children, sessionRef, sessionId, homeLogoEffectEnabled = true, homeComposerDrafts = {}, homeComposerAttentionRequest = 0, onHomeComposerDraftLoad, onHomeComposerDraftChange, onHomeComposerDraftAccepted, onHomeComposerDraftPatch, modelProfileRevision = 0, onSessionPromoted, onSessionExpertChanged, onHomeWorkspaceChange, onCommandWorkspaceChange, onConsoleCwdChange, onFindInConversation, onOpenModelSettings, health, autoFocusOnDesktopWindowFocus = false, onPermissionRequest, onQuestionRequest, permissionRequests = [], onPermissionDecision, questionRequest, onQuestionResolve, onPermissionModeChanged, onSubagentTasksChange, recentExpertIds = [], onRememberExpert, onInitialDraftConsumed, showSidePanel = false, sidePanelWidth = 280, onSidePanelResize, previewPanelWidth = 640, previewPanelAutoFit = false, onPreviewPanelResize, subagentPanelWidth = DEFAULT_SUBAGENT_PANEL_WIDTH, onSubagentPanelResize, onPreviewPanelVisibleChange, onRegisterPreviewLeaveGuard, sidePanelCollapsed = false, sidePanelListCollapsed = false, onToggleSidePanel, onRevealPreviewPanel, onToggleSidePanelList, onRevealSidePanelList, sidePanelMaximized = false, onToggleSidePanelMaximized, previewPresenting = false, onTogglePreviewPresentation, showAceCodeAvatar = false, messageAutoCollapse = true, nativeSurfacesVisible = true }) {
  const ref = useMemo(() => normalizeSessionRef(sessionRef, sessionId), [sessionRef, sessionId]);
  const sid = ref?.sessionId || ref?.id || '';
  const workbenchOwner = sessionWorkbench.ownerFor(ref);
  const sessionRuntimeUnavailable = ref?.resumePending === true || ref?.resumeFailed === true;
  const remoteControlBound = Boolean(ref?.remote_control_bound ?? ref?.remoteControlBound);
  const stagedExpertDraft = expertDispatchDraftFromRef(ref);
  const readOnlyExternalSession = !!(
    ref?.readOnly || ref?.read_only
  );
  const sidRef = useRef(sid);
  const api = useMemo(() => createApi(ref), [ref?.port, ref?.token, ref?.workspaceHash]);
  // 模型池负载:每 30s 轮询一次缓存快照,失败静默(监控不可用不影响主流程)。
  useEffect(() => {
    let alive = true;
    const fetchPool = () => {
      api.modelPoolStatus()
        .then((r) => { if (alive) setPoolModels(Array.isArray(r?.models) ? r.models : []); })
        .catch(() => {});
    };
    fetchPool();
    const id = window.setInterval(fetchPool, 30000);
    return () => { alive = false; window.clearInterval(id); };
  }, [api]);
  const completedTurnSelfHealScheduleRef = useRef(null);

  const transcript = useSessionTranscript(ref, {
    live: !readOnlyExternalSession,
    refreshIntervalMs: readOnlyExternalSession ? 1500 : 0,
    onPermissionRequest,
    onQuestionRequest: (payload) => {
      onQuestionRequest?.(payload);
    },
    onTurnCompleted: () => {
      completedTurnSelfHealScheduleRef.current?.schedule();
    },
    onError: (reason) => toast({
      kind: 'err',
      text: String(reason || '').startsWith('加载会话失败:')
        ? String(reason || '')
        : '错误:' + (reason || ''),
    }),
  });
  const {
    items,
    busy,
    activeTurnId,
    turns,
    title,
    status: transcriptStatus,
    loadState: transcriptLoadState,
    streamingId,
    abortPending,
    lastTurnOutcome,
    lastTerminalTurnId,
    trajectoryPartial,
    tokenUsage,
    goal,
    todos,
    todoSummary,
    activity,
    applyEvent,
    loadEarlier,
  } = transcript;

  const [subagentPanelOpen, setSubagentPanelOpen] = useState(false);
  const openSubagentPanelForSpawn = useCallback(() => {
    setSubagentPanelOpen(true);
  }, []);
  // 后台任务(spawn_subagent 子会话):数据 hook 常驻(运行中任务保持 WS
  // 订阅,权限/问题请求才能冒泡到主会话 UI);新调用开始时自动打开面板。
  // 子会话跟随父会话所在工作区:Desktop 的 daemon 同时服务多个工作区,子会话
  // 不在内存里时(Desktop 重启后、网状 agent 被换出)只能按这个工作区读盘。
  const subagentWorkspaceHash = isRealWorkspaceHash(ref?.workspaceHash || ref?.workspace_hash)
    ? (ref?.workspaceHash || ref?.workspace_hash)
    : '';
  const subagentTasks = useSubagentTasks(sid, {
    onSpawnStart: openSubagentPanelForSpawn,
  });
  // 当前视图可见的待答问题。提问挂起期间 composer dock 由提问框整体替换
  // (方案 A),所以它只驱动渲染,不再参与 submit 的分支判定。
  const questionForView = useMemo(() => {
    if (!questionRequest) return null;
    const reqSid = questionRequest.session_id || '';
    const ownerSid = questionRequest.owner_session_id || '';
    if (sid && ownerSid === sid) return questionRequest;
    if (!reqSid || (sid && reqSid === sid)) return questionRequest;
    // 后台任务(spawn_subagent 子会话)的 AskUserQuestion 冒泡到主会话回答,
    // transcript 窄条不承载交互(答案 payload 自带 session_id,路由回子会话)。
    if (sid && subagentTasks.tasks.some((t) => t.id === reqSid)) return questionRequest;
    return null;
  }, [questionRequest, sid, subagentTasks.tasks]);

  const questionOriginLabel = useMemo(() => {
    if (questionForView?.origin_label) return questionForView.origin_label;
    const reqSid = questionForView?.session_id || '';
    if (!reqSid || reqSid === sid) return '';
    const task = subagentTasks.tasks.find((t) => t.id === reqSid);
    return task ? `来自后台任务:${taskDisplayTitle(task)}` : '';
  }, [questionForView, sid, subagentTasks.tasks]);

  const [trajectoryOpen, setTrajectoryOpen] = useState(false);
  // 聊天流「调用了 N 个智能体」分组点某个智能体 → 打开面板并定位其 transcript。
  // focus.n 单调递增,让同一 sessionId 的重复点击也能触发 SubagentPanel 内 effect。
  const [subagentFocus, setSubagentFocus] = useState(null);
  useEffect(() => {
    setSubagentPanelOpen(false);
    setSubagentFocus(null);
    setTrajectoryOpen(false);
  }, [sid]);
  const openSubagentTranscript = useCallback((sessionId) => {
    if (!sessionId) return;
    setSubagentPanelOpen(true);
    setSubagentFocus((prev) => ({ id: sessionId, n: (prev?.n || 0) + 1 }));
  }, []);
  // sessionId → 任务,给分组卡片解析实时标题/运行态。
  const subagentTasksById = useMemo(
    () => new Map(subagentTasks.tasks.map((t) => [t.id, t])),
    [subagentTasks.tasks]);
  // 上报给 App:子任务 id → 标题映射,用于 question 请求的可见性放宽与
  // 权限/问题弹窗的「来自后台任务」来源标记。
  const onSubagentTasksChangeRef = useRef(onSubagentTasksChange);
  useEffect(() => { onSubagentTasksChangeRef.current = onSubagentTasksChange; }, [onSubagentTasksChange]);
  useEffect(() => {
    onSubagentTasksChangeRef.current?.({
      parentId: sid,
      titles: Object.fromEntries(
        subagentTasks.tasks.map((t) => [t.id, taskDisplayTitle(t)])),
    });
  }, [sid, subagentTasks.tasks]);
  const selfHealEnabled = completedTurnSelfHealEnabled(health);
  const selfHealRuntimeRef = useRef({
    sid: '',
    api: null,
    enabled: false,
    isLive: false,
  });
  const selfHealTranscriptRef = useRef({
    getState: null,
    updateState: null,
  });
  const selfHealSchedulerRef = useRef(null);
  if (!selfHealSchedulerRef.current) {
    selfHealSchedulerRef.current = createCompletedTurnSelfHealScheduler({
      getEnabled: () => selfHealRuntimeRef.current.enabled === true,
      getSessionId: () => selfHealRuntimeRef.current.sid,
      getIsLive: () => selfHealRuntimeRef.current.isLive === true,
      getState: () => selfHealTranscriptRef.current.getState?.() || null,
      isVisible: () => (
        typeof document === 'undefined' || document.visibilityState !== 'hidden'
      ),
      fetchCanonicalHistory: (sessionId) => (
        fetchCompletedTurnHistory(selfHealRuntimeRef.current.api, sessionId, selfHealTranscriptRef.current.getState?.())
      ),
      applyCanonicalHistory: (data, snapshot) => {
        const updateState = selfHealTranscriptRef.current.updateState;
        if (!updateState) return { replaced: false, reason: 'missing_state', state: null };
        // 读取当前状态、加载权威历史、比对最近一轮、覆写必须在同一次提交内完成。
        // 拆成“先 getState 再 updateState”会让这中间到达的流式增量被这份旧快照
        // 覆盖掉 —— 那正是长会话喷字丢片段的成因。
        let result = null;
        updateState((current) => {
          if (!current) {
            result = { replaced: false, reason: 'missing_state', state: current };
            return current;
          }
          const canonical = loadTranscriptHistory(current, data || {}).state;
          result = reconcileLatestCompletedTurn(current, canonical, snapshot);
          return result.replaced ? result.state : current;
        });
        return result || { replaced: false, reason: 'missing_state', state: null };
      },
    });
  }
  completedTurnSelfHealScheduleRef.current = selfHealSchedulerRef.current;
  useEffect(() => {
    selfHealRuntimeRef.current = {
      sid,
      api,
      enabled: selfHealEnabled,
      isLive: transcript.isLive === true,
    };
  }, [api, selfHealEnabled, sid, transcript.isLive]);
  useEffect(() => {
    selfHealTranscriptRef.current = {
      getState: transcript.getState,
      updateState: transcript.updateState,
    };
  }, [transcript.getState, transcript.updateState]);
  useEffect(() => () => {
    selfHealSchedulerRef.current?.cancel();
  }, [sid]);
  const [history,  setHistory]  = useState([]);
  const [homeWorkspaces, setHomeWorkspaces] = useState([]);
  const [homeWorkspaceHash, setHomeWorkspaceHash] = useState('');
  const [homeWorkspaceSelection, setHomeWorkspaceSelection] = usePreference(
    HOME_WORKSPACE_SELECTION_STORAGE_KEY,
    DEFAULT_HOME_WORKSPACE_SELECTION,
    validateHomeWorkspaceSelection,
  );
  const [homeSubmitting, setHomeSubmitting] = useState(false);
  const [pendingNewSessionFirstUserMessage, setPendingNewSessionFirstUserMessage] = useState(null);
  useEffect(() => {
    setPendingNewSessionFirstUserMessage((pending) => (
      pending && pending.sessionId !== sid ? null : pending
    ));
  }, [sid]);
  const [forkingMessageId, setForkingMessageId] = useState('');
  const forkActionGuardRef = useRef(createPendingActionGuard());
  const [projectDropdownOpen, setProjectDropdownOpen] = useState(false);
  const projectAnchorRef = useRef(null);
  const [createProjectOpen, setCreateProjectOpen] = useState(false);
  const [modelOptions, setModelOptions] = useState([]);
  const [modelListLoaded, setModelListLoaded] = useState(false);
  const [homeModelName, setHomeModelName] = useState('');
  const [homeReasoningEffort, setHomeReasoningEffort] = useState(null);
  const [reasoningSwitching, setReasoningSwitching] = useState(false);
  const reasoningRequestRef = useRef(0);
  const [experts, setExperts] = useState([]);
  const [homeExpertId, setHomeExpertId] = useState(() => String(
    ref?.expertId || ref?.expert_id || ref?.expert?.id || '',
  ));
  const [sessionExpertId, setSessionExpertId] = useState(() => String(
    ref?.expertId || ref?.expert_id || ref?.expert?.id || '',
  ));
  const [sessionExpertSnapshot, setSessionExpertSnapshot] = useState(
    () => (ref?.expert && typeof ref.expert === 'object' ? ref.expert : null),
  );
  const [expertSwitching, setExpertSwitching] = useState(false);
  const [expertDetaching, setExpertDetaching] = useState(false);
  const [pendingExpert, setPendingExpert] = useState(null);
  const pendingExpertRef = useRef(null);
  const latestExpertSwitchRequestRef = useRef({
    sequence: 0,
    expertId: '',
    settled: true,
  });
  const acceptedExpertSwitchRef = useRef(null);
  const expertSwitchQueueRef = useRef(Promise.resolve());
  const [expertPickerOpen, setExpertPickerOpen] = useState(false);

  const [modelState, setModelState] = useState(null);
  // 模型池负载快照(每 30s 轮询 /api/model-pool-status)。
  const [poolModels, setPoolModels] = useState([]);
  const [pendingModelName, setPendingModelName] = useState('');
  const [modelSwitching, setModelSwitching] = useState(false);
  const [modelRefreshing, setModelRefreshing] = useState(false);
  const [permissionMode, setPermissionMode] = useState('default');
  const [permissionSwitching, setPermissionSwitching] = useState(false);
  const [reviewRequest, setReviewRequest] = useWorkbenchState(workbenchOwner, 'reviewRequest', 0);
  const [fileLocateRequest, setFileLocateRequest] = useWorkbenchState(workbenchOwner, 'fileLocate', () => ({ path: '', token: 0 }));
  const [previewTabState, setPreviewTabState] = useWorkbenchState(workbenchOwner, 'previews', () => ({}));
  const getPreviewTabState = useCallback(() => sessionWorkbench.get(workbenchOwner, 'previews', () => ({})), [workbenchOwner]);
  const previewContextRef = useRef(null);
  const previewLeaveRequestRef = useRef(() => true);
  const [previewCloseConfirm, setPreviewCloseConfirm] = useState(null);
  const previewFileGuardRef = useRef(null);
  if (!previewFileGuardRef.current) {
    previewFileGuardRef.current = createUnsavedFileGuard(setPreviewCloseConfirm);
  }
  const [previewPanelHidden, setPreviewPanelHidden] = useWorkbenchState(workbenchOwner, 'previewHidden', false);
  const [dismissedDockSignatures, setDismissedDockSignatures] = usePreference(
    CHANGE_DOCK_DISMISSALS_STORAGE_KEY,
    {},
    validateDockDismissals,
  );
  // 本轮结果完成或下一轮提交时整体收起玻璃 dock:变更走 dismissChangeDock(持久化
  // 签名),todo 记会话内存级快照抑制 {sessionKey, signature}。真正的收起
  // 动作经 ref 中转 —— submit 的 useCallback 定义在 changeSignature /
  // todoSignature 之前(TDZ 不能进 deps),渲染期写 ref 是纯缓存,与
  // changeGroupsStableRef 同一模式。
  const [todoDockSuppression, setTodoDockSuppression] = useState(null);
  const dockAutoDismissRef = useRef(() => {});
  const changeDockRef = useRef(null);
  const [changeDockBottomPadding, setChangeDockBottomPadding] = useState(0);
  const [expandedActivityKeys, setExpandedActivityKeys] = useState(() => new Set());
  // 图像行默认展开,所以这里记的是「被用户收起的」行,与上面那个集合语义相反。
  const [collapsedMediaKeys, setCollapsedMediaKeys] = useState(() => new Set());
  const scrollRef = useRef(null);
  const [showScrollToBottom, setShowScrollToBottom] = useState(false);
  const transcriptContentRef = useRef(null);
  const tailFollowStateRef = useRef(CHAT_TAIL_FOLLOW_STATE.FOLLOWING);
  const tailFollowScrollRafRef = useRef({ first: 0, second: 0 });
  const activityExpansionAnchorRef = useRef(null);
  // 区分"用户滚动"与"流式渲染引起的 scrollTop 位移"用的上下文:上一次
  // scroll 事件的指标 + 指针是否按住(拖动滚动条/拖选期间的滚动算用户意图)。
  const scrollActivityRef = useRef({ prev: null, pointerActive: false });
  const lastUserTurnKeyRef = useRef('');
  const previewAutoRefreshRef = useRef({ sid: '', busy: false, completedTurnKey: '' });
  const inputRef = useRef(null);
  const [chatFileDropActive, setChatFileDropActive] = useState(false);
  const handleChatFileDragEnter = useCallback((event) => {
    if (!chatFileDropEventIsInsideScope(event)) return;
    inputRef.current?.handleFileDragEnter?.(event);
  }, []);
  const handleChatFileDragOver = useCallback((event) => {
    if (!chatFileDropEventIsInsideScope(event)) return;
    inputRef.current?.handleFileDragOver?.(event);
  }, []);
  const handleChatFileDragLeave = useCallback((event) => {
    if (!chatFileDropEventIsInsideScope(event)) return;
    inputRef.current?.handleFileDragLeave?.(event);
  }, []);
  const handleChatFileDrop = useCallback((event) => {
    if (!chatFileDropEventIsInsideScope(event)) return;
    inputRef.current?.handleFileDrop?.(event);
  }, []);
  const layoutRef = useRef(null);
  const subagentSplitRef = useRef(null);
  const [layoutWidth, setLayoutWidth] = useState(0);
  const sidePanelResizeActiveRef = useRef(false);
  const previewPanelResizeActiveRef = useRef(false);
  const subagentPanelResizeActiveRef = useRef(false);
  const subagentPanelResizeCleanupRef = useRef(null);
  const renderedPreviewPanelWidthRef = useRef(previewPanelWidth);
  const renderedSubagentPanelWidthRef = useRef(subagentPanelWidth);
  const [composerValue, setComposerText] = useState(
    () => stagedExpertDraft.text,
  );
  const [composerAttachments, setComposerAttachments] = useState([]);
  const [composerContent, setComposerContent] = useState(null);
  const composerContentRef = useRef(null);
  const composerAttachmentsRef = useRef([]);
  const homeDraftWorkspaceHashRef = useRef('');
  // 上箭头翻到旧超长历史时暂存在内存里、尚未上传的粘贴文件块({reserved, localIds}),
  // 只保留一组:开始编辑或发送时上传,翻到别的条目时释放。
  const deferredHistoryPasteRef = useRef(null);
  // 旧草稿 / fork 回填的超长文本折叠成文件块时暂存的块 { localIds }。这些块拿到服务端 id
  // 之前服务端草稿里的旧全文是唯一持久副本,首页与会话草稿保存都要跳过(legacyFoldUploadPending)。
  const legacyFoldGuardRef = useRef(null);
  const handleLargeTextPasteRef = useRef(null);
  const setComposerValue = useCallback((text, content = null) => {
    const normalized = normalizeComposerContent(content);
    composerValueRef.current = String(text || '');
    composerContentRef.current = normalized;
    setComposerText(String(text || ''));
    setComposerContent((previous) => composerContentSignature(previous) === composerContentSignature(normalized) ? previous : normalized);
  }, []);
  const [composerContexts, setComposerContexts] = useState([]);
  // 蜂群模式是会话级状态(add-mesh-swarm-mode):芯片 = 服务端值(transcript 的
  // swarmMode,来自 messages 快照与 session_updated)+ 尚未随消息提交的本地选择。
  const transcriptSwarmMode = normalizeSwarmMode(transcript.swarmMode);
  const [composerSwarmChoice, setComposerSwarmChoice] = useState(null);
  const composerSwarmMode = effectiveSwarmMode(composerSwarmChoice, transcriptSwarmMode);
  const changeComposerSwarmMode = useCallback((mode) => {
    setComposerSwarmChoice(reconcileSwarmChoice(normalizeSwarmMode(mode), transcriptSwarmMode));
  }, [transcriptSwarmMode]);
  // 服务端确认(或别处切到同一模式)后,本地选择清空,重新跟随服务端。
  useEffect(() => {
    setComposerSwarmChoice((choice) => reconcileSwarmChoice(choice, transcriptSwarmMode));
  }, [transcriptSwarmMode]);
  const [selectionPreview, setSelectionPreview] = useState(null);
  const [selectionAction, setSelectionAction] = useState(null);
  const [composerSubmitting, setComposerSubmitting] = useState(false);
  const retrySubmissionRef = useRef(null);
  useEffect(() => () => { retrySubmissionRef.current = null; }, [sid, api]);
  const [draftReadyKey, setDraftReadyKey] = useState('');
  const [acceptedHomeSubmission, setAcceptedHomeSubmission] = useState(null);
  const draftEditVersionRef = useRef(0);
  const draftSessionKeyRef = useRef('');
  const draftLastSavedRef = useRef({ key: '', text: '' });
  const draftSaveQueueRef = useRef(new Map());
  const composerValueRef = useRef('');
  const composerDirtyRef = useRef(false);
  const preserveComposerExtrasOnSessionChangeRef = useRef(false);
  const preserveComposerInputOnSessionChangeRef = useRef(false);
  const pendingForkComposerRef = useRef(null);
  const attachmentReservationsRef = useRef(null);
  if (!attachmentReservationsRef.current) {
    attachmentReservationsRef.current = createComposerAttachmentReservations();
  }
  const restoreComposerFocusAfterSubmitRef = useRef(false);
  const selectionPreviewFingerprintRef = useRef('');
  // 排队状态归 store 所有,渲染层只订阅。原来是 useState 镜像 + 一个可变引用
  // 的双写:被动 effect 把旧渲染快照写回引用,这中间提交的排队变更会被吞掉
  // (排队消息丢失或已取消的重新出现)。同 sessionTranscript 的实时状态修复。
  const queueStoreRef = useRef(null);
  if (queueStoreRef.current === null) {
    queueStoreRef.current = createSingleWriterStore(createChatInputQueueState());
  }
  const queueStore = queueStoreRef.current;
  const subscribeQueueStore = useCallback((listener) => queueStore.subscribe(listener), [queueStore]);
  const getQueueSnapshot = useCallback(() => queueStore.getState(), [queueStore]);
  const queueState = useSyncExternalStore(subscribeQueueStore, getQueueSnapshot, getQueueSnapshot);
  const [sideChatAnchor, setSideChatAnchor] = useState(null);
  const sideChat = useMemo(() => createSideChatController({
    startStream: (options) => api.streamSideChat(sid, options),
  }), [api, sid]);
  const sideChatState = useSyncExternalStore(
    sideChat.subscribe, sideChat.getSnapshot, sideChat.getSnapshot,
  );
  // Reset cancels the old request at the session boundary and is StrictMode-safe.
  useLayoutEffect(() => () => sideChat.reset(), [sideChat]);
  const turnInterruptInFlightRef = useRef(new Set());
  // GitSessionPill 的待生效意图(worktree 勾选 + 基线分支)。ref 不入 dep:
  // 只在发送首条消息那一刻读取,不驱动渲染。
  const gitPillIntentRef = useRef({ worktreeChecked: false, selectedBase: '' });
  const handleGitPillIntentChange = useCallback((intent) => {
    gitPillIntentRef.current = intent || { worktreeChecked: false, selectedBase: '' };
  }, []);
  // 本会话经首条消息创建的 worktree。响应直接带 path,侧栏刷新后再与
  // ref.worktree 合并,避免消息已进入 worktree 但文件面板仍停在主工作区。
  const [localWorktree, setLocalWorktree] = useState(null); // {sid, name, branch, path}
  const sessionWorktree = localWorktree && localWorktree.sid === sid
    ? {
        ...(ref?.worktree || {}),
        ...localWorktree,
        branch: localWorktree.branch || ref?.worktree?.branch || '',
        path: localWorktree.path || ref?.worktree?.path || '',
      }
    : (ref?.worktree || null);
  // 文件预览的根目录只取决于「这个会话在哪个目录里干活」,与它属不属于某个
  // workspace 无关。ref.cwd 对 no-workspace 会话恒为空(后端刻意清掉,那是
  // workspace 归属字段),真实目录走 workingCwd —— 没有它的时候整条预览链路
  // 从头就是死的:cwd 为空 → openFilePreview 直接 return,用户点自己刚生成的
  // 文件毫无反应。no-workspace 会话的文件同样在磁盘上,同样应该能预览。
  const homePanelWorkspace = !sid && !children
    ? homeWorkspaceOptionForHash(homeWorkspaces, homeWorkspaceHash)
    : null;
  const sessionIsNoWorkspace = homePanelWorkspace
    ? !!homePanelWorkspace.noWorkspace
    : !!(ref?.noWorkspace || ref?.no_workspace);
  const sidePanelCwd = sessionWorkingCwd({
    worktree: sessionWorktree,
    cwd: homePanelWorkspace
      ? homePanelWorkspace.cwd || ''
      : ref?.workingCwd || ref?.working_cwd || ref?.cwd || '',
    // health.cwd 是 daemon 进程自己的工作目录,只有当会话确实属于这个 daemon 服务的
    // workspace 时才是合理兜底。no-workspace 会话与它毫无关系,回退过去会让预览跑到一个
    // 无关目录里找文件并报「文件不存在」—— 一个看起来煞有介事的错误路径,比干脆打不开
    // 更难排查(实测把 cache/no-workspace/<id>/a.xlsx 找成了 N:/Users/shao/se/a.xlsx)。
    fallbackCwd: sessionIsNoWorkspace ? '' : (health?.cwd || ''),
  });
  const sidePanelFilesEnabled = !!sidePanelCwd;
  const drainRef = useRef(false);
  // 排队消息从 transcript 中分离出来,只喂给 InputBar 上方的 QueueCardList。
  // transcript 只渲染后端真实落库的消息,避免把"草稿/未发送"和"已发送"混在一起。
  const visibleQueuedItems = useMemo(() => buildQueuedMessageItems(queueState, sid), [queueState, sid]);
  // 当前会话的队列暂停态(用户中断回合后置位):卡片栈横幅与输入栏「继续」按钮共用。
  const queuePause = useMemo(() => queuedInputPause(queueState, sid), [queueState, sid]);
  const draftWorkspaceHash = isRealWorkspaceHash(ref?.workspaceHash) ? ref.workspaceHash : '';
  const draftSessionKey = sid ? `${draftWorkspaceHash}:${sid}` : '';
  const explicitHomeDraftWorkspaceHash = !sid && ref?.homeWorkspaceExplicit
    ? (isRealWorkspaceHash(ref?.workspaceHash) ? ref.workspaceHash : '')
    : null;
  const homeDraftWorkspaceHash = homeComposerScopedWorkspace(sid
    ? ''
    : (explicitHomeDraftWorkspaceHash ?? (
        isRealWorkspaceHash(homeWorkspaceHash) ? homeWorkspaceHash : ''
      )), sid ? '' : ref?.composerDraftScope);
  composerValueRef.current = composerValue;
  composerContentRef.current = composerContent;
  composerAttachmentsRef.current = composerAttachments;
  homeDraftWorkspaceHashRef.current = homeDraftWorkspaceHash;
  const rawItems = useMemo(
    () => withPendingNewSessionFirstUserMessage(
      items,
      pendingNewSessionFirstUserMessage,
      sid,
    ),
    [items, pendingNewSessionFirstUserMessage, sid],
  );
  // GitSessionPill 的 sessionStarted 判定在 submit 回调里读(ref 免 dep churn)。
  const rawItemsLengthRef = useRef(0);
  rawItemsLengthRef.current = rawItems.length;
  // 上下键翻的历史:per-cwd 输入历史 + 当前 transcript 会话中用户发过的消息
  const composerHistoryEntries = useMemo(
    () => buildComposerHistoryEntries({ cwdHistory: history, transcriptItems: rawItems }),
    [history, rawItems],
  );
  const composerHistory = useMemo(() => composerHistoryEntries.map((entry) => entry.text), [composerHistoryEntries]);
  const renderedItems = useMemo(
    () => projectCollapsedTranscriptItems(rawItems, {
      messageAutoCollapse,
      deferTrailingToolSummary: busy,
      ensureLiveActivity: busy,
      liveTurnId: currentTurnActivityId(rawItems, activeTurnId, sid),
    }),
    [rawItems, busy, activeTurnId, sid, messageAutoCollapse],
  );
  // 尾部窗口(渐进虚拟化,见 lib/transcriptWindow.js):大会话初始只渲染
  // 最近的一段投影行,DOM 行数从数百降到 ≤ INITIAL_TAIL_ITEMS。窗口状态
  // 用「首个可见行的稳定 key」锚定;anchorKey === undefined 表示该会话
  // 还没初始化,anchorKey === null 表示显式全量视图。render-phase 协调
  // 必须在当前投影切片前完成;useEffect 会先全量渲染一遍,虚拟化就白做了。
  const [transcriptWindow, setTranscriptWindow] = useState({ sid: '', anchorKey: undefined });
  const storedWindowAnchorKey = transcriptWindow.sid === sid
    ? transcriptWindow.anchorKey
    : undefined;
  const windowAnchorKey = reconcileTranscriptWindowAnchorKey(
    renderedItems,
    storedWindowAnchorKey,
  );
  if (transcriptWindow.sid !== sid || transcriptWindow.anchorKey !== windowAnchorKey) {
    setTranscriptWindow({ sid, anchorKey: windowAnchorKey });
  }
  const { visible: windowedItems, hiddenCount: windowHiddenCount } = useMemo(
    () => windowTranscriptItems(renderedItems, windowAnchorKey),
    [renderedItems, windowAnchorKey],
  );
  const windowHiddenCountRef = useRef(0);
  windowHiddenCountRef.current = windowHiddenCount;
  const historyPaging = useTranscriptHistory({
    sid, loadEarlier, getState: transcript.getState, scrollRef,
    hiddenCount: windowHiddenCount, anchorKey: windowAnchorKey,
    items: renderedItems, setWindow: setTranscriptWindow,
    onReview: () => {
      cancelActivityExpansionAnchor();
      cancelTailFollowScroll();
      setTailFollowFromAction({ type: 'review_pause' });
    },
  });
  const historyController = historyPaging.controller;
  const revealEarlierTranscript = historyController.reveal;
  const expandTranscriptWindow = historyController.expand;
  // 会话内查找(Ctrl+F)在 DOM 文本上搜索,窗口外的行搜不到 —— find 打开
  // 时直接全量展开(GlobalFindOverlay 广播的事件)。
  useEffect(() => {
    const handler = () => expandTranscriptWindow({ loadAll: true });
    window.addEventListener('acecode:conversation-find-open', handler);
    return () => window.removeEventListener('acecode:conversation-find-open', handler);
  }, [expandTranscriptWindow]);
  const lastUserTurnKey = useMemo(() => {
    for (let index = rawItems.length - 1; index >= 0; index -= 1) {
      const item = rawItems[index];
      if (item?.kind === 'msg' && item.role === 'user') {
        return String(item.messageId || item.id || index);
      }
    }
    return '';
  }, [rawItems]);
  // 决定每条 assistant 消息的 run 边界;ACECode 头像永久隐藏,空内容(且非
  // streaming)直接隐藏整行。详见 lib/assistantRunDirectives.js。
  const assistantRunDirectives = useMemo(
    () => buildAssistantRunDirectives(renderedItems, { deferLastFooter: busy }),
    [renderedItems, busy],
  );
  const itemsRef = useRef(renderedItems);
  const stickyRafRef = useRef(0);
  const conversationTurnRafRef = useRef(0);
  const conversationTurnsRef = useRef([]);
  const conversationTurnActivationRef = useRef(null);
  const searchJumpRetryRef = useRef({ frame: 0, timer: 0 });
  const [stickyUserContext, setStickyUserContext] = useState(null);
  const [conversationTurnState, setConversationTurnState] = useState({
    sid: '',
    turns: [],
  });
  const [activeConversationTurn, setActiveConversationTurn] = useState(-1);
  const preparedConversationTurns = useMemo(
    () => (
      conversationTurnState.sid === sid
        ? conversationTurnState.turns
        : []
    ),
    [conversationTurnState, sid],
  );
  const showConversationTurnScrubber = shouldShowConversationTurnScrubber(
    preparedConversationTurns,
  );
  const searchJumpOrdinal = useMemo(() => searchJumpOrdinalFromRef(ref), [ref]);
  const requestedSearchPosition = sessionJumpMessagePosition(ref);
  const searchTargetKey = `${sid}:${requestedSearchPosition ?? ''}:${searchJumpOrdinal ?? ''}`;
  const [resolvedSearchTarget, setResolvedSearchTarget] = useState(null);
  const searchJumpPosition = resolvedSearchTarget?.key === searchTargetKey
    ? resolvedSearchTarget.position : requestedSearchPosition;
  useEffect(() => {
    if (transcriptLoadState !== 'loaded' || (requestedSearchPosition === null && searchJumpOrdinal === null)) return undefined;
    let cancelled = false;
    loadEarlier(requestedSearchPosition !== null
      ? { messagePosition: requestedSearchPosition }
      : { messageOrdinal: searchJumpOrdinal }).then((position) => {
        if (!cancelled && position != null) setResolvedSearchTarget({ key: searchTargetKey, position: String(position) });
      });
    return () => { cancelled = true; };
  }, [transcriptLoadState, searchTargetKey, requestedSearchPosition, searchJumpOrdinal, loadEarlier]);

  useEffect(() => {
    if (!sid || transcriptLoadState !== 'loaded') return undefined;

    let cancelled = false;
    let frame = 0;
    let idle = 0;
    let timer = 0;
    const prepare = () => {
      if (cancelled) return;
      const nextTurns = buildConversationTurnPreviews(itemsRef.current, { busy });
      if (cancelled) return;
      setConversationTurnState({ sid, turns: nextTurns });
    };

    frame = window.requestAnimationFrame(() => {
      frame = 0;
      if (cancelled) return;
      if (typeof window.requestIdleCallback === 'function') {
        idle = window.requestIdleCallback(prepare, { timeout: 450 });
        return;
      }
      timer = window.setTimeout(prepare, 0);
    });

    return () => {
      cancelled = true;
      if (frame) window.cancelAnimationFrame(frame);
      if (idle && typeof window.cancelIdleCallback === 'function') {
        window.cancelIdleCallback(idle);
      }
      if (timer) window.clearTimeout(timer);
    };
  }, [busy, lastUserTurnKey, sid, transcriptLoadState]);

  const homeWorkspacePreferenceHash = homeWorkspaceSelection?.workspaceHash || '';
  const persistHomeWorkspaceHash = useCallback((hash = '') => {
    const workspaceHash = String(hash || '');
    setHomeWorkspaceSelection({ workspaceHash });
    writeDesktopHomeWorkspaceHash(workspaceHash).catch(() => {});
  }, [setHomeWorkspaceSelection]);

  const selectHomeWorkspace = useCallback((workspace) => {
    const selected = workspace?.noWorkspace ? noHomeWorkspaceOption() : workspace;
    const workspaceHash = String(selected?.hash || '');
    setHomeWorkspaceHash(workspaceHash);
    persistHomeWorkspaceHash(workspaceHash);
    onHomeWorkspaceChange?.(selected);
    setProjectDropdownOpen(false);
  }, [onHomeWorkspaceChange, persistHomeWorkspaceHash]);

  const handleProjectCreated = useCallback(async (workspace) => {
    const option = normalizeWorkspaceOption(workspace, homeWorkspaces.length);
    if (!option?.hash || !option?.cwd) {
      throw new Error('项目已创建，但返回的工作区信息不完整');
    }
    setHomeWorkspaces((previous) => [
      option,
      ...previous.filter((item) => item.hash !== option.hash),
    ]);
    selectHomeWorkspace(option);
    notifySessionListChanged({
      reason: 'project-created',
      workspaceHash: option.hash,
    });
    const directoryName = workspace?.directory_name || option.name;
    toast({
      kind: 'ok',
      text: workspace?.sanitized
        ? `已创建项目：${directoryName}（目录名已自动转换）`
        : `已创建项目：${directoryName}`,
    });
  }, [homeWorkspaces.length, selectHomeWorkspace]);

  const handleOpenExistingDirectory = useCallback(async () => {
    setProjectDropdownOpen(false);
    try {
      const workspace = await pickExistingWorkspace({ api });
      if (workspace == null) return;
      const option = normalizeWorkspaceOption(workspace, homeWorkspaces.length);
      if (!option?.hash || !option?.cwd) {
        throw new Error('打开的目录缺少工作区信息');
      }
      setHomeWorkspaces((previous) => [
        option,
        ...previous.filter((item) => item.hash !== option.hash),
      ]);
      selectHomeWorkspace(option);
      notifySessionListChanged({
        reason: 'workspace-opened',
        workspaceHash: option.hash,
      });
    } catch (error) {
      toast({ kind: 'err', text: `打开现有目录失败：${error?.message || ''}` });
    }
  }, [api, homeWorkspaces.length, selectHomeWorkspace]);

  const selectedHomeWorkspace = useMemo(() => {
    return homeWorkspaceOptionForHash(homeWorkspaces, homeWorkspaceHash);
  }, [homeWorkspaceHash, homeWorkspaces]);

  const commandWorkspaceHash = useMemo(() => commandWorkspaceHashForInput({
    activeRef: ref,
    selectedHomeWorkspace,
    hasSession: !!sid,
  }), [ref, selectedHomeWorkspace, sid]);

  useEffect(() => {
    const expertId = String(ref?.expertId || ref?.expert_id || ref?.expert?.id || '');
    if (sid) {
      setSessionExpertId(expertId);
      setSessionExpertSnapshot(
        ref?.expert && typeof ref.expert === 'object' ? ref.expert : null,
      );
      if (pendingExpertRef.current?.expert?.id === expertId) {
        pendingExpertRef.current = null;
        setPendingExpert(null);
      }
      if (!pendingExpertRef.current
          && acceptedExpertSwitchRef.current?.expert?.id !== expertId) {
        acceptedExpertSwitchRef.current = null;
      }
      return;
    }
    setHomeExpertId(expertId);
  }, [ref?.expert, ref?.expert?.id, ref?.expertId, ref?.expert_id, sid]);

  useEffect(() => {
    setExpertPickerOpen(false);
    pendingExpertRef.current = null;
    setPendingExpert(null);
    latestExpertSwitchRequestRef.current = {
      sequence: latestExpertSwitchRequestRef.current.sequence + 1,
      expertId: '',
      settled: true,
    };
    acceptedExpertSwitchRef.current = null;
    expertSwitchQueueRef.current = Promise.resolve();
    setExpertSwitching(false);
    setExpertDetaching(false);
  }, [sid]);

  useEffect(() => {
    let alive = true;
    api.listExperts(commandWorkspaceHash || '__local__')
      .then((result) => { if (alive) setExperts(normalizeExperts(result)); })
      .catch(() => { if (alive) setExperts([]); });
    return () => { alive = false; };
  }, [api, commandWorkspaceHash]);
  const consoleCwd = useMemo(() => consoleCwdForContext({
    activeRef: ref,
    selectedHomeWorkspace,
    health,
  }), [health?.cwd, ref, selectedHomeWorkspace]);

  useEffect(() => {
    onCommandWorkspaceChange?.(commandWorkspaceHash);
  }, [commandWorkspaceHash, onCommandWorkspaceChange]);
  useEffect(() => {
    onConsoleCwdChange?.(consoleCwd);
  }, [consoleCwd, onConsoleCwdChange]);

  useEffect(() => { sidRef.current = sid; }, [sid]);
  useEffect(() => { draftSessionKeyRef.current = draftSessionKey; }, [draftSessionKey]);
  useEffect(() => {
    previewFileGuardRef.current.cancelPending();
  }, [workbenchOwner]);
  useEffect(() => () => previewFileGuardRef.current.cancelPending(), []);

  const releaseDeferredPastes = useCallback(() => {
    const group = deferredHistoryPasteRef.current;
    deferredHistoryPasteRef.current = null;
    if (!group) return;
    const ids = new Set(group.localIds);
    for (const id of ids) releaseComposerAttachmentFile(attachmentReservationsRef.current, id);
    composerAttachmentsRef.current = composerAttachmentsRef.current.filter((item) => !ids.has(item?.local_id));
    setComposerAttachments((items) => items.filter((item) => !ids.has(item?.local_id)));
  }, []);

  const handleComposerChange = useCallback((next, content = null) => {
    const normalized = normalizeComposerContent(content);
    if (composerDraftFingerprint(next, normalized) === composerDraftFingerprint(composerValueRef.current, composerContentRef.current)) return;
    const deferred = deferredHistoryPasteRef.current;
    if (deferred && !(normalized?.parts || []).some((part) => (
      part.type === 'attachment' && deferred.localIds.includes(part.key)
    ))) {
      // 翻到了别的历史条目(或块被删掉):释放暂存的 File,不再上传。
      releaseDeferredPastes();
    }
    const userEdit = composerDraftEditFingerprint(next, normalized)
      !== composerDraftEditFingerprint(composerValueRef.current, composerContentRef.current);
    if (userEdit) {
      draftEditVersionRef.current += 1;
      composerDirtyRef.current = true;
    }
    setComposerValue(next, normalized);
    const restored = composerContentAttachments(normalized, composerAttachmentsRef.current, { sessionId: sid });
    setComposerAttachments((current) => mergeComposerAttachmentResources(current, restored));
    if (!sid) {
      const snapshot = composerDraftSnapshot(next, normalized, composerAttachmentsRef.current);
      // 旧长文本折叠的文件块还没上传完:不写首页草稿,保留旧全文(上传完成的回填会再走这里)。
      if (!legacyFoldUploadPending(legacyFoldGuardRef.current, snapshot.composer_content)) {
        onHomeComposerDraftChange?.(homeDraftWorkspaceHash, snapshot, api);
      }
    }
  }, [api, homeDraftWorkspaceHash, onHomeComposerDraftChange, releaseDeferredPastes, setComposerValue, sid]);

  const restoreComposerDraft = useCallback((draft, targetSid = '') => {
    const storedContent = normalizeComposerContent(draft?.composer_content);
    const storedText = String(draft?.text || '');
    // 本版之前的旧草稿(没有 composer_content)与从旧消息 fork 回填的超长文本:以空
    // 编辑器恢复,正文整段走粘贴块分类(文件块立即上传)。否则几 MB 文本直接进 Slate 卡死。
    const foldLegacy = typeof handleLargeTextPasteRef.current === 'function'
      && legacyTextNeedsFold(storedText, storedContent);
    const content = foldLegacy ? null : storedContent;
    deferredHistoryPasteRef.current = null;
    legacyFoldGuardRef.current = null;
    const resources = composerContentAttachments(content, draft?.attachments || [], { sessionId: targetSid });
    for (const resource of composerAttachmentsRef.current) {
      if (resource?.preview_url?.startsWith('blob:')) URL.revokeObjectURL(resource.preview_url);
    }
    clearComposerAttachmentReservations(attachmentReservationsRef.current);
    for (const resource of resources) {
      if (resource.file) {
        reserveComposerAttachmentFiles(attachmentReservationsRef.current, [resource.file], () => resource.local_id);
        if (resource.kind === 'image' && typeof URL.createObjectURL === 'function') resource.preview_url = URL.createObjectURL(resource.file);
      }
    }
    setComposerAttachments(resources);
    composerAttachmentsRef.current = resources;
    setComposerValue(foldLegacy ? '' : storedText, content);
    if (foldLegacy) handleLargeTextPasteRef.current(storedText, { legacyDraft: true });
  }, [setComposerValue]);

  const clearAttachmentReservations = useCallback(() => {
    clearComposerAttachmentReservations(attachmentReservationsRef.current);
  }, []);

  const reserveUniqueComposerFiles = useCallback((files) => {
    return reserveComposerAttachmentFiles(attachmentReservationsRef.current, files);
  }, []);

  const clearComposerExtras = useCallback(() => {
    clearAttachmentReservations();
    setComposerAttachments((items) => {
      for (const item of items) {
        if (item?.preview_url && item.preview_url.startsWith('blob:')) {
          URL.revokeObjectURL(item.preview_url);
        }
      }
      return [];
    });
    setComposerContexts([]);
    setSelectionAction(null);
    selectionPreviewFingerprintRef.current = '';
    setSelectionPreview(null);
    clearPreviewSelection();
  }, [clearAttachmentReservations]);

  const resetComposerContextSelections = useCallback(() => {
    clearComposerExtras();
    setComposerSwarmChoice(null);
  }, [clearComposerExtras]);

  const createHomeComposerSession = useCallback(async (text, {
    createOptions = null,
    firstUserMessageText = '',
    firstUserMessageContent = null,
    firstUserMessageAttachments = [],
    preserveExtras = false,
    title = '',
  } = {}) => {
    if (homeSubmitting || reasoningSwitching) return null;
    const sourcePreviewContext = previewContextRef.current;
    if (!await previewLeaveRequestRef.current()) return null;
    const target = selectedHomeWorkspace || fallbackWorkspaceOption(ref, health);
    const targetHash = target?.hash || '';
    const targetNoWorkspace = !!target?.noWorkspace;
    void refreshWorkspaceGitInfo(api, target).catch(() => {});
    const baseOptions = withCreateSessionPreferences(
      createOptions || sessionCreateOptionsForText(text),
      { modelName: homeModelName, permissionMode, reasoningEffort: homeReasoningEffort },
    );
    const expertOptions = homeExpertId ? { expert_id: homeExpertId, expertId: homeExpertId } : {};
    const options = targetNoWorkspace
      ? { ...baseOptions, ...expertOptions, no_workspace: true, noWorkspace: true }
      : { ...baseOptions, ...expertOptions };
    const create = isRealWorkspaceHash(targetHash)
      ? api.createWorkspaceSession(targetHash, options)
      : api.createSession(options);
    setHomeSubmitting(true);
    try {
      const r = await create;
      const id = r && (r.session_id || r.id);
      if (!id) throw new Error('missing session id');
      const next = newSessionRefFrom(ref, id);
      if (targetNoWorkspace) {
        next.noWorkspace = true;
        next.workspaceHash = '';
        next.workspaceName = '';
        next.cwd = '';
      } else if (r.workspace_hash || isRealWorkspaceHash(targetHash)) {
        next.workspaceHash = r.workspace_hash || targetHash;
        next.workspaceName = target?.name || ref?.workspaceName;
        next.cwd = r.cwd || target?.cwd || ref?.cwd;
      }
      // workingCwd 与 workspace 归属无关,两个分支都要带上:no-workspace 会话
      // 首轮生成的文件就靠它才能预览。
      const nextWorkingCwd = r.working_cwd || r.cwd || target?.cwd || '';
      if (nextWorkingCwd) next.workingCwd = nextWorkingCwd;
      next.title = title || sessionTitleSeedForPayload({ text });
      if (homeExpertId) {
        const selectedExpert = experts.find((expert) => expert.id === homeExpertId) || ref?.expert || null;
        next.expertId = homeExpertId;
        next.expert_id = homeExpertId;
        if (selectedExpert) next.expert = selectedExpert;
      }
      if (preserveExtras) {
        preserveComposerExtrasOnSessionChangeRef.current = true;
        preserveComposerInputOnSessionChangeRef.current = true;
      }
      const pendingFirstUserMessage = createPendingNewSessionFirstUserMessage({
        sessionId: id,
        text: firstUserMessageText,
        composerContent: firstUserMessageContent,
        attachments: firstUserMessageAttachments,
      });
      if (pendingFirstUserMessage) {
        setPendingNewSessionFirstUserMessage(pendingFirstUserMessage);
      }
      const nextOwner = sessionWorkbench.ownerFor(next);
      if (health?.console?.available) await api.transferPtyOwner(workbenchOwner, nextOwner);
      sessionWorkbench.transfer(workbenchOwner, nextOwner, (record) => ({
        ...record,
        previews: sourcePreviewContext ? transferPreviewTabs(record.previews || {}, sourcePreviewContext, {
          ...sourcePreviewContext, sessionId: id,
        }) : {},
      }));
      onSessionPromoted?.(next);
      notifySessionListChanged({
        reason: 'session-created',
        sessionId: id,
        workspaceHash: targetNoWorkspace ? '' : (next.workspaceHash || ''),
        noWorkspace: targetNoWorkspace,
        session: {
          ...next,
          id,
          workspace_hash: targetNoWorkspace ? '' : (next.workspaceHash || ''),
          no_workspace: targetNoWorkspace,
        },
      });
      return { id, response: r, target };
    } finally {
      setHomeSubmitting(false);
    }
  }, [api, experts, health, homeExpertId, homeModelName, homeReasoningEffort, homeSubmitting, reasoningSwitching, onSessionPromoted, permissionMode, ref, selectedHomeWorkspace, workbenchOwner]);

  const stageMediaFiles = useCallback((reservedFiles) => {
    const stagedItems = [];
    for (const reserved of Array.from(reservedFiles || [])) {
      const { file, identity, localId } = reserved;
      if (!file || !identity || !localId) continue;
      const sourceReference = fileSourceReference(file);
      const kind = sourceReference
        ? 'file'
        : (String(file.type || '').startsWith('image/') ? 'image' : 'file');
      const previewUrl = kind === 'image' ? URL.createObjectURL(file) : '';
      const sourcePath = sourceReference?.sourcePath || fileSourcePath(file);
      const localItem = {
        file,
        local_id: localId,
        name: file.name || 'attachment',
        kind,
        mime_type: file.type || '',
        size_bytes: sourceReference?.sizeBytes ?? file.size ?? 0,
        preview_url: previewUrl,
        source_path: sourcePath,
        attachment_identity: identity,
        pending_upload: true,
        uploading: false,
      };
      stagedItems.push(localItem);
    }
    if (stagedItems.length > 0) {
      setComposerAttachments((items) => [...items, ...stagedItems]);
      composerAttachmentsRef.current = [...composerAttachmentsRef.current, ...stagedItems];
      const current = composerContentRef.current || composerContentFromText(composerValueRef.current);
      const next = appendComposerImageAttachments(current, stagedItems);
      if (composerContentSignature(next) !== composerContentSignature(current)) {
        handleComposerChange(composerValueRef.current, next);
      }
    }
    return stagedItems;
  }, [handleComposerChange]);

  const persistMediaFilesToSession = useCallback(async (targetSid, reservedFiles) => {
    const persistOne = async (reserved) => {
      const { file, identity, localId } = reserved || {};
      if (!file || !identity || !localId) return null;
      // 粘贴的文本块:描述挂在 File 上(上传回填会整体替换资源,资源上的标记靠不住)。
      const paste = pastedTextFileMeta(file);
      const sourceReference = fileSourceReference(file);
      const sourcePath = sourceReference?.sourcePath || fileSourcePath(file);
      setComposerAttachments((items) => items.map((item) => (
        item.local_id === localId
          ? { ...item, pending_upload: false, uploading: true, upload_error: '', upload_deferred: false }
          : item
      )));
      try {
        const persistAttachment = sourceReference
          ? api.createSessionAttachmentReference(targetSid, {
              name: file.name || 'attachment',
              mime_type: file.type || '',
              source_path: sourceReference.sourcePath,
              reference_only: true,
            })
          : normalizeImageFile(file)
            .then((normalized) => {
              if (normalized.file.size > ATTACHMENT_HARD_LIMIT_BYTES) {
                throw new Error('附件超过 25MiB，且无法压缩到限制内');
              }
              return normalized;
            })
            .then(({ file: uploadFile }) => fileToBase64(uploadFile)
              .then((dataBase64) => ({ uploadFile, dataBase64 })))
            .then(({ uploadFile, dataBase64 }) => {
              const uploadName = uploadFile.name || file.name || 'attachment';
              const uploadMime = uploadFile.type || file.type || '';
              return api.uploadSessionAttachment(targetSid, {
                name: uploadName,
                mime_type: uploadMime,
                data_base64: dataBase64,
                ...(sourcePath ? { source_path: sourcePath } : {}),
                ...(paste ? pastedTextUploadBody(paste) : {}),
              }, paste ? { timeoutMs: PASTED_TEXT_UPLOAD_TIMEOUT_MS } : {});
            });
        const result = await Promise.resolve(persistAttachment);
        const attachment = result?.attachment || {};
        if (!attachment.id) {
          throw new Error(tr('composerAttachment.missingId'));
        }
        const uploadedItem = {
          ...attachment,
          local_id: localId,
          source_path: attachment?.metadata?.source_path || sourcePath,
          attachment_identity: identity,
          pending_upload: false,
          uploading: false,
          upload_error: '',
        };
        setComposerAttachments((items) => items.map((item) => (
          item.local_id === localId
            ? { ...uploadedItem, preview_url: item.preview_url || '', ...(paste ? { paste } : {}) }
            : item
        )));
        if (sidRef.current === targetSid) {
          // Persist the completed identity even when navigation beats the debounce.
          const content = reconcileComposerContentAttachments(composerContentRef.current, [uploadedItem]);
          if (composerContentSignature(content) !== composerContentSignature(composerContentRef.current)) {
            composerDirtyRef.current = true;
            setComposerValue(composerValueRef.current, content);
          }
        } else {
          const targetKey = [...draftSaveQueueRef.current.keys()].find((key) => key.endsWith(`:${targetSid}`))
            || `session:${targetSid}`;
          const previous = draftSaveQueueRef.current.get(targetKey) || Promise.resolve();
          const save = previous.catch(() => null)
            .then(() => completeDetachedComposerUpload(api, targetSid, uploadedItem));
          draftSaveQueueRef.current.set(targetKey, save);
          try {
            await save;
          } finally {
            if (draftSaveQueueRef.current.get(targetKey) === save) draftSaveQueueRef.current.delete(targetKey);
          }
        }
        return uploadedItem;
      } catch (error) {
        setComposerAttachments((items) => items.map((item) => (
          item.local_id === localId
            ? {
                ...item,
                pending_upload: true,
                uploading: false,
                upload_error: error?.message || tr('composerAttachment.uploadFailed'),
              }
            : item
        )));
        throw error;
      }
    };

    // 粘贴的文本分段顺序上传,其它附件照旧并行:服务端单个上传请求的峰值内存约为
    // 正文的 4~5 倍(请求体、解析后的 JSON、解码字节、落盘拷贝),9 段 24 MiB 并发就是
    // 1 GB 级别。一段失败不影响后面的段,全部跑完再抛出第一个错误。结果保持原顺序。
    const list = Array.from(reservedFiles || []);
    const results = new Array(list.length).fill(null);
    const pasteIndexes = [];
    const otherIndexes = [];
    list.forEach((reserved, index) => (
      pastedTextFileMeta(reserved?.file) ? pasteIndexes : otherIndexes
    ).push(index));
    let pasteError = null;
    await Promise.all([
      Promise.all(otherIndexes.map(async (index) => { results[index] = await persistOne(list[index]); })),
      (async () => {
        for (const index of pasteIndexes) {
          try {
            results[index] = await persistOne(list[index]);
          } catch (error) {
            pasteError ||= error;
          }
        }
      })(),
    ]);
    if (pasteError) throw pasteError;
    return results.filter(Boolean);
  }, [api, setComposerValue]);

  const handleMediaFiles = useCallback((files) => {
    const reservedFiles = reserveUniqueComposerFiles(files);
    if (reservedFiles.length === 0) return;
    stageMediaFiles(reservedFiles);
    if (!sid) return;
    persistMediaFilesToSession(sid, reservedFiles)
      .catch((e) => {
        toast({ kind: 'err', text: '附件上传失败:' + (e?.message || '') });
      });
  }, [
    persistMediaFilesToSession,
    reserveUniqueComposerFiles,
    sid,
    stageMediaFiles,
  ]);

  // ---- 粘贴的文本块(第 2 条反馈 f300) ----------------------------------------
  // 达到折叠阈值的粘贴不进 Slate:小的成为内联块({type:'pasted_text'}),大的
  // (UTF-8 >= 128 KiB,或内联合计 > 256 KiB)落成 text/plain 附件,composer 里是带
  // paste 描述的 attachment 部件。会话内上传为会话附件;首页(还没有会话)上传到
  // 工作区草稿附件区,刷新不丢,发送前经 materializeWorkspaceDraftPastes 复制成会话附件。

  // 文件块的暂存资源:与 stageMediaFiles 同形,另带 paste 描述(与 File 上登记的一致)。
  // replaceId 非空时替换该块(编辑 / 用剪贴板替换),否则追加在末尾。
  const stagePastedTextFiles = useCallback((reservedFiles, { deferUpload = false, replaceId = '' } = {}) => {
    const stagedItems = [];
    for (const reserved of Array.from(reservedFiles || [])) {
      const { file, identity, localId } = reserved || {};
      if (!file || !identity || !localId) continue;
      const paste = pastedTextFileMeta(file);
      stagedItems.push({
        file,
        local_id: localId,
        name: file.name || 'pasted-text.txt',
        kind: 'file',
        mime_type: 'text/plain',
        size_bytes: file.size ?? 0,
        preview_url: '',
        source_path: '',
        attachment_identity: identity,
        pending_upload: true,
        uploading: false,
        ...(paste ? { paste } : {}),
        ...(deferUpload ? { upload_deferred: true } : {}),
      });
    }
    if (stagedItems.length === 0) return stagedItems;
    setComposerAttachments((items) => [...items, ...stagedItems]);
    composerAttachmentsRef.current = [...composerAttachmentsRef.current, ...stagedItems];
    const current = composerContentRef.current || composerContentFromText(composerValueRef.current);
    const next = replaceId
      ? replacePasteBlock(current, replaceId, appendPasteFileAttachments(composerContentFromText(''), stagedItems).parts)
      : appendPasteFileAttachments(current, stagedItems);
    handleComposerChange(composerValueRef.current, next);
    return stagedItems;
  }, [handleComposerChange]);

  // 首页粘贴的文件块上传到工作区草稿附件区(各段顺序上传)。完成时仍在同一个首页
  // scope 就直接回填 id + store;已经离开首页则经 App 的草稿 store 回填最新草稿
  // (reconcileHomeDraftUpload:块已删除 / 已发送时不写,不会复活)。
  const persistPastedTextToWorkspaceDraft = useCallback(async (hash, scope, reservedFiles) => {
    const uploaded = [];
    let firstError = null;
    for (const reserved of Array.from(reservedFiles || [])) {
      const { file, identity, localId } = reserved || {};
      if (!file || !localId) continue;
      const paste = pastedTextFileMeta(file);
      setComposerAttachments((items) => items.map((item) => (
        item.local_id === localId
          ? { ...item, pending_upload: false, uploading: true, upload_error: '', upload_deferred: false }
          : item
      )));
      try {
        const dataBase64 = await fileToBase64(file);
        const result = await api.uploadWorkspaceDraftAttachment(scope, {
          name: file.name || 'pasted-text.txt',
          mime_type: 'text/plain',
          data_base64: dataBase64,
          ...pastedTextUploadBody(paste),
        }, { timeoutMs: PASTED_TEXT_UPLOAD_TIMEOUT_MS });
        const attachment = result?.attachment || {};
        if (!attachment.id) throw new Error(tr('composerAttachment.missingId'));
        const uploadedItem = {
          ...attachment,
          local_id: localId,
          attachment_identity: identity,
          store: WORKSPACE_DRAFT_STORE,
          store_scope: scope,
          pending_upload: false,
          uploading: false,
          upload_error: '',
          ...(paste ? { paste } : {}),
        };
        uploaded.push(uploadedItem);
        if (!sidRef.current && homeDraftWorkspaceHashRef.current === hash) {
          composerAttachmentsRef.current = composerAttachmentsRef.current.map((item) => (
            item.local_id === localId ? uploadedItem : item
          ));
          setComposerAttachments((items) => items.map((item) => (item.local_id === localId ? uploadedItem : item)));
          const content = reconcileComposerContentAttachments(composerContentRef.current, [uploadedItem]);
          if (composerContentSignature(content) !== composerContentSignature(composerContentRef.current)) {
            handleComposerChange(composerValueRef.current, content);
          }
        } else {
          onHomeComposerDraftPatch?.(hash, (draft) => reconcileHomeDraftUpload(draft, uploadedItem, scope), api);
        }
      } catch (error) {
        firstError ||= error;
        setComposerAttachments((items) => items.map((item) => (
          item.local_id === localId
            ? {
                ...item,
                pending_upload: true,
                uploading: false,
                upload_error: error?.message || tr('composerAttachment.uploadFailed'),
              }
            : item
        )));
      }
    }
    if (firstError) throw firstError;
    return uploaded;
  }, [api, handleComposerChange, onHomeComposerDraftPatch]);

  // 按当前目标上传粘贴的文件块:会话内 → 会话附件;首页 → 工作区草稿附件区;
  // AI 主题等临时首页(本来就不落盘)不上传,留给发送时的现有流程。
  const uploadPasteReservations = useCallback((reservedFiles) => {
    const targetSid = sidRef.current;
    let upload;
    if (targetSid) {
      upload = persistMediaFilesToSession(targetSid, reservedFiles);
    } else {
      const hash = homeDraftWorkspaceHashRef.current;
      const scope = homeDraftAttachmentScope(hash);
      if (!scope) return;
      upload = persistPastedTextToWorkspaceDraft(hash, scope, reservedFiles);
    }
    upload.catch((error) => {
      toast({ kind: 'err', text: '粘贴的文本上传失败:' + (error?.message || '') });
    });
  }, [persistMediaFilesToSession, persistPastedTextToWorkspaceDraft]);

  // RichComposer 的 onLargeTextPaste:达到折叠阈值的文本到这里分类。返回 false 表示
  // 不折叠(照常进编辑器)。deferUpload:来自上箭头翻到的旧历史,文件块只在内存暂存、
  // 显示「待上传」,开始编辑或发送时才上传(commitDeferredPastes)。
  // legacyDraft:来自 restoreComposerDraft 的旧长文本折叠,落文件时登记 legacyFoldGuardRef,
  // 在块拿到 id 之前不覆盖服务端草稿里的旧全文。
  const handleLargeTextPaste = useCallback((text, { deferUpload = false, legacyDraft = false } = {}) => {
    const normalized = normalizePastedText(text);
    const current = composerContentRef.current || composerContentFromText(composerValueRef.current);
    const plan = planPastedTextInsertion(current, normalized);
    if (plan.kind === 'plain') return false;
    if (plan.kind === 'inline') {
      handleComposerChange(composerValueRef.current, appendPastedTextPart(current, plan.text));
      return true;
    }
    if (plan.notice) toast({ kind: 'info', text: largePasteNotice(plan) });
    const reservedFiles = reserveUniqueComposerFiles(pastedTextFilesForPlan(plan));
    if (reservedFiles.length === 0) return true;
    if (deferUpload) releaseDeferredPastes();
    // 必须在 stagePastedTextFiles 之前登记:暂存会同步触发 handleComposerChange(首页草稿写入)。
    if (legacyDraft) legacyFoldGuardRef.current = { localIds: reservedFiles.map((reserved) => reserved.localId) };
    stagePastedTextFiles(reservedFiles, { deferUpload });
    if (deferUpload) {
      deferredHistoryPasteRef.current = {
        reserved: reservedFiles,
        localIds: reservedFiles.map((reserved) => reserved.localId),
      };
      return true;
    }
    uploadPasteReservations(reservedFiles);
    return true;
  }, [handleComposerChange, releaseDeferredPastes, reserveUniqueComposerFiles, stagePastedTextFiles, uploadPasteReservations]);
  handleLargeTextPasteRef.current = handleLargeTextPaste;

  // 用户在翻出来的旧历史条目上开始编辑(或直接发送):上传仍在输入框里的暂存块。
  const commitDeferredPastes = useCallback(() => {
    const group = deferredHistoryPasteRef.current;
    if (!group) return false;
    deferredHistoryPasteRef.current = null;
    const keys = new Set((normalizeComposerContent(composerContentRef.current)?.parts || [])
      .filter((part) => part.type === 'attachment').map((part) => part.key));
    const live = group.reserved.filter((reserved) => keys.has(reserved.localId));
    for (const reserved of group.reserved) {
      if (!keys.has(reserved.localId)) releaseComposerAttachmentFile(attachmentReservationsRef.current, reserved.localId);
    }
    if (live.length === 0) return false;
    const liveIds = new Set(live.map((reserved) => reserved.localId));
    setComposerAttachments((items) => items.map((item) => (
      liveIds.has(item?.local_id) ? { ...item, upload_deferred: false } : item
    )));
    uploadPasteReservations(live);
    return true;
  }, [uploadPasteReservations]);

  // 对话框保存 / 用剪贴板替换:删掉原块后按同一套分类在原位置放入新内容(换 key;
  // 落文件则上传新附件、替换引用)。编辑后变短也保持为块 —— 用户是在编辑这个块。
  const replacePasteBlockText = useCallback((id, text) => {
    const current = composerContentRef.current;
    if (!current) return;
    const normalized = normalizePastedText(text);
    if (!normalized) {
      handleComposerChange(composerValueRef.current, removePastedTextPart(current, id));
      return;
    }
    const plan = planPastedTextInsertion(removePastedTextPart(current, id), normalized);
    if (plan.kind !== 'file') {
      handleComposerChange(composerValueRef.current, replacePastedTextPart(current, id, normalized));
      return;
    }
    if (plan.notice) toast({ kind: 'info', text: largePasteNotice(plan) });
    const reservedFiles = reserveUniqueComposerFiles(pastedTextFilesForPlan(plan));
    if (reservedFiles.length === 0) return;
    stagePastedTextFiles(reservedFiles, { replaceId: id });
    uploadPasteReservations(reservedFiles);
  }, [handleComposerChange, reserveUniqueComposerFiles, stagePastedTextFiles, uploadPasteReservations]);

  // 卡片「上传失败，点击重试」:取回保留的 File,按当前目标重新上传。
  const retryPasteUpload = useCallback((key) => {
    const reservedFiles = composerAttachmentFilesForLocalIds(attachmentReservationsRef.current, [key]);
    if (reservedFiles.length === 0) {
      toast({ kind: 'err', text: '粘贴的文本已失效，请删除后重新粘贴' });
      return;
    }
    uploadPasteReservations(reservedFiles);
  }, [uploadPasteReservations]);

  // 排队消息编辑框里粘贴的大段文本:落文件时上传到当前会话,返回带 id 的附件部件。
  const uploadPastedTextForQueue = useCallback(async (plan) => {
    const targetSid = sidRef.current;
    if (!targetSid) throw new Error(tr('composerAttachment.uploadFailed'));
    const parts = [];
    // 顺序上传,理由同 persistMediaFilesToSession。
    for (const file of pastedTextFilesForPlan(plan)) {
      const paste = pastedTextFileMeta(file);
      const dataBase64 = await fileToBase64(file);
      const result = await api.uploadSessionAttachment(targetSid, {
        name: file.name,
        mime_type: 'text/plain',
        data_base64: dataBase64,
        ...pastedTextUploadBody(paste),
      }, { timeoutMs: PASTED_TEXT_UPLOAD_TIMEOUT_MS });
      const attachment = result?.attachment || {};
      if (!attachment.id) throw new Error(tr('composerAttachment.missingId'));
      parts.push({
        type: 'attachment',
        key: String(attachment.id),
        id: String(attachment.id),
        name: String(attachment.name || file.name),
        kind: 'file',
        mime_type: 'text/plain',
        ...(paste ? { paste } : {}),
      });
    }
    return parts;
  }, [api]);

  // 首页草稿附件 → 会话附件的唯一导入点(sendInputOrBuiltin 开头与 /turn 的
  // interruptTurn 之前)。导入用部件上记下的 store_scope,不能靠发送那一刻的首页
  // workspace 推断(两者可以不同)。仍停在目标会话时把新 id 回填进输入框,发送失败
  // 后重试不再重复导入。
  const materializeWorkspaceDraftPastes = useCallback(async (targetSid, payload) => {
    const refs = workspaceDraftPasteRefs(payload);
    if (refs.length === 0) return payload;
    const parts = normalizeComposerContent(payload?.composer_content)?.parts || [];
    const imported = [];
    for (const ref of refs) {
      let result;
      try {
        result = await api.importWorkspaceDraftAttachment(targetSid, { workspace: ref.workspace, id: ref.id });
      } catch (error) {
        if (error?.status === 404) throw new Error('粘贴的文本已失效，请删除后重新粘贴');
        throw error;
      }
      const attachment = result?.attachment;
      if (!attachment?.id) throw new Error(tr('composerAttachment.missingId'));
      const key = parts.find((part) => part.type === 'attachment' && part.id === ref.id)?.key || '';
      imported.push({ id: ref.id, key, attachment });
    }
    if (sidRef.current === targetSid) {
      const uploadedItems = imported.map(({ key, attachment }) => ({
        ...attachment,
        local_id: key || attachment.id,
        pending_upload: false,
        uploading: false,
        upload_error: '',
      }));
      const byKey = new Map(uploadedItems.map((item) => [item.local_id, item]));
      const replaceResource = (item) => {
        const next = byKey.get(item?.local_id);
        return next ? { ...next, ...(item.paste ? { paste: item.paste } : {}) } : item;
      };
      composerAttachmentsRef.current = composerAttachmentsRef.current.map(replaceResource);
      setComposerAttachments((items) => items.map(replaceResource));
      const content = reconcileComposerContentAttachments(composerContentRef.current, uploadedItems);
      if (composerContentSignature(content) !== composerContentSignature(composerContentRef.current)) {
        composerDirtyRef.current = true;
        setComposerValue(composerValueRef.current, content);
      }
    }
    return payloadWithImportedPastes(payload, imported);
  }, [api, setComposerValue]);

  const removeComposerAttachment = useCallback((key) => {
    const next = removeComposerAttachmentReference(composerContentRef.current, key);
    handleComposerChange(composerValueRef.current, next);
  }, [handleComposerChange]);

  const removeComposerContext = useCallback((key) => {
    setComposerContexts((items) => items.filter((item) => (item.local_id || item.id || item.type) !== key));
  }, []);

  const addBrowserContext = useCallback((context) => {
    const localId = context?.local_id || context?.id || `browser-${Date.now()}`;
    const normalized = normalizeComposerContext({
      ...context,
      type: 'browser',
      id: context?.id || localId,
    });
    if (!normalized?.content) return false;
    const nextContext = {
      ...normalized,
      local_id: localId,
      id: normalized.id || localId,
    };
    setComposerContexts((items) => [
      ...items.filter((item) => (item.local_id || item.id) !== localId),
      nextContext,
    ]);
    requestAnimationFrame(() => inputRef.current?.focus());
    return true;
  }, []);

  const pinSelectionContext = useCallback((context) => {
    const localId = context?.local_id || context?.id || nextSelectionContextId();
    const normalized = normalizeComposerContext({
      ...context,
      local_id: localId,
      id: context?.id || localId,
    });
    if (!normalized) return false;
    const pinned = {
      ...normalized,
      local_id: localId,
      id: normalized.id || localId,
    };
    setComposerContexts((items) => upsertSelectionContext(items, pinned));
    selectionPreviewFingerprintRef.current = '';
    setSelectionPreview(null);
    setSelectionAction(null);
    clearPreviewSelection();
    requestAnimationFrame(() => inputRef.current?.focus());
    return true;
  }, []);

  const pinSelectionAnnotation = useCallback((context, text) => {
    const annotation = createSelectionAnnotation({ text });
    if (!annotation) return false;
    return pinSelectionContext({
      ...context,
      annotations: mergeSelectionAnnotations(context?.annotations, annotation),
    });
  }, [pinSelectionContext]);

  const dismissSelectionAction = useCallback(() => {
    selectionPreviewFingerprintRef.current = '';
    setSelectionPreview(null);
    setSelectionAction(null);
    clearPreviewSelection();
  }, []);

  useEffect(() => {
    let raf = 0;
    let pendingActionTrigger = null;
    let pendingSelectionTarget = null;
    const updatePreview = () => {
      raf = 0;
      const actionTrigger = pendingActionTrigger;
      const selectionTarget = pendingSelectionTarget || document.activeElement;
      pendingActionTrigger = null;
      pendingSelectionTarget = null;
      const next = selectionContextFromWindowSelection({ target: selectionTarget });
      const isEditablePreviewText = selectionTarget?.matches?.(
        '[data-ace-editable-preview-text="true"]',
      );
      const rangeRect = next
        ? (isEditablePreviewText
            ? selectionTargetViewportRect(selectionTarget)
            : selectionRangeViewportRect() || selectionTargetViewportRect(selectionTarget))
        : null;
      const actionRect = actionTrigger?.rect || rangeRect;
      if (!next || !rangeRect) {
        selectionPreviewFingerprintRef.current = '';
        setSelectionPreview((prev) => (prev ? null : prev));
        setSelectionAction((prev) => (prev?.mode === 'annotation' ? prev : null));
        return;
      }
      const fingerprint = selectionContextFingerprint(next);
      selectionPreviewFingerprintRef.current = fingerprint;
      setSelectionPreview(next);
      setSelectionAction((prev) => {
        if (actionTrigger?.show && actionRect) {
          return {
            key: fingerprint,
            context: next,
            rect: actionRect,
            anchor: actionTrigger.anchor || 'selection',
            mode: 'actions',
          };
        }
        if (prev?.key === fingerprint) return prev;
        return prev?.mode === 'annotation' ? prev : null;
      });
    };
    const schedulePreviewUpdate = (actionTrigger = null, selectionTarget = null) => {
      if (actionTrigger) pendingActionTrigger = actionTrigger;
      if (selectionTarget) pendingSelectionTarget = selectionTarget;
      if (raf) cancelAnimationFrame(raf);
      raf = requestAnimationFrame(updatePreview);
    };
    const handleSelectionChange = () => schedulePreviewUpdate(null, document.activeElement);
    const handleMouseUp = (event) => {
      if (event.button !== 0) return;
      const target = event.target?.nodeType === Node.ELEMENT_NODE
        ? event.target
        : event.target?.parentElement;
      if (!target?.closest?.(SELECTION_PREVIEW_SELECTOR)) return;
      schedulePreviewUpdate({
        show: true,
        anchor: 'pointer',
        rect: selectionPointerViewportRect(event),
      }, target);
    };
    const handleKeyUp = (event) => {
      const target = event.target?.nodeType === Node.ELEMENT_NODE
        ? event.target
        : event.target?.parentElement;
      if (!event.shiftKey || !target?.closest?.(SELECTION_PREVIEW_SELECTOR)) return;
      schedulePreviewUpdate({
        show: true,
        anchor: 'selection',
        rect: null,
      }, target);
    };
    const handleSelect = (event) => schedulePreviewUpdate(null, event.target);

    document.addEventListener('selectionchange', handleSelectionChange);
    document.addEventListener('select', handleSelect, true);
    document.addEventListener('mouseup', handleMouseUp, true);
    document.addEventListener('keyup', handleKeyUp, true);
    return () => {
      if (raf) cancelAnimationFrame(raf);
      document.removeEventListener('selectionchange', handleSelectionChange);
      document.removeEventListener('select', handleSelect, true);
      document.removeEventListener('mouseup', handleMouseUp, true);
      document.removeEventListener('keyup', handleKeyUp, true);
    };
  }, []);

  const pinnedSelectionLocationKeys = useMemo(() => {
    const keys = new Set();
    for (const context of composerContexts) {
      const key = selectionContextLocationKey(context);
      if (key) keys.add(key);
    }
    return keys;
  }, [composerContexts]);

  const sentSelectionContexts = useMemo(
    () => selectionContextsFromTranscriptItems(rawItems),
    [rawItems],
  );

  const previewSelectionContexts = useMemo(
    () => [...sentSelectionContexts, ...composerContexts],
    [composerContexts, sentSelectionContexts],
  );

  const selectionAnnotationPresentations = useMemo(
    () => selectionAnnotationPresentationMap(previewSelectionContexts),
    [previewSelectionContexts],
  );

  const visibleSelectionPreview = useMemo(() => {
    if (!selectionPreview) return null;
    const key = selectionContextLocationKey(selectionPreview);
    return key && pinnedSelectionLocationKeys.has(key) ? null : selectionPreview;
  }, [pinnedSelectionLocationKeys, selectionPreview]);

  const composerInputProps = useMemo(() => ({
    attachments: composerAttachments,
    composerContent,
    contexts: composerContexts,
    annotationPresentations: selectionAnnotationPresentations,
    selectionPreview: visibleSelectionPreview,
    onMediaFiles: handleMediaFiles,
    onRemoveAttachment: removeComposerAttachment,
    onRemoveContext: removeComposerContext,
    onPinSelectionPreview: pinSelectionContext,
    onLargeTextPaste: handleLargeTextPaste,
    onReplacePasteBlock: replacePasteBlockText,
    onRetryPasteUpload: retryPasteUpload,
    onCommitDeferredPastes: commitDeferredPastes,
    attachmentTextLoader: api.readAttachmentText,
    swarmMode: composerSwarmMode,
    onSwarmModeChange: changeComposerSwarmMode,
  }), [
    api,
    commitDeferredPastes,
    composerAttachments,
    composerContent,
    composerContexts,
    composerSwarmMode,
    changeComposerSwarmMode,
    handleLargeTextPaste,
    handleMediaFiles,
    pinSelectionContext,
    removeComposerAttachment,
    removeComposerContext,
    replacePasteBlockText,
    retryPasteUpload,
    selectionAnnotationPresentations,
    visibleSelectionPreview,
  ]);

  useEffect(() => {
    if (preserveComposerExtrasOnSessionChangeRef.current) {
      preserveComposerExtrasOnSessionChangeRef.current = false;
      return;
    }
    resetComposerContextSelections();
  }, [draftSessionKey, resetComposerContextSelections]);

  const persistDraftValue = useCallback((targetSid, targetWorkspaceHash, targetKey, text, content = null) => {
    if (!targetSid || !targetKey) return Promise.resolve(null);
    const normalized = normalizeComposerContent(content);
    const fingerprint = composerDraftFingerprint(text, normalized);
    const previous = Promise.all([...draftSaveQueueRef.current.entries()]
      .filter(([key]) => key === targetKey || key.endsWith(`:${targetSid}`))
      .map(([, save]) => save.catch(() => null)));
    const save = previous.catch(() => null)
      .then(() => api.setSessionDraft(targetSid, text, targetWorkspaceHash, normalized))
      .then((result) => {
        if (draftSessionKeyRef.current === targetKey) {
          draftLastSavedRef.current = { key: targetKey, text, fingerprint };
          if (composerDraftFingerprint(composerValueRef.current, composerContentRef.current) === fingerprint) {
            composerDirtyRef.current = false;
          }
        }
        return result;
      })
      .catch(() => null);
    draftSaveQueueRef.current.set(targetKey, save);
    void save.finally(() => {
      if (draftSaveQueueRef.current.get(targetKey) === save) draftSaveQueueRef.current.delete(targetKey);
    });
    return save;
  }, [api]);

  const clearCurrentSessionDraft = useCallback(({ expectedText = null, expectedContent = undefined } = {}) => {
    const targetSid = sid;
    const targetWorkspaceHash = draftWorkspaceHash;
    const targetKey = draftSessionKey;
    if (!targetSid || !targetKey) return false;
    if (draftSessionKeyRef.current !== targetKey) return false;
    // 提交期间编辑区不再只读,所以一次发送的回执可能晚于用户写下的下一条。
    // expectedText 对不上就整条放弃清理,让草稿保存 effect 接着管新内容。
    if (expectedText !== null && composerValueRef.current !== expectedText) return false;
    if (expectedContent !== undefined && composerDraftEditFingerprint('', composerContentRef.current) !== composerDraftEditFingerprint('', expectedContent)) return false;
    if (draftSessionKeyRef.current === targetKey) {
      draftEditVersionRef.current += 1;
      setComposerValue('');
    }
    void persistDraftValue(targetSid, targetWorkspaceHash, targetKey, '');
    return true;
  }, [draftSessionKey, draftWorkspaceHash, persistDraftValue, setComposerValue, sid]);

  useEffect(() => {
    let cancelled = false;
    const targetSid = sid;
    const targetWorkspaceHash = draftWorkspaceHash;
    const targetKey = draftSessionKey;
    const editVersionAtLoad = draftEditVersionRef.current;
    const preserveComposerInput = preserveComposerInputOnSessionChangeRef.current;
    preserveComposerInputOnSessionChangeRef.current = false;
    const forkDraft = pendingForkComposerRef.current;
    pendingForkComposerRef.current = null;
    setDraftReadyKey('');
    if (!preserveComposerInput) setComposerSubmitting(false);

    if (!targetSid || !targetKey) {
      const loading = onHomeComposerDraftLoad?.(homeDraftWorkspaceHash, api);
      const initial = loading?.draft ?? homeComposerDraft(homeComposerDrafts, homeDraftWorkspaceHash);
      composerDirtyRef.current = !!initial.text;
      restoreComposerDraft(initial);
      if (loading) {
        void loading.ready.then((draft) => {
          if (cancelled || draftEditVersionRef.current !== editVersionAtLoad) return;
          composerDirtyRef.current = !!draft.text;
          restoreComposerDraft(draft);
        });
      }
      draftLastSavedRef.current = { key: '', text: '' };
      return () => { cancelled = true; };
    }

    if (forkDraft?.key === targetKey) {
      composerDirtyRef.current = true;
      draftEditVersionRef.current += 1;
      restoreComposerDraft(forkDraft, targetSid);
      draftLastSavedRef.current = { key: targetKey, text: '' };
      setDraftReadyKey(targetKey);
      return () => { cancelled = true; };
    }

    if (preserveComposerInput) {
      composerDirtyRef.current = !!composerValueRef.current;
      draftLastSavedRef.current = { key: targetKey, text: '' };
      setDraftReadyKey(targetKey);
      return () => { cancelled = true; };
    }

    composerDirtyRef.current = false;
    restoreComposerDraft({ text: '' }, targetSid);
    Promise.all([...draftSaveQueueRef.current.entries()]
      .filter(([key]) => key.endsWith(`:${targetSid}`)).map(([, save]) => save.catch(() => null)))
      .then(() => api.getSessionDraft(targetSid, targetWorkspaceHash))
      .then((result) => {
        if (cancelled || draftSessionKeyRef.current !== targetKey) return;
        const text = typeof result?.text === 'string' ? result.text : '';
        draftLastSavedRef.current = { key: targetKey, text, fingerprint: composerDraftFingerprint(text, result?.composer_content) };
        if (draftEditVersionRef.current === editVersionAtLoad) {
          restoreComposerDraft({ ...result, text }, targetSid);
        }
        setDraftReadyKey(targetKey);
      })
      .catch(() => {
        if (cancelled || draftSessionKeyRef.current !== targetKey) return;
        draftLastSavedRef.current = { key: targetKey, text: '' };
        setDraftReadyKey(targetKey);
      });

    return () => { cancelled = true; };
  // Home edits update App's draft store without triggering restoration again.
  // Only session/workspace changes should reset or load the scoped draft.
  }, [api, draftSessionKey, draftWorkspaceHash, homeDraftWorkspaceHash, onHomeComposerDraftLoad, restoreComposerDraft, sid]);

  useEffect(() => {
    if (!acceptedHomeSubmission || acceptedHomeSubmission.sessionId !== sid
      || draftReadyKey !== draftSessionKey) return;
    // A fast first-send receipt can precede React's session promotion. Wait for
    // the destination draft, then clear only the text/references we submitted.
    const cleared = clearCurrentSessionDraft({
      expectedText: acceptedHomeSubmission.text,
      expectedContent: acceptedHomeSubmission.content,
    });
    if (cleared && acceptedHomeSubmission.clearExtras) clearComposerExtras();
    setAcceptedHomeSubmission((current) => current === acceptedHomeSubmission ? null : current);
  }, [acceptedHomeSubmission, clearComposerExtras, clearCurrentSessionDraft, draftReadyKey, draftSessionKey, sid]);

  useEffect(() => {
    const targetSid = sid;
    const targetWorkspaceHash = draftWorkspaceHash;
    const targetKey = draftSessionKey;
    return () => {
      if (!targetSid || !targetKey || !composerDirtyRef.current) return;
      const content = reconcileComposerContentAttachments(composerContentRef.current, composerAttachmentsRef.current);
      // 旧长文本折叠的文件块还没上传完就离开:保留服务端草稿里的旧全文,下次打开重新折叠。
      if (legacyFoldUploadPending(legacyFoldGuardRef.current, content)) return;
      void persistDraftValue(targetSid, targetWorkspaceHash, targetKey, composerValueRef.current, content);
    };
  }, [draftSessionKey, draftWorkspaceHash, persistDraftValue, sid]);

  useEffect(() => {
    if (!sid || !draftSessionKey || draftReadyKey !== draftSessionKey) return undefined;
    const content = reconcileComposerContentAttachments(composerContent, composerAttachments);
    // 旧长文本折叠的文件块还没拿到 id:不保存,服务端草稿里的旧全文是唯一持久副本。上传
    // 完成回填资源后 composerAttachments 变化,本 effect 重跑再保存。
    if (legacyFoldUploadPending(legacyFoldGuardRef.current, content)) return undefined;
    if (draftLastSavedRef.current.key === draftSessionKey &&
        draftLastSavedRef.current.fingerprint === composerDraftFingerprint(composerValue, content)) {
      return undefined;
    }

    const targetSid = sid;
    const targetWorkspaceHash = draftWorkspaceHash;
    const targetKey = draftSessionKey;
    const text = composerValue;
    const timer = setTimeout(() => {
      void persistDraftValue(targetSid, targetWorkspaceHash, targetKey, text, content);
    }, 350);
    return () => clearTimeout(timer);
  }, [composerValue, composerContent, composerAttachments, draftReadyKey, draftSessionKey, draftWorkspaceHash, persistDraftValue, sid]);

  useEffect(() => {
    if (sid) return;
    // Retry discovery on home entry even if startup discovery failed. Profile
    // update notifications only reload local state and must not retrigger this.
    void requestSavedModelReasoningSync(api);
  }, [api, sid]);

  useEffect(() => {
    let cancelled = false;
    setPendingModelName('');
    setModelSwitching(false);
    setReasoningSwitching(false);
    reasoningRequestRef.current += 1;
    setModelRefreshing(false);
    setModelListLoaded(false);

    if (!sid) {
      setModelState(null);
      Promise.allSettled([
        api.listModels(),
        api.getDefaultModel(),
      ]).then(([modelsResult, defaultResult]) => {
        if (cancelled) return;
        const options = modelsResult.status === 'fulfilled'
          ? normalizeModelOptions(modelsResult.value)
          : [];
        setModelOptions(options);
        setModelListLoaded(true);
        const defaultName = defaultResult.status === 'fulfilled'
          ? (defaultResult.value?.name || defaultResult.value?.default_model_name || '')
          : '';
        setHomeModelName(resolveHomeModelName(options, defaultName, ''));
      });
      return () => { cancelled = true; };
    }

    api.listModels()
      .then((list) => {
        if (!cancelled) {
          setModelOptions(normalizeModelOptions(list));
          setModelListLoaded(true);
        }
      })
      .catch(() => {
        if (!cancelled) {
          setModelOptions([]);
          setModelListLoaded(true);
        }
      });

    api.getSessionModel(sid, ref?.workspaceHash || '')
      .then((state) => {
        if (!cancelled) setModelState(normalizeModelState(state));
      })
      .catch(() => {
        if (!cancelled) {
          setModelState(normalizeModelState({
            name: ref?.model_name || ref?.model_preset || '',
            provider: ref?.provider || '',
            model: ref?.model || '',
            context_window: ref?.context_window || 0,
            deleted: ref?.deleted || ref?.model_deleted || ref?.modelDeleted || false,
          }));
        }
      });

    return () => { cancelled = true; };
  }, [api, modelProfileRevision, ref?.context_window, ref?.deleted, ref?.model, ref?.modelDeleted, ref?.model_deleted, ref?.model_name, ref?.model_preset, ref?.provider, ref?.workspaceHash, sid]);

  const refreshSessionModels = useCallback(async () => {
    if (modelRefreshing || reasoningSwitching) return;
    const targetSid = sid;
    setModelRefreshing(true);
    void requestSavedModelReasoningSync(api);
    try {
      const requests = targetSid
        ? [api.listModels(), api.reloadSessionModel(targetSid)]
        : [api.listModels(), api.getDefaultModel(), api.getDefaultPermissionMode()];
      const [modelsResult, stateResult, permissionResult] = await Promise.allSettled(requests);
      if (targetSid && sidRef.current !== targetSid) return;
      const nextOptions = modelsResult.status === 'fulfilled'
        ? normalizeModelOptions(modelsResult.value)
        : modelOptions;
      if (modelsResult.status === 'fulfilled') {
        setModelOptions(nextOptions);
        setModelListLoaded(true);
      }
      if (targetSid && stateResult.status === 'fulfilled') {
        setModelState(normalizeModelState(stateResult.value?.model_state));
      } else if (!targetSid) {
        const defaultName = stateResult.status === 'fulfilled'
          ? (stateResult.value?.name || stateResult.value?.default_model_name || '')
          : '';
        setHomeModelName(resolveHomeModelName(nextOptions, defaultName, ''));
        if (permissionResult?.status === 'fulfilled') {
          setPermissionMode(normalizePermissionMode(permissionResult.value?.mode));
        }
      }
      if (targetSid && stateResult.status === 'fulfilled') {
        toast(sessionModelReloadFeedback(
          stateResult.value?.outcome,
          stateResult.value?.warning,
        ));
      } else if (targetSid) {
        toast({ kind: 'err', text: '模型配置刷新失败:' + (stateResult.reason?.message || '') });
      } else if (modelsResult.status === 'fulfilled') {
        toast({ kind: 'ok', text: '模型列表已刷新' });
      } else {
        toast({ kind: 'err', text: '模型列表刷新失败:' + (modelsResult.reason?.message || '') });
      }
    } finally {
      setModelRefreshing(false);
    }
  }, [api, modelOptions, modelRefreshing, sid, reasoningSwitching]);

  useEffect(() => {
    if (!sid) {
      let cancelled = false;
      setPermissionSwitching(false);
      api.getDefaultPermissionMode()
        .then((state) => {
          if (!cancelled) setPermissionMode(normalizePermissionMode(state?.mode));
        })
        .catch(() => {
          if (!cancelled) setPermissionMode('default');
        });
      return () => { cancelled = true; };
    }
    let cancelled = false;
    setPermissionSwitching(false);
    api.getSessionPermissionMode(sid)
      .then((state) => {
        if (!cancelled) setPermissionMode(normalizePermissionMode(state?.mode));
      })
      .catch(() => {
        if (!cancelled) setPermissionMode(normalizePermissionMode(ref?.permission_mode));
      });
    return () => { cancelled = true; };
  }, [api, ref?.permission_mode, sid]);

  // 只接受 producer:值形式允许调用方传入一份过期快照,正是这里要根除的写法。
  const updateQueueState = useCallback((producer) => queueStore.commit(producer), [queueStore]);

  const measureStickyContext = useCallback((rowMetrics = null) => {
    const el = scrollRef.current;
    if (!el) {
      setStickyUserContext(null);
      return;
    }
    const nextContext = findStickyUserContext({
      items: itemsRef.current,
      rowMetrics: Array.isArray(rowMetrics) ? rowMetrics : collectRowMetrics(el),
      scrollTop: el.scrollTop,
      clientHeight: el.clientHeight,
      scrollHeight: el.scrollHeight,
    });
    setStickyUserContext((prev) => (
      sameStickyUserContext(prev, nextContext) ? prev : nextContext
    ));
  }, []);

  const scheduleStickyMeasure = useCallback(() => {
    if (stickyRafRef.current) cancelAnimationFrame(stickyRafRef.current);
    stickyRafRef.current = requestAnimationFrame(() => {
      stickyRafRef.current = 0;
      measureStickyContext();
    });
  }, [measureStickyContext]);

  const measureConversationTurn = useCallback((rowMetrics = null) => {
    const el = scrollRef.current;
    const turnsForRail = conversationTurnsRef.current;
    if (!el || turnsForRail.length === 0) {
      conversationTurnActivationRef.current = null;
      setActiveConversationTurn(-1);
      return;
    }
    const activation = conversationTurnActivationRef.current;
    const activatedIndex = activation?.sid === sid
      ? resolveActivatedConversationTurnIndex(
        turnsForRail,
        activation,
        el.scrollTop,
      )
      : -1;
    if (activatedIndex >= 0) {
      setActiveConversationTurn((previous) => (
        previous === activatedIndex ? previous : activatedIndex
      ));
      return;
    }
    conversationTurnActivationRef.current = null;
    const nextIndex = resolveActiveConversationTurnIndex(
      turnsForRail,
      Array.isArray(rowMetrics) ? rowMetrics : collectRowMetrics(el),
      el.scrollTop,
    );
    setActiveConversationTurn((previous) => (
      previous === nextIndex ? previous : nextIndex
    ));
  }, [sid]);

  const scheduleTranscriptMeasures = useCallback(() => {
    if (stickyRafRef.current) {
      cancelAnimationFrame(stickyRafRef.current);
      stickyRafRef.current = 0;
    }
    if (conversationTurnRafRef.current) {
      cancelAnimationFrame(conversationTurnRafRef.current);
    }
    conversationTurnRafRef.current = requestAnimationFrame(() => {
      conversationTurnRafRef.current = 0;
      const el = scrollRef.current;
      const rowMetrics = collectRowMetrics(el);
      setShowScrollToBottom(shouldShowChatScrollToBottom(el));
      measureStickyContext(rowMetrics);
      measureConversationTurn(rowMetrics);
    });
  }, [measureConversationTurn, measureStickyContext]);

  const setTailFollowFromAction = useCallback((action) => {
    tailFollowStateRef.current = nextChatTailFollowState(tailFollowStateRef.current, action);
  }, []);

  const cancelTailFollowScroll = useCallback(() => {
    const pending = tailFollowScrollRafRef.current || {};
    if (pending.first) cancelAnimationFrame(pending.first);
    if (pending.second) cancelAnimationFrame(pending.second);
    tailFollowScrollRafRef.current = { first: 0, second: 0 };
  }, []);

  const cancelActivityExpansionAnchor = useCallback(() => {
    const anchor = activityExpansionAnchorRef.current;
    if (!anchor) return;
    if (anchor.firstFrame) cancelAnimationFrame(anchor.firstFrame);
    if (anchor.secondFrame) cancelAnimationFrame(anchor.secondFrame);
    activityExpansionAnchorRef.current = null;
  }, []);

  const preserveActivityExpansionAnchor = useCallback(() => {
    const anchor = activityExpansionAnchorRef.current;
    const container = scrollRef.current;
    const titleElement = anchor?.element;
    if (
      !anchor
      || !container
      || !titleElement
      || titleElement.isConnected === false
      || !container.contains(titleElement)
    ) {
      if (anchor) cancelActivityExpansionAnchor();
      return false;
    }

    const containerRect = container.getBoundingClientRect();
    const titleRect = titleElement.getBoundingClientRect();
    const nextScrollTop = scrollTopForPreservedActivityAnchor({
      scrollTop: container.scrollTop,
      anchorViewportTop: anchor.viewportTop,
      currentAnchorViewportTop: activityAnchorViewportTop({
        containerTop: containerRect.top,
        anchorTop: titleRect.top,
      }),
      clientHeight: container.clientHeight,
      scrollHeight: container.scrollHeight,
    });

    if (Math.abs(nextScrollTop - container.scrollTop) > 0.25) {
      container.scrollTop = nextScrollTop;
      anchor.programmaticScrollTop = container.scrollTop;
    }
    return true;
  }, [cancelActivityExpansionAnchor]);

  const scheduleActivityExpansionAnchorMeasure = useCallback(() => {
    const anchor = activityExpansionAnchorRef.current;
    if (!anchor) return;
    if (anchor.firstFrame) cancelAnimationFrame(anchor.firstFrame);
    if (anchor.secondFrame) cancelAnimationFrame(anchor.secondFrame);
    anchor.firstFrame = requestAnimationFrame(() => {
      if (activityExpansionAnchorRef.current !== anchor) return;
      anchor.firstFrame = 0;
      if (!preserveActivityExpansionAnchor()) return;
      anchor.secondFrame = requestAnimationFrame(() => {
        if (activityExpansionAnchorRef.current !== anchor) return;
        anchor.secondFrame = 0;
        preserveActivityExpansionAnchor();
      });
    });
  }, [preserveActivityExpansionAnchor]);

  const beginActivityExpansionAnchor = useCallback((titleElement) => {
    historyController.cancelAnchor();
    const container = scrollRef.current;
    cancelActivityExpansionAnchor();
    if (
      !container
      || !titleElement
      || typeof titleElement.getBoundingClientRect !== 'function'
      || !container.contains(titleElement)
    ) {
      return false;
    }

    const containerRect = container.getBoundingClientRect();
    const titleRect = titleElement.getBoundingClientRect();
    cancelTailFollowScroll();
    setTailFollowFromAction({ type: 'review_pause' });
    activityExpansionAnchorRef.current = {
      element: titleElement,
      viewportTop: activityAnchorViewportTop({
        containerTop: containerRect.top,
        anchorTop: titleRect.top,
      }),
      programmaticScrollTop: null,
      firstFrame: 0,
      secondFrame: 0,
    };
    return true;
  }, [cancelActivityExpansionAnchor, cancelTailFollowScroll, historyController, setTailFollowFromAction]);

  const scheduleTailFollowScroll = useCallback(() => {
    const scrollToBottom = () => {
      if (historyController.hasAnchor()) return false;
      if (activityExpansionAnchorRef.current) return false;
      if (!shouldAutoFollowChatTail(tailFollowStateRef.current)) return false;
      const el = scrollRef.current;
      if (!el) return false;
      el.scrollTop = el.scrollHeight;
      // At the clamped maximum another scroll event is not guaranteed.
      setShowScrollToBottom(shouldShowChatScrollToBottom(el));
      return true;
    };

    cancelTailFollowScroll();
    if (!scrollToBottom()) return;

    tailFollowScrollRafRef.current.first = requestAnimationFrame(() => {
      tailFollowScrollRafRef.current.first = 0;
      if (!scrollToBottom()) return;
      tailFollowScrollRafRef.current.second = requestAnimationFrame(() => {
        tailFollowScrollRafRef.current.second = 0;
        scrollToBottom();
      });
    });
  }, [cancelTailFollowScroll, historyController]);

  const jumpToChatTail = useCallback(() => {
    historyController.cancelAnchor();
    cancelActivityExpansionAnchor();
    setTailFollowFromAction({ type: 'jump_to_tail' });
    scheduleTailFollowScroll();
    scheduleTranscriptMeasures();
  }, [cancelActivityExpansionAnchor, historyController, scheduleTailFollowScroll, scheduleTranscriptMeasures, setTailFollowFromAction]);

  const pauseTailFollowForReview = useCallback(() => {
    cancelTailFollowScroll();
    if (!busy && transcriptStatus !== 'running') return;
    setTailFollowFromAction({ type: 'review_pause' });
  }, [busy, cancelTailFollowScroll, setTailFollowFromAction, transcriptStatus]);

  const handleMessagesScroll = useCallback(() => {
    historyController.onScroll({ pointerActive: scrollActivityRef.current.pointerActive });
    const el = scrollRef.current;
    if (el) {
      const metrics = chatScrollMetrics(el);
      const prevMetrics = scrollActivityRef.current.prev;
      const anchor = activityExpansionAnchorRef.current;
      let anchorOwnsScroll = false;
      if (anchor) {
        const topChanged = !!prevMetrics
          && Math.abs(metrics.scrollTop - prevMetrics.scrollTop) > 0.25;
        const contentHeightChanged = !!prevMetrics
          && metrics.scrollHeight !== prevMetrics.scrollHeight;
        if (matchesProgrammaticActivityScroll(metrics.scrollTop, anchor.programmaticScrollTop)) {
          anchor.programmaticScrollTop = null;
          anchorOwnsScroll = true;
        } else if (!prevMetrics || contentHeightChanged || !topChanged) {
          anchorOwnsScroll = true;
        } else {
          cancelActivityExpansionAnchor();
        }
      }
      if (!anchorOwnsScroll) {
        setTailFollowFromAction({
          type: 'scroll',
          metrics,
          prevMetrics,
          userGesture: scrollActivityRef.current.pointerActive,
        });
      }
      scrollActivityRef.current.prev = metrics;
    }
    scheduleTranscriptMeasures();
  }, [cancelActivityExpansionAnchor, historyController, scheduleTranscriptMeasures, setTailFollowFromAction]);

  // 滚轮上滚是最明确的"用户想往回看"信号,不等 scroll 事件的启发式判定,
  // 直接暂停跟随(仅回合进行中生效,见 pauseTailFollowForReview 内的门)。
  const handleMessagesWheel = useCallback((event) => {
    historyController.onWheel(event);
    cancelActivityExpansionAnchor();
    if (event.deltaY < 0) pauseTailFollowForReview();
  }, [cancelActivityExpansionAnchor, historyController, pauseTailFollowForReview]);

  const handleMessagesPointerDown = useCallback(() => {
    historyController.onPointerDown();
    cancelActivityExpansionAnchor();
    scrollActivityRef.current.pointerActive = true;
  }, [cancelActivityExpansionAnchor, historyController]);

  const handleMessagesKeyDownCapture = useCallback((event) => {
    historyController.onKeyDown(event);
    if (!['ArrowUp', 'ArrowDown', 'PageUp', 'PageDown', 'Home', 'End', ' '].includes(event.key)) {
      return;
    }
    cancelActivityExpansionAnchor();
  }, [cancelActivityExpansionAnchor, historyController]);

  useEffect(() => {
    const clearPointerActive = () => {
      scrollActivityRef.current.pointerActive = false;
    };
    window.addEventListener('pointerup', clearPointerActive);
    window.addEventListener('pointercancel', clearPointerActive);
    return () => {
      window.removeEventListener('pointerup', clearPointerActive);
      window.removeEventListener('pointercancel', clearPointerActive);
    };
  }, []);

  const jumpToStickyUserSource = useCallback((context) => {
    const el = scrollRef.current;
    const targetId = String(context?.itemId || '');
    if (!el || !targetId) return;

    const targetRow = Array.from(el.querySelectorAll('[data-chat-row="true"]'))
      .find((row) => row.getAttribute('data-chat-item-id') === targetId);
    if (!targetRow) return;

    const containerRect = el.getBoundingClientRect();
    const rowRect = targetRow.getBoundingClientRect();
    historyController.cancelAnchor();
    el.scrollTo({
      top: scrollTopForStickySourceRow({
        scrollTop: el.scrollTop,
        containerTop: containerRect.top,
        rowTop: rowRect.top,
      }),
      behavior: 'smooth',
    });
    requestAnimationFrame(scheduleStickyMeasure);
    window.setTimeout(scheduleStickyMeasure, 220);
  }, [historyController, scheduleStickyMeasure]);

  const jumpToConversationTurn = useCallback((turn, index, retried = false) => {
    historyController.cancelAnchor();
    const el = scrollRef.current;
    const targetId = String(turn?.itemId || '');
    if (!el || !targetId) return;

    const targetRow = Array.from(el.querySelectorAll('[data-chat-row="true"]'))
      .find((row) => row.getAttribute('data-chat-item-id') === targetId);
    if (!targetRow) {
      // scrubber 的回合列表来自全量 itemsRef,目标可能在尾部窗口之外:
      // 全量展开后下一帧重试一次(只重试一次,防坏 id 死循环)。
      if (!retried && windowHiddenCountRef.current > 0) {
        expandTranscriptWindow();
        window.requestAnimationFrame(() => jumpToConversationTurn(turn, index, true));
      }
      return;
    }

    const containerRect = el.getBoundingClientRect();
    const rowRect = targetRow.getBoundingClientRect();
    const targetScrollTop = scrollTopForStickySourceRow({
      scrollTop: el.scrollTop,
      containerTop: containerRect.top,
      rowTop: rowRect.top,
      topInset: 20,
    });

    pauseTailFollowForReview();
    conversationTurnActivationRef.current = {
      sid,
      itemId: targetId,
      scrollTop: el.scrollTop,
    };
    flushSync(() => {
      setActiveConversationTurn(index);
    });
    el.scrollTop = targetScrollTop;
    conversationTurnActivationRef.current = {
      sid,
      itemId: targetId,
      scrollTop: el.scrollTop,
    };
    window.requestAnimationFrame(scheduleTranscriptMeasures);
  }, [expandTranscriptWindow, historyController, pauseTailFollowForReview, scheduleTranscriptMeasures, sid]);

  const focusChatInput = useCallback((force = false) => {
    if (questionRequest) return;
    if (!force && isEditableElement(document.activeElement)) return;
    inputRef.current?.focus();
  }, [questionRequest]);

  const restoreChatInputFocusSoon = useCallback((force = false) => {
    requestAnimationFrame(() => {
      focusChatInput(force);
      requestAnimationFrame(() => focusChatInput(force));
      window.setTimeout(() => focusChatInput(force), 80);
    });
  }, [focusChatInput]);

  useEffect(() => {
    if (sid || !stagedExpertDraft.present) return;
    // The expert page shares this mounted ChatView. A new prompt can arrive
    // without a scope change; apply it after normal draft restoration.
    draftEditVersionRef.current += 1;
    composerDirtyRef.current = !!stagedExpertDraft.text;
    setComposerValue(stagedExpertDraft.text);
    onHomeComposerDraftChange?.(homeDraftWorkspaceHash, stagedExpertDraft.text, api);
    onInitialDraftConsumed?.();
    restoreChatInputFocusSoon(true);
  }, [
    api,
    sid,
    stagedExpertDraft.present,
    stagedExpertDraft.text,
    homeDraftWorkspaceHash,
    onHomeComposerDraftChange,
    onInitialDraftConsumed,
    restoreChatInputFocusSoon,
  ]);

  useEffect(() => {
    if (composerSubmitting || !restoreComposerFocusAfterSubmitRef.current) return;
    restoreComposerFocusAfterSubmitRef.current = false;
    restoreChatInputFocusSoon(false);
  }, [composerSubmitting, restoreChatInputFocusSoon]);

  useLayoutEffect(() => {
    cancelActivityExpansionAnchor();
    setTailFollowFromAction({ type: 'session_reset' });
    setShowScrollToBottom(false);
    lastUserTurnKeyRef.current = '';
    scrollActivityRef.current = { prev: null, pointerActive: false };
    return cancelActivityExpansionAnchor;
  }, [cancelActivityExpansionAnchor, sid, setTailFollowFromAction]);

  useEffect(() => {
    if (!sid || !lastUserTurnKey) return;
    const prev = lastUserTurnKeyRef.current;
    if (!prev || prev !== lastUserTurnKey) {
      historyController.cancelAnchor();
      cancelActivityExpansionAnchor();
      setTailFollowFromAction({ type: 'new_turn' });
    }
    lastUserTurnKeyRef.current = lastUserTurnKey;
  }, [cancelActivityExpansionAnchor, historyController, lastUserTurnKey, setTailFollowFromAction, sid]);

  // 只在用户仍跟随底部时自动滚到底。审查栏会异步测量高度并给消息区补
  // bottom padding,因此跟随模式下仍需在 padding 生效后补几帧滚动。
  useLayoutEffect(() => {
    scheduleTailFollowScroll();
    return cancelTailFollowScroll;
  }, [
    activity?.detail,
    activity?.label,
    activity?.phase,
    activity?.toolCallId,
    activity?.toolIndex,
    busy,
    cancelTailFollowScroll,
    changeDockBottomPadding,
    permissionRequests,
    renderedItems,
    scheduleTailFollowScroll,
    sid,
  ]);

  const handleTranscriptContentResize = useCallback(() => {
    scheduleTranscriptMeasures();
    if (historyController.preserveAnchor()) return;
    if (preserveActivityExpansionAnchor()) return;
    scheduleTailFollowScroll();
  }, [historyController, preserveActivityExpansionAnchor, scheduleTailFollowScroll, scheduleTranscriptMeasures]);

  useEffect(() => observeChatTailContent(
    transcriptContentRef.current,
    handleTranscriptContentResize,
  ), [handleTranscriptContentResize, sid]);

  useLayoutEffect(() => {
    const previous = searchJumpRetryRef.current || {};
    if (previous.frame) cancelAnimationFrame(previous.frame);
    if (previous.timer) window.clearTimeout(previous.timer);
    searchJumpRetryRef.current = { frame: 0, timer: 0 };

    if (!sid || (searchJumpOrdinal === null && searchJumpPosition === null)) return undefined;

    historyController.cancelAnchor();

    let cancelled = false;
    const task = { frame: 0, timer: 0, attempts: 0, settled: 0 };
    searchJumpRetryRef.current = task;

    const scheduleRetry = (delay) => {
      task.timer = window.setTimeout(() => {
        task.timer = 0;
        task.frame = requestAnimationFrame(run);
      }, delay);
    };

    const run = () => {
      task.frame = 0;
      if (cancelled || searchJumpRetryRef.current !== task) return;
      task.attempts += 1;

      const el = scrollRef.current;
      const targetRow = searchJumpTargetRow(el, searchJumpOrdinal, searchJumpPosition);
      if (el && targetRow) {
        cancelTailFollowScroll();
        setTailFollowFromAction({ type: 'review_pause' });
        el.scrollTop = scrollTopForCenteredRow(el, targetRow);
        scheduleStickyMeasure();
        task.settled += 1;
      } else if (el && windowHiddenCountRef.current > 0) {
        // 目标行可能在尾部窗口之外(深链指向老消息)—— 全量展开后让既有
        // 重试循环在下一帧找到它。
        expandTranscriptWindow();
      }

      if (task.settled >= 3 || task.attempts >= 12) {
        if (searchJumpRetryRef.current === task) {
          searchJumpRetryRef.current = { frame: 0, timer: 0 };
        }
        return;
      }
      scheduleRetry(targetRow ? 70 : 90);
    };

    run();
    return () => {
      cancelled = true;
      if (task.frame) cancelAnimationFrame(task.frame);
      if (task.timer) window.clearTimeout(task.timer);
      if (searchJumpRetryRef.current === task) {
        searchJumpRetryRef.current = { frame: 0, timer: 0 };
      }
    };
  }, [
    cancelTailFollowScroll,
    changeDockBottomPadding,
    expandTranscriptWindow,
    historyController,
    ref,
    renderedItems,
    scheduleStickyMeasure,
    searchJumpOrdinal,
    searchJumpPosition,
    setTailFollowFromAction,
    sid,
  ]);

  useEffect(() => {
    let timer = 0;
    const id = requestAnimationFrame(() => {
      focusChatInput(true);
      timer = window.setTimeout(() => focusChatInput(true), 80);
    });
    return () => {
      cancelAnimationFrame(id);
      if (timer) window.clearTimeout(timer);
    };
  }, [sid, focusChatInput]);

  useEffect(() => {
    return bindDesktopComposerAutoFocus({
      enabled: autoFocusOnDesktopWindowFocus,
      onFocus: () => restoreChatInputFocusSoon(true),
    });
  }, [autoFocusOnDesktopWindowFocus, restoreChatInputFocusSoon]);

  useLayoutEffect(() => {
    itemsRef.current = renderedItems;
    scheduleTranscriptMeasures();
  }, [renderedItems, scheduleTranscriptMeasures]);

  useLayoutEffect(() => {
    scheduleTranscriptMeasures();
  }, [permissionRequests, scheduleTranscriptMeasures]);

  useLayoutEffect(() => {
    conversationTurnsRef.current = preparedConversationTurns;
    scheduleTranscriptMeasures();
  }, [preparedConversationTurns, scheduleTranscriptMeasures]);

  useEffect(() => {
    conversationTurnActivationRef.current = null;
    setStickyUserContext(null);
    setActiveConversationTurn(-1);
    scheduleTranscriptMeasures();
  }, [sid, scheduleTranscriptMeasures]);

  useEffect(() => () => {
    if (stickyRafRef.current) {
      cancelAnimationFrame(stickyRafRef.current);
      stickyRafRef.current = 0;
    }
    if (conversationTurnRafRef.current) {
      cancelAnimationFrame(conversationTurnRafRef.current);
      conversationTurnRafRef.current = 0;
    }
  }, []);

  useEffect(() => {
    const el = scrollRef.current;
    if (!el) return undefined;

    const onResize = () => scheduleTranscriptMeasures();
    window.addEventListener('resize', onResize);

    let mutationObserver = null;
    if (typeof MutationObserver !== 'undefined') {
      mutationObserver = new MutationObserver(scheduleTranscriptMeasures);
      mutationObserver.observe(el, { childList: true, subtree: true, characterData: true });
    }

    let resizeObserver = null;
    if (typeof ResizeObserver !== 'undefined') {
      resizeObserver = new ResizeObserver(scheduleTranscriptMeasures);
      resizeObserver.observe(el);
    }

    scheduleTranscriptMeasures();
    return () => {
      window.removeEventListener('resize', onResize);
      mutationObserver?.disconnect();
      resizeObserver?.disconnect();
    };
  }, [sid, scheduleTranscriptMeasures]);

  useEffect(() => {
    if (sid || !ref?.homeWorkspaceExplicit) return;
    persistHomeWorkspaceHash(ref?.workspaceHash || '');
  }, [persistHomeWorkspaceHash, ref?.homeWorkspaceExplicit, ref?.workspaceHash, sid]);

  useEffect(() => {
    if (sid) setCreateProjectOpen(false);
  }, [sid]);

  // ref 携带 homeWorkspaceExplicit 时,工作区归属是导航意图的一部分:
  // 同步落到 homeWorkspaceHash,不等 /api/workspaces 返回。否则点击
  // 「新建任务」后,聊天区的工作区显示、提交目标、命令工作区、输入历史
  // 都要等整个列表请求回来才切到无工作区(实测 3-4 秒)。
  useEffect(() => {
    if (sid || !ref?.homeWorkspaceExplicit) return;
    setHomeWorkspaceHash(ref?.workspaceHash || '');
  }, [ref?.homeWorkspaceExplicit, ref?.workspaceHash, sid]);

  useEffect(() => {
    if (sid) return undefined;
    let cancelled = false;

    const load = async () => {
      const explicitHomeWorkspace = !!ref?.homeWorkspaceExplicit;
      let preferredHash = explicitHomeWorkspace ? '' : homeWorkspacePreferenceHash;
      if (!explicitHomeWorkspace) {
        const desktopHash = await readDesktopHomeWorkspaceHash();
        if (desktopHash !== null) {
          preferredHash = desktopHash;
          if (desktopHash !== homeWorkspacePreferenceHash) {
            setHomeWorkspaceSelection({ workspaceHash: desktopHash });
          }
        }
      }

      let options = [];
      try {
        const list = await api.listWorkspaces();
        options = Array.isArray(list)
          ? list.map((w, i) => normalizeWorkspaceOption(w, i)).filter(Boolean)
          : [];
      } catch {
        options = [];
      }

      if (options.length === 0 && hasDesktopBridge()) {
        try {
          const list = parseDesktopResult(await window.aceDesktop_listWorkspaces());
          options = Array.isArray(list)
            ? list.map((w, i) => normalizeWorkspaceOption(w, i)).filter(Boolean)
            : [];
        } catch {
          options = [];
        }
      }

      const fallback = fallbackWorkspaceOption(ref, health);
      if (options.length === 0 || (isRealWorkspaceHash(fallback.hash) && !options.some((w) => w.hash === fallback.hash))) {
        options = [fallback, ...options];
      }
      if (options.length === 0) options = [fallback];

      if (cancelled) return;
      setHomeWorkspaces(options);
      setHomeWorkspaceHash((prev) => {
        const explicitHash = ref?.homeWorkspaceExplicit ? (ref?.workspaceHash || '') : '';
        return resolveHomeWorkspaceHash({
          preferredHash,
          explicitHash,
          explicitHashSet: explicitHomeWorkspace,
          previousHash: prev,
          options,
        });
      });
    };

    load();
    return () => { cancelled = true; };
  }, [api, health, homeWorkspacePreferenceHash, ref, setHomeWorkspaceSelection, sid]);

  // 拉 history(per-cwd)
  useEffect(() => {
    const cwd = sid
      ? (ref?.cwd || health?.cwd || '')
      : (selectedHomeWorkspace?.cwd || ref?.cwd || '');
    if (!cwd) {
      // 无工作空间的新任务没有 per-cwd 历史;显式清掉上一次工作区残留,
      // 避免上下键翻出上一个项目的输入历史。
      setHistory([]);
      return;
    }
    api.getHistory(cwd, 200)
      .then((r) => setHistory(Array.isArray(r) ? r : []))
      .catch(() => {});
  }, [api, health?.cwd, ref?.cwd, selectedHomeWorkspace?.cwd, sid]);

  const recordInputHistory = useCallback((text) => {
    api.appendHistory(text).catch(() => {});
    setHistory((h) => [...h, text]);
  }, [api]);

  const enqueueInput = useCallback((payload) => {
    if (!sid) return;
    // cwd 输入历史只记编辑器文本:内联粘贴块的正文不进历史。
    const text = typeof payload === 'string' ? payload : inputHistoryTextForPayload(payload);
    updateQueueState((prev) => enqueueQueuedInput(prev, { sessionId: sid, payload }));
    if (text.trim()) recordInputHistory(text);
    // 标题不在本地用消息全文改写:服务端落盘后经 session_updated{summary} 下发
    // 截断后的摘要,顶部与侧栏同源。
  }, [recordInputHistory, sid, updateQueueState]);

  const cancelQueued = useCallback((queuedId) => {
    updateQueueState((prev) => cancelQueuedInput(prev, queuedId));
  }, [updateQueueState]);

  // 解除「用户中断回合」带来的队列暂停。实际出队仍由下面的 drain effect 在
  // queueState 变化后接管(busy=false 且不再 paused 才会发),这里只改标记。
  const resumeQueue = useCallback(() => {
    const targetSid = sidRef.current;
    if (!targetSid) return;
    const current = transcript.getState();
    updateQueueState((prev) => resumeQueuedInput(prev, targetSid, {
      afterAbortTurnId: current.abortPending ? (current.abortTurnId || 'pending-stop') : '',
    }));
  }, [updateQueueState, transcript.getState]);

  const retryQueued = useCallback((queuedId) => {
    // 点「重试」是用户明确要发这条消息,暂停态一并解除,否则按钮按了没反应。
    updateQueueState((prev) => {
      const next = retryQueuedInput(prev, queuedId);
      const sessionId = next.items.find((item) => item?.queued?.id === queuedId)?.queued?.sessionId;
      const current = transcript.getState();
      return sessionId ? resumeQueuedInput(next, sessionId, {
        afterAbortTurnId: sessionId === sidRef.current && current.abortPending
          ? (current.abortTurnId || 'pending-stop') : '',
      }) : next;
    });
  }, [updateQueueState, transcript.getState]);

  const saveQueuedEdit = useCallback((queuedId, text, composerContent) => {
    const queuedItem = queueStore.getState().items.find(
      (item) => item?.queued?.id === queuedId,
    );
    if (queuedItem?.queued?.state !== QUEUED_INPUT_STATE.QUEUED &&
        queuedItem?.queued?.state !== QUEUED_INPUT_STATE.FAILED) {
      return;
    }
    // 编辑框给的是编辑器文本;消息正文还要拼上内联粘贴块(与 normalizeComposerPayload 一致)。
    const nextText = appendPastedTextToSubmission(String(text ?? ''), composerContent);
    const payload = queuedItem.queued.payload || {};
    const hasExtras = (Array.isArray(payload.attachments) && payload.attachments.length > 0)
      || (Array.isArray(payload.contexts) && payload.contexts.length > 0)
      || normalizeComposerContent(composerContent)?.parts.some((part) => part.type === 'attachment');
    if (!nextText.trim() && !hasExtras) {
      toast({ kind: 'err', text: '消息不能为空' });
      return;
    }
    updateQueueState((prev) => updateQueuedInputContent(prev, queuedId, nextText, { composerContent }));
  }, [queueStore, updateQueueState]);

  const runSideQuestion = useCallback((
    rawQuestion,
    { command = 'btw', recordHistory = false, historyText = null } = {},
  ) => {
    const question = String(rawQuestion || '').trim();
    const targetSid = sidRef.current;
    if (!targetSid) {
      toast({ kind: 'err', text: '请先在已有会话中使用 /btw 或 /side' });
      return null;
    }
    setSideChatAnchor(null);
    sideChat.open();
    if (!question) return true;
    if (sideChat.getSnapshot().busy) {
      toast({ kind: 'err', text: '已有旁路提问正在回答，请稍候' });
      return false;
    }
    const started = sideChat.submit(question);
    // historyText:输入框提交时的编辑器文本(不含内联粘贴块正文,见 inputHistoryTextForPayload)。
    // question 里已经拼上了内联块正文,不能直接进 cwd 历史。
    if (started && recordHistory) {
      const entry = typeof historyText === 'string' ? historyText : `/${command} ${question}`;
      if (entry.trim()) recordInputHistory(entry);
    }
    return started;
  }, [sideChat, recordInputHistory]);

  const openSideQuestionComposer = useCallback(() => {
    if (!sidRef.current) {
      toast({ kind: 'err', text: '请先在已有会话中使用 /btw 或 /side' });
      return;
    }
    setSideChatAnchor(null);
    sideChat.open();
  }, [sideChat]);

  const guideQueued = useCallback((queuedId) => {
    const targetSid = sidRef.current;
    const expectedTurnId = String(activeTurnId || '');
    if (!targetSid || !busy || !expectedTurnId) {
      toast({ kind: 'err', text: '当前没有可插话的运行中回合' });
      return null;
    }
    const queuedItem = queueStore.getState().items.find(
      (item) => item?.queued?.id === queuedId,
    );
    if (queuedItem?.queued?.state !== QUEUED_INPUT_STATE.QUEUED &&
        queuedItem?.queued?.state !== QUEUED_INPUT_STATE.FAILED) {
      return null;
    }
    const requestPayload = queuedInputRequestPayload(queuedItem);
    if (!requestPayload) {
      toast({ kind: 'err', text: '找不到这条排队消息' });
      return null;
    }

    updateQueueState((prev) => beginQueuedGuidance(
      prev,
      queuedId,
      { turnId: expectedTurnId },
    ));
    // AskUserQuestion 挂起时,排队卡片的「插话」同样走提问插话而不是打断:问题
    // 以「用户改为直接输入」收掉,消息紧跟工具结果进入同一回合,不 abort。
    // 问题已在别处结束(NO_PENDING_QUESTION)再退回立即打断。
    const pendingQuestion = questionForView?.request_id
      ? { sid: questionForView.session_id || targetSid, requestId: questionForView.request_id }
      : null;
    const interruptNow = () => api.interruptTurn(targetSid, {
      ...requestPayload,
      expected_turn_id: expectedTurnId,
    });
    const submitGuidance = pendingQuestion
      ? api.interjectQuestion(pendingQuestion.sid, {
          ...requestPayload,
          request_id: pendingQuestion.requestId,
        }).catch((e) => {
          if (e?.code !== 'NO_PENDING_QUESTION') throw e;
          onQuestionResolve?.();
          return interruptNow();
        })
      : interruptNow();
    return submitGuidance
      .then((result) => {
        updateQueueState((prev) => markQueuedGuidanceAccepted(
          prev,
          queuedId,
          { turnId: result?.turn_id || expectedTurnId },
        ));
        return true;
      })
      .catch((e) => {
        updateQueueState((prev) => finishQueuedGuidance(
          prev,
          queuedId,
          { succeeded: false },
        ));
        toast({ kind: 'err', text: '插话提交失败:' + (e?.message || '未知错误') });
        return false;
      });
  }, [activeTurnId, api, busy, onQuestionResolve, questionForView, queueStore, updateQueueState]);

  const executeBuiltinCommand = useCallback((targetSid, command) => (
    api.executeCommand(targetSid, command).then((result) => {
      const refreshDetail = remoteControlSessionRefreshForCommand(command, targetSid);
      if (refreshDetail) {
        const noWorkspace = sid
          ? Boolean(ref?.noWorkspace || ref?.no_workspace)
          : Boolean(selectedHomeWorkspace?.noWorkspace);
        notifySessionListChanged({
          ...refreshDetail,
          workspaceHash: noWorkspace ? '' : commandWorkspaceHash,
          noWorkspace,
        });
      }
      return result;
    })
  ), [api, commandWorkspaceHash, ref?.noWorkspace, ref?.no_workspace, selectedHomeWorkspace?.noWorkspace, sid]);

  const retryUserMessageId = trailingUserMessageRetryId({
    sessionId: sid, items, loadState: transcriptLoadState, busy,
    status: transcriptStatus, streamingId, abortPending,
    disabled: readOnlyExternalSession || sessionRuntimeUnavailable,
  });

  // 首页发送、会话内发送、排队出队三条路径都经过这里。
  const sendInputOrBuiltin = useCallback(async (targetSid, payload) => {
    const requestPayload = typeof payload === 'string' ? { text: payload } : payload;
    const hasExtras = payloadHasExtras(requestPayload);
    const route = inputRouteForPayload(requestPayload);
    if (!hasExtras && route.kind === 'builtin') {
      return executeBuiltinCommand(targetSid, route.command);
    }
    const materialized = await materializeWorkspaceDraftPastes(targetSid, payload);
    return api.sendInput(targetSid, materialized);
  }, [api, executeBuiltinCommand, materializeWorkspaceDraftPastes]);

  const submit = useCallback((text) => {
    if (sessionRuntimeUnavailable) return;
    const submittedContent = reconcileComposerContentAttachments(composerContentRef.current, composerAttachments);
    const activeAttachments = submittedContent ? composerContentAttachments(submittedContent, composerAttachments, { sessionId: sid }) : composerAttachments;
    if (activeAttachments.some((item) => item.error || (!item.id && !item.pending_upload && !item.uploading))) {
      toast({ kind: 'err', text: tr('composerAttachment.stagedUnavailable') });
      return;
    }
    if (activeAttachments.some((item) => item.uploading)) {
      toast({ kind: 'err', text: '附件仍在上传，请稍后发送' });
      return;
    }
    const pendingAttachmentLocalIds = activeAttachments
      .filter((item) => item?.pending_upload && item?.local_id)
      .map((item) => item.local_id);
    const pendingAttachmentFiles = composerAttachmentFilesForLocalIds(
      attachmentReservationsRef.current,
      pendingAttachmentLocalIds,
    );
    const hasPendingAttachments = pendingAttachmentLocalIds.length > 0;
    if (pendingAttachmentFiles.length !== pendingAttachmentLocalIds.length) {
      toast({ kind: 'err', text: tr('composerAttachment.stagedUnavailable') });
      return;
    }
    if (sid && hasPendingAttachments) {
      const pendingItems = activeAttachments.filter((item) => item?.pending_upload && item?.local_id);
      if (pendingItems.every((item) => item.upload_deferred)) {
        // 翻到旧历史、还没上传的粘贴块:现在开始上传,完成后由用户再发送一次。
        // 暂存组已不在(例如从首页草稿带过来)时按保留的 File 直接上传。
        if (!commitDeferredPastes()) uploadPasteReservations(pendingAttachmentFiles);
        toast({ kind: 'info', text: '粘贴的文本正在上传，完成后请再发送' });
        return;
      }
      toast({ kind: 'err', text: tr('composerAttachment.uploadRequired') });
      return;
    }
    const payload = normalizeComposerPayload(
      text,
      activeAttachments,
      composerContexts,
      swarmModeForSubmission(composerSwarmChoice, transcriptSwarmMode),
      submittedContent,
    );
    // 提交那一刻输入框里的原文。发送回执回来时拿它比对,用户在等待窗口里
    // 写下的下一条就不会被这次发送的清理吞掉。
    const submittedComposerText = composerValueRef.current;
    const submittedComposerContent = composerContentRef.current;
    const hasExtras = payloadHasExtras(payload) || hasPendingAttachments;
    const hasSwarmMode = typeof payload.swarm_mode === 'string';
    if (!payload.text.trim() && !hasExtras) {
      if (!retryUserMessageId || composerSubmitting || retrySubmissionRef.current) return;
      const latest = transcript.getState();
      const latestRetryId = trailingUserMessageRetryId({
        ...latest, sessionId: sid, loadState: transcriptLoadState,
        disabled: readOnlyExternalSession || sessionRuntimeUnavailable,
      });
      if (latestRetryId !== retryUserMessageId) return;
      const request = { sessionId: sid };
      retrySubmissionRef.current = request;
      setComposerSubmitting(true);
      setTailFollowFromAction({ type: 'new_turn' });
      dockAutoDismissRef.current();
      api.retryLastUserMessage(sid, latestRetryId)
        .catch((error) => {
          if (retrySubmissionRef.current !== request || sidRef.current !== request.sessionId) return;
          toast({ kind: 'err', text: '发送失败:' + (error.message || '') });
        })
        .finally(() => {
          if (retrySubmissionRef.current !== request || sidRef.current !== request.sessionId) return;
          retrySubmissionRef.current = null;
          setComposerSubmitting(false);
          restoreChatInputFocusSoon(false);
        });
      return;
    }
    const route = inputRouteForPayload(payload);
    if (route.kind === 'paste_too_long') {
      // /goal、/btw 带文件块或合并后超过服务端上限:明确提示并保留输入框,不偷偷改发
      // 成一条字面的「/goal …」普通消息。
      toast({
        kind: 'err',
        text: `粘贴的文本太长，超出 /${route.command} 的上限（${route.limitBytes} 字节）。去掉命令可作为普通消息发送，或缩短内容`,
      });
      return;
    }
    const historyText = inputHistoryTextForPayload(payload);
    if (route.kind === 'desktop_feedback') {
      if (!sid) {
        toast({ kind: 'err', text: tr('feedbackCommand.requiresSession') });
        return;
      }
      if (hasExtras) {
        toast({ kind: 'err', text: tr('feedbackCommand.textOnly') });
        return;
      }
      if (composerSubmitting) return;

      const noWorkspace = !!(ref?.noWorkspace || ref?.no_workspace);
      const requestPayload = buildCurrentSessionDesktopFeedbackPayload({
        feedbackText: route.feedbackText,
        sessionId: sid,
        workspaceHash:
          ref?.workspaceHash || ref?.workspace_hash || draftWorkspaceHash,
        noWorkspace,
      });
      if (!requestPayload) {
        toast({ kind: 'err', text: tr('feedbackCommand.unknownSession') });
        return;
      }

      setComposerSubmitting(true);
      api.submitDesktopFeedback(requestPayload)
        .then((result) => {
          // cwd 历史只记编辑器文本(route.display_text 拼上了内联粘贴块正文)。
          if (historyText.trim()) recordInputHistory(historyText);
          clearCurrentSessionDraft();
          const packageName = String(result?.package_filename || '').trim();
          toast({
            kind: 'ok',
            text: packageName
              ? tr('feedbackCommand.uploadedWithPackage', { packageName })
              : tr('feedbackCommand.uploaded'),
          });
        })
        .catch((e) => {
          toast({
            kind: 'err',
            text: tr('feedbackCommand.uploadFailed', {
              error: e?.message || tr('common.unknown'),
            }),
          });
        })
        .finally(() => {
          setComposerSubmitting(false);
          restoreChatInputFocusSoon(false);
        });
      return;
    }
    if (route.kind === 'side_question') {
      if (hasExtras) {
        toast({ kind: 'err', text: `/${route.command} 暂不支持附件或上下文，请仅提交文字问题` });
        return;
      }
      const started = runSideQuestion(route.question, {
        command: route.command,
        recordHistory: true,
        historyText,
      });
      if (started) {
        clearCurrentSessionDraft();
        clearComposerExtras();
      }
      return;
    }
    if (route.kind === 'turn_steer') {
      if (!sid) {
        toast({ kind: 'err', text: '请先在运行中的会话里使用 /turn' });
        return;
      }
      if (!busy || !activeTurnId) {
        toast({ kind: 'err', text: '当前没有可插话的运行中回合' });
        return;
      }
      if (!route.guidance && !hasExtras) {
        toast({ kind: 'err', text: '用法：/turn <插话内容>' });
        return;
      }
      if (composerSubmitting) return;

      const targetSid = sid;
      const expectedTurnId = activeTurnId;
      const interruptRequestKey = `${targetSid}\u0000${expectedTurnId}`;
      if (turnInterruptInFlightRef.current.has(interruptRequestKey)) return;
      const steerPayload = {
        ...payload,
        text: route.guidance,
        ...(payload.composer_content ? { composer_content: composerContentForGuidance(payload.composer_content) } : {}),
        expected_turn_id: expectedTurnId,
        client_message_id:
          `turn-${targetSid}-${Date.now()}-${Math.random().toString(36).slice(2, 8)}`,
      };
      turnInterruptInFlightRef.current.add(interruptRequestKey);
      setComposerSubmitting(true);
      // 首页草稿里的粘贴文件块先导入成会话附件(唯一导入点),再提交插话。
      materializeWorkspaceDraftPastes(targetSid, steerPayload)
        // eslint-disable-next-line no-shadow -- 导入后的请求体就是要提交的插话
        .then((steerPayload) => api.interruptTurn(targetSid, steerPayload)
          .then(() => {
            // cwd 历史只记编辑器文本(带 /turn 前缀),不含内联粘贴块正文 —— 否则翻回
            // 这条历史时整段(含 /turn)被折叠成粘贴块,发出去就不再是插话。
            if (historyText.trim()) recordInputHistory(historyText);
            if (clearCurrentSessionDraft({ expectedText: submittedComposerText, expectedContent: submittedComposerContent })) clearComposerExtras();
            toast({ kind: 'ok', text: '插话已提交，正在打断当前回合' });
          }))
        .catch((e) => {
          toast({ kind: 'err', text: '插话提交失败:' + (e?.message || '未知错误') });
        })
        .finally(() => {
          turnInterruptInFlightRef.current.delete(interruptRequestKey);
          setComposerSubmitting(false);
          restoreChatInputFocusSoon(false);
        });
      return;
    }
    const isBuiltin = !hasExtras && route.kind === 'builtin';
    if (!isBuiltin || hasExtras) {
      setTailFollowFromAction({ type: 'new_turn' });
      // 新一轮对话开始:上一轮的玻璃 dock(变更汇总 + todo 环)自动收起,
      // 本轮产生新变更 / todo 更新后按签名机制重现。
      dockAutoDismissRef.current();
    }
    // 提问挂起期间没有插话入口:composer dock 被提问框整体替换(方案 A),
    // 输入框不渲染,submit 只可能来自「没有待答问题」的那一帧渲染。这里不做
    // 提问插话分支,避免在不可达路径上保留第二套提问收尾逻辑。
    if (!sid) {
      // 自动新建会话。普通消息由 daemon auto_start 接管;builtin 先创建
      // 空会话,再走专门 command endpoint。
      const trimmed = String(payload.text || '').trim();
      if ((!trimmed && !hasExtras) || homeSubmitting || composerSubmitting) return;
      const submittedHomeDraftWorkspaceHash = homeDraftWorkspaceHash;
      const submittedHomeDraftText = homeComposerDraft(homeComposerDrafts, homeDraftWorkspaceHash);
      // GitSessionPill 的 worktree 意图:命中时改走 auto_start:false +
      // 首条消息携带 worktree 字段(daemon 在入队前创建并切 cwd)。
      // builtin(/init 等)不带 worktree —— 意图只作用于普通消息。
      const worktreeIntent = !isBuiltin
        ? buildWorktreeIntent({ ...gitPillIntentRef.current, sessionStarted: false })
        : null;
      const explicitHomeSend = !isBuiltin && (hasExtras || hasSwarmMode || !!worktreeIntent || !!payload.composer_content);
      const createOptions = explicitHomeSend
        ? { auto_start: false }
        : sessionCreateOptionsForText(payload.text);
      let sessionCreated = false;
      let createdSessionId = '';
      setComposerSubmitting(true);
      createHomeComposerSession(payload.text, {
        createOptions,
        firstUserMessageText: !isBuiltin ? payload.text : '',
        firstUserMessageContent: !isBuiltin ? payload.composer_content : null,
        firstUserMessageAttachments: !isBuiltin ? activeAttachments : [],
        preserveExtras: hasExtras || hasSwarmMode || !!payload.composer_content,
        // 标题种子只用编辑器文本(截 200 字符),没有就用第一个粘贴块的标题:payload.text
        // 里拼着内联块的正文,不能拿来当标题。
        title: sessionTitleSeedForPayload(payload) || (hasPendingAttachments
          ? (activeAttachments[0]?.name || '附件消息')
          : ''),
      })
        .then(async (created) => {
          const id = created?.id;
          if (!id) return;
          sessionCreated = true;
          createdSessionId = id;
          const materializedAttachments = pendingAttachmentFiles.length > 0
            ? await persistMediaFilesToSession(id, pendingAttachmentFiles)
            : [];
          const materializedPayload = payloadWithAttachmentIds(payload, materializedAttachments);
          if (!isBuiltin && materializedPayload.composer_content) {
            setPendingNewSessionFirstUserMessage((pending) => pending?.sessionId === id
              ? createPendingNewSessionFirstUserMessage({
                  sessionId: id,
                  text: materializedPayload.text,
                  composerContent: materializedPayload.composer_content,
                  attachments: materializedAttachments,
                  timestampMs: pending.item.ts,
                })
              : pending);
          }
          if (isBuiltin) {
            await executeBuiltinCommand(id, route.command);
          } else if (explicitHomeSend) {
            applyEvent({ type: 'busy_changed', payload: { busy: true } }, { emitEffects: false });
            const sendPayload = worktreeIntent
              ? { ...materializedPayload, worktree: worktreeIntent }
              : materializedPayload;
            const queued = await sendInputOrBuiltin(id, sendPayload);
            if (worktreeIntent) {
              setLocalWorktree({
                sid: id,
                name: queued?.worktree?.name || `ses-${id}`,
                branch: queued?.worktree?.branch || '',
                path: queued?.worktree?.path || '',
              });
              const noWorkspace = !!created?.target?.noWorkspace;
              notifySessionListChanged({
                reason: 'worktree-created',
                sessionId: id,
                workspaceHash: noWorkspace
                  ? ''
                  : (created?.response?.workspace_hash || created?.target?.hash || ''),
                noWorkspace,
              });
            }
          }
          if (historyText.trim()) recordInputHistory(historyText);
          if (!isBuiltin && explicitHomeSend) {
            setAcceptedHomeSubmission({
              sessionId: id,
              text: submittedComposerText,
              content: submittedComposerContent,
              clearExtras: hasExtras || hasSwarmMode,
            });
          }
          onHomeComposerDraftAccepted?.(
            submittedHomeDraftWorkspaceHash,
            submittedHomeDraftText,
            api,
          );
        })
        .catch((e) => {
          if (explicitHomeSend && createdSessionId) {
            setPendingNewSessionFirstUserMessage((pending) => (
              pending?.sessionId === createdSessionId ? null : pending
            ));
          }
          if (explicitHomeSend) {
            applyEvent({ type: 'busy_changed', payload: { busy: false } }, { emitEffects: false });
          }
          toast({
            kind: 'err',
            text: (sessionCreated ? '发送失败:' : '新建会话失败:') + (e.message || ''),
          });
        })
        .finally(() => {
          setComposerSubmitting(false);
          restoreChatInputFocusSoon(false);
        });
      return;
    }
    if (composerSubmitting) return;
    // 用户再次主动发送 / 排队 = 解除「中断回合」带来的队列暂停:这条新消息先走,
    // 排队里的旧消息在它之后照常出队(与卡片栈上的「继续」同义)。
    const liveTranscript = transcript.getState();
    updateQueueState((prev) => resumeQueuedInput(prev, sid, {
      afterAbortTurnId: liveTranscript.abortPending
        ? (liveTranscript.abortTurnId || 'pending-stop') : '',
    }));
    if ((liveTranscript.busy || liveTranscript.abortPending) && !isBuiltin) {
      enqueueInput(payload);
      clearCurrentSessionDraft();
      clearComposerExtras();
      restoreChatInputFocusSoon(false);
      return;
    }
    const targetSid = sid;
    restoreComposerFocusAfterSubmitRef.current = true;
    setComposerSubmitting(true);
    if (!isBuiltin) {
      applyEvent({ type: 'busy_changed', payload: { busy: true } }, { emitEffects: false });
    }
    // 空会话的首条消息:GitSessionPill 勾了 worktree 时随消息带创建意图。
    const sessionWorktreeIntent = !isBuiltin
      ? buildWorktreeIntent({
          ...gitPillIntentRef.current,
          sessionStarted: rawItemsLengthRef.current > 0,
        })
      : null;
    const sessionSendPayload = sessionWorktreeIntent
      ? { ...payload, worktree: sessionWorktreeIntent }
      : { ...payload };
    if (!isBuiltin) {
      sessionSendPayload.client_message_id = `input-${targetSid}-${globalThis.crypto?.randomUUID?.() || `${Date.now()}-${Math.random().toString(36).slice(2)}`}`;
    }
    sendInputOrBuiltin(targetSid, sessionSendPayload)
      .then((queued) => {
        if (!isBuiltin && sidRef.current === targetSid) {
          applyEvent(acceptedUserInputEvent(sessionSendPayload), { emitEffects: false });
        }
        if (sessionWorktreeIntent) {
          setLocalWorktree({
            sid: targetSid,
            name: queued?.worktree?.name || `ses-${targetSid}`,
            branch: queued?.worktree?.branch || '',
            path: queued?.worktree?.path || '',
          });
          const noWorkspace = !!(ref?.noWorkspace || ref?.no_workspace);
          notifySessionListChanged({
            reason: 'worktree-created',
            sessionId: targetSid,
            workspaceHash: noWorkspace
              ? ''
              : (ref?.workspaceHash || ref?.workspace_hash || ''),
            noWorkspace,
          });
        }
        if (historyText.trim()) recordInputHistory(historyText);
        if (sidRef.current === targetSid && clearCurrentSessionDraft({ expectedText: submittedComposerText, expectedContent: submittedComposerContent })) clearComposerExtras();
      })
      .catch((e) => {
        toast({ kind: 'err', text: '发送失败:' + (e.message || '') });
        if (sidRef.current === targetSid) {
          applyEvent({ type: 'busy_changed', payload: { busy: false } }, { emitEffects: false });
        }
      })
      .finally(() => setComposerSubmitting(false));
  }, [sid, busy, activeTurnId, api, homeSubmitting, recordInputHistory, enqueueInput, updateQueueState, applyEvent, sendInputOrBuiltin, executeBuiltinCommand, composerSubmitting, clearCurrentSessionDraft, composerAttachments, composerContexts, composerSwarmChoice, transcriptSwarmMode, clearComposerExtras, createHomeComposerSession, persistMediaFilesToSession, restoreChatInputFocusSoon, setTailFollowFromAction, runSideQuestion, draftWorkspaceHash, homeDraftWorkspaceHash, homeComposerDrafts, onHomeComposerDraftAccepted, ref?.noWorkspace, ref?.no_workspace, ref?.workspaceHash, ref?.workspace_hash, sessionRuntimeUnavailable, retryUserMessageId, transcript.getState, transcriptLoadState, readOnlyExternalSession, commitDeferredPastes, materializeWorkspaceDraftPastes, uploadPasteReservations]);

  const drainQueuedInput = useCallback(() => {
    const targetSid = sidRef.current;
    if (!targetSid || busy || drainRef.current || sessionRuntimeUnavailable) return;
    const current = transcript.getState();
    if (current.busy || current.abortPending || current.loadState !== 'loaded') return;
    // 取出待发送项与标记 sending 在同一次提交内完成,避免这中间的取消/编辑被
    // 一份过期快照覆盖。drainRef 仍在提交之前置位,保持原来的重入保护顺序。
    drainRef.current = true;
    let queuedItem = null;
    queueStore.commit((prev) => {
      queuedItem = nextQueuedInput(prev, targetSid);
      return queuedItem ? markQueuedInputSending(prev, queuedItem.queued.id) : prev;
    });
    if (!queuedItem) {
      drainRef.current = false;
      return;
    }
    setTailFollowFromAction({ type: 'new_turn' });
    const queuedPayload = queuedItem.queued?.payload || queuedItem.content;
    const queuedIsBuiltin = !payloadHasExtras(queuedPayload) &&
      inputRouteForPayload(typeof queuedPayload === 'string' ? { text: queuedPayload } : queuedPayload).kind === 'builtin';
    const sendPayload = queuedIsBuiltin
      ? queuedPayload
      : (queuedInputRequestPayload(queuedItem) || queuedPayload);
    if (!queuedIsBuiltin) {
      applyEvent({ type: 'busy_changed', payload: { busy: true } }, { emitEffects: false });
    }
    sendInputOrBuiltin(targetSid, sendPayload)
      .then(() => {
        if (!queuedIsBuiltin && sidRef.current === targetSid) {
          const acceptedEvent = acceptedQueuedInputEvent(queuedItem);
          if (acceptedEvent) applyEvent(acceptedEvent, { emitEffects: false });
        }
        updateQueueState((prev) => markQueuedInputCompleted(prev, queuedItem.queued.id));
      })
      .catch((e) => {
        const message = e?.message || '发送失败';
        if (!queuedIsBuiltin && sidRef.current === targetSid) {
          applyEvent({ type: 'busy_changed', payload: { busy: false } }, { emitEffects: false });
        }
        updateQueueState((prev) => markQueuedInputFailed(prev, queuedItem.queued.id, message));
        toast({ kind: 'err', text: '排队发送失败:' + message });
      })
      .finally(() => {
        drainRef.current = false;
      });
  }, [applyEvent, busy, queueStore, sendInputOrBuiltin, setTailFollowFromAction, updateQueueState, sessionRuntimeUnavailable, transcript.getState]);

  const prevBusyRef = useRef(busy);
  useEffect(() => {
    const wasBusy = prevBusyRef.current;
    prevBusyRef.current = busy;
    // 切会话瞬间 transcript 会 reset 成 busy=false + loadState=loading。
    // 在 loaded 之前禁止 drain,否则排队卡片会在还在运行的会话上被误发并消失。
    if (!shouldDrainQueuedInput({
      sessionId: sid,
      busy,
      loadState: transcriptLoadState,
      paused: isQueuedInputPaused(queueState, sid),
    })) {
      return;
    }
    // 回合是被中断收尾的:排队消息不能替用户「继续」,转入暂停等用户明确操作。
    // 本端点停止已在 abort() 里先行暂停;这里兜住 TUI / 其它客户端 / IM 通道
    // 发起的中断 —— 它们只以 outcome=aborted 的 busy_changed / done 到达。
    if (wasBusy && shouldPauseQueuedInputAfterAbort({
      state: queueState,
      sessionId: sid,
      lastTurnOutcome,
      turnId: lastTerminalTurnId,
    })) {
      updateQueueState((prev) => pauseQueuedInput(prev, sid));
      return;
    }
    if (wasBusy || !hasSendingQueuedInput(queueState, sid)) {
      drainQueuedInput();
    }
  }, [busy, drainQueuedInput, lastTurnOutcome, lastTerminalTurnId, queueState, sid, transcriptLoadState, updateQueueState]);

  useEffect(() => {
    if (!sid || items.length === 0) return;
    // 整轮遍历与写回必须在同一次提交内:拆成"先读引用、后写回"时,这中间新入队
    // 的消息会被这份过期快照覆盖掉。producer 返回同一对象时 store 自动跳过。
    updateQueueState((prev) => {
      let nextState = prev;
      for (const item of items) {
        if (item.kind !== 'msg' || item.role !== 'user') continue;
        nextState = completeQueuedInputForMessage(nextState, {
          sessionId: sid,
          content: item.content || '',
          ts: item.ts,
          clientMessageId: item.metadata?.client_message_id,
        });
      }
      return nextState;
    });
  }, [items, sid, updateQueueState]);

  const abort = useCallback(() => {
    if (!sid) return;
    const current = transcript.getState();
    if (!current.busy || current.abortPending) return;
    if (!connection.sendAbort(sid)) {
      toast({ kind: 'err', text: '停止请求发送失败，连接恢复后请重试' });
      return;
    }
    // 请求成功发出后保持 busy，等待服务端确认，期间新输入仍进入可见队列。
    updateQueueState((prev) => pauseQueuedInput(prev, sid));
    applyEvent({
      type: 'turn_abort_requested',
      payload: { turn_id: current.activeTurnId },
      timestamp_ms: Date.now(),
    }, { emitEffects: false });
  }, [applyEvent, sid, updateQueueState, transcript.getState]);

  const stopCurrentWork = useCallback(() => {
    if (!sid || !busy) return;
    abort();
  }, [abort, busy, sid]);

  useEffect(() => {
    const onDisconnect = () => {
      if (!transcript.getState().abortPending) return;
      applyEvent({ type: 'turn_abort_failed' }, { emitEffects: false });
      toast({ kind: 'err', text: '连接已断开，停止状态尚未确认' });
    };
    connection.addEventListener('disconnect', onDisconnect);
    return () => connection.removeEventListener('disconnect', onDisconnect);
  }, [applyEvent, transcript.getState]);

  const runGoalCommand = useCallback(async (action, objective = '') => {
    if (!sid) throw new Error('当前会话不可用');
    const commandLabels = {
      edit: '编辑目标',
      pause: '暂停目标',
      resume: '恢复目标',
      clear: '清除目标',
    };
    const label = commandLabels[action];
    if (!label) throw new Error('不支持的目标操作');
    const args = action === 'edit' ? `edit ${objective}` : action;
    try {
      return await executeBuiltinCommand(sid, {
        name: 'goal',
        args,
        display_text: `/goal ${args}`,
      });
    } catch (error) {
      toast({ kind: 'err', text: `${label}失败：${error?.message || '未知错误'}` });
      throw error;
    }
  }, [executeBuiltinCommand, sid]);

  const editGoal = useCallback(
    (objective) => runGoalCommand('edit', objective),
    [runGoalCommand],
  );
  const changeGoalStatus = useCallback(
    (action) => runGoalCommand(action),
    [runGoalCommand],
  );
  const clearGoal = useCallback(
    () => runGoalCommand('clear'),
    [runGoalCommand],
  );

  const selectHomeModel = useCallback(async (name) => {
    const nextName = String(name || '');
    const previousName = String(homeModelName || '');
    if (!nextName || nextName === previousName || modelRefreshing || modelSwitching || reasoningSwitching) return;
    setHomeModelName(nextName);
    setModelSwitching(true);
    try {
      const state = await api.setDefaultModel(nextName);
      const confirmedName = String(state?.default_model_name || state?.name || nextName);
      setHomeModelName(confirmedName || nextName);
      toast({ kind: 'ok', text: '默认模型已设为 ' + (confirmedName || nextName) });
    } catch (e) {
      setHomeModelName(previousName);
      toast({ kind: 'err', text: '默认模型设置失败:' + (e?.message || '') });
    } finally {
      setModelSwitching(false);
    }
  }, [api, homeModelName, modelRefreshing, modelSwitching, reasoningSwitching]);

  const switchSessionModel = useCallback(async (name) => {
    const nextName = String(name || '');
    const currentName = selectedModelName(modelState);
    if (!sid || !nextName || nextName === currentName || modelSwitching || reasoningSwitching) return;
    setPendingModelName(nextName);
    setModelSwitching(true);
    try {
      const nextState = normalizeModelState(await api.switchModel(sid, nextName));
      setModelState(nextState);
      toast({ kind: 'ok', text: '已切换到 ' + modelDisplayLabel(nextState, nextName) });
    } catch (e) {
      toast({ kind: 'err', text: '模型切换失败:' + (e?.message || '') });
    } finally {
      setPendingModelName('');
      setModelSwitching(false);
    }
  }, [api, modelState, modelSwitching, reasoningSwitching, sid]);

  const changeComposerModel = useCallback((name) => {
    if (sid) void switchSessionModel(name);
    else void selectHomeModel(name);
  }, [selectHomeModel, sid, switchSessionModel]);

  useEffect(() => {
    setHomeReasoningEffort(null);
  }, [homeModelName, sid]);

  useEffect(() => {
    const selected = modelOptions.find((option) => option.name === homeModelName);
    setHomeReasoningEffort((current) => composerReasoningOptions(selected, current)?.selectedEffort ?? null);
  }, [homeModelName, modelOptions]);

  const changeComposerReasoning = useCallback(async (effort) => {
    if (homeSubmitting || composerSubmitting || reasoningSwitching || modelSwitching || modelRefreshing) return;
    const selected = sid ? modelState : modelOptions.find((option) => option.name === homeModelName);
    const choices = composerReasoningOptions(selected, sid ? undefined : homeReasoningEffort);
    if (!choices || !choices.items.some((item) => item.effort === effort)) return;
    if (!sid) {
      setHomeReasoningEffort(effort);
      return;
    }
    const targetSid = sid;
    const request = ++reasoningRequestRef.current;
    setReasoningSwitching(true);
    try {
      const state = await api.setSessionReasoning(targetSid, effort);
      if (sidRef.current === targetSid && reasoningRequestRef.current === request) {
        setModelState(normalizeModelState(state));
      }
    } catch (error) {
      if (sidRef.current === targetSid && reasoningRequestRef.current === request) {
        toast({ kind: 'err', text: '思考深度设置失败：' + (error?.message || '') });
      }
    } finally {
      if (sidRef.current === targetSid && reasoningRequestRef.current === request) setReasoningSwitching(false);
    }
  }, [api, composerSubmitting, homeModelName, homeReasoningEffort, homeSubmitting, modelOptions, modelRefreshing, modelState, modelSwitching, reasoningSwitching, sid]);

  const reasoningShortcutPending = useRef(false);
  const shortcutComposerVisible = () => !!inputRef.current?.getElement()?.getClientRects().length
    && !questionForView && permissionRequests.length === 0;
  const stepReasoning = (direction) => {
    if (!shortcutComposerVisible()) return false;
    if (reasoningShortcutPending.current) return;
    const model = sid ? modelState : modelOptions.find((option) => option.name === homeModelName);
    const effort = nextReasoningEffort(model, direction, sid ? undefined : homeReasoningEffort);
    if (!effort) return;
    reasoningShortcutPending.current = true;
    void changeComposerReasoning(effort).finally(() => { reasoningShortcutPending.current = false; });
  };
  useAppShortcuts({
    focusInput: () => { if (!shortcutComposerVisible()) return false; inputRef.current.focus(); },
    stop: () => {
      if (!shortcutComposerVisible() || !busy || abortPending) return false;
      stopCurrentWork();
    },
    reasoningUp: () => stepReasoning(1),
    reasoningDown: () => stepReasoning(-1),
  });

  const switchHomeDefaultPermissionMode = useCallback(async (mode) => {
    const nextMode = normalizePermissionMode(mode);
    const previousMode = normalizePermissionMode(permissionMode);
    if (nextMode === previousMode || permissionSwitching) return;
    setPermissionMode(nextMode);
    setPermissionSwitching(true);
    try {
      const state = await api.setDefaultPermissionMode(nextMode);
      const confirmedMode = normalizePermissionMode(state?.mode || nextMode);
      setPermissionMode(confirmedMode);
      toast({ kind: 'ok', text: '默认权限模式已设为 ' + permissionModeOption(confirmedMode).label });
    } catch (e) {
      setPermissionMode(previousMode);
      toast({ kind: 'err', text: '默认权限模式设置失败:' + (e?.message || '') });
    } finally {
      setPermissionSwitching(false);
    }
  }, [api, permissionMode, permissionSwitching]);

  const switchPermissionMode = useCallback(async (mode) => {
    const nextMode = normalizePermissionMode(mode);
    const previousMode = normalizePermissionMode(permissionMode);
    if (!sid || nextMode === previousMode || permissionSwitching) return;
    setPermissionMode(nextMode);
    setPermissionSwitching(true);
    try {
      const state = await api.setSessionPermissionMode(sid, nextMode);
      const confirmedMode = normalizePermissionMode(state?.mode || nextMode);
      setPermissionMode(confirmedMode);
      onPermissionModeChanged?.({ sessionId: sid, mode: confirmedMode });
      toast({ kind: 'ok', text: '权限模式已切换为 ' + permissionModeOption(confirmedMode).label });
    } catch (e) {
      setPermissionMode(previousMode);
      toast({ kind: 'err', text: '权限模式切换失败:' + (e?.message || '') });
    } finally {
      setPermissionSwitching(false);
    }
  }, [api, onPermissionModeChanged, permissionMode, permissionSwitching, sid]);

  const changeComposerPermissionMode = useCallback((mode) => {
    if (sid) void switchPermissionMode(mode);
    else void switchHomeDefaultPermissionMode(mode);
  }, [sid, switchHomeDefaultPermissionMode, switchPermissionMode]);

  useLayoutEffect(() => {
    const element = layoutRef.current;
    if (!element) return undefined;
    const measure = () => {
      setLayoutWidth(Math.ceil(element.getBoundingClientRect().width || 0));
    };
    measure();
    if (typeof ResizeObserver === 'undefined') return undefined;
    const observer = new ResizeObserver(measure);
    observer.observe(element);
    return () => observer.disconnect();
  }, [sid]);

  const startSidePanelResize = useCallback((event) => {
    if (!showSidePanel || !onSidePanelResize) return;
    if (event.button != null && event.button !== 0) return;
    if (sidePanelResizeActiveRef.current) return;
    sidePanelResizeActiveRef.current = true;
    event.preventDefault();
    const contentWidth = layoutRef.current?.getBoundingClientRect().width || 0;
    const startX = event.clientX;
    const startWidth = sidePanelWidth;
    document.body.classList.add('ace-resizing');
    if (event.pointerId != null) event.currentTarget.setPointerCapture?.(event.pointerId);

    const onMove = (moveEvent) => {
      onSidePanelResize(startWidth + startX - moveEvent.clientX, contentWidth);
    };
    const onStop = () => {
      sidePanelResizeActiveRef.current = false;
      document.body.classList.remove('ace-resizing');
      window.removeEventListener('pointermove', onMove);
      window.removeEventListener('pointerup', onStop);
      window.removeEventListener('pointercancel', onStop);
      window.removeEventListener('mousemove', onMove);
      window.removeEventListener('mouseup', onStop);
    };

    window.addEventListener('pointermove', onMove);
    window.addEventListener('pointerup', onStop, { once: true });
    window.addEventListener('pointercancel', onStop, { once: true });
    window.addEventListener('mousemove', onMove);
    window.addEventListener('mouseup', onStop, { once: true });
  }, [onSidePanelResize, showSidePanel, sidePanelWidth]);

  const onSidePanelHandleKeyDown = useCallback((event) => {
    if (!onSidePanelResize) return;
    const step = event.shiftKey ? 32 : 12;
    if (event.key === 'ArrowLeft' || event.key === 'ArrowRight') {
      event.preventDefault();
      const delta = event.key === 'ArrowLeft' ? step : -step;
      const contentWidth = layoutRef.current?.getBoundingClientRect().width || 0;
      onSidePanelResize(sidePanelWidth + delta, contentWidth);
    }
  }, [onSidePanelResize, sidePanelWidth]);

  const startPreviewPanelResize = useCallback((event) => {
    if (!onPreviewPanelResize) return;
    if (event.button != null && event.button !== 0) return;
    if (previewPanelResizeActiveRef.current) return;
    previewPanelResizeActiveRef.current = true;
    event.preventDefault();
    const contentWidth = layoutRef.current?.getBoundingClientRect().width || 0;
    const startX = event.clientX;
    const startWidth = renderedPreviewPanelWidthRef.current;
    document.body.classList.add('ace-resizing');
    if (event.pointerId != null) event.currentTarget.setPointerCapture?.(event.pointerId);

    const onMove = (moveEvent) => {
      onPreviewPanelResize(startWidth + startX - moveEvent.clientX, contentWidth);
    };
    const onStop = () => {
      previewPanelResizeActiveRef.current = false;
      document.body.classList.remove('ace-resizing');
      window.removeEventListener('pointermove', onMove);
      window.removeEventListener('pointerup', onStop);
      window.removeEventListener('pointercancel', onStop);
      window.removeEventListener('mousemove', onMove);
      window.removeEventListener('mouseup', onStop);
    };

    window.addEventListener('pointermove', onMove);
    window.addEventListener('pointerup', onStop, { once: true });
    window.addEventListener('pointercancel', onStop, { once: true });
    window.addEventListener('mousemove', onMove);
    window.addEventListener('mouseup', onStop, { once: true });
  }, [onPreviewPanelResize]);

  const onPreviewPanelHandleKeyDown = useCallback((event) => {
    if (!onPreviewPanelResize) return;
    const step = event.shiftKey ? 32 : 12;
    if (event.key === 'ArrowLeft' || event.key === 'ArrowRight') {
      event.preventDefault();
      const delta = event.key === 'ArrowLeft' ? step : -step;
      const contentWidth = layoutRef.current?.getBoundingClientRect().width || 0;
      onPreviewPanelResize(renderedPreviewPanelWidthRef.current + delta, contentWidth);
    }
  }, [onPreviewPanelResize]);

  const startSubagentPanelResize = useCallback((event) => {
    if (!subagentPanelOpen || !sid || !onSubagentPanelResize) return;
    if (event.button != null && event.button !== 0) return;
    if (subagentPanelResizeActiveRef.current) return;
    subagentPanelResizeActiveRef.current = true;
    event.preventDefault();
    const contentWidth = subagentSplitRef.current?.getBoundingClientRect().width || 0;
    const startX = event.clientX;
    const startWidth = renderedSubagentPanelWidthRef.current;
    document.body.classList.add('ace-resizing');
    if (event.pointerId != null) event.currentTarget.setPointerCapture?.(event.pointerId);

    const onMove = (moveEvent) => {
      onSubagentPanelResize(startWidth + startX - moveEvent.clientX, contentWidth);
    };
    const onStop = () => {
      if (!subagentPanelResizeActiveRef.current) return;
      subagentPanelResizeActiveRef.current = false;
      subagentPanelResizeCleanupRef.current = null;
      document.body.classList.remove('ace-resizing');
      window.removeEventListener('pointermove', onMove);
      window.removeEventListener('pointerup', onStop);
      window.removeEventListener('pointercancel', onStop);
      window.removeEventListener('mousemove', onMove);
      window.removeEventListener('mouseup', onStop);
    };
    subagentPanelResizeCleanupRef.current = onStop;

    window.addEventListener('pointermove', onMove);
    window.addEventListener('pointerup', onStop, { once: true });
    window.addEventListener('pointercancel', onStop, { once: true });
    window.addEventListener('mousemove', onMove);
    window.addEventListener('mouseup', onStop, { once: true });
  }, [onSubagentPanelResize, sid, subagentPanelOpen]);

  const onSubagentPanelHandleKeyDown = useCallback((event) => {
    if (!onSubagentPanelResize) return;
    const step = event.shiftKey ? 32 : 12;
    if (event.key === 'ArrowLeft' || event.key === 'ArrowRight') {
      event.preventDefault();
      const delta = event.key === 'ArrowLeft' ? step : -step;
      const contentWidth = subagentSplitRef.current?.getBoundingClientRect().width || 0;
      onSubagentPanelResize(
        renderedSubagentPanelWidthRef.current + delta,
        contentWidth,
      );
    }
  }, [onSubagentPanelResize]);

  useEffect(() => {
    if (!subagentPanelOpen) subagentPanelResizeCleanupRef.current?.();
  }, [subagentPanelOpen]);

  useEffect(() => () => {
    subagentPanelResizeCleanupRef.current?.();
  }, []);

  // fork: 调后端 POST /api/sessions/:id/fork,成功后切到新 session(同 ref)。
  // 失败弹 toast 不打断当前 session。新 session 不会自动启 turn,
  // 用户在新 session 自己输入消息才开始。
  const forkAndSwitch = useCallback(async (messageId) => {
    const sourceMessageId = messageId == null ? '' : String(messageId);
    if (!sid || !sourceMessageId) return;
    if (!forkActionGuardRef.current.acquire(FORK_ACTION_KEY)) return;
    setForkingMessageId(sourceMessageId);
    try {
      const r = await api.forkSession(sid, sourceMessageId, '');
      if (!r || !r.session_id) {
        toast({ kind: 'err', text: '分叉失败:无 session_id' });
        return;
      }
      const noWorkspace = !!(ref?.noWorkspace || r?.no_workspace || r?.noWorkspace);
      const workspaceHash = noWorkspace ? '' : (r.workspace_hash || ref?.workspaceHash || '');
      const cwd = noWorkspace ? '' : (r.cwd || ref?.cwd || '');
      const workingCwd = r.working_cwd || r.cwd || ref?.workingCwd || ref?.cwd || '';
      const now = new Date().toISOString();
      const forkedSession = {
        ...r,
        id: r.session_id,
        active: true,
        status: 'idle',
        attention_state: 'read',
        read_state: 'read',
        no_workspace: noWorkspace,
        noWorkspace,
        workspace_hash: workspaceHash,
        cwd,
        working_cwd: workingCwd,
        title: r.title,
        created_at: r.created_at || now,
        updated_at: r.updated_at || now,
      };
      // 分叉点命中 user 提示词时,后端已把该提示词从历史中剔除并返回原文。
      // 这里回填输入框待用户修改后重发,不自动发送。
      // 先保留源会话输入,让旧会话 cleanup 保存自己的草稿;目标会话加载时再回填。
      const restoredPrompt = forkRestoredPrompt(r);
      const restoredContent = normalizeComposerContent(r.restored_composer_content);
      if (restoredPrompt || restoredContent) {
        pendingForkComposerRef.current = {
          key: `${workspaceHash}:${r.session_id}`,
          text: restoredPrompt,
          composer_content: restoredContent,
          attachments: Array.isArray(r.restored_attachments) ? r.restored_attachments : [],
        };
      }

      onSessionPromoted?.({
        ...newSessionRefFrom(ref, r.session_id),
        title: r.title,
        workspaceHash,
        cwd,
        workingCwd,
        noWorkspace,
      });
      notifySessionListChanged({
        reason: 'fork',
        sessionId: r.session_id,
        workspaceHash,
        noWorkspace,
        session: forkedSession,
      });
      toast({
        kind: 'ok',
        text: restoredPrompt
          ? '已创建分支会话'
          : '已分叉到 ' + (r.title || r.session_id),
      });
    } catch (e) {
      toast({ kind: 'err', text: '分叉失败:' + (e?.message || '') });
    } finally {
      forkActionGuardRef.current.release(FORK_ACTION_KEY);
      setForkingMessageId((current) => (current === sourceMessageId ? '' : current));
    }
  }, [sid, api, ref, onSessionPromoted]);

  useEffect(() => {
    const handler = (event) => {
      const detail = event.detail || {};
      const { action, target } = detail;
      if (action !== DESKTOP_CONTEXT_ACTIONS.FORK_MESSAGE) return;
      if (target?.type !== 'message' || !target.messageId) return;
      detail.handled = true;
      forkAndSwitch(target.messageId);
    };
    window.addEventListener(DESKTOP_CONTEXT_ACTION_EVENT, handler);
    return () => window.removeEventListener(DESKTOP_CONTEXT_ACTION_EVENT, handler);
  }, [forkAndSwitch]);

  useEffect(() => {
    const handler = (event) => {
      const detail = event.detail || {};
      const { action, target } = detail;
      if (action !== DESKTOP_CONTEXT_ACTIONS.ADD_SELECTION_CONTEXT) return;
      const context = detail.selectionContext
        || selectionContextFromWindowSelection({
          target,
          selectedText: detail.selectedText || '',
        });
      detail.handled = true;
      if (!pinSelectionContext(context)) {
        toast({ kind: 'err', text: '没有可引用的选中文本' });
        return;
      }
      toast({ kind: 'ok', text: '已引用到聊天' });
    };
    window.addEventListener(DESKTOP_CONTEXT_ACTION_EVENT, handler);
    return () => window.removeEventListener(DESKTOP_CONTEXT_ACTION_EVENT, handler);
  }, [pinSelectionContext]);

  useEffect(() => {
    const handler = (event) => {
      const detail = event.detail || {};
      const { action, target } = detail;
      if (action !== DESKTOP_CONTEXT_ACTIONS.ADD_FILE_CONTEXT) return;
      detail.handled = true;
      const filePath = normalizeReferencePath(
        target?.relativePath || target?.absolutePath || '',
      );
      if (!filePath) {
        toast({ kind: 'err', text: '无法获取文件路径' });
        return;
      }
      const insertion = inputRef.current?.insertPathReference?.(filePath, {
        directory: false,
      });
      if (!insertion) {
        toast({ kind: 'err', text: '输入框当前不可用' });
        return;
      }
      toast({ kind: 'ok', text: '已添加到会话' });
    };
    window.addEventListener(DESKTOP_CONTEXT_ACTION_EVENT, handler);
    return () => window.removeEventListener(DESKTOP_CONTEXT_ACTION_EVENT, handler);
  }, []);

  useEffect(() => {
    const handler = (event) => {
      const detail = event.detail || {};
      const { action, target } = detail;
      if (action !== DESKTOP_CONTEXT_ACTIONS.ADD_DIRECTORY_CONTEXT) return;
      detail.handled = true;
      const referencePath = normalizeReferencePath(
        target?.relativePath || target?.absolutePath || '',
      );
      if (target?.kind !== 'directory' || !referencePath) {
        toast({ kind: 'err', text: '无法获取文件夹路径' });
        return;
      }
      const insertion = inputRef.current?.insertPathReference?.(referencePath, {
        directory: true,
      });
      if (!insertion) {
        toast({ kind: 'err', text: '输入框当前不可用' });
        return;
      }
      toast({ kind: 'ok', text: '已添加到会话' });
    };
    window.addEventListener(DESKTOP_CONTEXT_ACTION_EVENT, handler);
    return () => window.removeEventListener(DESKTOP_CONTEXT_ACTION_EVENT, handler);
  }, []);

  const status = useMemo(() => {
    if (!sid) return null;
    return busy || transcriptStatus === 'running' ? 'running' : 'idle';
  }, [sid, busy, transcriptStatus]);
  const sessionWorkspaceHash = ref?.workspaceHash || ref?.workspace_hash || '';
  // 会话头部原本显示「运行中 / 空闲」,但运行状态在输入区与侧栏都已有更明确的
  // 呈现,这个胸章改为标出会话归属的工作区。ChatView 只在首页视图拉 workspace
  // 列表(/api/workspaces 要扫上万个目录,不能每次切会话都打),所以这里按
  // ref → 已加载列表 → 会话 cwd 目录名 的顺序退让;WorkspaceRegistry 默认名就是
  // cwd 的目录名,因此没改过名的工作区第三级兜底与侧栏显示一致。
  const workspaceLabel = useMemo(() => {
    const fromRef = String(ref?.workspaceName || ref?.workspace_name || '').trim();
    if (fromRef) return fromRef;
    if (isRealWorkspaceHash(sessionWorkspaceHash)) {
      const hit = homeWorkspaces.find((w) => w.hash === sessionWorkspaceHash);
      const name = String(hit?.name || '').trim();
      if (name) return name;
    }
    return pathBaseName(ref?.cwd || health?.cwd || '') || '当前项目';
  }, [health?.cwd, homeWorkspaces, ref?.cwd, ref?.workspaceName, ref?.workspace_name, sessionWorkspaceHash]);
  useEffect(() => () => closeDesktopContextMenu(), [sid]);
  const sessionPath = ref?.sessionPath || ref?.session_path || '';
  const sessionPinned = !!(ref?.pinned || ref?.isPinned || ref?.is_pinned);
  // 顶栏就地重命名:从会话头部菜单(「会话菜单」按钮左键 / 顶栏任意处右键)进入时,
  // 输入框出现在顶部标题位置,而不是跳去侧栏那一行里编辑。
  const [headerRenaming, setHeaderRenaming] = useState(false);
  useEffect(() => { setHeaderRenaming(false); }, [sid]);
  const sessionTitleLabelRef = useRef(null);
  const sessionMenuButtonRef = useRef(null);
  const startHeaderRename = useCallback(() => {
    if (!sid || readOnlyExternalSession) return;
    setHeaderRenaming(true);
  }, [sid, readOnlyExternalSession]);
  const cancelHeaderRename = useCallback(() => setHeaderRenaming(false), []);
  const commitHeaderRename = useCallback(async (draft) => {
    setHeaderRenaming(false);
    const targetSid = sid;
    const next = resolveSessionTitleRename(draft, title);
    if (!targetSid || !next.changed) return;
    try {
      const updated = await api.setSessionTitle(targetSid, next.title, sessionTitleRenameWorkspaceHash({
        workspaceHash: sessionWorkspaceHash,
        noWorkspace: !!(ref?.noWorkspace || ref?.no_workspace),
      }));
      const nextTitle = updated?.title ?? next.title;
      // daemon 也会经 session_updated 推送新标题(侧栏据此同步);这里先本地合并,
      // 顶栏不必等 WS 往返才变。
      if (sidRef.current === targetSid) {
        applyEvent({
          type: 'session_updated',
          payload: {
            session_id: targetSid,
            title: nextTitle,
            title_source: updated?.title_source ?? (nextTitle ? 'user' : ''),
          },
        }, { emitEffects: false });
      }
      toast({ kind: 'ok', text: next.title ? '已重命名' : '已清除标题' });
    } catch (e) {
      toast({ kind: 'err', text: '重命名失败:' + (e?.message || '') });
    }
  }, [api, applyEvent, ref?.noWorkspace, ref?.no_workspace, sessionWorkspaceHash, sid, title]);

  // 「会话菜单」按钮左键、按钮右键、顶栏其它位置右键共用这一份菜单。
  const openSessionMenu = useCallback(({ x, y, placement, anchorRect }) => {
    if (!sid || typeof window === 'undefined') return;
    const button = sessionMenuButtonRef.current;
    const target = readOnlyExternalSession ? button : sidebarSessionContextTarget(sid, sessionWorkspaceHash, button);
    openDesktopContextMenu({
      target,
      trigger: button,
      x,
      y,
      placement,
      leadingItems: [
        ...(!readOnlyExternalSession ? [{
          id: 'side_chat', label: '侧边聊天', icon: 'chat', group: 'session-view',
          onSelect: () => {
            openSideQuestionComposer();
            setSideChatAnchor({ left: anchorRect.left, top: anchorRect.top });
          },
        }] : []),
        ...(onFindInConversation ? [{
          id: 'find_conversation', label: '查找', icon: 'search', group: 'session-view',
          onSelect: onFindInConversation,
        }] : []),
      ],
      includeContextActions: !readOnlyExternalSession,
      actionOverrides: readOnlyExternalSession ? null : {
        [DESKTOP_CONTEXT_ACTIONS.RENAME_SESSION]: startHeaderRename,
      },
    });
  }, [sessionWorkspaceHash, sid, readOnlyExternalSession, openSideQuestionComposer, onFindInConversation, startHeaderRename]);
  const openSessionContextMenu = useCallback((event) => {
    event.preventDefault();
    event.stopPropagation();
    const rect = event.currentTarget.getBoundingClientRect();
    openSessionMenu({ x: rect.right, y: rect.bottom + 4, anchorRect: rect });
  }, [openSessionMenu]);
  useEffect(() => {
    if (!sid) return undefined;
    const handler = (event) => {
      const detail = event.detail || {};
      if (detail.handled || detail.name !== SESSION_HEADER_CONTEXT_MENU_DELEGATE) return;
      // 同一时刻可能有多个 ChatView(网格视图),只认领标题渲染在这块头部里的那个。
      const label = sessionTitleLabelRef.current;
      if (!label || !detail.element?.contains?.(label)) return;
      detail.handled = true;
      const button = sessionMenuButtonRef.current;
      openSessionMenu({
        x: detail.x,
        y: detail.y,
        placement: 'pointer',
        anchorRect: button?.getBoundingClientRect?.() || { left: detail.x, top: detail.y },
      });
    };
    window.addEventListener(CONTEXT_MENU_DELEGATE_EVENT, handler);
    return () => window.removeEventListener(CONTEXT_MENU_DELEGATE_EVENT, handler);
  }, [openSessionMenu, sid]);

  const modelListEmptyLoaded = modelListLoaded && modelOptions.length === 0;
  const noModelLabel = '未配置模型';
  const currentModelFallback = isEmptyModelState(modelState)
    ? noModelLabel
    : (ref?.model_name || ref?.model_preset || ref?.model || (modelListEmptyLoaded ? noModelLabel : '加载中'));
  const currentModelLabel = modelDisplayLabel(modelState, currentModelFallback);
  const currentModelName = modelSelectValue(modelState, pendingModelName);
  const homeModelFallback = !homeModelName && modelListEmptyLoaded ? noModelLabel : (homeModelName || '加载中');
  const selectedHomeModel = modelOptions.find((option) => option.name === homeModelName);
  const homeModelLabel = modelDisplayLabel(
    selectedHomeModel || (homeModelName ? { name: homeModelName } : null),
    homeModelFallback,
  );
  const boundExpertId = sid
    ? sessionExpertId
    : String(ref?.expertId || ref?.expert_id || ref?.expert?.id || '');
  const boundExpertSnapshot = sid ? sessionExpertSnapshot : ref?.expert;
  const displayedExperts = useMemo(() => {
    if (!boundExpertId || experts.some((expert) => expert.id === boundExpertId)) return experts;
    if (boundExpertSnapshot && typeof boundExpertSnapshot === 'object') {
      return normalizeExperts([boundExpertSnapshot, ...experts]);
    }
    return [{
      id: boundExpertId,
      display_name: boundExpertSnapshot?.missing ? `${boundExpertId}（已缺失）` : boundExpertId,
      type: 'agent',
      source: 'global',
      managed_global: false,
      quick_prompts: [],
    }, ...experts];
  }, [boundExpertId, boundExpertSnapshot, experts]);
  const recentExperts = useMemo(
    () => resolveRecentExperts(experts, recentExpertIds),
    [experts, recentExpertIds],
  );
  const composerExpertId = sid ? boundExpertId : homeExpertId;
  const composerExpert = displayedExperts.find((expert) => expert.id === composerExpertId)
    || null;
  const selectComposerExpert = useCallback(async (expert, options = {}) => {
    const expertId = String(expert?.id || '');
    if (!expertId) return false;
    const hasDraftText = Object.prototype.hasOwnProperty.call(options, 'draftText');
    const requestedDraftText = hasDraftText ? String(options.draftText ?? '') : '';
    if (!sid) {
      onRememberExpert?.(expert);
      setHomeExpertId(expertId);
      onSessionExpertChanged?.('', expert);
      if (hasDraftText) {
        draftEditVersionRef.current += 1;
        composerDirtyRef.current = true;
        setComposerValue(requestedDraftText);
        restoreChatInputFocusSoon(true);
      }
      return true;
    }

    const latestRequest = latestExpertSwitchRequestRef.current;
    if (!shouldRequestExpertSwitch({
      expertId,
      currentExpertId: sessionExpertId,
      pendingExpertId: pendingExpertRef.current?.expert?.id || '',
      requestInFlight: latestRequest.settled === false,
      hasDraftText,
    })) {
      onRememberExpert?.(expert);
      return true;
    }

    const targetSessionId = sid;
    const requestSequence = latestRequest.sequence + 1;
    const previousId = sessionExpertId;
    const previousSnapshot = sessionExpertSnapshot;
    const previousPending = pendingExpertRef.current;
    const requested = {
      expert,
      confirmed: false,
      previousId,
      previousSnapshot,
      requestSequence,
    };
    latestExpertSwitchRequestRef.current = {
      sequence: requestSequence,
      expertId,
      settled: false,
    };
    pendingExpertRef.current = requested;
    setPendingExpert(requested);
    setExpertSwitching(true);

    const requestOptions = hasDraftText ? { draftText: requestedDraftText } : {};
    const responsePromise = expertSwitchQueueRef.current
      .catch(() => undefined)
      .then(() => api.setSessionExpert(targetSessionId, expertId, requestOptions));
    expertSwitchQueueRef.current = responsePromise.then(() => undefined, () => undefined);

    try {
      const result = await responsePromise;
      const confirmedExpert = normalizeExperts([result?.expert || result])[0] || expert;
      const receipt = normalizeExpertSwitchReceipt(result, expertId);
      const accepted = {
        expert: confirmedExpert,
        confirmed: true,
        previousId,
        previousSnapshot,
        requestSequence,
        receipt,
        effectiveBoundary: receipt.effectiveBoundary,
      };
      acceptedExpertSwitchRef.current = accepted;
      if (!shouldApplyExpertSwitchResponse(
        requestSequence,
        latestExpertSwitchRequestRef.current.sequence,
      ) || sidRef.current !== targetSessionId) {
        return true;
      }

      onRememberExpert?.(confirmedExpert);
      if (receipt.applied) {
        setSessionExpertId(confirmedExpert.id);
        setSessionExpertSnapshot(confirmedExpert);
        pendingExpertRef.current = null;
        setPendingExpert(null);
        onSessionExpertChanged?.(sid, confirmedExpert);
      } else {
        pendingExpertRef.current = accepted;
        setPendingExpert(accepted);
      }

      if (hasDraftText) {
        const confirmedDraft = Object.prototype.hasOwnProperty.call(result || {}, 'draft_text')
          ? String(result.draft_text ?? '')
          : requestedDraftText;
        draftEditVersionRef.current += 1;
        composerDirtyRef.current = true;
        draftLastSavedRef.current = {
          key: draftSessionKeyRef.current,
          text: confirmedDraft,
        };
        setComposerValue(confirmedDraft);
        restoreChatInputFocusSoon(true);
      }
      return true;
    } catch (error) {
      if (shouldApplyExpertSwitchResponse(
        requestSequence,
        latestExpertSwitchRequestRef.current.sequence,
      ) && sidRef.current === targetSessionId) {
        const acceptedFallback = acceptedExpertSwitchRef.current;
        if (acceptedFallback && acceptedFallback.requestSequence < requestSequence) {
          const restoredFallback = {
            ...acceptedFallback,
            requestSequence,
          };
          acceptedExpertSwitchRef.current = restoredFallback;
          latestExpertSwitchRequestRef.current = {
            ...latestExpertSwitchRequestRef.current,
            expertId: restoredFallback.expert.id,
          };
          if (restoredFallback.receipt?.applied) {
            setSessionExpertId(restoredFallback.expert.id);
            setSessionExpertSnapshot(restoredFallback.expert);
            pendingExpertRef.current = null;
            setPendingExpert(null);
            onSessionExpertChanged?.(targetSessionId, restoredFallback.expert);
          } else {
            pendingExpertRef.current = restoredFallback;
            setPendingExpert(restoredFallback);
          }
        } else {
          setSessionExpertId(previousId);
          setSessionExpertSnapshot(previousSnapshot);
          pendingExpertRef.current = previousPending;
          setPendingExpert(previousPending);
        }
        toast({ kind: 'err', text: `切换专家失败：${error?.message || ''}` });
      }
      return false;
    } finally {
      if (shouldApplyExpertSwitchResponse(
        requestSequence,
        latestExpertSwitchRequestRef.current.sequence,
      ) && sidRef.current === targetSessionId) {
        latestExpertSwitchRequestRef.current = {
          ...latestExpertSwitchRequestRef.current,
          settled: true,
        };
        setExpertSwitching(false);
      }
    }
  }, [
    api,
    onRememberExpert,
    onSessionExpertChanged,
    restoreChatInputFocusSoon,
    sessionExpertId,
    sessionExpertSnapshot,
    sid,
  ]);

  const detachComposerExpert = useCallback(async () => {
    if (expertDetaching) return false;
    const hasBoundExpert = sid ? !!sessionExpertId : !!homeExpertId;
    if (!hasBoundExpert && !pendingExpertRef.current) return true;

    const nextSequence = latestExpertSwitchRequestRef.current.sequence + 1;
    latestExpertSwitchRequestRef.current = {
      sequence: nextSequence,
      expertId: '',
      settled: true,
    };
    pendingExpertRef.current = null;
    acceptedExpertSwitchRef.current = null;
    setPendingExpert(null);
    setExpertSwitching(false);

    if (!sid) {
      setHomeExpertId('');
      onSessionExpertChanged?.('', null);
      return true;
    }

    const targetSessionId = sid;
    setExpertDetaching(true);
    const detachPromise = expertSwitchQueueRef.current
      .catch(() => undefined)
      .then(() => api.clearSessionExpert(targetSessionId));
    expertSwitchQueueRef.current = detachPromise.then(() => undefined, () => undefined);
    try {
      await detachPromise;
      if (sidRef.current !== targetSessionId) return true;
      setSessionExpertId('');
      setSessionExpertSnapshot(null);
      onSessionExpertChanged?.(targetSessionId, null);
      return true;
    } catch (error) {
      if (sidRef.current === targetSessionId) {
        toast({ kind: 'err', text: `解除专家失败：${error?.message || ''}` });
      }
      return false;
    } finally {
      if (sidRef.current === targetSessionId) setExpertDetaching(false);
    }
  }, [
    api,
    expertDetaching,
    homeExpertId,
    onSessionExpertChanged,
    sessionExpertId,
    sid,
  ]);

  useEffect(() => {
    if (busy || expertSwitching || !pendingExpert?.confirmed || !sid) return undefined;
    const targetSessionId = sid;
    const targetExpert = pendingExpert.expert;
    const targetExpertId = String(targetExpert?.id || '');
    const requestSequence = Number(pendingExpert.requestSequence || 0);
    if (!targetExpertId || requestSequence <= 0) return undefined;

    let cancelled = false;
    let retryTimer = 0;
    let attempt = 0;
    let lastLoadError = null;
    const isCurrentRequest = () => {
      const latest = latestExpertSwitchRequestRef.current;
      const currentPending = pendingExpertRef.current;
      return !cancelled
        && sidRef.current === targetSessionId
        && shouldApplyExpertSwitchResponse(requestSequence, latest.sequence)
        && String(latest.expertId || '') === targetExpertId
        && Number(currentPending?.requestSequence || 0) === requestSequence
        && String(currentPending?.expert?.id || '') === targetExpertId;
    };
    const clearConfirmedPending = () => {
      if (!isCurrentRequest()) return false;
      pendingExpertRef.current = null;
      acceptedExpertSwitchRef.current = null;
      setPendingExpert(null);
      return true;
    };
    const canonicalExpertSnapshot = (session, canonicalExpertId, fallback = null) => {
      const fromSession = session?.expert && typeof session.expert === 'object'
        ? normalizeExperts([session.expert])[0]
        : null;
      if (fromSession?.id === canonicalExpertId) return fromSession;
      const fromCatalog = experts.find((item) => item.id === canonicalExpertId);
      if (fromCatalog) return fromCatalog;
      if (fallback?.id === canonicalExpertId) return fallback;
      if (!canonicalExpertId) return null;
      return {
        id: canonicalExpertId,
        display_name: canonicalExpertId,
        type: 'agent',
        source: 'global',
        managed_global: false,
        quick_prompts: [],
      };
    };
    const scheduleRetry = (poll) => {
      retryTimer = window.setTimeout(
        poll,
        EXPERT_SWITCH_CANONICAL_POLL_INTERVAL_MS,
      );
    };
    const pollCanonicalSession = async () => {
      if (!isCurrentRequest()) return;
      attempt += 1;
      let sessions = [];
      let loadError = null;
      try {
        sessions = isRealWorkspaceHash(sessionWorkspaceHash)
          ? await api.listWorkspaceSessions(sessionWorkspaceHash)
          : await api.listSessions();
      } catch (error) {
        loadError = error;
        lastLoadError = error;
      }
      if (!isCurrentRequest()) return;

      const latest = latestExpertSwitchRequestRef.current;
      const resolution = resolveCanonicalExpertSwitchPoll({
        sessions,
        sessionId: targetSessionId,
        targetExpertId,
        requestSequence,
        latestRequestSequence: latest.sequence,
        latestTargetExpertId: latest.expertId,
        attempt,
        maxAttempts: EXPERT_SWITCH_CANONICAL_POLL_ATTEMPTS,
        loadError,
      });
      if (resolution.status === 'stale') return;
      if (resolution.status === 'retry') {
        scheduleRetry(pollCanonicalSession);
        return;
      }
      if (resolution.status === 'matched') {
        const confirmedExpert = canonicalExpertSnapshot(
          resolution.session,
          targetExpertId,
          targetExpert,
        );
        if (!clearConfirmedPending()) return;
        setSessionExpertId(targetExpertId);
        setSessionExpertSnapshot(confirmedExpert);
        onSessionExpertChanged?.(targetSessionId, confirmedExpert);
        return;
      }
      if (!clearConfirmedPending()) return;

      if (resolution.status === 'mismatch') {
        const canonicalExpert = canonicalExpertSnapshot(
          resolution.session,
          resolution.canonicalExpertId,
        );
        setSessionExpertId(resolution.canonicalExpertId);
        setSessionExpertSnapshot(canonicalExpert);
        onSessionExpertChanged?.(targetSessionId, canonicalExpert);
        toast({
          kind: 'err',
          text: `专家切换结果与当前会话不一致，已同步为 ${canonicalExpert?.display_name || '未派遣专家'}`,
        });
        return;
      }
      if (resolution.status === 'missing') {
        toast({ kind: 'err', text: '无法确认专家切换：当前对话已不存在或不可见' });
        return;
      }
      toast({
        kind: 'err',
        text: `确认专家切换状态失败：${lastLoadError?.message || '无法读取当前对话'}`,
      });
    };

    pollCanonicalSession();
    return () => {
      cancelled = true;
      if (retryTimer) window.clearTimeout(retryTimer);
    };
  }, [
    api,
    busy,
    expertSwitching,
    experts,
    onSessionExpertChanged,
    pendingExpert,
    sessionWorkspaceHash,
    sid,
  ]);

  const selectExpertOpeningPrompt = useCallback(async (expert, prompt) => {
    return selectComposerExpert(expert, { draftText: String(prompt || '') });
  }, [selectComposerExpert]);
  const currentContextWindow = Number(modelState?.contextWindow || ref?.context_window || ref?.contextWindow || 0) || 0;
  const tokenBudget = useMemo(() => normalizeTokenBudget({
    usage: tokenUsage,
    contextWindow: currentContextWindow,
  }), [currentContextWindow, tokenUsage]);
  const homeContextWindow = Number(selectedHomeModel?.contextWindow || 0) || 0;
  const homeTokenBudget = useMemo(() => normalizeTokenBudget({
    usage: null,
    contextWindow: homeContextWindow,
  }), [homeContextWindow]);
  const displayedModelOptions = useMemo(() => {
    const currentName = selectedModelName(modelState);
    if (!currentName || modelOptions.some((m) => m.name === currentName)) return modelOptions;
    const normalized = normalizeModelState(modelState);
    return normalized ? [normalized, ...modelOptions] : modelOptions;
  }, [modelOptions, modelState]);
  // 当前/首屏模型的池负载(按 model id 精确匹配 modelPoolName;未命中 → null)。
  const currentModelLoad = useMemo(
    () => pickModelLoad(poolModels, modelState?.model),
    [poolModels, modelState],
  );
  const homeModelLoad = useMemo(() => {
    const opt = modelOptions.find((option) => option.name === homeModelName);
    return pickModelLoad(poolModels, opt?.model);
  }, [poolModels, modelOptions, homeModelName]);

  // 三处 diff UI 共用同一份数据源:把 items 里 tool 项的 hunks 抽成消息格式。
  // 必须放在 early return 之前,否则空态/有 session 之间 hooks 数量不一致 → React #310。
  const changeMessages = useMemo(() => collectHunkMessagesFromItems(items), [items]);
  const latestSuccessfulChangedFiles = useMemo(
    () => latestTurnSuccessfulChangedFiles(items),
    [items],
  );
  const agentBrowserActivity = useMemo(
    () => agentBrowserActivityFromItems(items),
    [items],
  );
  // 浏览器页签从 App 级页面归属登记表派生(lib/agentBrowserPages.js),而不是从
  // 当前 transcript 的实时工具活动推断:后者只在「正在看这个会话 + 工具正在执行」
  // 的瞬间成立,用户切走会话时页面就成了孤儿(会话 20260915-120207-bdf9)。
  const agentBrowserRegistry = useSyncExternalStore(
    agentBrowserPageStoreSubscribe,
    agentBrowserPageStoreSnapshot,
    agentBrowserPageStoreSnapshot,
  );
  const sessionBrowserPages = useMemo(
    () => agentBrowserPagesForSession(agentBrowserRegistry, sid),
    [agentBrowserRegistry, sid],
  );
  const sessionBrowserTargetPageId = useMemo(
    () => agentBrowserSessionTargetPageId(agentBrowserRegistry, sid),
    [agentBrowserRegistry, sid],
  );
  // 彩虹边框只给 Agent 正在操作的那一页:显式 page_id 优先,否则取本会话的
  // Agent 默认目标页;没有浏览器工具在跑时为空。
  const agentBrowserActivePageId = agentBrowserActivity.active
    ? (agentBrowserActivity.pageId || sessionBrowserTargetPageId)
    : '';
  // 每轮「本轮改动文件」列表:collectTurnChangeSetsFromItems 按 user 消息切
  // 回合聚合变更;列表渲染在回合末尾 = 下一个 user 行之前,最后一轮挂在
  // transcript 末尾(tail)。锚定基于 renderedItems(折叠投影后的视图)里的
  // user 行 —— user 行不参与活动折叠,id 与 rawItems 一致;万一锚找不到
  // (极端投影差异)兜底进 tail,列表不丢。
  const turnChangeSets = useMemo(() => collectTurnChangeSetsFromItems(items), [items]);
  const turnFileListPlacement = useMemo(() => {
    const before = new Map();
    const tail = [];
    if (!turnChangeSets.length) return { before, tail };
    const userIds = [];
    const userIndexById = new Map();
    for (const it of renderedItems) {
      if (it?.kind === 'msg' && it.role === 'user') {
        userIndexById.set(it.id, userIds.length);
        userIds.push(it.id);
      }
    }
    for (const set of turnChangeSets) {
      let nextUserId;
      if (set.userItemId) {
        const anchorIndex = userIndexById.get(set.userItemId);
        if (anchorIndex === undefined) {
          tail.push(set);
          continue;
        }
        nextUserId = userIds[anchorIndex + 1];
      } else {
        // orphan 变更集(回合头 user 消息缺失,如截断的 resume)排在首个 user 行之前
        nextUserId = userIds[0];
      }
      if (!nextUserId) {
        tail.push(set);
        continue;
      }
      const list = before.get(nextUserId) || [];
      list.push(set);
      before.set(nextUserId, list);
    }
    return { before, tail };
  }, [renderedItems, turnChangeSets]);
  const rawChangeGroups = useMemo(() => aggregateHunksFromMessages(changeMessages), [changeMessages]);
  const changeSignature = useMemo(() => changeGroupsSignature(rawChangeGroups), [rawChangeGroups]);
  // 引用稳定化:流式期间每个 WS 帧 items 都换新引用,rawChangeGroups 即使内容
  // 没变也是新数组,会让 DiffPreview 等下游 memo 每帧失效、整棵重建 diff DOM,
  // 用户在变更视图里的滚动位置因此丢失(fix-preview-scroll-during-stream)。
  // 签名一致时复用旧数组引用;ref 在渲染期写入是纯缓存,并发渲染丢弃也无害
  // (签名相同意味着内容相同,最多多算一次)。
  const changeGroupsStableRef = useRef(null);
  changeGroupsStableRef.current = stableBySignature(
    changeGroupsStableRef.current,
    { signature: changeSignature, value: rawChangeGroups },
  );
  const changeGroups = changeGroupsStableRef.current.value;
  const changeSummary = useMemo(() => summarizeChangeGroups(changeGroups), [changeGroups]);
  // changeMessages 同理:groups 由 messages 确定性推导,签名相同即内容相同,
  // 复用同一签名让 SidePanel 的 fallback 聚合 memo 在流式期间不再每帧失效。
  const changeMessagesStableRef = useRef(null);
  changeMessagesStableRef.current = stableBySignature(
    changeMessagesStableRef.current,
    { signature: changeSignature, value: changeMessages },
  );
  const stableChangeMessages = changeMessagesStableRef.current.value;
  const changeDockDismissalKey = useMemo(
    () => dockDismissalKey(ref, sid),
    [ref?.workspaceHash, sid],
  );
  const dismissedDockSignature = useMemo(
    () => dismissedDockSignatureFor(dismissedDockSignatures, changeDockDismissalKey),
    [changeDockDismissalKey, dismissedDockSignatures],
  );
  const fileTreeRefreshKey = useMemo(() => fileTreeRefreshKeyFromItems(items), [items]);
  // todo 环的下一轮自动收起:提交时记录的快照签名仍与当前一致 → 抑制;
  // agent 更新 todo(签名变化)或切换会话后自动解除。
  const todoSignature = useMemo(() => todoDockSignature(todos, todoSummary), [todos, todoSummary]);
  const todosSuppressed = isTodoDockSuppressed(todoDockSuppression, sid, todoSignature);
  const dockTodos = todosSuppressed ? [] : todos;
  const dockTodoSummary = todosSuppressed ? null : todoSummary;
  const hasVisibleTodos = Array.isArray(dockTodos) && dockTodos.length > 0;
  const showChangeDetails = changeSummary.hasChanges
    && !!changeSignature
    && dismissedDockSignature !== changeSignature;
  const completedTurnResultVisible = useMemo(
    () => hasCompletedTurnResult(renderedItems, assistantRunDirectives, { busy }),
    [renderedItems, assistantRunDirectives, busy],
  );
  const showChangeDock = !completedTurnResultVisible && (showChangeDetails || hasVisibleTodos);

  useLayoutEffect(() => {
    if (!showChangeDock) {
      setChangeDockBottomPadding(0);
      return undefined;
    }

    const measure = () => {
      const height = changeDockRef.current?.getBoundingClientRect().height || 0;
      setChangeDockBottomPadding(height ? Math.ceil(height) + 3 : 0);
    };

    measure();
    const element = changeDockRef.current;
    if (!element || typeof ResizeObserver === 'undefined') return undefined;
    const observer = new ResizeObserver(measure);
    observer.observe(element);
    return () => observer.disconnect();
  }, [
    showChangeDock,
    showChangeDetails,
    hasVisibleTodos,
    changeSummary.fileCount,
    changeSummary.totalAdditions,
    changeSummary.totalDeletions,
  ]);

  useEffect(() => {
    setExpandedActivityKeys(new Set());
    setCollapsedMediaKeys(new Set());
  }, [sid]);

  const toggleActivitySummary = useCallback((key, titleElement) => {
    const anchored = beginActivityExpansionAnchor(titleElement);
    if (!anchored) pauseTailFollowForReview();
    flushSync(() => {
      setExpandedActivityKeys((prev) => {
        const next = new Set(prev);
        if (next.has(key)) next.delete(key);
        else next.add(key);
        return next;
      });
    });
    preserveActivityExpansionAnchor();
    scheduleActivityExpansionAnchorMeasure();
  }, [
    beginActivityExpansionAnchor,
    pauseTailFollowForReview,
    preserveActivityExpansionAnchor,
    scheduleActivityExpansionAnchorMeasure,
  ]);

  const toggleMediaGroup = useCallback((key, titleElement) => {
    const anchored = beginActivityExpansionAnchor(titleElement);
    if (!anchored) pauseTailFollowForReview();
    flushSync(() => {
      setCollapsedMediaKeys((prev) => {
        const next = new Set(prev);
        if (next.has(key)) next.delete(key);
        else next.add(key);
        return next;
      });
    });
    preserveActivityExpansionAnchor();
    scheduleActivityExpansionAnchorMeasure();
  }, [
    beginActivityExpansionAnchor,
    pauseTailFollowForReview,
    preserveActivityExpansionAnchor,
    scheduleActivityExpansionAnchorMeasure,
  ]);

  const openReviewPanel = useCallback(() => {
    if (!showSidePanel || !sid) return;
    if (sidePanelCollapsed || sidePanelListCollapsed) onRevealSidePanelList?.();
    setReviewRequest((n) => n + 1);
  }, [onRevealSidePanelList, showSidePanel, sid, sidePanelCollapsed, sidePanelListCollapsed]);

  const locateInFileTree = useCallback((path) => {
    if (!showSidePanel || !path) return;
    if (sidePanelCollapsed || sidePanelListCollapsed) onRevealSidePanelList?.();
    setFileLocateRequest((prev) => ({ path, token: (prev.token || 0) + 1 }));
  }, [onRevealSidePanelList, showSidePanel, sidePanelCollapsed, sidePanelListCollapsed]);

  const dismissChangeDock = useCallback(() => {
    if (!changeDockDismissalKey || !changeSignature) return;
    setDismissedDockSignatures((prev) => dismissChangeDockSignature(
      prev,
      changeDockDismissalKey,
      changeSignature,
    ));
  }, [changeDockDismissalKey, changeSignature, setDismissedDockSignatures]);

  // 渲染期刷新 ref(纯缓存):submit 提交新一轮对话时经此收起整个 dock。
  dockAutoDismissRef.current = () => {
    dismissChangeDock();
    if (sid) setTodoDockSuppression({ sessionKey: sid, signature: todoSignature });
  };

  // 完成结果出现的同次渲染已隐藏整个 dock;再保存当前签名,避免下一轮
  // 重新显示旧进度。完成后迟到的 diff / todo 快照也更新抑制签名。
  useEffect(() => {
    if (!completedTurnResultVisible) return;
    dismissChangeDock();
    if (sid) setTodoDockSuppression({ sessionKey: sid, signature: todoSignature });
  }, [completedTurnResultVisible, dismissChangeDock, sid, todoSignature]);

  const conversationActivity = useMemo(() => selectConversationActivity({
    foregroundBusy: busy,
    foregroundStopping: abortPending,
    foregroundActivity: activity,
    permissionRequests,
    questionRequest: questionForView,
    subagentTasks: subagentTasks.tasks,
  }), [
    activity,
    busy,
    abortPending,
    permissionRequests,
    questionForView,
    subagentTasks.tasks,
  ]);

  const resolveQuestion = useCallback(() => {
    onQuestionResolve?.();
    requestAnimationFrame(() => inputRef.current?.focus());
  }, [onQuestionResolve]);


  const sidePanelMounted = showSidePanel;
  const sidePanelNavigationCollapsed = sidePanelCollapsed || sidePanelListCollapsed;
  const previewScope = useMemo(
    () => {
      if (!sidePanelFilesEnabled) return sid || '';
      return previewScopeKey({
        cwd: sidePanelCwd,
        workspaceHash: ref?.workspaceHash || '',
        worktreePath: sessionWorktree?.path || '',
      });
    },
    [ref?.workspaceHash, sessionWorktree?.path, sid, sidePanelCwd, sidePanelFilesEnabled],
  );
  const previewContext = useMemo(
    () => previewTabContext({ scopeKey: previewScope, sessionId: sid || workbenchOwner }),
    [previewScope, sid, workbenchOwner],
  );
  previewContextRef.current = previewContext;
  const previewTabs = useMemo(
    () => visiblePreviewTabs(previewTabState, previewContext),
    [previewContext, previewTabState],
  );
  const activePreview = useMemo(
    () => activePreviewTab(previewTabState, previewContext),
    [previewContext, previewTabState],
  );
  const requestPreviewApproval = useCallback((getTabs, kind = 'switch') => (
    previewFileGuardRef.current.request({
      kind,
      getTabs,
      discard: (tabs) => {
        setPreviewTabState((state) => tabs.reduce((next, tab) => discardFileTabDraft(next, {
          ...previewContext, tabKey: tab.key,
        }), state));
      },
      save: async (tabs) => {
        const updateDraft = (tab, patch) => setPreviewTabState((state) => updateFileTabDraft(state, {
          ...previewContext, tabKey: tab.key, patch,
        }));
        const result = await saveEditableFileDraftBatch(api, {
          tabs,
          fallbackCwd: sidePanelCwd,
          onSaving: (tab) => updateDraft(tab, { saving: true, error: '' }),
          onSaved: (tab, saved) => updateDraft(tab, saved.patch),
        });
        if (!result.ok) {
          const message = editableFileError(result.error, '保存失败');
          updateDraft(result.tab, {
            saving: false,
            externalChanged: editableFileConflict(result.error),
            error: message,
          });
          throw new Error(`${result.tab.title || result.tab.path}：${message}`);
        }
      },
    })
  ), [api, previewContext, setPreviewTabState, sidePanelCwd]);
  const requestPreviewLeave = useCallback(() => requestPreviewApproval(
    () => visiblePreviewTabs(getPreviewTabState(), previewContext),
  ), [getPreviewTabState, previewContext, requestPreviewApproval]);
  previewLeaveRequestRef.current = requestPreviewLeave;
  useLayoutEffect(() => onRegisterPreviewLeaveGuard?.(requestPreviewLeave), [
    onRegisterPreviewLeaveGuard, requestPreviewLeave,
  ]);
  const requestActiveFileLeave = useCallback(() => {
    const key = activePreviewTab(getPreviewTabState(), previewContext)?.key;
    return requestPreviewApproval(() => visiblePreviewTabs(getPreviewTabState(), previewContext)
      .filter((tab) => tab.key === key));
  }, [getPreviewTabState, previewContext, requestPreviewApproval]);
  const selectPreview = useCallback((producer, afterSelect) => {
    if (previewFileGuardRef.current.isPending()) return false;
    const state = getPreviewTabState();
    const current = activePreviewTab(state, previewContext);
    const next = activePreviewTab(producer(state), previewContext);
    const approval = current?.key === next?.key ? true : requestActiveFileLeave();
    return runAfterFileApproval(approval, () => {
      setPreviewTabState(producer);
      afterSelect?.();
      return true;
    });
  }, [getPreviewTabState, previewContext, requestActiveFileLeave, setPreviewTabState]);
  const previewTabsOpen = previewTabs.length > 0;
  // 总开关必须连最大化详情一起隐藏;恢复时仍保留最大化偏好与原页签。
  const previewPanelVisible = previewTabsOpen && !sidePanelCollapsed && !previewPanelHidden;
  const previewPanelMaximized = sidePanelMaximized && previewPanelVisible;
  const previewCloseConfirmMessage = previewCloseConfirm
    ? `${previewCloseConfirm.dirtyCount} 个文件有未保存的修改。是否保存后继续？`
    : '';
  const selectedChangeFile = activePreview?.type === PREVIEW_TAB_TYPES.SESSION_CHANGES
    ? activePreview.expandedFile || ''
    : '';
  const selectedChangeFileRevision = activePreview?.type === PREVIEW_TAB_TYPES.SESSION_CHANGES
    ? activePreview.expandedFileRevision || 0
    : 0;
  const selectedGitChangeFile = activePreview?.type === PREVIEW_TAB_TYPES.GIT_CHANGES
    ? activePreview.expandedFile || ''
    : '';
  const contentLayout = useMemo(() => solveSingleContentLayout({
    contentWidth: layoutWidth,
    sidePanelWidth,
    previewPanelWidth,
    sidePanelVisible: sidePanelMounted,
    sidePanelCollapsed: sidePanelNavigationCollapsed,
    previewPanelVisible,
    previewPanelMaximized,
    previewPanelAutoFit,
  }), [
    layoutWidth,
    previewPanelAutoFit,
    previewPanelMaximized,
    previewPanelVisible,
    previewPanelWidth,
    sidePanelNavigationCollapsed,
    sidePanelMounted,
    sidePanelWidth,
  ]);
  const effectiveChatWidth = layoutWidth > 0 ? contentLayout.chatWidth : 0;
  const effectivePreviewPanelWidth = layoutWidth > 0 ? contentLayout.previewPanelWidth : previewPanelWidth;
  const effectiveSidePanelWidth = layoutWidth > 0 ? contentLayout.sidePanelWidth : sidePanelWidth;
  const renderedSubagentPanelWidth = normalizeSubagentPanelWidth(
    subagentPanelWidth,
    effectiveChatWidth,
  );
  const subagentPanelRange = subagentPanelWidthRange(effectiveChatWidth);
  const subagentPanelAriaMax = Number.isFinite(subagentPanelRange.max)
    ? subagentPanelRange.max
    : Math.max(subagentPanelRange.min, renderedSubagentPanelWidth);
  renderedPreviewPanelWidthRef.current = effectivePreviewPanelWidth;
  renderedSubagentPanelWidthRef.current = renderedSubagentPanelWidth;

  useEffect(() => {
    if (!sid) return;
    setPreviewTabState((prev) => updateSessionChangesTab(prev, {
      sessionId: sid,
      fileCount: changeSummary.fileCount,
    }));
  }, [changeSummary.fileCount, sid]);

  useEffect(() => {
    onPreviewPanelVisibleChange?.(previewPanelVisible);
  }, [onPreviewPanelVisibleChange, previewPanelVisible]);

  // line 由聊天正文的文件链接(foo.cpp:42)带入,预览打开后滚动到该行并高亮;
  // 文件树等其它入口不带 line,走原「只打开」语义。
  const openFilePreview = useCallback((path, line = null) => {
    const location = previewFileLocation({ cwd: sidePanelCwd, path });
    if (!previewScope || !location.cwd || !location.path) return;
    // 侧栏折叠时开预览 tab 也不会显示(previewPanelVisible 依赖非折叠),先展开。
    return selectPreview((prev) => openFileTab(prev, {
      ...previewContext,
      cwd: location.cwd,
      path: location.path,
      line,
    }), () => {
      if (sidePanelCollapsed) onToggleSidePanel?.();
      setPreviewPanelHidden(false);
    });
  }, [previewContext, previewScope, selectPreview, sidePanelCwd, sidePanelCollapsed, onToggleSidePanel]);

  // transcript 里本地文件链接的兜底入口。assistant 气泡在 Message.jsx 里自带
  // 拦截,但同一片区域还有别的 markdown 渲染位置没有各自的拦截器
  // (ToolBlock 的 task_complete 完成总结、会话摘要),那里的文件链接以前点了
  // 没有任何反应。链接本身已经没有 href(markdown.js 里删掉了),不会跳走,
  // 这里只负责把它们接回预览/文件树。
  const handleTranscriptFileLink = useCallback((event) => {
    if (event.type === 'keydown' && event.key !== 'Enter' && event.key !== ' ') return;
    const anchor = event.target?.closest?.('a[data-file-path]');
    if (!anchor) return;
    const path = anchor.getAttribute('data-file-path') || '';
    if (!path) return;
    event.preventDefault();
    const kind = anchor.getAttribute('data-file-kind') || 'file';
    const lineAttr = anchor.getAttribute('data-file-line');
    if (kind === 'directory') locateInFileTree(path);
    else openFilePreview(path, lineAttr ? Number(lineAttr) : null);
  }, [locateInFileTree, openFilePreview]);

  // 有专用原生文件选择器(Desktop 壳)走原生,否则走 web 路径选择器(add-web-path-picker)。
  const openPreviewFilePicker = useCallback(async () => {
    if (!sidePanelCwd) return;
    try {
      const picked = await pickPreviewFile(sidePanelCwd, { api });
      if (!picked.cancelled && picked.path) openFilePreview(picked.path);
    } catch (error) {
      toast({ kind: 'err', text: error?.message || '选择器不可用' });
    }
  }, [api, openFilePreview, sidePanelCwd]);

  const showBrowserPage = useCallback((pageId, title, favicon) => {
    if (!sid || !pageId) return;
    return selectPreview((prev) => openBrowserTab(prev, {
      scopeKey: previewScope,
      sessionId: sid,
      pageId,
      title,
      favicon,
    }), () => {
      onRevealPreviewPanel?.();
      setPreviewPanelHidden(false);
      void selectAgentBrowserPage(pageId);
    });
  }, [onRevealPreviewPanel, previewScope, selectPreview, sid]);

  const browserOpenPendingRef = useRef(false);
  const openBrowserPreview = useCallback(async (url = '') => {
    if (!sid || !hasNativeAgentBrowser() || browserOpenPendingRef.current) return;
    browserOpenPendingRef.current = true;
    try {
      if (!await requestActiveFileLeave() || sidRef.current !== sid) return;
      const created = await createAgentBrowserPage(agentBrowserOwnerForSession(ref));
      if (created?.ok === false || !created?.page_id) {
        throw new Error(created?.error || '浏览器操作失败');
      }
      if (sidRef.current !== sid) {
        await closeAgentBrowserPage(created.page_id);
        return;
      }
      const shown = await showBrowserPage(
        created.page_id,
        created.title || defaultBrowserTabTitle(),
        created.favicon,
      );
      if (!shown) {
        await closeAgentBrowserPage(created.page_id);
        return;
      }
      if (typeof url === 'string' && url) {
        const ready = await waitForAgentBrowserPageReady(created.page_id, {
          isCurrent: () => sidRef.current === sid,
        });
        if (ready.cancelled) {
          await closeAgentBrowserPage(created.page_id);
          return;
        }
        if (ready.ok === false) throw new Error(ready.error || '浏览器操作失败');
        const navigated = await runAgentBrowserBridgeAction('aceDesktop_agentBrowserNavigate', {
          page_id: created.page_id, url,
        });
        if (navigated?.ok === false) throw new Error(navigated.error || '浏览器操作失败');
      }
    } catch (error) {
      toast({ kind: 'err', text: error?.message || '浏览器操作失败' });
    } finally {
      browserOpenPendingRef.current = false;
    }
  }, [ref, requestActiveFileLeave, showBrowserPage, sid]);

  // 切会话时向 Desktop 对账一次 native 页面池;事件流已经在 App 级持续镜像。
  useEffect(() => {
    if (!sid || !hasNativeAgentBrowser()) return;
    void reconcileAgentBrowserPageStore(sid);
  }, [sid]);

  // 旧版 Desktop 的状态事件不带 owner:退回按「当前会话有正在执行的浏览器工具」
  // 认领,只影响本地登记表镜像,native 已归属的页面不会被抢走。
  useEffect(() => {
    if (!sid || !agentBrowserActivity.active) return undefined;
    const onLegacyBrowserState = (event) => {
      const detail = event?.detail;
      const pageId = String(detail?.page_id || '');
      if (!pageId || detail?.owner || detail?.closed || !detail?.active) return;
      agentBrowserPageStore().commit((state) => (
        claimUnownedAgentBrowserPage(state, pageId, sid, ref?.workspaceHash || '')
      ));
    };
    window.addEventListener(AGENT_BROWSER_STATE_EVENT, onLegacyBrowserState);
    return () => window.removeEventListener(AGENT_BROWSER_STATE_EVENT, onLegacyBrowserState);
  }, [agentBrowserActivity.active, ref?.workspaceHash, sid]);

  // 登记表 → 页签:补缺、去已关闭、同步标题与图标。本 ChatView 首次见到的页面
  // 自动打开并激活(包括用户切走期间 Agent 开的页,切回来时页签就在);再次切回
  // 同一会话不重复抢焦点。
  const revealedBrowserPagesRef = useRef(new Map());
  useEffect(() => {
    if (!sid) return;
    const restoredPages = visiblePreviewTabs(getPreviewTabState(), previewContext)
      .filter((tab) => tab.type === PREVIEW_TAB_TYPES.BROWSER).map((tab) => tab.pageId);
    setPreviewTabState((prev) => syncBrowserTabsForSession(prev, {
      scopeKey: previewScope,
      sessionId: sid,
      pages: sessionBrowserPages,
    }));
    let revealed = revealedBrowserPagesRef.current.get(sid);
    if (!revealed) {
      revealed = new Set(restoredPages);
      revealedBrowserPagesRef.current.set(sid, revealed);
    }
    for (const page of sessionBrowserPages) {
      if (revealed.has(page.pageId)) continue;
      revealed.add(page.pageId);
      showBrowserPage(page.pageId, page.title, page.favicon);
    }
    for (const pageId of Array.from(revealed)) {
      if (!sessionBrowserPages.some((page) => page.pageId === pageId)) {
        revealed.delete(pageId);
      }
    }
  }, [getPreviewTabState, previewContext, previewScope, sessionBrowserPages, showBrowserPage, sid]);

  // Agent 切换默认目标页(browser_open / 显式选页)且有浏览器工具正在执行时,把
  // 那一页的页签激活到前台;目标不属于本会话(显式操作别的会话的页)则不动。
  const agentBrowserTargetRef = useRef('');
  useEffect(() => {
    const scoped = agentBrowserActivePageId ? `${sid}:${agentBrowserActivePageId}` : '';
    if (!scoped) {
      agentBrowserTargetRef.current = '';
      return;
    }
    if (agentBrowserTargetRef.current === scoped) return;
    agentBrowserTargetRef.current = scoped;
    if (!sessionBrowserPages.some((page) => page.pageId === agentBrowserActivePageId)) return;
    showBrowserPage(agentBrowserActivePageId);
  }, [agentBrowserActivePageId, sessionBrowserPages, showBrowserPage, sid]);

  const openSessionChangePreview = useCallback((filePath, turnUserMessageId = '') => {
    if (!sid || !filePath) return;
    const turnChangeSet = turnUserMessageId
      ? turnChangeSets.find((set) => set.userMessageId === turnUserMessageId)
      : null;
    return selectPreview((prev) => openSessionChangesTab(prev, {
      scopeKey: previewScope,
      sessionId: sid,
      expandedFile: filePath,
      fileCount: turnUserMessageId
        ? (turnChangeSet?.summary?.fileCount || 0)
        : changeSummary.fileCount,
      turnUserMessageId,
    }), () => {
      if (sidePanelCollapsed) onToggleSidePanel?.();
      setPreviewPanelHidden(false);
    });
  }, [changeSummary.fileCount, onToggleSidePanel, previewScope, selectPreview, sid, sidePanelCollapsed, turnChangeSets]);

  // git 变更点击文件 → 在中间详情栏开/聚焦「变更」页签(复刻会话级变更旧行为)。
  // gitBase 只有从 SidePanel 导航列表点击时才带;详情栏内点文件不带,由
  // openGitChangesTab 保留页签原 base。
  const openGitChangePreview = useCallback((filePath, gitBase, gitFileCount) => {
    if (!previewContext.sessionId || !filePath) return;
    return selectPreview((prev) => openGitChangesTab(prev, {
      ...previewContext,
      cwd: sidePanelCwd,
      base: gitBase,
      expandedFile: filePath,
      fileCount: gitFileCount,
    }), () => {
      if (sidePanelCollapsed) onToggleSidePanel?.();
      setPreviewPanelHidden(false);
    });
  }, [onToggleSidePanel, previewContext, selectPreview, sidePanelCollapsed, sidePanelCwd]);

  // SidePanel 切基线时,若「变更」页签已打开则同步其 base(详情栏跟着换比较对象)。
  const updateGitChangeBase = useCallback((gitBase) => {
    if (!previewContext.sessionId) return;
    setPreviewTabState((prev) => updateGitChangesTab(prev, { ...previewContext, base: gitBase || '' }));
  }, [previewContext]);

  const activatePreview = useCallback((tabKey) => {
    const tab = previewTabs.find((candidate) => candidate.key === tabKey);
    return selectPreview((prev) => activatePreviewTab(prev, {
      ...previewContext,
      tabKey,
    }), () => {
      if (tab?.type === PREVIEW_TAB_TYPES.BROWSER && tab.pageId) {
        void selectAgentBrowserPage(tab.pageId);
      }
    });
  }, [previewContext, previewTabs, selectPreview]);

  const refreshPreview = useCallback((tabKey) => {
    setPreviewTabState((prev) => refreshPreviewTab(prev, {
      ...previewContext,
      tabKey,
    }));
  }, [previewContext]);

  const updateFilePreviewDraft = useCallback((tabKey, patch) => {
    setPreviewTabState((prev) => updateFileTabDraft(prev, {
      ...previewContext,
      tabKey,
      patch,
    }));
  }, [previewContext, setPreviewTabState]);

  useEffect(() => {
    const transition = nextAutoPreviewRefresh(previewAutoRefreshRef.current, {
      sid,
      busy,
      turnKey: lastUserTurnKey,
      activeTab: activePreview,
      changedPaths: latestSuccessfulChangedFiles,
    });
    previewAutoRefreshRef.current = transition.state;
    if (transition.tabKey) refreshPreview(transition.tabKey);
  }, [activePreview, busy, lastUserTurnKey, latestSuccessfulChangedFiles, refreshPreview, sid]);

  const previewTabsForCloseAction = useCallback((kind, tabKey = '') => {
    if (kind === 'all') return previewTabs;
    const index = previewTabs.findIndex((tab) => tab.key === tabKey);
    if (index < 0) return [];
    if (kind === 'one') return [previewTabs[index]];
    if (kind === 'others') return previewTabs.filter((tab) => tab.key !== tabKey);
    if (kind === 'right') return previewTabs.slice(index + 1);
    return [];
  }, [previewTabs]);

  const performPreviewClose = useCallback((kind, tabKey = '') => {
    const affected = previewTabsForCloseAction(kind, tabKey);
    if (affected.length === 0) return;
    affected.forEach((tab) => {
      if (tab.type === PREVIEW_TAB_TYPES.BROWSER && tab.pageId) {
        void closeAgentBrowserPage(tab.pageId);
      }
    });
    setPreviewTabState((prev) => {
      const options = { ...previewContext, tabKey };
      if (kind === 'one') return closePreviewTab(prev, options);
      if (kind === 'others') return closeOtherPreviewTabs(prev, options);
      if (kind === 'right') return closePreviewTabsToRight(prev, options);
      return closeVisiblePreviewTabs(prev, options);
    });
    if (affected.length >= previewTabs.length && sidePanelMaximized) {
      onToggleSidePanelMaximized?.();
    }
  }, [
    onToggleSidePanelMaximized,
    previewContext,
    previewTabs.length,
    previewTabsForCloseAction,
    sidePanelMaximized,
  ]);

  const requestPreviewClose = useCallback((kind, tabKey = '') => {
    const affected = previewTabsForCloseAction(kind, tabKey);
    if (affected.length === 0) return;
    const keys = new Set(affected.map((tab) => tab.key));
    return runAfterFileApproval(requestPreviewApproval(
      () => visiblePreviewTabs(getPreviewTabState(), previewContext)
        .filter((tab) => keys.has(tab.key)),
      'close',
    ), () => performPreviewClose(kind, tabKey));
  }, [
    getPreviewTabState,
    performPreviewClose,
    previewContext,
    previewTabsForCloseAction,
    requestPreviewApproval,
  ]);

  const closePreview = useCallback((tabKey) => {
    requestPreviewClose('one', tabKey);
  }, [requestPreviewClose]);

  const closeAllPreviews = useCallback(() => {
    requestPreviewClose('all');
  }, [requestPreviewClose]);

  const closeOtherPreviews = useCallback((tabKey) => {
    requestPreviewClose('others', tabKey);
  }, [requestPreviewClose]);

  const closePreviewsToRight = useCallback((tabKey) => {
    requestPreviewClose('right', tabKey);
  }, [requestPreviewClose]);

  const hidePreviewPanel = useCallback(() => {
    return runAfterFileApproval(requestActiveFileLeave(), () => setPreviewPanelHidden(true));
  }, [requestActiveFileLeave]);

  const reorderPreview = useCallback((sourceKey, targetKey, placement) => {
    setPreviewTabState((prev) => reorderPreviewTab(prev, {
      ...previewContext,
      sourceKey,
      targetKey,
      placement,
    }));
  }, [previewContext]);

  // Home and auxiliary pages share the same workspace navigation and previews.
  let homeContent = null;
  if (!sid && !children) {
    const homeProjectName = selectedHomeWorkspace?.name || '当前项目';
    const homeProjectTitle = selectedHomeWorkspace?.noWorkspace
      ? '我们该做什么？'
      : `我们该在 ${homeProjectName} 中做什么？`;
    const homeProjectLabel = homeWorkspaces.find(w => w.hash === homeWorkspaceHash)?.name
      || selectedHomeWorkspace?.name
      || '当前项目';
    const homeHints = [
      { icon: 'edit', title: '编辑代码', desc: '让 Agent 帮你重构、修 bug、加测试' },
      { icon: 'searchSparkle', title: '探索代码库', desc: '问“这个函数在哪里被调用”' },
      { icon: 'run', title: '运行命令', desc: 'bash / npm / git 等 Agent 会逐步确认' },
      { icon: 'lightbulb', title: '使用 Skills', desc: '预定义工作流，从侧边栏开启' },
    ];
    homeContent = (
      <div
        className="ace-home-drop-scope ace-chat-file-drop-scope flex-1 min-h-0 min-w-0 flex flex-col bg-surface"
        data-chat-file-drop-scope="true"
        data-session-content-loading-anchor="true"
        data-file-drop-active={chatFileDropActive ? 'true' : undefined}
        onDragEnter={handleChatFileDragEnter}
        onDragOver={handleChatFileDragOver}
        onDragLeave={handleChatFileDragLeave}
        onDrop={handleChatFileDrop}
      >
        <div className="ace-home-panel ace-scrollbar flex-1">
          <div className="ace-home-content">
            <InteractiveHomeLogo enabled={homeLogoEffectEnabled} />
            <h1 className="ace-home-title">{homeProjectTitle}</h1>
            <div data-tour-target="home-composer" className="ace-home-composer">
              {questionForView ? (
                <QuestionPicker
                  request={questionForView}
                  onResolve={resolveQuestion}
                  originLabel={questionOriginLabel}
                />
              ) : (
                <InputBar
                  ref={inputRef}
                  mainComposer
                  variant="hero"
                  attentionRequest={homeComposerAttentionRequest}
                  pathReferenceApi={api}
                  currentSessionId=""
                  cwd={selectedHomeWorkspace?.cwd || ''}
                  expertOptions={recentExperts}
                  selectedExpertId={composerExpertId}
                  selectedExpertName={composerExpert?.display_name || ''}
                  selectedExpertType={composerExpert?.type || 'agent'}
                  expertRemoving={expertDetaching}
                  onSelectExpert={selectComposerExpert}
                  onRemoveExpert={detachComposerExpert}
                  onOpenExpertComponents={() => setExpertPickerOpen(true)}
                  history={composerHistory}
                  historyEntries={composerHistoryEntries}
                  value={composerValue}
                  onChange={handleComposerChange}
                  onSubmit={submit}
                  submitting={homeSubmitting || reasoningSwitching}
                  placeholder="向 ACECode 描述任务，或输入 / 命令..."
                  {...composerInputProps}
                  fileDropManagedExternally
                  onFileDragActiveChange={setChatFileDropActive}
                  sessionControls={{
                    model: homeModelLabel,
                    modelOptions,
                    selectedModelName: homeModelName,
                    modelLoad: homeModelLoad,
                    modelSwitching: modelSwitching || reasoningSwitching,
                    modelRefreshing,
                    reasoningOptions: composerReasoningOptions(selectedHomeModel, homeReasoningEffort),
                    reasoningDisabled: homeSubmitting || composerSubmitting || reasoningSwitching || modelSwitching || modelRefreshing,
                    onReasoningChange: changeComposerReasoning,
                    onModelChange: changeComposerModel,
                    onRefreshModels: refreshSessionModels,
                    onOpenModelSettings,
                    tokenBudget: homeTokenBudget,
                    permissionMode,
                    permissionSwitching,
                    onPermissionModeChange: changeComposerPermissionMode,
                  }}
                />
              )}
            </div>
            <div className="flex items-center gap-2 mr-auto ml-0">
            <div className="relative">
              <button
                ref={projectAnchorRef}
                data-tour-target="home-workspace"
                type="button"
                aria-expanded={projectDropdownOpen}
                className="ace-home-project-row group"
                onClick={() => setProjectDropdownOpen(!projectDropdownOpen)}
                title={selectedHomeWorkspace?.cwd || homeProjectName}
              >
                <VsIcon name="folder" size={15} />
                <span className="text-fg-mute">项目</span>
                <span className="ace-home-project-select truncate">
                  {homeProjectLabel}
                </span>
                <VsIcon name="expandDown" size={14} className="opacity-50 group-hover:opacity-100 transition-opacity -ml-[2px]" />
                {homeSubmitting && <span className="ace-spinner w-3 h-3 ml-1" />}
              </button>

              {projectDropdownOpen && (
                  <AnchoredMenu
                    anchorRef={projectAnchorRef}
                    onClose={() => setProjectDropdownOpen(false)}
                    width={280}
                    maxHeightRatio={0.4}
                    className="ace-home-project-menu bg-surface border border-border ace-shadow rounded-xl z-50 py-1.5"
                  >
                    <div className="px-3 pb-1 mb-1 text-[11px] font-semibold text-fg-mute border-b border-border/50 uppercase tracking-wider">
                      工作区
                    </div>
                    <button
                      type="button"
                      className="group w-full h-8 px-3 text-left text-[13px] flex items-center gap-2 text-fg hover:bg-surface-hi transition-colors"
                      onClick={() => {
                        setProjectDropdownOpen(false);
                        setCreateProjectOpen(true);
                      }}
                    >
                      <VsIcon name="folderAdd" size={15} className="text-fg-mute group-hover:text-fg shrink-0" />
                      <span className="truncate">新建项目</span>
                    </button>
                    <button
                      type="button"
                      className="group w-full h-8 px-3 text-left text-[13px] flex items-center gap-2 text-fg hover:bg-surface-hi transition-colors"
                      onClick={handleOpenExistingDirectory}
                    >
                      <VsIcon name="folderOpen" size={15} className="text-fg-mute group-hover:text-fg shrink-0" />
                      <span className="truncate">打开现有目录</span>
                    </button>
                    <div className="my-1 border-t border-border/50" aria-hidden="true" />
                    {homeWorkspaces.map((w) => (
                      <button
                        key={w.hash || w.cwd || w.name}
                        type="button"
                        className={clsx(
                          "w-full text-left px-3 py-1.5 text-[13px] flex flex-col gap-[2px] transition-colors",
                          w.hash === homeWorkspaceHash ? "bg-accent/10 text-accent font-medium" : "text-fg hover:bg-surface-hi"
                        )}
                        onClick={() => selectHomeWorkspace(w)}
                      >
                        <div className="truncate leading-tight">{w.name}</div>
                        <div className={clsx("text-[10.5px] truncate leading-tight", w.hash === homeWorkspaceHash ? "text-accent/60" : "text-fg-mute/70")} title={w.cwd}>
                          {w.cwd.replace(/\\/g, '/')}
                        </div>
                      </button>
                    ))}
                    <div className="my-1 border-t border-border/50" aria-hidden="true" />
                    <button
                      type="button"
                      className={clsx(
                        "w-full text-left px-3 py-2 text-[13px] transition-colors",
                        !homeWorkspaceHash ? "bg-accent/10 text-accent font-medium" : "text-fg hover:bg-surface-hi"
                      )}
                      onClick={() => selectHomeWorkspace(noHomeWorkspaceOption())}
                    >
                      <div className="truncate leading-tight">{noHomeWorkspaceOption().name}</div>
                    </button>
                  </AnchoredMenu>
              )}
            </div>
            <GitSessionPill
              key={workbenchOwner}
              owner={workbenchOwner}
              api={api}
              cwd={selectedHomeWorkspace?.cwd || ''}
              variant="hero"
              busy={homeSubmitting}
              onIntentChange={handleGitPillIntentChange}
            />
            </div>
            {/* 暂时隐藏首页底部四个快捷提示卡片，保留数据和渲染代码便于恢复。 */}
            <div className="ace-home-hints hidden" aria-hidden="true">
              {homeHints.map((c) => (
                <div key={c.title} className="ace-home-hint-card">
                  <VsIcon name={c.icon} size={22} className="mb-1" />
                  <div className="text-[13px] font-semibold mb-0.5">{c.title}</div>
                  <div className="text-[11px] text-fg-mute leading-relaxed">{c.desc}</div>
                </div>
              ))}
            </div>
          </div>
        </div>

        {createProjectOpen && (
          <CreateProjectModal
            api={api}
            onClose={() => setCreateProjectOpen(false)}
            onCreated={handleProjectCreated}
          />
        )}
        <ChatFileDropOverlay active={chatFileDropActive} />
      </div>
    );
  }

  const chatColumnStyle = previewPanelMaximized
    ? undefined
    : (layoutWidth > 0
      ? {
          flex: `0 0 ${Math.max(0, effectiveChatWidth)}px`,
          width: Math.max(0, effectiveChatWidth),
          minWidth: Math.min(MIN_CHAT_WIDTH, Math.max(0, effectiveChatWidth)),
        }
      : { minWidth: MIN_CHAT_WIDTH });
  const previewShellStyle = previewPanelMaximized
    ? {
        flex: '1 1 auto',
        width: 'auto',
      }
    : {
        width: Math.max(0, effectivePreviewPanelWidth),
      };
  const sidePanelShellStyle = {
    width: sidePanelNavigationCollapsed ? 0 : Math.max(0, effectiveSidePanelWidth),
  };

  return (
    <div ref={layoutRef} className="flex-1 flex min-w-0 ace-chat-layout">
      {children || homeContent ? (
        <div className={clsx('flex-1 flex flex-col min-w-0 min-h-0', previewPanelMaximized && 'hidden')}
          style={chatColumnStyle}>
          {children || homeContent}
        </div>
      ) : (
      <div
        className={clsx(
          'ace-session-panel ace-chat-file-drop-scope flex-1 flex flex-col min-w-0 relative',
          previewPanelMaximized && 'hidden',
        )}
        data-chat-file-drop-scope="true"
        data-session-content-loading-anchor="true"
        data-file-drop-active={chatFileDropActive ? 'true' : undefined}
        onDragEnter={handleChatFileDragEnter}
        onDragOver={handleChatFileDragOver}
        onDragLeave={handleChatFileDragLeave}
        onDrop={handleChatFileDrop}
        style={chatColumnStyle}
      >
      {sid && !readOnlyExternalSession && transcriptLoadState === 'loaded' && (
        <TaskSuggestionCards
          key={`${ref?.workspaceHash || ''}:${sid}`}
          api={api}
          sessionId={sid}
          sourceRef={ref}
          busy={busy}
          onOpenSession={onSessionPromoted}
        />
      )}
      <SessionTitleBar titleTarget={titleTarget} actionsTarget={actionsTarget}
        title={title} workspaceLabel={workspaceLabel} remoteControlBound={remoteControlBound}
        labelRef={sessionTitleLabelRef}
        renaming={headerRenaming && !!sid && !readOnlyExternalSession}
        onRenameCommit={commitHeaderRename}
        onRenameCancel={cancelHeaderRename}>
          {sid && (
            <button
              type="button"
              onClick={() => setTrajectoryOpen((open) => !open)}
              className={clsx(
                'w-6 h-6 rounded-md flex items-center justify-center shrink-0 transition focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-accent/25',
                trajectoryOpen
                  ? 'bg-accent-bg text-accent hover:bg-accent-bg'
                  : 'text-fg-mute hover:bg-surface-hi hover:text-fg',
              )}
              title={trajectoryOpen ? 'Conversation' : 'Trajectory'}
              aria-label={trajectoryOpen ? 'Conversation' : 'Trajectory'}
              aria-pressed={trajectoryOpen}
            >
              <VsIcon name="trajectory" size={16} />
            </button>
          )}
          {sid && (
            <LspIndicator
              api={api}
              cwd={sidePanelCwd}
              refreshKey={`${turns}:${busy ? 1 : 0}`}
            />
          )}
          {sid && (subagentTasks.tasks.length > 0 || subagentPanelOpen) && (
            <button
              type="button"
              onClick={() => setSubagentPanelOpen((v) => !v)}
              className={clsx(
                'relative w-7 h-7 rounded-md flex items-center justify-center transition focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-accent/25',
                subagentPanelOpen || subagentTasks.runningCount > 0
                  ? 'bg-accent-bg text-accent hover:bg-accent-bg'
                  : 'text-fg-mute hover:bg-surface-hi hover:text-fg',
              )}
              title="后台任务"
              aria-label="后台任务"
            >
              <VsIcon name="embedding" size={15} />
              {subagentTasks.runningCount > 0 && (
                <span className="absolute -top-1 -right-1 min-w-[14px] h-[14px] px-0.5 rounded-full bg-accent text-white text-[9px] leading-[14px] text-center font-semibold">
                  {subagentTasks.runningCount}
                </span>
              )}
            </button>
          )}
          {sid && hasNativeAgentBrowser() && (
            <button
              type="button"
              onClick={openBrowserPreview}
              className="w-7 h-7 rounded-md bg-surface-hi/0 text-fg-mute flex items-center justify-center transition hover:bg-surface-hi hover:text-fg focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-accent/25"
              title="打开浏览器"
              aria-label="打开浏览器"
            >
              <VsIcon name="globe" size={16} />
            </button>
          )}
          {sid && (
            <button
              ref={sessionMenuButtonRef}
              type="button"
              data-desktop-session-id={readOnlyExternalSession ? undefined : sid || undefined}
              data-desktop-session-workspace={sessionWorkspaceHash || undefined}
              data-desktop-session-path={sessionPath || undefined}
              data-desktop-session-pinned={sessionPinned ? 'true' : 'false'}
              data-desktop-session-title={title || undefined}
              data-desktop-session-archive="true"
              onClick={openSessionContextMenu}
              className="w-7 h-7 rounded-md bg-surface-hi/0 text-fg-mute flex items-center justify-center transition hover:bg-surface-hi hover:text-fg focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-accent/25"
              title="会话菜单"
              aria-label="会话菜单"
              aria-haspopup="menu"
            >
              <VsIcon name="ellipsisVertical" size={16} />
            </button>
          )}
      </SessionTitleBar>

      <div
        ref={subagentSplitRef}
        className={clsx('relative flex-1 min-h-0 flex', trajectoryOpen && 'hidden')}
        aria-hidden={trajectoryOpen ? 'true' : undefined}
      >
        <div className="relative flex-1 min-w-0 h-full pr-1.5">
        <div
          ref={scrollRef}
          data-conversation-find-root="true"
          onScroll={handleMessagesScroll}
          onWheel={handleMessagesWheel}
          onPointerDown={handleMessagesPointerDown}
          onTouchStart={historyController.onTouchStart}
          onTouchMove={historyController.onTouchMove}
          onTouchEnd={historyController.onTouchEnd}
          onTouchCancel={historyController.onTouchEnd}
          onKeyDownCapture={handleMessagesKeyDownCapture}
          onClick={handleTranscriptFileLink}
          onKeyDown={handleTranscriptFileLink}
          className="ace-chat-transcript-scroll h-full overflow-y-auto pl-[35px] pr-3.5 py-3"
          style={changeDockBottomPadding > 0 ? { paddingBottom: changeDockBottomPadding } : undefined}
        >
          <div ref={transcriptContentRef} className="flex flex-col gap-3">
          <TranscriptHistoryBoundary
            boundaryRef={historyPaging.boundaryRef}
            phase={historyPaging.phase}
            hiddenCount={windowHiddenCount}
            hasMore={transcript.historyHasMore}
            onEarlier={revealEarlierTranscript}
            onAll={() => expandTranscriptWindow({ compensateScroll: true, loadAll: true })}
          />
          <AttachmentTextLoaderContext.Provider value={api.readAttachmentText}>
          <TranscriptItems
            items={windowedItems}
            messageAutoCollapse={messageAutoCollapse}
            assistantRunDirectives={assistantRunDirectives}
            expandedActivityKeys={expandedActivityKeys}
            collapsedMediaKeys={collapsedMediaKeys}
            onToggleActivity={toggleActivitySummary}
            onToggleMedia={toggleMediaGroup}
            conversationActivity={conversationActivity}
            subagentTasksById={subagentTasksById}
            onOpenSubagent={openSubagentTranscript}
            sessionRunning={status === 'running'}
            onReviewToggle={pauseTailFollowForReview}
            onFork={forkAndSwitch}
            forkingMessageId={forkingMessageId}
            onOpenFilePreview={openFilePreview}
            onLocateInFileTree={locateInFileTree}
            showAceCodeAvatar={showAceCodeAvatar}
            annotationPresentations={selectionAnnotationPresentations}
            renderBeforeItem={(it) => (
              (turnFileListPlacement.before.get(it.id) || []).map((set) => (
                <div
                  key={`turn-files-${set.userItemId || 'head'}`}
                  className="ace-chat-row flex flex-col ace-chat-row-assistant-gutter"
                  data-chat-row="true"
                  data-chat-kind="turn_files"
                >
                  <TurnFileList
                    groups={set.groups}
                    summary={set.summary}
                    cwd={sidePanelCwd}
                    turnUserMessageId={set.userMessageId}
                    onOpenChanges={openSessionChangePreview}
                    onOpenFile={openFilePreview}
                  />
                </div>
              ))
            )}
          />
          </AttachmentTextLoaderContext.Provider>
          {/* tail = 当前最后一轮的文件列表。回合进行中(busy)不渲染 ——
              流式期间变更集随 tool_end 实时增长,列表会先于/夹着正文出现,
              观感突兀;等整轮吐完(busy 结束)再一次性显示在正文之后。
              历史回合(before 锚定)不受影响。 */}
          {!busy && turnFileListPlacement.tail.map((set) => (
            <div
              key={`turn-files-${set.userItemId || 'head'}`}
              className="ace-chat-row flex flex-col ace-chat-row-assistant-gutter"
              data-chat-row="true"
              data-chat-kind="turn_files"
            >
              <TurnFileList
                groups={set.groups}
                summary={set.summary}
                cwd={sidePanelCwd}
                turnUserMessageId={set.userMessageId}
                onOpenChanges={openSessionChangePreview}
                onOpenFile={openFilePreview}
              />
            </div>
          ))}
          {permissionRequests.map((request) => (
            <div
              key={`permission-${request.request_id}`}
              className="ace-chat-row flex flex-col ace-chat-row-assistant-gutter"
              data-chat-row="true"
              data-chat-item-id={`permission-${request.request_id}`}
              data-chat-kind="permission"
            >
              <PermissionCard
                request={request}
                originLabel={request.origin_label || ''}
                onDecision={onPermissionDecision}
              />
            </div>
          ))}
          {conversationActivity.kind === CONVERSATION_ACTIVITY_KIND.BACKGROUND && (
            <ActivityLine
              running
              label={conversationActivity.label}
              detail={conversationActivity.detail}
              live
            />
          )}
          </div>
        </div>
        {showConversationTurnScrubber && (
          <ConversationTurnScrubberBoundary key={sid}>
            <Suspense fallback={null}>
              <LazyConversationTurnScrubber
                turns={preparedConversationTurns}
                activeIndex={activeConversationTurn}
                onJump={jumpToConversationTurn}
              />
            </Suspense>
          </ConversationTurnScrubberBoundary>
        )}
        {sid && showScrollToBottom && (
          <button
            type="button"
            onClick={jumpToChatTail}
            className="absolute left-1/2 -translate-x-1/2 z-20 flex h-9 w-9 items-center justify-center rounded-full border border-border bg-surface text-fg ace-shadow-lg hover:bg-surface-hi focus-visible:outline focus-visible:outline-2 focus-visible:outline-offset-2 focus-visible:outline-accent"
            style={{ bottom: Math.max(12, changeDockBottomPadding + 12) }}
            title={status === 'running' ? '运行中' : '滚动到底部'}
            aria-label="滚动到底部"
          >
            {status === 'running' ? (
              <span className="ace-chat-tail-progress" aria-hidden="true">
                <span />
                <span />
                <span />
              </span>
            ) : <VsIcon name="ArrowDown" size={18} />}
          </button>
        )}
        <StickyUserContext context={stickyUserContext} onJumpToSource={jumpToStickyUserSource} />
        {showChangeDock && (
          <ChangeGlassDock
            dockRef={changeDockRef}
            summary={changeSummary}
            showChanges={showChangeDetails}
            onReview={openReviewPanel}
            onDismiss={dismissChangeDock}
            todos={dockTodos}
            todoSummary={dockTodoSummary}
          />
        )}
        </div>
        {subagentPanelOpen && (
          <div
            role="separator"
            aria-label="调整后台任务面板宽度"
            aria-orientation="vertical"
            aria-valuemin={subagentPanelRange.min}
            aria-valuemax={subagentPanelAriaMax}
            aria-valuenow={renderedSubagentPanelWidth}
            tabIndex={0}
            className="ace-resize-handle ace-resize-handle-subagent"
            data-subagent-splitter="true"
            onPointerDown={startSubagentPanelResize}
            onMouseDown={startSubagentPanelResize}
            onKeyDown={onSubagentPanelHandleKeyDown}
            title="拖动调整后台任务面板宽度"
          />
        )}
        <SubagentPanel
          messageAutoCollapse={messageAutoCollapse}
          open={subagentPanelOpen}
          width={renderedSubagentPanelWidth}
          focus={subagentFocus}
          onClose={() => setSubagentPanelOpen(false)}
          tasks={subagentTasks.tasks}
          workspaceHash={subagentWorkspaceHash}
          onAbort={(task) => {
            if (subagentTasks.abortTask(task.id) === false) {
              toast({ kind: 'err', text: '停止请求发送失败，连接恢复后请重试' });
            }
          }}
        />
      </div>

      {trajectoryOpen && (
        <Suspense
          fallback={<div className="flex-1 min-h-0 flex items-center justify-center text-[12px] text-fg-mute">Loading trajectory…</div>}
        >
          <LazyTrajectoryView
            api={api}
            sessionId={sid}
            workspaceHash={sessionWorkspaceHash}
            active={trajectoryOpen}
            busy={busy}
            livePartial={trajectoryPartial}
          />
        </Suspense>
      )}


      <SideChatWindow
        {...sideChatState}
        anchor={sideChatAnchor}
        onDraftChange={sideChat.setDraft}
        onSubmit={() => sideChat.submit()}
        onStop={sideChat.stop}
        onClear={sideChat.clear}
        onClose={sideChat.close}
        onFileLink={handleTranscriptFileLink}
      />
      <QueueCardList
        items={visibleQueuedItems}
        paused={queuePause}
        onResume={resumeQueue}
        onCancel={cancelQueued}
        onRetry={retryQueued}
        onGuide={guideQueued}
        onSaveEdit={saveQueuedEdit}
        guideDisabled={!busy || !activeTurnId}
        onUploadPastedText={uploadPastedTextForQueue}
        sessionId={sid}
        attachmentTextLoader={api.readAttachmentText}
      />
      {readOnlyExternalSession ? (
        <div
          role="status"
          className="min-h-11 shrink-0 border-t border-border bg-surface px-4 py-2.5 text-[12px] leading-5 text-fg-mute"
        >
          {tr('externalSession.tuiReadOnly')}
        </div>
      ) : (
        <div className="ace-composer-dock" data-question-pending={questionForView ? 'true' : undefined}>
          {questionForView ? (
            <QuestionPicker
              request={questionForView}
              onResolve={resolveQuestion}
              originLabel={questionOriginLabel}
              className="mx-2.5"
            />
          ) : (
            <>
          <InputBar
            ref={inputRef}
            mainComposer
            pathReferenceApi={api}
            currentSessionId={sid}
            cwd={sidePanelCwd}
            expertOptions={recentExperts}
            selectedExpertId={composerExpertId}
            selectedExpertName={composerExpert?.display_name || ''}
            selectedExpertType={composerExpert?.type || 'agent'}
            pendingExpertName={pendingExpert?.confirmed ? pendingExpert.expert?.display_name : ''}
            pendingExpertType={pendingExpert?.confirmed ? pendingExpert.expert?.type : 'agent'}
            expertRemoving={expertDetaching}
            onSelectExpert={selectComposerExpert}
            onRemoveExpert={detachComposerExpert}
            onOpenExpertComponents={() => setExpertPickerOpen(true)}
            busy={busy}
            stopping={abortPending}
            goal={goal}
            onGoalEdit={editGoal}
            onGoalStatusChange={changeGoalStatus}
            onGoalClear={clearGoal}
            history={composerHistory}
            historyEntries={composerHistoryEntries}
            value={composerValue}
            onChange={handleComposerChange}
            onSubmit={submit}
            onAbort={stopCurrentWork}
            {...composerInputProps}
            fileDropManagedExternally
            onFileDragActiveChange={setChatFileDropActive}
            submitting={composerSubmitting || reasoningSwitching}
            canRetryLastUserMessage={!!retryUserMessageId}
            queuePaused={!!queuePause}
            onResumeQueue={resumeQueue}
            // 提问期间输入框整体被提问框替换(方案 A):不渲染 composer,
            // 避免出现「直接输入=插话」的入口与反馈卡冲突。
            sessionControls={{
              model: currentModelLabel,
              modelOptions: displayedModelOptions,
              selectedModelName: currentModelName,
              modelLoad: currentModelLoad,
              modelSwitching: modelSwitching || reasoningSwitching,
              modelRefreshing,
              reasoningOptions: composerReasoningOptions(modelState),
              reasoningDisabled: homeSubmitting || composerSubmitting || reasoningSwitching || modelSwitching || modelRefreshing,
              onReasoningChange: changeComposerReasoning,
              onModelChange: changeComposerModel,
              onRefreshModels: refreshSessionModels,
              onOpenModelSettings,
              tokenBudget,
              permissionMode,
              permissionSwitching,
              onPermissionModeChange: changeComposerPermissionMode,
            }}
          />
          <GitSessionPill
            owner={workbenchOwner}
            key={`session-${sid}`}
            api={api}
            cwd={sidePanelCwd}
            variant="bar"
            sessionLoaded={transcriptLoadState === 'loaded'}
            sessionStarted={rawItems.length > 0}
            worktreeSession={sessionWorktree}
            busy={busy}
            onIntentChange={handleGitPillIntentChange}
          />
          </>
          )}
        </div>
      )}
      <SessionContentLoading
        phase={sessionContentLoadingPhase({
          transcriptLoadState,
          resumePending: ref?.resumePending,
          resumeFailed: ref?.resumeFailed,
          readOnly: readOnlyExternalSession,
        })}
      />
      <ChatFileDropOverlay active={chatFileDropActive} />
      </div>
      )}
      {previewPanelVisible && !previewPanelMaximized && (
        <div
          role="separator"
          aria-label="调整预览面板宽度"
          aria-orientation="vertical"
          tabIndex={0}
          className="ace-resize-handle ace-resize-handle-preview"
          onPointerDown={startPreviewPanelResize}
          onMouseDown={startPreviewPanelResize}
          onKeyDown={onPreviewPanelHandleKeyDown}
          title="拖动调整预览面板宽度"
        />
      )}
      {previewPanelVisible && (
        <div
          className="ace-preview-details-shell"
          data-maximized={previewPanelMaximized ? 'true' : 'false'}
          style={previewShellStyle}
        >
          <PreviewDetailsPanel
            key={workbenchOwner}
            owner={workbenchOwner}
            api={api}
            cwd={sidePanelCwd}
            workspaceCwd={sessionIsNoWorkspace ? '' : sidePanelCwd}
            tabs={previewTabs}
            activeTab={activePreview}
            changeGroups={changeGroups}
            changeSummary={changeSummary}
            sessionChangesReady={transcriptLoadState === 'loaded'}
            turnChangeSets={turnChangeSets}
            maximized={previewPanelMaximized}
            presenting={previewPresenting}
            onTogglePresentation={onTogglePreviewPresentation}
            busy={busy}
            selectionContexts={previewSelectionContexts}
            sidePanelListCollapsed={sidePanelListCollapsed}
            onActivateTab={activatePreview}
            onCloseTab={closePreview}
            onCloseOthers={closeOtherPreviews}
            onCloseToRight={closePreviewsToRight}
            onCloseAll={closeAllPreviews}
            onRefreshTab={refreshPreview}
            onEditFileTab={updateFilePreviewDraft}
            onReorderTab={reorderPreview}
            onToggleMaximize={onToggleSidePanelMaximized}
            onToggleSidePanelList={onToggleSidePanelList}
            onOpenFile={sidePanelCwd ? openPreviewFilePicker : null}
            onOpenBrowser={sid && hasNativeAgentBrowser() ? openBrowserPreview : null}
            onOpenSideChat={openSideQuestionComposer}
            onHide={hidePreviewPanel}
            agentBrowserActive={agentBrowserActivePageId}
            nativeSurfacesVisible={nativeSurfacesVisible}
            onAddBrowserContext={addBrowserContext}
            onSelectChangeFile={openSessionChangePreview}
            onSelectGitChangeFile={openGitChangePreview}
            onOpenFilePreview={openFilePreview}
          />
        </div>
      )}
      {previewCloseConfirm && (
        <Modal
          onClose={() => previewFileGuardRef.current.choose('cancel')}
          dismissOnBackdrop={false}
          dismissOnEscape={!previewCloseConfirm.saving}
          labelledBy="unsaved-file-dialog-title"
          width={440}
        >
          {() => (
            <div className="p-4">
              <div id="unsaved-file-dialog-title" className="text-[14px] font-semibold mb-2">有未保存的修改</div>
              <div className="text-[12.5px] text-fg-mute leading-relaxed mb-4">
                {previewCloseConfirmMessage}
              </div>
              {previewCloseConfirm.error && (
                <div className="mb-4 text-[12px] leading-relaxed text-danger" role="alert">
                  {previewCloseConfirm.error}
                </div>
              )}
              <div className="flex justify-end gap-2">
                <button
                  type="button"
                  className="px-3 py-1.5 text-[12.5px] rounded-lg border border-border hover:bg-surface-hi transition-colors disabled:opacity-50"
                  disabled={previewCloseConfirm.saving}
                  onClick={() => previewFileGuardRef.current.choose('cancel')}
                >
                  取消
                </button>
                <button
                  type="button"
                  className="px-3 py-1.5 text-[12.5px] rounded-lg border border-border hover:bg-surface-hi transition-colors disabled:opacity-50"
                  disabled={previewCloseConfirm.saving}
                  onClick={() => previewFileGuardRef.current.choose('discard')}
                >
                  不保存
                </button>
                <button
                  type="button"
                  data-ace-dialog-primary="true"
                  className="px-3 py-1.5 text-[12.5px] rounded-lg bg-accent text-white hover:opacity-90 transition-opacity disabled:opacity-50"
                  disabled={previewCloseConfirm.saving}
                  onClick={() => previewFileGuardRef.current.choose('save')}
                >
                  {previewCloseConfirm.saving ? '保存中...' : '保存'}
                </button>
              </div>
            </div>
          )}
        </Modal>
      )}
      <SelectionActionPopover
        snapshot={selectionAction}
        mode={selectionAction?.mode || 'actions'}
        onQuote={() => {
          if (!pinSelectionContext(selectionAction?.context)) {
            toast({ kind: 'err', text: '没有可引用的选中文本' });
          }
        }}
        onStartAnnotation={() => {
          setSelectionAction((prev) => (prev ? { ...prev, mode: 'annotation' } : prev));
        }}
        onSubmitAnnotation={(text) => {
          if (!pinSelectionAnnotation(selectionAction?.context, text)) {
            toast({ kind: 'err', text: '没有可批注的选中文本' });
          }
        }}
        onCancel={dismissSelectionAction}
      />
      {sidePanelMounted && (
        <>
          {!sidePanelNavigationCollapsed && (
            <div
              role="separator"
              aria-label="调整右侧栏宽度"
              aria-orientation="vertical"
              tabIndex={0}
              className="ace-resize-handle ace-resize-handle-right"
              onPointerDown={startSidePanelResize}
              onMouseDown={startSidePanelResize}
              onKeyDown={onSidePanelHandleKeyDown}
              title="拖动调整右侧栏宽度"
            />
          )}
          <div
            className="ace-side-panel-shell"
            data-collapsed={sidePanelNavigationCollapsed ? 'true' : 'false'}
            data-maximized="false"
            style={sidePanelShellStyle}
          >
            <SidePanel
              key={workbenchOwner}
              owner={workbenchOwner}
              sessionRef={ref}
              sessionId={sid}
              cwd={sidePanelCwd}
              messages={stableChangeMessages}
              changeGroups={changeGroups}
              changeSummary={changeSummary}
              fileRefreshKey={fileTreeRefreshKey}
              reviewRequest={reviewRequest}
              fileLocateRequest={fileLocateRequest}
              filesEnabled={sidePanelFilesEnabled}
              width={sidePanelWidth}
              collapsed={sidePanelListCollapsed}
              busy={busy}
              onToggleCollapse={onToggleSidePanelList}
              onOpenFilePreview={openFilePreview}
              onOpenSessionChangePreview={openSessionChangePreview}
              onOpenGitChangePreview={openGitChangePreview}
              onGitBaseChange={updateGitChangeBase}
              selectedChangeFile={selectedChangeFile}
              selectedChangeFileRevision={selectedChangeFileRevision}
              selectedGitChangeFile={selectedGitChangeFile}
            />
          </div>
        </>
      )}
      {expertPickerOpen && (
        <ExpertPickerDialog
          workspaceHash={commandWorkspaceHash}
          recentIds={recentExpertIds}
          onClose={() => {
            setExpertPickerOpen(false);
            restoreChatInputFocusSoon(true);
          }}
          onDispatch={selectComposerExpert}
          onOpeningPrompt={selectExpertOpeningPrompt}
        />
      )}
    </div>
  );
}
