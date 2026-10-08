import { useDesktopOffice } from './lib/useDesktopOffice.js';
import { isOfficeWelcomeVisible } from './lib/desktopOfficePreferences.js';
import { VirtualOfficeWelcome } from './components/VirtualOfficeWelcome.jsx';
// 顶层 App:鉴权 gate(401 → TokenPrompt)+ 主壳。
//
// 视觉对齐设计稿方向 C:顶部 41px TopBar + 270px Sidebar + 主区(单会话/4宫格/9宫格)
// 会话控制内嵌在聊天输入框。所有面板/弹框作为 overlay 渲染在主区之上。

import { useCallback, useEffect, useLayoutEffect, useMemo, useRef, useState } from 'react';
import { useTranslation } from 'react-i18next';
import { api, apiConnectionScope, ApiError, createApi } from './lib/api.js';
import { computerUseSettingsStore } from './lib/computerUseSettings.js';
import { observeComputerUsePointerTheme } from './lib/computerUsePointerTheme.js';
import { useTheme } from './theme.jsx';
import { useThemeDownloads } from './lib/useThemeDownloads.js';
import { aiThemeCreationRef, createLiveThemeCreationMonitor } from './lib/aiThemeCreation.js';
import { ThemeDownloadFailureDialog } from './components/ThemeDownloadFailureDialog.jsx';
import { setToken } from './lib/auth.js';
import { connection } from './lib/connection.js';
import { subscribeModelProfileUpdates } from './lib/modelReasoningSync.js';
import {
  installAgentBrowserPageListener,
  reconcileAgentBrowserPageStore,
} from './lib/agentBrowserPages.js';
import { loadUiLocale } from './lib/uiLocale.js';
import { installNativeFileDropRouter } from './lib/macNativeFileDrag.js';
import {
  createDesktopNotificationMonitor,
  notificationEventKey,
} from './lib/desktopNotificationMonitor.js';
import {
  maybeNotify,
  maybeNotifyChannelRequest,
  buildChannelRequestPayload,
  noteHostWindowFocus,
  isHostWindowFocused,
  notificationBodyFromEvent,
  shouldNotifySessionCompletion,
} from './lib/desktopNotify.js';
import { createNewSessionForActiveWorkspace } from './lib/newSession.js';
import { refreshWorkspaceGitInfo } from './lib/gitInfoCache.js';
import {
  goBack,
  goForward,
  navigationHistoryFromHash,
  normalizeHistory,
  pushNavigation,
  sameNavigationRef,
  stripNavigationHistoryHash,
} from './lib/navigationHistory.js';
import {
  addPendingQuestionRequest,
  clearResolvedQuestionRequests,
  closePendingQuestionRequest,
  pendingQuestionSessionIds,
  questionOriginLabel,
  visibleQuestionRequest,
} from './lib/pendingQuestions.js';
import { sessionDisplayTitle } from './lib/sessionTitle.js';
import {
  normalizeSessionListChangedDetail,
  notifySessionListChanged,
  SESSION_LIST_CHANGED_EVENT,
} from './lib/sessionListEvents.js';
import { normalizeRemoteControlSessionSelected } from './lib/remoteControlSessionNavigation.js';
import { channelStateBindingSignature } from './lib/channelsSettings.js';
import { beginSessionOpen } from './lib/sessionOpenDiagnostics.js';
import { usePreference, mergeNextValue, readWithFallback } from './lib/usePreference.js';
import { sessionWorkbench } from './lib/sessionWorkbench.js';
import { useWorkbenchState } from './lib/useWorkbenchState.js';
import {
  appearanceBootstrapPreferences,
  createAppearancePersistenceController,
  initialAppearancePreferences,
} from './lib/appearancePreferences.js';
import {
  DEFAULT_RECENT_EXPERT_IDS,
  RECENT_EXPERTS_STORAGE_KEY,
  recordRecentExpert,
  validateRecentExpertIds,
} from './lib/recentExperts.js';
import {
  DEFAULT_UI_PREFS,
  effectiveFontSize,
  effectiveMessageAutoCollapse,
  effectiveSidebarSessionTime,
  effectiveSidePanelListCollapsed,
  revealRightPanelDetails,
  rightPanelHidden,
  toggleRightPanel,
  UI_PREFS_STORAGE_KEY,
  validateUiPrefs,
} from './lib/uiPrefs.js';
import { useAppShortcuts } from './lib/useAppShortcuts.js';
import { appShortcutContextAllows } from './lib/appShortcuts.js';
import { TopBar } from './components/TopBar.jsx';
import { FeedbackForm } from './components/FeedbackForm.jsx';
import { Sidebar } from './components/Sidebar.jsx';
import { ChatView } from './components/ChatView.jsx';
import { SearchPalette } from './components/SearchPalette.jsx';
import { PathPickerHost } from './components/PathPickerHost.jsx';
import { SessionNavigationMask } from './components/SessionNavigationMask.jsx';
import { SessionContentLoading } from './components/SessionContentLoading.jsx';
import { TokenPrompt } from './components/TokenPrompt.jsx';
import { SettingsPage } from './components/SettingsPage.jsx';
import { WorkspaceCleanupNotice } from './components/WorkspaceCleanupNotice.jsx';
import { DesktopContextMenu } from './components/DesktopContextMenu.jsx';
import { usePreviewPresentation } from './lib/usePreviewPresentation.js';
import { Toaster, toast } from './components/Toast.jsx';
import { SlashCommandsProvider } from './components/SlashCommandsContext.jsx';
import { FramelessResizeHandles } from './components/FramelessResizeHandles.jsx';
import { GlobalFindOverlay } from './components/GlobalFindOverlay.jsx';
import { canOpenConversationFind } from './lib/globalFind.js';
import { ConsoleDock } from './components/ConsoleDock.jsx';
import { DesktopGuidedTour } from './components/DesktopGuidedTour.jsx';
import { DesktopCloseDialog } from './components/DesktopCloseDialog.jsx';
import { ConfigRecoveryDialog } from './components/ConfigRecoveryDialog.jsx';
import { UpdateDialog } from './components/UpdateDialog.jsx';
import InteractiveHomeLogo from './components/InteractiveHomeLogo.jsx';
import { LoopPage } from './components/LoopPage.jsx';
import { scheduledTaskCreationRef } from './lib/scheduledTaskCreation.js';
import { ExpertComponentsPage } from './components/ExpertComponentsPage.jsx';
import {
  CONSOLE_DOCK_DEFAULT_HEIGHT,
  clampDockHeight,
  consoleCwdForContext,
} from './lib/consoleDock.js';
import { createHomeComposerDraftStore } from './lib/homeComposerDraftStore.js';
import { nextHomeLogoEffectEnabled } from './lib/homeLogoEffectPolicy.js';
import { homeRefFromWorkspace, noHomeWorkspaceOption } from './lib/homeWorkspaceSelection.js';
import {
  DEFAULT_SINGLE_LAYOUT,
  normalizeSingleLayoutPreference,
  normalizePreviewPanelWidth,
  normalizeSidePanelWidth,
  normalizeSidebarWidth,
  normalizeSubagentPanelWidth,
  previewPanelWidthIsUserSized,
  validateLayoutWidths,
} from './lib/singleLayout.js';
import { initInactiveSelection } from './lib/inactiveSelection.js';
import { runAfterFileApproval } from './lib/unsavedFileGuard.js';
import {
  desktopOpenSessionUrl,
  openSessionTargetFromSearch,
  sessionJumpId,
  sessionJumpMessageOrdinal,
  sessionJumpMessagePosition,
  sessionJumpNoWorkspace,
  sessionJumpReadOnly,
  sessionJumpWorkspaceHash,
  sessionJumpWorkspaceVisible,
  sessionRefFromJumpTarget,
  resumeSessionFromTarget,
  stripOpenSessionParams,
} from './lib/sessionJump.js';
import { threadSessionTargetFromClickEvent } from './lib/fileLink.js';
import { desktopUiMode } from './lib/desktopShellMode.js';
import {
  initialDesktopStartupProgress,
  reportDesktopStartupMilestone,
  subscribeDesktopStartupProgress,
} from './lib/desktopStartupProgress.js';
import { installDesktopExternalLinkRouter } from './lib/externalUrl.js';
import { shouldAutoFocusDesktopComposer } from './lib/composerCaretRestore.js';
import { requestDesktopAppExit, showDesktopAboutDialog } from './lib/desktopAppActions.js';
import {
  normalizeConfigRecoveryNotice,
  recoveryNoticeBlocksStartup,
} from './lib/configRecoveryNotice.js';
import {
  DESKTOP_CLOSE_BEHAVIORS,
  getDesktopCloseBehavior,
  hideDesktopToTray,
  performDesktopCloseChoice,
  setDesktopCloseBehavior,
  subscribeDesktopCloseRequest,
} from './lib/desktopCloseBehavior.js';
import {
  desktopUpdateRestartAvailable,
  requestDesktopUpdateRestart,
  updateJobProgress,
  updateJobIsActive,
} from './lib/updateJob.js';
import {
  clearResolvedPermissionRequests,
  closePermissionRequest,
  hasUnresolvedPermission,
  markPermissionSubmitting,
  pendingPermissionSessionIds,
  permissionOriginLabel,
  pushPermissionRequest,
  visiblePermissionRequests,
} from './lib/permissionRequestQueue.js';
import {
  DESKTOP_GUIDED_TOUR_TARGET_MAX_ATTEMPTS,
  DESKTOP_GUIDED_TOUR_TARGET_RETRY_MS,
  desktopGuidedTourHasModel,
  desktopGuidedTourModeEligible,
  desktopGuidedTourTargetProbeAction,
  desktopGuidedTourTargetsReady,
  shouldAutoStartDesktopGuidedTour,
  shouldPrepareDesktopGuidedTour,
} from './lib/desktopGuidedTour.js';

const SINGLE_LAYOUT_STORAGE_KEY = 'acecode.singleLayoutWidths.v1';

// 导航遮罩的最后兜底。要比 api.js 的 DEFAULT_REQUEST_TIMEOUT_MS 长 —— 正常
// 情况下应该是请求先超时、上层收尾;这个计时器只负责接住「上层压根没收尾」
// 的漏网路径,不该抢在请求超时之前触发。
const SESSION_NAVIGATION_MASK_TIMEOUT_MS = 45000;
// 控制台停靠区偏好(add-console-dock):开关 + 高度跨刷新持久化。
const CONSOLE_DOCK_STORAGE_KEY = 'acecode.consoleDock.v1';
const DEFAULT_CONSOLE_DOCK = { open: false, height: CONSOLE_DOCK_DEFAULT_HEIGHT };
function expertManagerCreationDraft() {
  return '/expert-manager 帮我创建一个 XXX 专家，擅长 XXXXX。我的经验是：[请补充你的行业背景、相关经验]。';
}
function validateConsoleDock(value) {
  return !!value && typeof value === 'object'
    && typeof value.open === 'boolean'
    && Number.isFinite(value.height);
}

function parseDesktopBridgeResult(raw) {
  if (!raw) return null;
  if (typeof raw === 'string') return JSON.parse(raw);
  return raw;
}

export function App() {
  // Static product copy is marked at build time. Subscribing the shell to the
  // i18n instance refreshes mounted descendants immediately, while compiled
  // module-scope metadata resolves translations lazily on the rerender.
  const { t } = useTranslation();
  const {
    theme,
    colorTheme,
    set: setTheme,
    setColorTheme,
    prepareTheme,
    forgetTheme,
    themeMode,
  } = useTheme();
  const initialAppearance = useMemo(() => initialAppearancePreferences(), []);
  const bootstrapAppearance = useMemo(() => appearanceBootstrapPreferences(), []);
  useEffect(() => installNativeFileDropRouter(), []);

  const [authState, setAuthState] = useState('checking'); // 'checking' | 'ok' | 'need-token'
  const computerUseScope = apiConnectionScope(api);
  useEffect(() => {
    if (authState !== 'ok') return undefined;
    return observeComputerUsePointerTheme(computerUseSettingsStore(api));
  }, [authState, computerUseScope]);
  const [health,    setHealth]    = useState(null);
  const [desktopStartupProgress, setDesktopStartupProgress] = useState(
    () => initialDesktopStartupProgress(),
  );

  const [activeRef,    setActiveRef]    = useState(null);
  const office = useDesktopOffice(activeRef);
  const [officeWelcomeOpen, setOfficeWelcomeOpen] = useState(false);
  const officeWelcomeAttempted = useRef(false);
  const workbenchOwner = sessionWorkbench.ownerFor(activeRef);
  const [sessionTitleTarget, setSessionTitleTarget] = useState(null);
  const [sessionActionsTarget, setSessionActionsTarget] = useState(null);
  const [sidebarSessionLoadState, setSidebarSessionLoadState] = useState(null);
  const [sidebarSessionLoadResetSequence, setSidebarSessionLoadResetSequence] = useState(0);
  const [homeLogoEffectEnabled, setHomeLogoEffectEnabled] = useState(true);
  const [homeComposerDrafts, setHomeComposerDrafts] = useState({});
  const [homeDraftStore] = useState(() => createHomeComposerDraftStore({ onChange: setHomeComposerDrafts }));
  const [homeComposerAttentionRequest, setHomeComposerAttentionRequest] = useState(0);
  const [navHistory, setNavHistory] = useState(() => (
    (typeof window !== 'undefined' && navigationHistoryFromHash(window.location.hash))
    || { back: [], forward: [] }
  ));
  const [commandWorkspaceHash, setCommandWorkspaceHash] = useState('');
  const [consoleCwd, setConsoleCwd] = useState('');
  const [showSettings, setShowSettings] = useState(false);
  const [showFeedback, setShowFeedback] = useState(false);
  const [settingsNavKey, setSettingsNavKey] = useState('general');
  const [settingsNavRequest, setSettingsNavRequest] = useState(0);
  // 从全局搜索面板(Ctrl+K)跳进设置时携带的搜索种子:{query, resultId, section, nonce}。
  // 普通打开设置一律清空,否则上一次的搜索会在下次打开时重新出现。
  const [settingsSearchSeed, setSettingsSearchSeed] = useState(null);
  const [desktopCloseDialogOpen, setDesktopCloseDialogOpen] = useState(false);
  const [rememberDesktopCloseChoice, setRememberDesktopCloseChoice] = useState(false);
  const [desktopCloseBusy, setDesktopCloseBusy] = useState(false);
  const [desktopTrayAvailable, setDesktopTrayAvailable] = useState(true);
  const [configRecoveryNotice, setConfigRecoveryNotice] = useState(null);
  const [configRecoveryNoticeChecked, setConfigRecoveryNoticeChecked] = useState(false);
  const [configRecoveryDialogOpen, setConfigRecoveryDialogOpen] = useState(false);
  const [configRecoveryAckBusy, setConfigRecoveryAckBusy] = useState(false);
  const [modelProfileRevision, setModelProfileRevision] = useState(0);
  const [permReqs,     setPermReqs]     = useState([]);
  const [questionReqs, setQuestionReqs] = useState([]);
  // 当前主会话的后台任务(spawn_subagent 子会话)索引,由 ChatView 上报。
  // 用于:1) 子任务的 question_request 在主会话可见;2) 权限/问题卡片的
  // 「来自后台任务」来源标记。
  const [subagentIndex, setSubagentIndex] = useState({ parentId: '', titles: {} });
  // 已经发现过的 child -> parent/title 关系跨会话切换保留,供后台权限请求
  // 在父会话不活跃时仍能把侧边栏提示和 inline 请求路由到父会话。
  const [subagentDirectory, setSubagentDirectory] = useState({ owners: {}, titles: {} });
  const [searchOpen,   setSearchOpen]   = useState(false);
  const [conversationFindRequest, setConversationFindRequest] = useState(0);
  const [workspaceActivationRequest, setWorkspaceActivationRequest] = useState(null);
  const [updateStatus, setUpdateStatus] = useState(null);
  const [updateStarting, setUpdateStarting] = useState(false);
  const [updateCancelling, setUpdateCancelling] = useState(false);
  const [updateChecking, setUpdateChecking] = useState(false);
  const [updateRestarting, setUpdateRestarting] = useState(false);
  const [updateJob, setUpdateJob] = useState(null);
  const [updateDialogOpen, setUpdateDialogOpen] = useState(false);
  const [singleLayout, setSingleLayout] = usePreference(
    SINGLE_LAYOUT_STORAGE_KEY, DEFAULT_SINGLE_LAYOUT, validateLayoutWidths);
  const previewPanelUserSized = previewPanelWidthIsUserSized(singleLayout);
  const initialUiPrefs = useMemo(() => ({
    ...DEFAULT_UI_PREFS,
    fontSize: initialAppearance.fontSize,
    sidebarSessionTime: initialAppearance.sidebarSessionTime,
    messageAutoCollapse: initialAppearance.messageAutoCollapse,
  }), [initialAppearance]);
  const [globalUiPrefs, setGlobalUiPrefs] = usePreference(
    UI_PREFS_STORAGE_KEY, initialUiPrefs, validateUiPrefs);
  const [panelPrefs, setPanelPrefs] = useWorkbenchState(workbenchOwner, 'panels', () => ({
    sidePanelCollapsed: true, sidePanelListCollapsed: false, sidePanelMaximized: false,
  }));
  const storedUiPrefs = useMemo(() => ({ ...globalUiPrefs, ...panelPrefs }), [globalUiPrefs, panelPrefs]);
  const setStoredUiPrefs = useCallback((updater) => {
    const next = mergeNextValue(storedUiPrefs, updater);
    const panelKeys = ['sidePanelCollapsed', 'sidePanelListCollapsed', 'sidePanelMaximized'];
    setPanelPrefs((previous) => panelKeys.some((key) => previous[key] !== next[key])
      ? Object.fromEntries(panelKeys.map((key) => [key, next[key]])) : previous);
    const changes = Object.fromEntries(Object.entries(next).filter(([key, value]) => (
      !panelKeys.includes(key) && value !== storedUiPrefs[key]
    )));
    if (Object.keys(changes).length) setGlobalUiPrefs(changes);
  }, [storedUiPrefs, setPanelPrefs, setGlobalUiPrefs]);
  const [previewPanelVisible, setPreviewPanelVisible] = useWorkbenchState(workbenchOwner, '$previewVisible', false);
  const { uiPrefs, setUiPrefs, active: previewPresenting, toggle: togglePreviewPresentation } = usePreviewPresentation(
    workbenchOwner, storedUiPrefs, setStoredUiPrefs, previewPanelVisible,
  );
  useLayoutEffect(() => {
    if (bootstrapAppearance) {
      setUiPrefs({ messageAutoCollapse: bootstrapAppearance.messageAutoCollapse });
    }
  }, [bootstrapAppearance, setUiPrefs]);
  const [consoleDock, setConsoleDock] = useWorkbenchState(workbenchOwner, 'consoleDock', () => ({
    ...readWithFallback(CONSOLE_DOCK_STORAGE_KEY, DEFAULT_CONSOLE_DOCK, validateConsoleDock), open: false,
  }));
  const [recentExpertIds, setRecentExpertIds] = usePreference(
    RECENT_EXPERTS_STORAGE_KEY,
    DEFAULT_RECENT_EXPERT_IDS,
    validateRecentExpertIds,
  );
  const rememberRecentExpert = useCallback((expertOrId) => {
    const id = typeof expertOrId === 'string' ? expertOrId : expertOrId?.id;
    if (!id) return;
    setRecentExpertIds((current) => recordRecentExpert(current, id));
  }, [setRecentExpertIds]);
  const loadHomeComposerDraft = useCallback((workspaceHash, client = api) => ({
    draft: homeDraftStore.read(client, workspaceHash),
    ready: homeDraftStore.load(client, workspaceHash),
  }), [homeDraftStore]);
  const updateHomeComposerDraft = useCallback((workspaceHash, text, client = api) => {
    homeDraftStore.update(client, workspaceHash, text);
  }, [homeDraftStore]);
  const acceptHomeComposerDraft = useCallback((workspaceHash, submittedText, client = api) => {
    void homeDraftStore.accept(client, workspaceHash, submittedText);
  }, [homeDraftStore]);
  // 首页粘贴的文件块上传完成时用户已离开首页:在 store 里的最新草稿上回填,
  // 不能用 ChatView 手里的旧快照覆盖期间的编辑。
  const patchHomeComposerDraft = useCallback((workspaceHash, updater, client = api) => {
    homeDraftStore.patch(client, workspaceHash, updater);
  }, [homeDraftStore]);
  useEffect(() => {
    const flush = () => { void homeDraftStore.flush(); };
    const onVisibility = () => { if (document.visibilityState === 'hidden') flush(); };
    window.addEventListener('pagehide', flush);
    window.addEventListener('beforeunload', flush);
    document.addEventListener('visibilitychange', onVisibility);
    return () => {
      window.removeEventListener('pagehide', flush);
      window.removeEventListener('beforeunload', flush);
      document.removeEventListener('visibilitychange', onVisibility);
      flush();
    };
  }, [homeDraftStore]);
  useEffect(() => { void homeDraftStore.flush(); }, [activeRef, homeDraftStore]);
  // grid4/grid9 入口暂时隐藏:主界面固定单会话,避免旧 localStorage 把用户卡在未完善视图。
  const view = 'single';
  const fontSize = effectiveFontSize(uiPrefs);
  const sidebarSessionTime = effectiveSidebarSessionTime(uiPrefs);
  const messageAutoCollapse = effectiveMessageAutoCollapse(uiPrefs);
  const applyAppearance = useCallback((next) => {
    setTheme(next.theme);
    setColorTheme(next.colorTheme);
    setUiPrefs({
      fontSize: next.fontSize,
      sidebarSessionTime: next.sidebarSessionTime,
      messageAutoCollapse: next.messageAutoCollapse,
    });
  }, [setColorTheme, setTheme, setUiPrefs]);
  // The persistence controller outlives individual renders. Keep its callback
  // current so a later appearance choice is compared against the latest UI
  // preferences rather than the values from the controller's first render.
  const applyAppearanceRef = useRef(applyAppearance);
  applyAppearanceRef.current = applyAppearance;
  const appearanceControllerRef = useRef(null);
  if (!appearanceControllerRef.current) {
    appearanceControllerRef.current = createAppearancePersistenceController({
      initial: {
        theme: bootstrapAppearance?.theme || themeMode,
        colorTheme,
        fontSize,
        sidebarSessionTime,
        messageAutoCollapse: bootstrapAppearance?.messageAutoCollapse ?? messageAutoCollapse,
      },
      apply: (next) => applyAppearanceRef.current(next),
      save: (payload) => api.setUiPreferences(payload),
      onError: (error) => {
        toast({
          kind: 'err',
          text: '外观设置保存失败，已恢复上次配置：' + (error?.message || '未知错误'),
        });
      },
    });
  }
  const changeAppearance = useCallback((patch, options) => (
    appearanceControllerRef.current.change(patch, options)
  ), []);
  const themeDownloads = useThemeDownloads({
    enabled: authState === 'ok' && showSettings,
    prepare: prepareTheme,
    apply: (id, options) => changeAppearance({ colorTheme: id }, options),
    remove: (id) => appearanceControllerRef.current.removeColorTheme(id, () => api.deleteTheme(id)),
    forget: forgetTheme,
  });
  // sidePanelCollapsed 是列表 + 详情的总开关;listCollapsed 只隐藏最右导航列表。
  const sidePanelCollapsed = uiPrefs.sidePanelCollapsed;
  const sidePanelListCollapsed = effectiveSidePanelListCollapsed(uiPrefs);
  const sidePanelNavigationCollapsed = sidePanelCollapsed || sidePanelListCollapsed;
  const sidePanelMaximized = !!uiPrefs.sidePanelMaximized;
  const projectSidebarCollapsed = !!uiPrefs.sidebarCollapsed;
  const showAceCodeAvatar = false;
  const singleShellRef = useRef(null);
  const sidebarResizeActiveRef = useRef(false);
  const activeRefRef = useRef(activeRef);
  const themeCreationMonitor = useMemo(() => createLiveThemeCreationMonitor({
    onStart: () => themeDownloads.controller.beginCreation(),
    onCreated: (created, intent) => { void themeDownloads.controller.created(created, intent); },
  }), [themeDownloads.controller]);
  useLayoutEffect(() => {
    themeCreationMonitor.setSession(activeRef?.sessionId || activeRef?.id || '');
  }, [activeRef?.sessionId, activeRef?.id, themeCreationMonitor]);
  useEffect(() => {
    const message = (event) => themeCreationMonitor.accept(event.detail);
    connection.addEventListener('message', message);
    return () => {
      connection.removeEventListener('message', message);
    };
  }, [themeCreationMonitor]);
  // Agent Browser 页面归属登记表在 App 级镜像 native 状态事件:不管用户正在看
  // 哪个会话,后台会话的 browser_open 都会被记下,切回去时页签就在;挂载时再向
  // Desktop 对账一次,把刷新 / 切工作区之前就存在的页面找回来。
  useEffect(() => {
    const dispose = installAgentBrowserPageListener();
    void reconcileAgentBrowserPageStore();
    return dispose;
  }, []);
  const healthRef = useRef(health);
  const subagentIndexRef = useRef(subagentIndex);
  const subagentDirectoryRef = useRef(subagentDirectory);
  const notificationContextRef = useRef({
    assistantText: new Map(),
    titles: new Map(),
    workspaces: new Map(),
  });
  const notificationMonitorRef = useRef(null);
  if (!notificationMonitorRef.current) {
    notificationMonitorRef.current = createDesktopNotificationMonitor({
      retainSession: (sessionId) => connection.retainSession(sessionId),
      releaseSession: (sessionId) => connection.releaseSession(sessionId),
    });
  }
  const navHistoryRef = useRef(navHistory);
  const updatePollRef = useRef(0);
  const desktopModeRef = useRef(desktopUiMode());
  const startupOpenTargetRef = useRef(
    typeof window === 'undefined' ? null : openSessionTargetFromSearch(window.location.search),
  );
  const startupNavigationStartedRef = useRef(false);
  const pendingSessionNavigationIdsRef = useRef(new Set());
  const nextSessionNavigationIdRef = useRef(0);
  const sessionNavigationTimersRef = useRef(new Map());
  const [sessionNavigationPending, setSessionNavigationPending] = useState(
    () => !!startupOpenTargetRef.current,
  );
  const [startupNavigationSettled, setStartupNavigationSettled] = useState(
    () => !startupOpenTargetRef.current,
  );
  const [guidedTourState, setGuidedTourState] = useState({
    loaded: false,
    dismissed: true,
    hasModel: true,
  });
  const [guidedTourPreparing, setGuidedTourPreparing] = useState(false);
  const [guidedTourRun, setGuidedTourRun] = useState(false);
  const [guidedTourForced, setGuidedTourForced] = useState(false);
  const guidedTourAutoAttemptedRef = useRef(false);
  const homeLogoActiveSessionId = sessionJumpId(activeRef || {});
  const guidedTourHasActiveSession = !!(activeRef?.sessionId || activeRef?.id);
  const configRecoveryBlocking = (
    authState === 'ok' && !configRecoveryNoticeChecked
  ) || recoveryNoticeBlocksStartup(configRecoveryNotice, configRecoveryDialogOpen);
  const officeInvitationBlocked = showSettings || showFeedback || searchOpen || updateDialogOpen
    || desktopCloseDialogOpen
    || configRecoveryBlocking
    || questionReqs.length > 0;
  // A tour that started while the native welcome claim was pending finishes first.
  // Blocking it with the queued invitation would make both surfaces wait forever.
  const officeWelcomeVisible = isOfficeWelcomeVisible({ requested: officeWelcomeOpen,
    blocked: officeInvitationBlocked, tourPreparing: guidedTourPreparing, tourRunning: guidedTourRun });
  const guidedTourBlocked = officeInvitationBlocked || officeWelcomeVisible;

  useEffect(() => {
    if (officeWelcomeAttempted.current || !office.welcomePending || authState !== 'ok' || !health
        || officeInvitationBlocked || guidedTourPreparing || guidedTourRun) return;
    officeWelcomeAttempted.current = true;
    void office.claimWelcome().then(result => {
      if (result.ok && result.show) setOfficeWelcomeOpen(true);
    });
  }, [office.welcomePending, office.claimWelcome, authState, health, officeInvitationBlocked, guidedTourPreparing, guidedTourRun]);
  useEffect(() => { if (office.error) toast({ kind: 'err', text: office.error }); }, [office.error]);

  useEffect(() => initInactiveSelection(), []);
  useEffect(() => {
    const handleProgress = (snapshot) => {
      setDesktopStartupProgress(snapshot);
    };
    const unsubscribe = subscribeDesktopStartupProgress(handleProgress);
    return unsubscribe;
  }, []);
  useEffect(() => installDesktopExternalLinkRouter({
    onError: (error) => {
      toast({
        kind: 'err',
        text: t('desktop.externalBrowserOpenFailed', {
          error: error || t('common.unknown'),
        }),
      });
    },
  }), [t]);
  useEffect(() => subscribeDesktopCloseRequest(() => {
    setRememberDesktopCloseChoice(false);
    setDesktopCloseBusy(false);
    setDesktopCloseDialogOpen(true);
    getDesktopCloseBehavior().then((state) => {
      setDesktopTrayAvailable(state?.trayAvailable !== false);
    });
  }), []);
  useEffect(() => {
    document.documentElement.setAttribute('data-font-size', fontSize);
  }, [fontSize]);
  useEffect(() => {
    setHomeLogoEffectEnabled((current) => (
      nextHomeLogoEffectEnabled(current, homeLogoActiveSessionId)
    ));
  }, [homeLogoActiveSessionId]);
  useEffect(() => { activeRefRef.current = activeRef; }, [activeRef]);
  // Track host-window attention for suppress_when_focused. WebView2 can
  // briefly report document.hasFocus()=false when native chrome steals focus;
  // window focus/blur keeps a sticky attentive flag as fallback.
  useEffect(() => {
    const onFocus = () => noteHostWindowFocus(true);
    const onBlur = () => noteHostWindowFocus(false);
    noteHostWindowFocus(
      typeof document !== 'undefined'
        && typeof document.hasFocus === 'function'
        && document.hasFocus(),
    );
    window.addEventListener('focus', onFocus);
    window.addEventListener('blur', onBlur);
    return () => {
      window.removeEventListener('focus', onFocus);
      window.removeEventListener('blur', onBlur);
    };
  }, []);
  useEffect(() => {
    const expertId = activeRef?.expertId || activeRef?.expert_id || activeRef?.expert?.id || '';
    if (expertId) rememberRecentExpert(expertId);
  }, [
    activeRef?.expert?.id,
    activeRef?.expertId,
    activeRef?.expert_id,
    rememberRecentExpert,
  ]);
  useEffect(() => { healthRef.current = health; }, [health]);
  useEffect(() => { subagentIndexRef.current = subagentIndex; }, [subagentIndex]);
  useEffect(() => { subagentDirectoryRef.current = subagentDirectory; }, [subagentDirectory]);
  useEffect(() => { navHistoryRef.current = navHistory; }, [navHistory]);

  const resetSidebarSessionLoading = useCallback(() => {
    setSidebarSessionLoadState(null);
    setSidebarSessionLoadResetSequence((sequence) => sequence + 1);
  }, []);

  const previewLeaveGuardRef = useRef(null);
  const registerPreviewLeaveGuard = useCallback((guard) => {
    previewLeaveGuardRef.current = guard;
    return () => {
      if (previewLeaveGuardRef.current === guard) previewLeaveGuardRef.current = null;
    };
  }, []);
  const requestPreviewLeave = useCallback((nextRef) => {
    if (nextRef && sameNavigationRef(activeRefRef.current, nextRef)) return true;
    return previewLeaveGuardRef.current?.() ?? true;
  }, []);

  const replaceActiveRef = useCallback((nextRefOrUpdater) => {
    const current = activeRefRef.current;
    const next = typeof nextRefOrUpdater === 'function'
      ? nextRefOrUpdater(current)
      : nextRefOrUpdater;
    return runAfterFileApproval(requestPreviewLeave(next), () => {
      resetSidebarSessionLoading();
      activeRefRef.current = next;
      setActiveRef(next);
      return true;
    });
  }, [requestPreviewLeave, resetSidebarSessionLoading]);

  const navigateToRef = useCallback((nextRefOrUpdater, options = {}) => {
    const current = activeRefRef.current;
    const next = typeof nextRefOrUpdater === 'function'
      ? nextRefOrUpdater(current)
      : nextRefOrUpdater;
    return runAfterFileApproval(requestPreviewLeave(next), () => {
      if (!options.preserveSidebarSessionLoading) resetSidebarSessionLoading();
      const nextHistory = pushNavigation(navHistoryRef.current, current, next);
      navHistoryRef.current = nextHistory;
      activeRefRef.current = next;
      setNavHistory(nextHistory);
      setActiveRef(next);
      return true;
    });
  }, [requestPreviewLeave, resetSidebarSessionLoading]);

  const syncActiveRemoteControlBound = useCallback((detail = {}) => {
    const sessionId = String(detail.sessionId || detail.session_id || '').trim();
    if (!sessionId) return;
    const current = activeRefRef.current;
    if (sessionJumpId(current) !== sessionId) return;
    const workspaceHash = String(detail.workspaceHash || detail.workspace_hash || '').trim();
    const currentWorkspaceHash = sessionJumpWorkspaceHash(current);
    if (workspaceHash && currentWorkspaceHash && workspaceHash !== currentWorkspaceHash) return;
    const remoteControlBound = detail.remoteControlBound === true;
    const currentBound = Boolean(current?.remote_control_bound ?? current?.remoteControlBound);
    if (currentBound === remoteControlBound) return;
    const next = {
      ...current,
      remote_control_bound: remoteControlBound,
      remoteControlBound,
    };
    activeRefRef.current = next;
    setActiveRef(next);
  }, []);

  useEffect(() => {
    const handler = (event) => {
      const detail = normalizeSessionListChangedDetail(event.detail || {});
      if (detail.reason !== 'remote-control-bound'
          && detail.reason !== 'remote-control-unbound') return;
      syncActiveRemoteControlBound({
        ...detail,
        remoteControlBound: detail.reason === 'remote-control-bound',
      });
    };
    window.addEventListener(SESSION_LIST_CHANGED_EVENT, handler);
    return () => window.removeEventListener(SESSION_LIST_CHANGED_EVENT, handler);
  }, [syncActiveRemoteControlBound]);

  const replaceNavigationState = useCallback((nextRef, nextHistory) => {
    return runAfterFileApproval(requestPreviewLeave(nextRef), () => {
      resetSidebarSessionLoading();
      const normalized = normalizeHistory(nextHistory);
      navHistoryRef.current = normalized;
      activeRefRef.current = nextRef;
      setNavHistory(normalized);
      setActiveRef(nextRef);
      return true;
    });
  }, [requestPreviewLeave, resetSidebarSessionLoading]);

  const finishSessionNavigation = useCallback((navigationId) => {
    const timer = sessionNavigationTimersRef.current.get(navigationId);
    if (timer !== undefined) {
      clearTimeout(timer);
      sessionNavigationTimersRef.current.delete(navigationId);
    }
    pendingSessionNavigationIdsRef.current.delete(navigationId);
    if (pendingSessionNavigationIdsRef.current.size === 0) {
      setSessionNavigationPending(false);
    }
  }, []);

  // SessionNavigationMask 是全屏的、吞掉所有指针与键盘事件的遮罩。它只在
  // finishSessionNavigation 里关闭,所以任何一条不 settle 的 resume 都会
  // 让界面永久点不动(用户只能从托盘杀进程)。api.js 现在给 fetch 加了
  // 超时,这里再兜一层:无论上层因为什么原因没能收尾,遮罩都必须自己散。
  const beginSessionNavigation = useCallback(() => {
    const navigationId = ++nextSessionNavigationIdRef.current;
    pendingSessionNavigationIdsRef.current.add(navigationId);
    setSessionNavigationPending(true);
    const timer = setTimeout(() => {
      if (!pendingSessionNavigationIdsRef.current.has(navigationId)) return;
      toast({ kind: 'err', text: '打开会话超时,请重试' });
      finishSessionNavigation(navigationId);
    }, SESSION_NAVIGATION_MASK_TIMEOUT_MS);
    sessionNavigationTimersRef.current.set(navigationId, timer);
    return navigationId;
  }, [finishSessionNavigation]);

  // 用户主动取消(遮罩上按 Esc):清掉全部在途导航,立刻还回操作权。
  const cancelSessionNavigation = useCallback(() => {
    const pending = Array.from(pendingSessionNavigationIdsRef.current);
    pending.forEach((id) => finishSessionNavigation(id));
  }, [finishSessionNavigation]);

  useEffect(() => {
    const timers = sessionNavigationTimersRef.current;
    return () => {
      timers.forEach((timer) => clearTimeout(timer));
      timers.clear();
    };
  }, []);

  const resumeAndOpenSession = useCallback(async (target, options = {}) => {
    const sessionId = sessionJumpId(target);
    if (!sessionId) return false;
    beginSessionOpen(sessionId);
    if (!await requestPreviewLeave(target)) return false;
    resetSidebarSessionLoading();
    const navigationId = beginSessionNavigation();
    const navigationIsPending = () =>
      pendingSessionNavigationIdsRef.current.has(navigationId);
    let handedOffToPageLoad = false;
    try {
      const noWorkspace = sessionJumpNoWorkspace(target);
      const readOnly = sessionJumpReadOnly(target);
      const targetHash = sessionJumpWorkspaceHash(target);
      const shouldResume = !readOnly && (options.forceResume || target?.active !== true);
      const suppliedHistory = options.navigationHistory
        ? normalizeHistory(options.navigationHistory)
        : null;
      const commitRef = suppliedHistory
        ? (nextRef) => replaceNavigationState(nextRef, suppliedHistory)
        : (options.replace ? replaceActiveRef : navigateToRef);
      const resumeWith = (client, workspaceHash) => resumeSessionFromTarget(client, sessionId, {
        noWorkspace, workspaceHash, shouldResume,
      });

      if (!noWorkspace
          && targetHash
          && sessionJumpWorkspaceVisible(target)
          && options.allowDesktopActivate !== false
          && targetHash !== activeRefRef.current?.workspaceHash
          && typeof window !== 'undefined'
          && typeof window.aceDesktop_activateWorkspace === 'function') {
        try {
          const r = parseDesktopBridgeResult(await window.aceDesktop_activateWorkspace(targetHash));
          if (!navigationIsPending()) return false;
          if (r && !r.error && r.port && r.token) {
            let resumed = {};
            try {
              resumed = await resumeWith(createApi({ port: r.port, token: r.token }), targetHash);
            } catch (e) {
              if (navigationIsPending()) {
                toast({ kind: 'err', text: '恢复失败:' + (e.message || '') });
              }
              return false;
            }
            if (!navigationIsPending()) return false;
            const nextRef = sessionRefFromJumpTarget(target, resumed, {
              workspaceHash: targetHash,
            });
            const redirectHistory = suppliedHistory || (
              options.replace
                ? normalizeHistory(navHistoryRef.current)
                : pushNavigation(navHistoryRef.current, activeRefRef.current, nextRef)
            );
            const url = desktopOpenSessionUrl({
              port: r.port,
              token: r.token,
              sessionId,
              workspaceHash: nextRef.workspaceHash,
              noWorkspace: nextRef.noWorkspace,
              readOnly,
              messageOrdinal: sessionJumpMessageOrdinal(target),
              messagePosition: sessionJumpMessagePosition(target),
              navigationHistory: redirectHistory,
              protocol: window.location?.protocol || 'http:',
            });
            if (url) {
              window.location.href = url;
              handedOffToPageLoad = true;
              return true;
            }
            commitRef(nextRef);
            return true;
          }
        } catch {
          if (!navigationIsPending()) return false;
          // Bridge 不可用或返回格式异常时，降级到当前 daemon 的 workspace-scoped resume。
        }
      }

      try {
        const resumed = await resumeWith(api, targetHash);
        if (!navigationIsPending()) return false;
        commitRef(sessionRefFromJumpTarget(target, resumed, {
          workspaceHash: targetHash,
          noWorkspace,
        }));
        return true;
      } catch (e) {
        if (navigationIsPending()) {
          toast({ kind: 'err', text: '恢复失败:' + (e.message || '') });
        }
        return false;
      }
    } finally {
      if (!handedOffToPageLoad) finishSessionNavigation(navigationId);
    }
  }, [
    beginSessionNavigation,
    finishSessionNavigation,
    navigateToRef,
    replaceActiveRef,
    replaceNavigationState,
    requestPreviewLeave,
    resetSidebarSessionLoading,
  ]);

  const openHistoryDestination = useCallback((result) => {
    if (!result?.activeRef) return Promise.resolve(false);
    if (!sessionJumpId(result.activeRef)) {
      return Promise.resolve(replaceNavigationState(result.activeRef, result.history));
    }
    return resumeAndOpenSession(result.activeRef, {
      forceResume: true,
      navigationHistory: result.history,
      replace: true,
    });
  }, [replaceNavigationState, resumeAndOpenSession]);

  const goBackActiveRef = useCallback(() => (
    openHistoryDestination(goBack(navHistoryRef.current, activeRefRef.current))
  ), [openHistoryDestination]);

  const goForwardActiveRef = useCallback(() => (
    openHistoryDestination(goForward(navHistoryRef.current, activeRefRef.current))
  ), [openHistoryDestination]);

  const openSettingsSection = useCallback((key = 'general') => {
    setSettingsNavKey(key || 'general');
    setSettingsNavRequest((request) => request + 1);
    setSettingsSearchSeed(null);
    setShowSettings(true);
  }, []);

  // 搜索面板选中一条设置:关面板 → 定位到该设置所在分区 → 让设置窗口以同一个
  // 查询重跑搜索并选中同一条结果(滚动 + 波浪下划线由 SettingsPage 自己完成)。
  const handleSelectSetting = useCallback((result, paletteQuery = '') => {
    if (!result) return;
    setSearchOpen(false);
    setSettingsNavKey(result.section || 'general');
    setSettingsSearchSeed({
      query: String(paletteQuery || result.label || ''),
      resultId: result.id || '',
      section: result.section || '',
      label: result.label || '',
      nonce: Date.now(),
    });
    setShowSettings(true);
  }, []);

  useEffect(() => {
    setSingleLayout((prev) => normalizeSingleLayoutPreference(prev));
  }, [setSingleLayout]);

  const probe = useCallback(async () => {
    reportDesktopStartupMilestone('daemon_connecting');
    try {
      const h = await api.health();
      setHealth(h);
      setAuthState('ok');
      reportDesktopStartupMilestone('daemon_connected');
    } catch (e) {
      if (e instanceof ApiError && e.status === 401) {
        reportDesktopStartupMilestone('daemon_connected');
        setAuthState('need-token');
      } else {
        reportDesktopStartupMilestone('daemon_connection_failed');
        toast({ kind: 'err', text: '连接 daemon 失败:' + e.message });
        setAuthState('need-token');
      }
    }
  }, []);

  useEffect(() => { probe(); }, [probe]);

  useEffect(() => {
    if (authState !== 'ok' || desktopModeRef.current !== 'shell') return undefined;
    let firstFrame = 0;
    let secondFrame = 0;
    firstFrame = window.requestAnimationFrame(() => {
      secondFrame = window.requestAnimationFrame(() => {
        reportDesktopStartupMilestone('ui_ready');
      });
    });
    return () => {
      if (firstFrame) window.cancelAnimationFrame(firstFrame);
      if (secondFrame) window.cancelAnimationFrame(secondFrame);
    };
  }, [authState]);

  useEffect(() => {
    if (authState !== 'ok') return;
    void prepareTheme(colorTheme).catch(() => {});
  }, [authState, colorTheme, prepareTheme]);

  useEffect(() => {
    if (authState !== 'ok') return;
    // Desktop has a pre-mount native injection for a flash-free first frame.
    // Browser mode has no such bootstrap, so confirm the daemon preference as
    // soon as authentication succeeds; localStorage is only a paint cache.
    loadUiLocale(api).catch(() => {
      // Older/offline daemons keep the injected or cached locale usable.
    });
    api.getUiPreferences().then((preferences) => {
      appearanceControllerRef.current.restore(preferences);
    }).catch(() => {
      // Older/offline daemons keep the injected or cached appearance usable.
    });
  }, [authState]);

  useEffect(() => {
    if (authState !== 'ok') {
      setConfigRecoveryNoticeChecked(false);
      return undefined;
    }
    let cancelled = false;
    setConfigRecoveryNoticeChecked(false);
    api.getConfigRecoveryNotice()
      .then((value) => {
        if (cancelled) return;
        const notice = normalizeConfigRecoveryNotice(value);
        if (notice) {
          setConfigRecoveryNotice(notice);
          setConfigRecoveryDialogOpen(true);
        }
        setConfigRecoveryNoticeChecked(true);
      })
      .catch(() => {
        // Notice transport failure must not add a second startup warning.
        // A durable pending notice is retried on the next app entry.
        if (!cancelled) setConfigRecoveryNoticeChecked(true);
      });
    return () => {
      cancelled = true;
    };
  }, [authState]);

  const acknowledgeConfigRecovery = useCallback(async () => {
    if (configRecoveryAckBusy) return;
    setConfigRecoveryAckBusy(true);
    try {
      await api.acknowledgeConfigRecoveryNotice();
    } catch {
      // Close this page's already-seen warning without a toast. The durable
      // notice remains pending and will be offered again on the next entry.
    } finally {
      setConfigRecoveryAckBusy(false);
      setConfigRecoveryDialogOpen(false);
      setConfigRecoveryNotice(null);
    }
  }, [configRecoveryAckBusy]);

  const pollUpdateJob = useCallback((jobId) => {
    if (!jobId) return;
    if (updatePollRef.current) window.clearTimeout(updatePollRef.current);
    let failures = 0;
    const tick = async () => {
      try {
        const job = await api.getUpdateJob(jobId);
        failures = 0;
        setUpdateJob(job);
        if (updateJobIsActive(job)) {
          updatePollRef.current = window.setTimeout(tick, 350);
          return;
        }
        updatePollRef.current = 0;
        setUpdateDialogOpen(true);
        if (job?.state === 'succeeded') {
          toast({
            kind: 'ok',
            text: desktopUpdateRestartAvailable()
              ? '升级安装完成，可立即重启 ACECode'
              : '升级安装完成，请完全退出并重新启动 ACECode',
          });
        } else if (job?.state === 'failed') {
          toast({ kind: 'err', text: '升级失败:' + (job.error || '未知错误') });
        } else if (job?.state === 'cancelled') {
          toast({ kind: 'info', text: '升级已取消，当前安装未修改' });
        }
      } catch (e) {
        failures += 1;
        if (failures < 5) {
          updatePollRef.current = window.setTimeout(tick, 600);
          return;
        }
        updatePollRef.current = 0;
        setUpdateJob((prev) => ({
          ...(prev || { job_id: jobId }),
          state: 'failed',
          error: e?.message || '无法获取升级进度',
        }));
        setUpdateDialogOpen(true);
      }
    };
    tick();
  }, []);

  useEffect(() => () => {
    if (updatePollRef.current) window.clearTimeout(updatePollRef.current);
  }, []);

  useEffect(() => {
    if (authState !== 'ok') return undefined;
    let cancelled = false;
    Promise.allSettled([api.getUpdateStatus(), api.getLatestUpdateJob()])
      .then(([statusResult, jobResult]) => {
        if (cancelled) return;
        setUpdateStatus(statusResult.status === 'fulfilled' ? statusResult.value : null);
        if (jobResult.status !== 'fulfilled') return;
        const job = jobResult.value;
        setUpdateJob(job);
        if (updateJobIsActive(job)) {
          setUpdateDialogOpen(true);
          pollUpdateJob(job.job_id);
        }
      });
    return () => {
      cancelled = true;
    };
  }, [authState, pollUpdateJob]);

  useEffect(() => {
    if (authState !== 'ok' || !desktopGuidedTourModeEligible(desktopModeRef.current)) {
      return undefined;
    }
    let cancelled = false;
    Promise.allSettled([api.getDesktopOnboarding(), api.listModels()])
      .then(([statusResult, modelsResult]) => {
        if (cancelled || statusResult.status !== 'fulfilled') return;
        setGuidedTourState({
          loaded: true,
          dismissed: !!statusResult.value?.dismissed,
          hasModel: modelsResult.status === 'fulfilled'
            ? desktopGuidedTourHasModel(modelsResult.value)
            : true,
        });
      });
    return () => {
      cancelled = true;
    };
  }, [authState]);

  // 解析 URL 上的 ?open=<sessionId>&workspace=<hash>(跨 workspace 跳转后落地用)。
  // 解析后立即从 URL 抹掉,避免刷新二次触发。
  useEffect(() => {
    if (typeof window === 'undefined' || authState !== 'ok') return;
    const target = startupOpenTargetRef.current;
    if (!target || startupNavigationStartedRef.current) return;
    startupNavigationStartedRef.current = true;
    const qs = stripOpenSessionParams(window.location.search);
    const hash = stripNavigationHistoryHash(window.location.hash);
    const newUrl = window.location.pathname
      + (qs ? '?' + qs : '')
      + (hash ? '#' + hash : '');
    window.history.replaceState(null, '', newUrl);
    resumeAndOpenSession(target, { replace: true, allowDesktopActivate: false })
      .catch(() => {})
      .finally(() => setStartupNavigationSettled(true));
  }, [authState, resumeAndOpenSession]);

  // 桌面壳 native 通知 click_handler 走 webview eval 调这两个 window 全局函数。
  // 同 workspace:focusSessionFromBridge → resume + setActiveRef
  // 跨 workspace:activateAndOpenSession → 激活 workspace 后 resume + 整页 navigate
  useEffect(() => {
    if (typeof window === 'undefined') return;
    window.aceDesktop_focusSessionFromBridge = function focusSessionFromBridge(sessionId, workspaceHash, readOnly = false) {
      if (!sessionId) return;
      const hasWorkspaceArg = workspaceHash !== undefined;
      const targetHash = hasWorkspaceArg ? String(workspaceHash || '') : (activeRefRef.current?.workspaceHash || '');
      resumeAndOpenSession({
        sessionId: String(sessionId),
        workspaceHash: targetHash,
        noWorkspace: hasWorkspaceArg && !targetHash,
        readOnly: readOnly === true,
        externalSurface: readOnly === true ? 'tui' : '',
      }, { allowDesktopActivate: false }).catch(() => {});
    };
    window.aceDesktop_activateAndOpenSession = async (workspaceHash, sessionId, readOnly = false) => {
      if (!sessionId) return;
      const targetHash = String(workspaceHash || '');
      await resumeAndOpenSession({
        sessionId: String(sessionId),
        workspaceHash: targetHash,
        noWorkspace: !targetHash,
        readOnly: readOnly === true,
        externalSurface: readOnly === true ? 'tui' : '',
      });
    };
    return () => {
      try {
        delete window.aceDesktop_focusSessionFromBridge;
        delete window.aceDesktop_activateAndOpenSession;
      } catch {
        // strict mode 下 delete window prop 偶发抛错;static assignment 兜底
        window.aceDesktop_focusSessionFromBridge = undefined;
        window.aceDesktop_activateAndOpenSession = undefined;
      }
    };
  }, [resumeAndOpenSession]);

  useEffect(() => {
    if (authState !== 'ok') return undefined;
    return subscribeModelProfileUpdates(connection, () => {
      setModelProfileRevision((value) => value + 1);
    });
  }, [authState]);

  // Successful remote-control selections are authoritative on the daemon.
  // The frontend follows as a best-effort hint and never feeds failures back
  // into the already committed channel binding.
  useEffect(() => {
    if (authState !== 'ok') return undefined;
    const handler = (event) => {
      const selected = normalizeRemoteControlSessionSelected(event.detail || {});
      if (!selected) return;
      notifySessionListChanged({
        reason: 'remote-control-session-selected',
        sessionId: selected.sessionId,
        workspaceHash: selected.workspaceHash,
        noWorkspace: selected.noWorkspace,
        session: selected.session,
      });
      resumeAndOpenSession(selected, { allowDesktopActivate: false }).catch(() => {});
    };
    connection.addEventListener('message', handler);
    return () => connection.removeEventListener('message', handler);
  }, [authState, resumeAndOpenSession]);

  // 消息通道(QQ / Telegram):新的配对请求在窗口失焦时弹系统通知,否则页面内提示;
  // 某个平台绑定的会话集合变化时让侧栏立即刷新,电脑图标随之移动。
  useEffect(() => {
    if (authState !== 'ok') return undefined;
    const bindingSignatures = new Map();
    const handler = (event) => {
      const message = event.detail || {};
      if (message.type === 'channels_request') {
        const request = message.payload || {};
        if (!maybeNotifyChannelRequest(request, { cfg: healthRef.current?.notifications })) {
          const payload = buildChannelRequestPayload(request);
          toast({ kind: 'info', text: `${payload.title}:${payload.body}`, duration: 6000 });
        }
        return;
      }
      if (message.type !== 'channels_state') return;
      const signature = channelStateBindingSignature(message.payload);
      if (signature === null) return;
      const platform = message.payload.platform;
      if (bindingSignatures.get(platform) === signature) return;
      bindingSignatures.set(platform, signature);
      notifySessionListChanged({ reason: 'channel-binding-changed', noWorkspace: true });
    };
    connection.addEventListener('message', handler);
    return () => connection.removeEventListener('message', handler);
  }, [authState]);

  // Ctrl+` toggle 控制台(终端聚焦时 xterm 的 customKeyEventHandler 放行该
  // 组合,事件照常冒泡到 window)。后端 console 不可用时快捷键惰化。
  const consoleAvailable = !!health?.console?.available;
  const toggleConsoleDock = useCallback(() => {
    if (!consoleAvailable) return;
    setConsoleDock((prev) => ({ ...prev, open: !prev.open }));
  }, [consoleAvailable, setConsoleDock]);
  const setConsoleDockHeight = useCallback((next) => {
    setConsoleDock((prev) => ({
      ...prev,
      height: clampDockHeight(next, window.innerHeight),
    }));
  }, [setConsoleDock]);
  const setConsoleDockOpen = useCallback((open) => {
    setConsoleDock((prev) => ({ ...prev, open: !!open }));
  }, [setConsoleDock]);

  const handleSelectSession = useCallback(async (session) => {
    if (!session?.id) return;
    setSearchOpen(false);
    await resumeAndOpenSession(session);
  }, [resumeAndOpenSession]);

  useEffect(() => {
    if (typeof window === 'undefined') return undefined;
    const onClick = (event) => {
      const target = threadSessionTargetFromClickEvent(event);
      if (!target) return;
      event.preventDefault();
      event.stopPropagation();
      resumeAndOpenSession(target).catch((error) => {
        toast({ kind: 'err', text: '打开会话失败:' + (error?.message || '') });
      });
    };
    window.addEventListener('click', onClick, true);
    return () => window.removeEventListener('click', onClick, true);
  }, [resumeAndOpenSession]);
  const openConversationFind = useCallback(() => {
    setConversationFindRequest((request) => request + 1);
  }, []);
  const handleSelectWorkspace = useCallback((workspace) => {
    const workspaceHash = workspace?.hash || workspace?.workspaceHash || '';
    if (!workspaceHash) return;
    setSearchOpen(false);
    setWorkspaceActivationRequest((previous) => ({
      requestId: (previous?.requestId || 0) + 1,
      workspaceHash,
    }));
  }, []);

  useEffect(() => {
    const monitor = notificationMonitorRef.current;
    const context = notificationContextRef.current;

    const rememberCurrentTitle = (sessionId) => {
      const current = activeRefRef.current || {};
      const currentId = current.sessionId || current.id || '';
      if (!sessionId || currentId !== sessionId) return;
      const title = sessionDisplayTitle(current);
      if (title) context.titles.set(sessionId, title);
    };

    const rememberSubagentOwner = (sessionId, parentSessionId, title = '') => {
      if (!sessionId || !parentSessionId || sessionId === parentSessionId) return;
      const previous = subagentDirectoryRef.current || { owners: {}, titles: {} };
      if (previous.owners?.[sessionId] === parentSessionId
          && (!title || previous.titles?.[sessionId] === title)) {
        return;
      }
      const next = {
        owners: {
          ...(previous.owners || {}),
          [sessionId]: parentSessionId,
        },
        titles: {
          ...(previous.titles || {}),
          ...(title ? { [sessionId]: title } : {}),
        },
      };
      // Update the ref synchronously so an immediately following permission or
      // question event cannot race React's state commit.
      subagentDirectoryRef.current = next;
      setSubagentDirectory(next);
    };

    const conversationOwnerForSession = (sessionId, eventPayload = {}) => {
      if (!sessionId) return '';
      const explicitParentId = String(eventPayload.parent_session_id || '').trim();
      if (explicitParentId) return explicitParentId;
      const directory = subagentDirectoryRef.current || {};
      if (directory.owners?.[sessionId]) return directory.owners[sessionId];
      const currentIndex = subagentIndexRef.current || {};
      if (currentIndex.parentId && currentIndex.titles?.[sessionId]) {
        return currentIndex.parentId;
      }
      return sessionId;
    };

    const sendDesktopNotification = (type, msg, payload) => {
      const sessionId = payload.session_id || msg.session_id || '';
      if (!sessionId) return;
      const key = notificationEventKey(type, sessionId, msg, payload);
      if (!monitor.markSeen(key)) return;
      rememberCurrentTitle(sessionId);
      const workspaceHash = payload.workspace_hash
        || msg.workspace_hash
        || context.workspaces.get(sessionId)
        || '';
      if (workspaceHash) context.workspaces.set(sessionId, workspaceHash);
      const current = activeRefRef.current || {};
      const currentId = current.sessionId || current.id || '';
      const sessionTitle = context.titles.get(sessionId)
        || subagentIndexRef.current?.titles?.[sessionId]
        || (currentId === sessionId ? sessionDisplayTitle(current) : '');
      maybeNotify({
        type,
        sessionId,
        workspaceHash,
        sessionTitle,
        bodyText: notificationBodyFromEvent(type, payload),
        hasFocus: isHostWindowFocused(),
        cfg: healthRef.current?.notifications,
      });
    };

    const finishMonitoredSession = (sessionId, msg, payload) => {
      const outcome = String(payload.outcome || '');
      const assistantText = context.assistantText.get(sessionId) || '';
      const ownerSessionId = conversationOwnerForSession(sessionId, payload);
      const completionNotificationAllowed = shouldNotifySessionCompletion({
        sessionId,
        parentSessionId: payload.parent_session_id || msg.parent_session_id || '',
        ownerSessionId,
      });
      if (completionNotificationAllowed
          && (!outcome || outcome === 'completed')
          && assistantText.trim()) {
        sendDesktopNotification('completion', msg, {
          ...payload,
          session_id: sessionId,
          final_assistant_text: assistantText,
        });
      }
      context.assistantText.delete(sessionId);
      monitor.release(sessionId);
    };

    const handler = (e) => {
      const msg = e.detail || {};
      const payload = { ...(msg.payload || {}) };
      if (msg.session_id && !payload.session_id) payload.session_id = msg.session_id;
      if ((msg.type === 'question_request'
          || msg.type === 'question_closed'
          || msg.type === 'permission_request'
          || msg.type === 'permission_closed')
          && !payload.session_id) {
        const current = activeRefRef.current || {};
        payload.session_id = current.sessionId || current.id || '';
      }
      const sessionId = payload.session_id || msg.session_id || '';
      const parentSessionId = payload.parent_session_id || msg.parent_session_id || '';
      if (sessionId && parentSessionId) {
        rememberSubagentOwner(sessionId, parentSessionId, payload.title || '');
      }
      const workspaceHash = payload.workspace_hash || msg.workspace_hash || '';
      if (sessionId && workspaceHash) {
        context.workspaces.set(sessionId, workspaceHash);
      }
      if (sessionId) rememberCurrentTitle(sessionId);

      if (msg.type === 'session_updated' && sessionId && payload.title) {
        context.titles.set(sessionId, String(payload.title));
      }
      if (msg.type === 'message' && sessionId && payload.role === 'assistant') {
        const text = String(payload.content || '');
        if (text.trim()) context.assistantText.set(sessionId, text);
      }
      if (msg.type === 'message' && sessionId && payload.role === 'error') {
        context.assistantText.delete(sessionId);
      }
      if (msg.type === 'token' && sessionId && payload.text) {
        const previous = context.assistantText.get(sessionId) || '';
        context.assistantText.set(
          sessionId,
          (previous + String(payload.text)).slice(-4096),
        );
      }
      if (msg.type === 'busy_changed' && sessionId) {
        if (payload.busy === true) {
          context.assistantText.delete(sessionId);
          monitor.retain(sessionId);
        } else {
          if (payload.outcome) {
            setPermReqs((prev) => clearResolvedPermissionRequests(prev, sessionId));
            setQuestionReqs((prev) => clearResolvedQuestionRequests(prev, sessionId));
          }
          finishMonitoredSession(sessionId, msg, payload);
        }
      }
      if (msg.type === 'session_status_snapshot') {
        const sessions = Array.isArray(payload.sessions) ? payload.sessions : [];
        for (const status of sessions) {
          const statusSessionId = status?.session_id || '';
          if (!statusSessionId) continue;
          if (status.parent_session_id) {
            rememberSubagentOwner(
              statusSessionId,
              status.parent_session_id,
              status.title || '',
            );
          }
          if (status.workspace_hash) {
            context.workspaces.set(statusSessionId, status.workspace_hash);
          }
          if (status?.busy === true) monitor.retain(statusSessionId);
        }
      }
      if (msg.type === 'session_status' && sessionId) {
        if (payload.busy === true) monitor.retain(sessionId);
      }
      if (msg.type === 'permission_request') {
        if (sessionId) monitor.retain(sessionId);
        const ownerSessionId = conversationOwnerForSession(sessionId, payload);
        setPermReqs((prev) => pushPermissionRequest(prev, payload, {
          ownerSessionId: ownerSessionId !== sessionId ? ownerSessionId : '',
        }));
      }
      if (msg.type === 'permission_closed') {
        const ownerSessionId = conversationOwnerForSession(sessionId, payload);
        setPermReqs((prev) => closePermissionRequest(prev, payload, {
          ownerSessionId: ownerSessionId !== sessionId ? ownerSessionId : '',
        }));
      }
      if (msg.type === 'question_request') {
        if (sessionId) monitor.retain(sessionId);
        const ownerSessionId = conversationOwnerForSession(sessionId, payload);
        setQuestionReqs((prev) => addPendingQuestionRequest(prev, payload, {
          ownerSessionId: ownerSessionId !== sessionId ? ownerSessionId : '',
        }));
      }
      if (msg.type === 'question_closed') {
        const ownerSessionId = conversationOwnerForSession(sessionId, payload);
        setQuestionReqs((prev) => closePendingQuestionRequest(prev, payload, {
          ownerSessionId: ownerSessionId !== sessionId ? ownerSessionId : '',
        }));
      }
      if (msg.type === 'done' && sessionId) {
        setPermReqs((prev) => clearResolvedPermissionRequests(prev, sessionId));
        setQuestionReqs((prev) => clearResolvedQuestionRequests(prev, sessionId));
        finishMonitoredSession(sessionId, msg, payload);
      }
      const permissionTimeoutDiagnostic = msg.type === 'error'
        && payload.reason === 'permission_timeout'
        && !!payload.request_id;
      if ((msg.type === 'error' || msg.type === 'turn_aborted')
          && sessionId
          && !permissionTimeoutDiagnostic) {
        setPermReqs((prev) => clearResolvedPermissionRequests(prev, sessionId));
        setQuestionReqs((prev) => clearResolvedQuestionRequests(prev, sessionId));
        context.assistantText.delete(sessionId);
        monitor.release(sessionId);
      }
    };
    connection.addEventListener('message', handler);
    return () => {
      connection.removeEventListener('message', handler);
      monitor.dispose();
    };
  }, []);

  const onSubmitToken = useCallback(async (token) => {
    setToken(token);
    await probe();
  }, [probe]);

  const toggleSidePanel = useCallback(() => {
    const approval = rightPanelHidden(uiPrefs, previewPanelVisible) ? true : requestPreviewLeave();
    return runAfterFileApproval(approval, () => {
      setUiPrefs((prev) => toggleRightPanel(prev, previewPanelVisible));
    });
  }, [previewPanelVisible, requestPreviewLeave, setUiPrefs, uiPrefs]);

  const revealPreviewPanel = useCallback(() => {
    setUiPrefs(revealRightPanelDetails);
  }, [setUiPrefs]);

  const toggleSidePanelList = useCallback(() => {
    setUiPrefs((prev) => ({
      ...prev,
      sidePanelListCollapsed: !effectiveSidePanelListCollapsed(prev),
    }));
  }, [setUiPrefs]);

  const revealSidePanelList = useCallback(() => {
    setUiPrefs((prev) => {
      if (!prev.sidePanelCollapsed && !effectiveSidePanelListCollapsed(prev)) return prev;
      return {
        ...prev,
        sidePanelCollapsed: false,
        sidePanelListCollapsed: false,
      };
    });
  }, [setUiPrefs]);

  // 最大化 / 还原中间预览面板。沿用旧字段名保存偏好,但 UI 控件已迁到预览面板。
  // 最大化时强制确保右侧 SidePanel 未折叠,符合"右侧文件栏仍然可用"的行为。
  const toggleSidePanelMaximized = useCallback(() => {
    setUiPrefs((prev) => {
      const nextMax = !prev.sidePanelMaximized;
      return {
        ...prev,
        sidePanelMaximized: nextMax,
        sidePanelCollapsed: nextMax ? false : prev.sidePanelCollapsed,
      };
    });
  }, [setUiPrefs]);

  const toggleProjectSidebar = useCallback(() => {
    setUiPrefs((prev) => ({
      ...prev,
      sidebarCollapsed: !prev.sidebarCollapsed,
    }));
  }, [setUiPrefs]);

  const openUpdateDialog = useCallback(() => {
    if (!updateJobIsActive(updateJob)
        && updateJob?.target_version
        && updateStatus?.latest_version
        && updateJob.target_version !== updateStatus.latest_version) {
      setUpdateJob(null);
    }
    setUpdateDialogOpen(true);
  }, [updateJob, updateStatus]);

  const checkForUpdates = useCallback(async () => {
    if (updateChecking) return;
    setUpdateChecking(true);
    try {
      const status = await api.getUpdateStatus();
      if (!['available', 'up_to_date', 'store_managed'].includes(status?.status)) {
        throw new Error(
          status?.error
          || (status?.status ? `更新服务返回状态 ${status.status}` : '更新服务返回无效响应'),
        );
      }
      setUpdateStatus(status);
      if (status.status === 'store_managed') {
        setUpdateJob(null);
        setUpdateDialogOpen(true);
        return;
      }
      const keepCurrentJob = updateJobIsActive(updateJob)
        || (updateJob?.state === 'succeeded' && updateJob?.restart_required);
      if (!keepCurrentJob) {
        const staleTarget = updateJob?.target_version
          && status?.latest_version
          && updateJob.target_version !== status.latest_version;
        if (status.status === 'up_to_date' || staleTarget) {
          setUpdateJob(null);
        }
      }
      setUpdateDialogOpen(true);
    } catch (e) {
      toast({ kind: 'err', text: '检查更新失败:' + (e?.message || '未知错误') });
    } finally {
      setUpdateChecking(false);
    }
  }, [updateChecking, updateJob]);

  const showAboutAceCode = useCallback(async () => {
    const result = await showDesktopAboutDialog();
    if (!result?.ok) openSettingsSection('about');
  }, [openSettingsSection]);

  const exitAceCode = useCallback(async () => {
    const result = await requestDesktopAppExit();
    if (result?.ok) return;
    toast({
      kind: 'err',
      text: result?.unavailable
        ? '当前环境不支持退出 ACECode'
        : '退出 ACECode 失败:' + (result?.error || '未知错误'),
    });
  }, []);

  const chooseDesktopCloseAction = useCallback(async (behavior) => {
    if (desktopCloseBusy) return;
    setDesktopCloseBusy(true);
    const result = await performDesktopCloseChoice({
      behavior,
      remember: rememberDesktopCloseChoice,
      persist: setDesktopCloseBehavior,
      hideToTray: hideDesktopToTray,
      exitApp: requestDesktopAppExit,
    });
    if (!result?.ok) {
      toast({
        kind: 'err',
        text: result?.stage === 'persist'
          ? '保存关闭窗口设置失败:' + (result?.error || '未知错误')
          : '关闭窗口操作失败:' + (result?.error || '未知错误'),
      });
      setDesktopCloseBusy(false);
      return;
    }
    setDesktopCloseDialogOpen(false);
    setRememberDesktopCloseChoice(false);
    setDesktopCloseBusy(false);
  }, [desktopCloseBusy, rememberDesktopCloseChoice]);

  const startUpdate = useCallback(async () => {
    if (!updateStatus?.update_available || updateStarting || updateJobIsActive(updateJob)) return;
    setUpdateStarting(true);
    try {
      const job = await api.startUpdate();
      setUpdateJob(job);
      setUpdateDialogOpen(true);
      pollUpdateJob(job?.job_id);
    } catch (e) {
      if (e?.code === 'UPDATE_IN_PROGRESS' && e?.body?.job) {
        const job = e.body.job;
        setUpdateJob(job);
        setUpdateDialogOpen(true);
        pollUpdateJob(job.job_id);
        return;
      }
      toast({ kind: 'err', text: '启动升级失败:' + (e?.message || '') });
    } finally {
      setUpdateStarting(false);
    }
  }, [pollUpdateJob, updateJob, updateStarting, updateStatus]);

  const cancelUpdate = useCallback(async () => {
    if (!updateJob?.job_id
        || !updateJobIsActive(updateJob)
        || updateJob?.can_cancel === false
        || updateJob?.cancel_requested
        || updateCancelling) return;
    setUpdateCancelling(true);
    try {
      const job = await api.cancelUpdate(updateJob.job_id);
      setUpdateJob(job);
      pollUpdateJob(job?.job_id || updateJob.job_id);
    } catch (e) {
      if (e?.code === 'UPDATE_NOT_CANCELLABLE' && e?.body?.job) {
        setUpdateJob(e.body.job);
        toast({ kind: 'info', text: '升级已进入安装阶段，不能再取消' });
        return;
      }
      toast({ kind: 'err', text: '取消升级失败:' + (e?.message || '未知错误') });
    } finally {
      setUpdateCancelling(false);
    }
  }, [pollUpdateJob, updateCancelling, updateJob]);

  const restartAfterUpdate = useCallback(async () => {
    if (updateRestarting
        || updateJob?.state !== 'succeeded'
        || !updateJob?.restart_required) return;
    setUpdateRestarting(true);
    try {
      await requestDesktopUpdateRestart();
    } catch (e) {
      setUpdateRestarting(false);
      toast({ kind: 'err', text: '自动重启失败:' + (e?.message || '未知错误') });
    }
  }, [updateJob, updateRestarting]);

  const openHomeForWorkspace = useCallback((workspace = null, { composerFeedback = false } = {}) => {
    const current = activeRefRef.current || {};
    if (composerFeedback && view === 'single' && !sessionJumpId(current)
        && !current?.loop && !current?.expertComponents) {
      setHomeComposerAttentionRequest((request) => request + 1);
    }
    const target = workspace == null ? noHomeWorkspaceOption() : workspace;
    const next = homeRefFromWorkspace(target, activeRefRef.current, health);
    void refreshWorkspaceGitInfo(createApi(next), next).catch(() => {});
    navigateToRef(next);
  }, [health, navigateToRef, view]);

  const openLoopPage = useCallback(() => {
    navigateToRef({ loop: true });
  }, [navigateToRef]);

  const openExpertComponents = useCallback(() => {
    const current = activeRefRef.current || {};
    const base = homeRefFromWorkspace(current, current, health);
    navigateToRef({
      ...base,
      home: false,
      expertComponents: true,
    });
  }, [health, navigateToRef]);

  const startConversationalExpertCreation = useCallback(() => {
    const current = activeRefRef.current || {};
    const base = homeRefFromWorkspace(current, current, health);
    void refreshWorkspaceGitInfo(createApi(base), base).catch(() => {});
    navigateToRef({
      ...base,
      initialDraftText: expertManagerCreationDraft(),
    });
    return true;
  }, [health, navigateToRef]);

  const startAiThemeCreation = useCallback(() => {
    const next = aiThemeCreationRef(activeRefRef.current || {}, health);
    setShowSettings(false);
    navigateToRef(next);
  }, [health, navigateToRef]);

  const startScheduledTaskCreation = useCallback(() => {
    const next = scheduledTaskCreationRef(activeRefRef.current || {}, health);
    void refreshWorkspaceGitInfo(createApi(next), next).catch(() => {});
    navigateToRef(next);
  }, [health, navigateToRef]);

  const dispatchExpertToNewTask = useCallback((expert, prompt = expert?.quick_prompts?.[0] || '') => {
    const expertId = String(expert?.id || '');
    if (!expertId) return false;
    const current = activeRefRef.current || {};
    const base = homeRefFromWorkspace(current, current, health);
    void refreshWorkspaceGitInfo(createApi(base), base).catch(() => {});
    navigateToRef({
      ...base,
      expertId,
      expert_id: expertId,
      expert,
      ...(prompt ? { initialDraftText: String(prompt) } : {}),
    });
    rememberRecentExpert(expert);
    return true;
  }, [health, navigateToRef, rememberRecentExpert]);

  const consumeInitialDraftText = useCallback(() => {
    replaceActiveRef((current) => {
      if (!current || !Object.prototype.hasOwnProperty.call(current, 'initialDraftText')) {
        return current;
      }
      const { initialDraftText: _consumed, ...next } = current;
      return next;
    });
  }, [replaceActiveRef]);

  const replaceActiveSessionExpert = useCallback((sessionId, expert) => {
    const expertId = String(expert?.id || '');
    replaceActiveRef((current) => {
      const currentSessionId = sessionJumpId(current);
      if (sessionId ? currentSessionId !== sessionId : !!currentSessionId) {
        return current;
      }
      return {
        ...current,
        expertId,
        expert_id: expertId,
        expert: expertId ? expert : null,
      };
    });
  }, [replaceActiveRef]);

  const replaceHomeWorkspace = useCallback((workspace) => {
    replaceActiveRef((current) => {
      const next = homeRefFromWorkspace(workspace, current, health);
      if (['ai-theme', 'scheduled-task'].includes(current?.composerDraftScope)) {
        next.composerDraftScope = current.composerDraftScope;
      }
      void refreshWorkspaceGitInfo(createApi(next), next).catch(() => {});
      return next;
    });
  }, [health, replaceActiveRef]);

  const abortGuidedTour = useCallback(() => {
    setGuidedTourRun(false);
    setGuidedTourPreparing(false);
    setGuidedTourForced(false);
  }, []);

  const dismissGuidedTour = useCallback(async ({ openModels = false } = {}) => {
    setGuidedTourRun(false);
    setGuidedTourPreparing(false);
    setGuidedTourForced(false);
    try {
      const state = await api.dismissDesktopOnboarding();
      setGuidedTourState((prev) => ({
        ...prev,
        loaded: true,
        dismissed: !!state?.dismissed,
      }));
    } catch (e) {
      toast({ kind: 'err', text: '指引已关闭，但状态保存失败，下次启动可能再次显示：' + (e?.message || '') });
    }
    if (openModels) openSettingsSection('models');
  }, [openSettingsSection]);

  const replayGuidedTour = useCallback(async () => {
    setShowSettings(false);
    setSearchOpen(false);
    setGuidedTourRun(false);
    setGuidedTourForced(true);
    navigateToRef(homeRefFromWorkspace(activeRefRef.current || {}, activeRefRef.current, health));
    try {
      const models = await api.listModels();
      setGuidedTourState((prev) => ({
        ...prev,
        hasModel: desktopGuidedTourHasModel(models),
      }));
    } catch {
      // 重播仍可继续；模型列表读取失败时沿用最近一次已知状态。
    } finally {
      setGuidedTourPreparing(true);
    }
  }, [health, navigateToRef]);

  useEffect(() => {
    if (guidedTourForced) return;
    const shouldStart = shouldAutoStartDesktopGuidedTour({
      mode: desktopModeRef.current,
      authState,
      stateLoaded: guidedTourState.loaded,
      dismissed: guidedTourState.dismissed,
      startupNavigationSettled,
      hasActiveSession: guidedTourHasActiveSession,
      blocked: guidedTourBlocked,
      attempted: guidedTourAutoAttemptedRef.current,
    });
    if (!shouldStart) return;
    guidedTourAutoAttemptedRef.current = true;
    setGuidedTourPreparing(true);
  }, [
    authState,
    guidedTourBlocked,
    guidedTourForced,
    guidedTourHasActiveSession,
    guidedTourState.dismissed,
    guidedTourState.loaded,
    startupNavigationSettled,
  ]);

  useEffect(() => {
    if (!guidedTourPreparing || !shouldPrepareDesktopGuidedTour({
      mode: desktopModeRef.current,
      authState,
      startupNavigationSettled,
      hasActiveSession: guidedTourHasActiveSession,
      blocked: guidedTourBlocked,
    })) {
      return undefined;
    }
    let timer = null;
    let attempt = 0;
    const probeTargets = () => {
      const action = desktopGuidedTourTargetProbeAction({
        targetsReady: desktopGuidedTourTargetsReady(),
        attempt,
        maxAttempts: DESKTOP_GUIDED_TOUR_TARGET_MAX_ATTEMPTS,
      });
      if (action === 'start') {
        setGuidedTourRun(true);
        setGuidedTourPreparing(false);
        return;
      }
      if (action === 'abort') {
        abortGuidedTour();
        return;
      }
      attempt += 1;
      timer = window.setTimeout(
        probeTargets,
        DESKTOP_GUIDED_TOUR_TARGET_RETRY_MS,
      );
    };
    timer = window.setTimeout(
      probeTargets,
      DESKTOP_GUIDED_TOUR_TARGET_RETRY_MS,
    );
    return () => {
      if (timer !== null) window.clearTimeout(timer);
    };
  }, [
    abortGuidedTour,
    authState,
    guidedTourBlocked,
    guidedTourHasActiveSession,
    guidedTourPreparing,
    startupNavigationSettled,
  ]);

  useEffect(() => {
    if (!guidedTourHasActiveSession) return;
    // 导航到会话属于非终止性中断；回到 Home 后允许当前未关闭版本重新尝试。
    guidedTourAutoAttemptedRef.current = false;
    if (guidedTourPreparing || guidedTourRun) abortGuidedTour();
  }, [abortGuidedTour, guidedTourHasActiveSession, guidedTourPreparing, guidedTourRun]);

  const createDesktopTraySession = useCallback(async () => {
    if (!await requestPreviewLeave()) return;
    try {
      const next = await createNewSessionForActiveWorkspace(api, activeRefRef.current, health);
      void refreshWorkspaceGitInfo(createApi(next), next).catch(() => {});
      const sessionId = next?.sessionId || next?.id || '';
      const noWorkspace = !!(next?.noWorkspace || next?.no_workspace);
      notifySessionListChanged({
        reason: 'session-created',
        sessionId,
        workspaceHash: noWorkspace ? '' : (next?.workspaceHash || next?.workspace_hash || ''),
        noWorkspace,
        session: {
          ...next,
          id: sessionId,
          workspace_hash: noWorkspace ? '' : (next?.workspaceHash || next?.workspace_hash || ''),
          no_workspace: noWorkspace,
        },
      });
      const opened = await navigateToRef(next);
      return opened ? next : null;
    } catch (e) {
      toast({ kind: 'err', text: '新建会话失败:' + (e.message || '') });
    }
  }, [health, navigateToRef, requestPreviewLeave]);

  const handleSubagentTasksChange = useCallback((info) => {
    const parentId = info?.parentId || '';
    const titles = info?.titles && typeof info.titles === 'object' ? info.titles : {};
    const nextIndex = { parentId, titles };
    subagentIndexRef.current = nextIndex;
    setSubagentIndex(nextIndex);
    if (!parentId || Object.keys(titles).length === 0) return;
    setSubagentDirectory((previous) => {
      const owners = { ...previous.owners };
      const mergedTitles = { ...previous.titles };
      for (const [childId, title] of Object.entries(titles)) {
        if (!childId) continue;
        owners[childId] = parentId;
        mergedTitles[childId] = title;
      }
      const next = { owners, titles: mergedTitles };
      subagentDirectoryRef.current = next;
      return next;
    });
  }, []);

  const handlePermissionDecision = useCallback((request, choice) => {
    if (!request?.request_id || !choice) return;
    setPermReqs((prev) => markPermissionSubmitting(prev, request.request_id, choice));
    connection.sendDecision(request.request_id, choice, request.session_id);
  }, []);

  const handleDesktopNotificationsChanged = useCallback((notifications) => {
    setHealth((previous) => {
      const next = {
        ...(previous || {}),
        notifications: {
          ...(previous?.notifications || {}),
          ...(notifications || {}),
        },
      };
      healthRef.current = next;
      return next;
    });
  }, []);

  // 暴露 aceDesktop_createNewSession 给 desktop 壳的托盘 "新建会话" 菜单调用。
  // 设计:openspec/changes/enhance-desktop-tray-menu。
  useEffect(() => {
    if (typeof window === 'undefined') return undefined;
    window.aceDesktop_createNewSession = () => {
      createDesktopTraySession();
    };
    return () => {
      if (window.aceDesktop_createNewSession) delete window.aceDesktop_createNewSession;
    };
  }, [createDesktopTraySession]);

  const shortcutNewSessionPending = useRef(false);
  useAppShortcuts(authState === 'ok' && !configRecoveryBlocking ? {
    search: () => setSearchOpen((open) => !open),
    console: () => { if (!consoleAvailable) return false; toggleConsoleDock(); },
    settings: () => { if (!showSettings) openSettingsSection('general'); },
    shortcuts: () => {
      openSettingsSection('shortcuts');
      requestAnimationFrame(() => document.querySelector('[data-shortcut-search]')?.focus());
    },
    toggleSidebar: toggleProjectSidebar,
    toggleRightPanel: () => { void toggleSidePanel(); },
    previewPresentation: togglePreviewPresentation,
    forward: () => { if (navHistoryRef.current.forward.length) void goForwardActiveRef(); },
    back: () => { if (navHistoryRef.current.back.length) void goBackActiveRef(); },
    newSession: () => {
      if (shortcutNewSessionPending.current) return;
      shortcutNewSessionPending.current = true;
      void createDesktopTraySession().then((next) => {
        if (!next) return;
        requestAnimationFrame(() => {
          if (activeRefRef.current?.sessionId !== next.sessionId
              || !appShortcutContextAllows('focusInput', document.activeElement)) return;
          document.querySelector('[data-main-composer="true"] [contenteditable="true"]')?.focus();
        });
      }).finally(() => { shortcutNewSessionPending.current = false; });
    },
  } : {});

  const setSidebarWidth = useCallback((nextWidth, shellWidth = 0) => {
    const sidePanelVisible = !sidePanelNavigationCollapsed;
    setSingleLayout((prev) => {
      const sidebar = normalizeSidebarWidth(nextWidth, {
        shellWidth,
        sidePanelWidth: prev.sidePanel,
        sidePanelVisible,
        previewPanelWidth: prev.previewPanel,
        previewPanelVisible,
      });
      return sidebar === prev.sidebar ? prev : { ...prev, sidebar };
    });
  }, [
    previewPanelVisible,
    setSingleLayout,
    sidePanelNavigationCollapsed,
  ]);

  const setSidePanelWidth = useCallback((nextWidth, contentWidth = 0) => {
    setSingleLayout((prev) => {
      const sidePanel = normalizeSidePanelWidth(nextWidth, {
        contentWidth,
        previewPanelWidth: prev.previewPanel,
        previewPanelVisible,
        previewPanelMaximized: sidePanelMaximized,
      });
      return sidePanel === prev.sidePanel ? prev : { ...prev, sidePanel };
    });
  }, [previewPanelVisible, setSingleLayout, sidePanelMaximized]);

  const setPreviewPanelWidth = useCallback((nextWidth, contentWidth = 0) => {
    const sidePanelVisible = !sidePanelNavigationCollapsed;
    setSingleLayout((prev) => {
      const previewPanel = normalizePreviewPanelWidth(nextWidth, {
        contentWidth,
        sidePanelWidth: prev.sidePanel,
        sidePanelVisible,
        sidePanelCollapsed: sidePanelNavigationCollapsed,
      });
      if (previewPanel === prev.previewPanel && prev.previewPanelUserSized === true) {
        return prev;
      }
      return { ...prev, previewPanel, previewPanelUserSized: true };
    });
  }, [
    setSingleLayout,
    sidePanelNavigationCollapsed,
  ]);

  const setSubagentPanelWidth = useCallback((nextWidth, contentWidth = 0) => {
    setSingleLayout((prev) => {
      const subagentPanel = normalizeSubagentPanelWidth(nextWidth, contentWidth);
      return subagentPanel === prev.subagentPanel ? prev : { ...prev, subagentPanel };
    });
  }, [setSingleLayout]);

  const startSidebarResize = useCallback((event) => {
    if (view !== 'single') return;
    if (event.button != null && event.button !== 0) return;
    if (sidebarResizeActiveRef.current) return;
    sidebarResizeActiveRef.current = true;
    event.preventDefault();
    const shellWidth = singleShellRef.current?.getBoundingClientRect().width || 0;
    const startX = event.clientX;
    const startWidth = singleLayout.sidebar;
    document.body.classList.add('ace-resizing');
    if (event.pointerId != null) event.currentTarget.setPointerCapture?.(event.pointerId);

    const onMove = (moveEvent) => {
      setSidebarWidth(startWidth + moveEvent.clientX - startX, shellWidth);
    };
    const onStop = () => {
      sidebarResizeActiveRef.current = false;
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
  }, [setSidebarWidth, singleLayout.sidebar, view]);

  const onSidebarHandleKeyDown = useCallback((event) => {
    if (view !== 'single') return;
    const step = event.shiftKey ? 32 : 12;
    if (event.key === 'ArrowLeft' || event.key === 'ArrowRight') {
      event.preventDefault();
      const delta = event.key === 'ArrowRight' ? step : -step;
      const shellWidth = singleShellRef.current?.getBoundingClientRect().width || 0;
      setSidebarWidth(singleLayout.sidebar + delta, shellWidth);
    }
  }, [setSidebarWidth, singleLayout.sidebar, view]);

  const activeId = activeRef?.sessionId || activeRef?.id || '';
  const openLoopRun = useCallback((run) => {
    const sessionId = run?.session_id || '';
    if (!sessionId) return;
    resumeAndOpenSession({
      id: sessionId,
      sessionId,
      workspaceHash: run.workspace_hash || '',
      cwd: run.workspace_cwd || '',
      no_workspace: !run.workspace_hash,
      active: false,
    }, { forceResume: true });
  }, [resumeAndOpenSession]);
  const activeRefConsoleCwd = consoleCwdForContext({ activeRef, health });
  const preferredConsoleCwd = activeId ? activeRefConsoleCwd : (consoleCwd || activeRefConsoleCwd);
  const permissionOwnership = useMemo(() => ({
    owners: subagentDirectory.owners,
    titles: {
      ...subagentDirectory.titles,
      ...subagentIndex.titles,
    },
    parentId: subagentIndex.parentId,
  }), [subagentDirectory, subagentIndex]);
  const visiblePermissionEntries = useMemo(
    () => visiblePermissionRequests(permReqs, activeId, permissionOwnership)
      .map((entry) => ({
        ...entry,
        origin_label: permissionOriginLabel(entry, permissionOwnership),
      })),
    [activeId, permReqs, permissionOwnership],
  );
  const visiblePermissionUnresolved = hasUnresolvedPermission(visiblePermissionEntries);
  const pendingPermissionSessionIdsForSidebar = useMemo(
    () => pendingPermissionSessionIds(permReqs, activeId, permissionOwnership),
    [activeId, permReqs, permissionOwnership],
  );
  const pendingQuestionSessionIdsForSidebar = useMemo(
    () => pendingQuestionSessionIds(questionReqs, activeId, permissionOwnership),
    [questionReqs, activeId, permissionOwnership],
  );
  // Keep every hook above the authentication early returns.
  const visibleQuestionReq = useMemo(() => {
    if (visiblePermissionUnresolved) return null;
    const request = visibleQuestionRequest(questionReqs, activeId, permissionOwnership);
    return request
      ? {
          ...request,
          origin_label: questionOriginLabel(request, permissionOwnership),
        }
      : null;
  }, [visiblePermissionUnresolved, questionReqs, activeId, permissionOwnership]);
  if (authState === 'checking') {
    if (desktopModeRef.current === 'shell') {
      const desktopStartupStatus = desktopStartupProgress?.current || null;
      return (
        <>
          <div
            className="ace-desktop-startup-screen"
            data-desktop-startup-screen="true"
          >
            <InteractiveHomeLogo
              className="ace-desktop-startup-logo"
              enabled={false}
            />
            <div
              className="ace-desktop-startup-status"
              data-desktop-startup-status={desktopStartupStatus?.stage || 'daemon_connecting'}
            >
              <span className="ace-spinner ace-desktop-startup-spinner" />
              <span>{desktopStartupStatus?.message || t('desktop.startupConnecting')}</span>
            </div>
          </div>
          <FramelessResizeHandles />
          <DesktopContextMenu />
          <Toaster />
        </>
      );
    }
    return (
      <>
        <div className="h-full flex items-center justify-center text-fg-mute text-sm">
          <span className="ace-spinner mr-2" /> 连接 daemon…
        </div>
        <FramelessResizeHandles />
        <DesktopContextMenu />
        <Toaster />
      </>
    );
  }
  if (authState === 'need-token') {
    return (
      <>
        <TokenPrompt onSubmit={onSubmitToken} />
        <FramelessResizeHandles />
        <DesktopContextMenu />
        <Toaster />
      </>
    );
  }

  const sidebarCollapsed = view !== 'single'
    || (projectSidebarCollapsed && !guidedTourPreparing && !guidedTourRun);

  const nativeSurfacesVisible = !showSettings
    && !officeWelcomeOpen
    && !showFeedback
    && !searchOpen
    && !updateDialogOpen
    && !desktopCloseDialogOpen
    && !configRecoveryBlocking
    && !guidedTourPreparing
    && !guidedTourRun
    && !sessionNavigationPending;
  const resolveVisibleQuestion = () => {
    if (!visibleQuestionReq?.request_id) return;
    setQuestionReqs((prev) => closePendingQuestionRequest(prev, visibleQuestionReq, {
      ownerSessionId: visibleQuestionReq.owner_session_id || '',
    }));
  };
  const autoFocusChatOnDesktopWindowFocus = shouldAutoFocusDesktopComposer({
    desktopMode: desktopModeRef.current,
    chatVisible: view === 'single' && !activeRef?.loop && !activeRef?.expertComponents,
    blockingSurfaceOpen: showSettings || showFeedback || searchOpen || updateDialogOpen
      || officeWelcomeOpen
      || desktopCloseDialogOpen || configRecoveryBlocking
      || !!visibleQuestionReq || guidedTourPreparing || guidedTourRun,
  });
  const conversationFindEnabled = canOpenConversationFind({
    view,
    activeSessionId: activeId,
    loop: !!activeRef?.loop,
    showSettings: showSettings || showFeedback,
    searchOpen,
    updateDialogOpen: updateDialogOpen || desktopCloseDialogOpen || configRecoveryBlocking || officeWelcomeOpen,
    permissionOpen: false,
    questionOpen: !!visibleQuestionReq,
    guidedTourPreparing,
    guidedTourRun,
  });

  return (
    <SlashCommandsProvider workspaceHash={commandWorkspaceHash}>
    <div
      className={[
        'ace-app-shell h-full w-full flex flex-col text-fg font-sans bg-bg',
      ].join(' ')}
      data-home-wallpaper={view === 'single' && !activeId && !activeRef?.loop && !activeRef?.expertComponents && !showSettings && !showFeedback ? 'true' : undefined}
      style={{ '--ace-home-sidebar-width': sidebarCollapsed ? '0px' : `${singleLayout.sidebar || 0}px` }}
    >
      <TopBar
        sessionTitleRef={setSessionTitleTarget}
        sessionActionsRef={setSessionActionsTarget}
        onOpenSearch={() => setSearchOpen(true)}
        onToggleConsole={toggleConsoleDock}
        consoleAvailable={consoleAvailable}
        consoleOpen={consoleDock.open}
        rightPanelCollapsed={rightPanelHidden(uiPrefs, previewPanelVisible)}
        onToggleRightPanel={toggleSidePanel}
        sidebarCollapsed={sidebarCollapsed}
        sidebarWidth={singleLayout.sidebar}
        onToggleSidebar={toggleProjectSidebar}
        onGoBack={goBackActiveRef}
        onGoForward={goForwardActiveRef}
        canGoBack={navHistory.back.length > 0}
        canGoForward={navHistory.forward.length > 0}
      />
      <div
        ref={singleShellRef}
        className={[
          'flex-1 flex overflow-hidden relative min-h-0 ace-single-shell',
          activeRef?.expertComponents ? 'ace-expert-components-shell' : '',
        ].filter(Boolean).join(' ')}
      >
        <Sidebar
          activeId={activeId}
          activeRef={activeRef}
          onSelect={navigateToRef}
          onBeforeNavigate={requestPreviewLeave}
          onActiveRemoteControlBoundChange={syncActiveRemoteControlBound}
          onSessionLoadStateChange={setSidebarSessionLoadState}
          sessionLoadResetSequence={sidebarSessionLoadResetSequence}
          collapsed={sidebarCollapsed}
          width={singleLayout.sidebar}
          onOpenHome={openHomeForWorkspace}
          onNewTask={() => openHomeForWorkspace(null, { composerFeedback: true })}
          onNewLoop={openLoopPage}
          appVersion={health?.version || ''}
          workspaceActivationRequest={workspaceActivationRequest}
          onOpenSettingsSection={openSettingsSection}
          office={office}
          onOpenFeedback={() => setShowFeedback(true)}
          onOpenExpertComponents={openExpertComponents}
          onOpenSearch={() => setSearchOpen(true)}
          onAbout={showAboutAceCode}
          onCheckUpdates={checkForUpdates}
          onExit={exitAceCode}
          updateChecking={updateChecking}
          updateStatus={updateStatus}
          updateStarting={updateStarting}
          updateRunning={updateJobIsActive(updateJob)}
          updateReady={updateJob?.state === 'succeeded' && !!updateJob?.restart_required}
          updateProgress={updateJobProgress(updateJob)}
          onStartUpdate={openUpdateDialog}
          pendingPermissionSessionIds={pendingPermissionSessionIdsForSidebar}
          pendingQuestionSessionIds={pendingQuestionSessionIdsForSidebar}
          showSessionTime={sidebarSessionTime}
        />
        {view === 'single' && !sidebarCollapsed && (
          <div
            role="separator"
            aria-label="调整左侧栏宽度"
            aria-orientation="vertical"
            tabIndex={0}
            className="ace-resize-handle ace-resize-handle-left"
            onPointerDown={startSidebarResize}
            onMouseDown={startSidebarResize}
            onKeyDown={onSidebarHandleKeyDown}
            title="拖动调整左侧栏宽度"
          />
        )}
        <div
          className={[
            'ace-main-content flex-1 flex flex-col overflow-hidden transition-all duration-200 bg-surface',
            'opacity-100 scale-100',
          ].join(' ')}
        >
          <div className="relative flex-1 flex overflow-hidden min-h-0">
            {view === 'single' && (
              <ChatView
                titleTarget={sessionTitleTarget}
                actionsTarget={sessionActionsTarget}
                sessionRef={activeRef}
                homeLogoEffectEnabled={homeLogoEffectEnabled}
                homeComposerDrafts={homeComposerDrafts}
                onHomeComposerDraftLoad={loadHomeComposerDraft}
                homeComposerAttentionRequest={homeComposerAttentionRequest}
                onHomeComposerDraftChange={updateHomeComposerDraft}
                onHomeComposerDraftAccepted={acceptHomeComposerDraft}
                onHomeComposerDraftPatch={patchHomeComposerDraft}
                modelProfileRevision={modelProfileRevision}
                onSessionPromoted={navigateToRef}
                onRegisterPreviewLeaveGuard={registerPreviewLeaveGuard}
                onSessionExpertChanged={replaceActiveSessionExpert}
                onHomeWorkspaceChange={replaceHomeWorkspace}
                onCommandWorkspaceChange={setCommandWorkspaceHash}
                onConsoleCwdChange={setConsoleCwd}
                onFindInConversation={openConversationFind}
                onOpenModelSettings={() => openSettingsSection('models')}
                health={health}
                autoFocusOnDesktopWindowFocus={autoFocusChatOnDesktopWindowFocus}
                showSidePanel
                sidePanelWidth={singleLayout.sidePanel}
                onSidePanelResize={setSidePanelWidth}
                previewPanelWidth={singleLayout.previewPanel}
                previewPanelAutoFit={!previewPanelUserSized}
                onPreviewPanelResize={setPreviewPanelWidth}
                subagentPanelWidth={singleLayout.subagentPanel ?? DEFAULT_SINGLE_LAYOUT.subagentPanel}
                onSubagentPanelResize={setSubagentPanelWidth}
                onPreviewPanelVisibleChange={setPreviewPanelVisible}
                sidePanelCollapsed={sidePanelCollapsed}
                sidePanelListCollapsed={sidePanelListCollapsed}
                onToggleSidePanel={toggleSidePanel}
                onRevealPreviewPanel={revealPreviewPanel}
                onToggleSidePanelList={toggleSidePanelList}
                onRevealSidePanelList={revealSidePanelList}
                sidePanelMaximized={sidePanelMaximized}
                onToggleSidePanelMaximized={toggleSidePanelMaximized}
                previewPresenting={previewPresenting}
                onTogglePreviewPresentation={togglePreviewPresentation}
                showAceCodeAvatar={showAceCodeAvatar}
                messageAutoCollapse={messageAutoCollapse}
                permissionRequests={visiblePermissionEntries}
                onPermissionDecision={handlePermissionDecision}
                questionRequest={visibleQuestionReq}
                onQuestionResolve={resolveVisibleQuestion}
                onSubagentTasksChange={handleSubagentTasksChange}
                recentExpertIds={recentExpertIds}
                onRememberExpert={rememberRecentExpert}
                onInitialDraftConsumed={consumeInitialDraftText}
                nativeSurfacesVisible={nativeSurfacesVisible}
              >
                {activeRef?.loop ? (
                  <LoopPage onOpenSession={openLoopRun} onSmartAdd={startScheduledTaskCreation} />
                ) : activeRef?.expertComponents ? (
                  <ExpertComponentsPage
                    workspaceHash={activeRef?.workspaceHash || ''}
                    recentExpertIds={recentExpertIds}
                    onRememberExpert={rememberRecentExpert}
                    onDispatchToNewTask={dispatchExpertToNewTask}
                    onStartConversationalCreation={startConversationalExpertCreation}
                  />
                ) : null}
              </ChatView>
            )}
            <SessionContentLoading
              phase={activeRef?.resumePending ? '' : (sidebarSessionLoadState?.phase || '')}
              title={sidebarSessionLoadState?.title || ''}
              anchorSelector="[data-session-content-loading-anchor='true']"
            />
          </div>
          {consoleAvailable && (
            <ConsoleDock
              owner={workbenchOwner}
              open={consoleDock.open}
              height={consoleDock.height}
              onHeightChange={setConsoleDockHeight}
              onToggle={setConsoleDockOpen}
              consoleInfo={health?.console}
              preferredCwd={preferredConsoleCwd}
            />
          )}
        </div>
        {showFeedback && <FeedbackForm onClose={() => setShowFeedback(false)} />}
        {showSettings && (
          <SettingsPage
            office={office}
            onClose={() => setShowSettings(false)}
            onCheckUpdates={() => {
              setShowSettings(false);
              void checkForUpdates();
            }}
            initialNavKey={settingsNavKey}
            navigationRequest={settingsNavRequest}
            initialSearch={settingsSearchSeed}
            health={health}
            activeSessionId={activeId}
            activeWorkspaceHash={commandWorkspaceHash}
            onModelProfileUpdated={() => setModelProfileRevision((value) => value + 1)}
            onDesktopNotificationsChanged={handleDesktopNotificationsChanged}
            onReplayGuidedTour={desktopGuidedTourModeEligible(desktopModeRef.current)
              ? replayGuidedTour
              : undefined}
            fontSize={fontSize}
            onThemeChange={(nextTheme) => changeAppearance({ theme: nextTheme })}
            onColorThemeChange={(nextColorTheme) => (
              themeDownloads.controller.select(nextColorTheme)
            )}
            themeDownloads={themeDownloads}
            onCreateAiTheme={startAiThemeCreation}
            onFontSizeChange={(nextFontSize) => changeAppearance({ fontSize: nextFontSize })}
            sidebarSessionTime={sidebarSessionTime}
            onSidebarSessionTimeChange={(next) => changeAppearance({ sidebarSessionTime: next })}
            messageAutoCollapse={messageAutoCollapse}
            onMessageAutoCollapseChange={(next) => changeAppearance({ messageAutoCollapse: next })}
          />
        )}
        <ThemeDownloadFailureDialog failure={themeDownloads.failure} onClose={() => themeDownloads.controller.dismissFailure()} />
        <PathPickerHost />
        <SearchPalette
          open={searchOpen}
          onClose={() => setSearchOpen(false)}
          currentWorkspaceHash={activeRef?.workspaceHash || ''}
          onSelectSession={handleSelectSession}
          onSelectWorkspace={handleSelectWorkspace}
          onSelectSetting={handleSelectSetting}
        />
      </div>
      <FramelessResizeHandles />
      <GlobalFindOverlay
        enabled={conversationFindEnabled}
        openRequest={conversationFindRequest}
        scopeKey={activeId}
      />
      <DesktopContextMenu />
      <WorkspaceCleanupNotice enabled={authState === 'ok' && !!health && !configRecoveryBlocking} />
      <ConfigRecoveryDialog
        open={configRecoveryDialogOpen}
        notice={configRecoveryNotice}
        busy={configRecoveryAckBusy}
        onAcknowledge={acknowledgeConfigRecovery}
      />
      <UpdateDialog
        open={updateDialogOpen && !configRecoveryBlocking}
        updateStatus={updateStatus}
        job={updateJob}
        starting={updateStarting}
        cancelling={updateCancelling}
        restarting={updateRestarting}
        restartAvailable={desktopUpdateRestartAvailable()}
        onConfirm={startUpdate}
        onCancel={cancelUpdate}
        onRetry={startUpdate}
        onRestart={restartAfterUpdate}
        onClose={() => setUpdateDialogOpen(false)}
      />
      <DesktopCloseDialog
        open={desktopCloseDialogOpen}
        remember={rememberDesktopCloseChoice}
        busy={desktopCloseBusy}
        trayAvailable={desktopTrayAvailable}
        onRememberChange={setRememberDesktopCloseChoice}
        onMinimizeToTray={() => chooseDesktopCloseAction(
          DESKTOP_CLOSE_BEHAVIORS.MINIMIZE_TO_TRAY,
        )}
        onExit={() => chooseDesktopCloseAction(DESKTOP_CLOSE_BEHAVIORS.EXIT)}
        onClose={() => {
          if (desktopCloseBusy) return;
          setDesktopCloseDialogOpen(false);
          setRememberDesktopCloseChoice(false);
        }}
      />
      <DesktopGuidedTour
        run={guidedTourRun && !guidedTourBlocked}
        hasModel={guidedTourState.hasModel}
        onDismiss={dismissGuidedTour}
        onAbort={abortGuidedTour}
      />
      {officeWelcomeVisible && (
        <VirtualOfficeWelcome office={office} onClose={() => setOfficeWelcomeOpen(false)} />
      )}
      <SessionNavigationMask
        open={sessionNavigationPending}
        onCancel={cancelSessionNavigation}
      />
      <Toaster />
    </div>
    </SlashCommandsProvider>
  );
}
