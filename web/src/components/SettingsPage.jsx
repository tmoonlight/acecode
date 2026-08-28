// 全屏设置页:左栏导航 + 右栏内容(Codex 风格)。
//
// 左侧导航按 Codex 风格分组,section key 与深链行为保持稳定。
// 后端真实接入的 section:常规 (权限模式) / 外观 (主题) / 配置 / 个性化 / 技能 / 模型 / 工具。
// 其余 section (MCP / 使用情况) 当前部分为 UI 占位
// — 状态走本地 useState,提交按钮无网络副作用,待后端接口就绪后接入。

import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { useTranslation } from 'react-i18next';
import { useTheme } from '../theme.jsx';
import { localePreference } from '../i18n/index.js';
import { api } from '../lib/api.js';
import { openExternalUrl } from '../lib/externalUrl.js';
import { copyTextToSystemClipboard } from '../lib/systemClipboard.js';
import {
  getMacNotificationAuthorization,
  macNotificationAuthorizationAvailable,
  notificationAuthorizationPresentation,
  openMacNotificationSettings,
  requestMacNotificationAuthorization,
  subscribeMacNotificationAuthorization,
} from '../lib/desktopNotificationAuthorization.js';
import {
  desktopBackgroundProcessAvailable,
  getDesktopBackgroundProcess,
  setDesktopBackgroundProcess,
} from '../lib/desktopBackgroundProcess.js';
import {
  DESKTOP_CLOSE_BEHAVIORS,
  DESKTOP_CLOSE_BEHAVIOR_OPTIONS,
  desktopCloseBehaviorAvailable,
  getDesktopCloseBehavior,
  setDesktopCloseBehavior,
} from '../lib/desktopCloseBehavior.js';
import { Modal, Toggle } from './Modal.jsx';
import { ModelSettingsSection } from './model-settings/ModelSettingsSection.jsx';
import { clsx, formatCount, relativeTime } from '../lib/format.js';
import { lookupErrorMessage } from '../lib/errors.js';
import { buildMcpServerList, countEnabledMcp, applyMcpToggle } from '../lib/mcpServers.js';
import { normalizeConnectorList, applyConnectorToggle } from '../lib/connectors.js';
import { PERMISSION_MODES, normalizePermissionMode } from '../lib/permissionMode.js';
import { sessionDisplayTitle } from '../lib/sessionTitle.js';
import {
  allArchivedSessionsSelected,
  archivedSessionKey,
  archivedSessionTarget,
  removeArchivedSessionsByKey,
  selectedArchivedSessions,
  shouldToggleArchivedSessionRow,
  toggleAllArchivedSessionSelection,
} from '../lib/archivedSessions.js';
import { formatUsageTokens, normalizeUsageStats, usageDataNote } from '../lib/usageStats.js';
import {
  NO_FEEDBACK_SESSION_KEY,
  buildDesktopFeedbackPayload,
  feedbackSessionKey,
  normalizeDesktopFeedbackSessions,
  selectedFeedbackSessionFromKey,
} from '../lib/desktopFeedback.js';
import {
  hookActionState,
  hookEmptyState,
  hookSettingsErrorMessage,
  hookStatusLabel,
  normalizeHookSnapshot,
} from '../lib/hooksSettings.js';
import {
  formatProgramVersion,
  formatWebCoreDetail,
  formatWebCoreLabel,
  getCurrentWebCoreInfo,
} from '../lib/webCoreInfo.js';
import {
  enabledRatioLabel,
  filterSkills,
  groupSkillsBySource,
  normalizeSkillList,
  normalizeWorkspaceList,
  skillsEnabledSummary,
  workspaceAutoExpand,
} from '../lib/skillsSettings.js';
import {
  SETTINGS_NAV_GROUPS,
  SETTINGS_NAV_ITEMS,
  settingsNavIndexForKey,
} from '../lib/settingsNavigation.js';
import { useSlashCommands } from './SlashCommandsContext.jsx';
import { RefreshIcon, VsIcon } from './Icon.jsx';
import { toast } from './Toast.jsx';
import { loadUiLocale, persistUiLocale } from '../lib/uiLocale.js';
import {
  normalizeRemoteWebState,
  remoteWebOriginSurvivesLocalMode,
  selectRemoteWebConnection,
  waitForRemoteWebMode,
} from '../lib/remoteWeb.js';
import {
  WindowControls,
  isInteractiveTarget,
  nativePointerEvent,
  useFramelessWindowState,
} from './WindowControls.jsx';

const DEFAULT_UPGRADE_SERVICE_URL = 'http://2017studio.imwork.net:82/aupdate/';
const FONT_SIZE_OPTIONS = [
  { key: 'small', label: '小' },
  { key: 'medium', label: '中' },
  { key: 'large', label: '大' },
];
const COLOR_THEME_OPTIONS = [
  { key: 'blue', label: '蓝色' },
  { key: 'orange', label: '橙色' },
];
const NOTIFICATION_AUTHORIZATION_TONE = {
  ok: {
    text: 'text-ok',
    dot: 'bg-ok shadow-[0_0_4px_var(--ace-ok)]',
  },
  danger: {
    text: 'text-danger',
    dot: 'bg-danger shadow-[0_0_4px_var(--ace-danger)]',
  },
  warn: {
    text: 'text-warn',
    dot: 'bg-warn shadow-[0_0_4px_var(--ace-warn)]',
  },
  muted: {
    text: 'text-fg-mute',
    dot: 'bg-fg-mute',
  },
};

export function SettingsPage({
  onClose,
  health,
  activeSessionId = '',
  onModelProfileUpdated,
  onPermissionModeChanged,
  onDesktopNotificationsChanged,
  onReplayGuidedTour,
  initialNavKey = 'general',
  fontSize = 'medium',
  onThemeChange,
  onColorThemeChange,
  onFontSizeChange = () => {},
}) {
  const {
    theme,
    colorTheme,
    set: setThemeCache,
    setColorTheme: setColorThemeCache,
  } = useTheme();
  const setTheme = onThemeChange || setThemeCache;
  const setColorTheme = onColorThemeChange || setColorThemeCache;
  const [activeNav, setActiveNav] = useState(
    () => settingsNavIndexForKey(initialNavKey),
  );
  const [show, setShow] = useState(false);
  const { framelessDesktop, isMaximized } = useFramelessWindowState();
  const activeNavKey = SETTINGS_NAV_ITEMS[activeNav]?.key || 'general';

  useEffect(() => { requestAnimationFrame(() => setShow(true)); }, []);
  useEffect(() => {
    setActiveNav(settingsNavIndexForKey(initialNavKey));
  }, [initialNavKey]);
  const close = () => { setShow(false); setTimeout(onClose, 240); };

  const onHeaderMouseDown = (event) => {
    if (!framelessDesktop || event.button !== 0 || isInteractiveTarget(event.target)) return;
    event.preventDefault();
    if (event.detail >= 2 && typeof window.aceDesktop_toggleMaximizeWindow === 'function') {
      window.aceDesktop_toggleMaximizeWindow();
      return;
    }
    window.aceDesktop_startWindowDrag(nativePointerEvent(event));
  };

  return (
    <div
      data-ace-native-overlay="blocking"
      className={clsx(
        'fixed inset-0 z-[300] bg-bg flex flex-col transition-all duration-250',
        show ? 'opacity-100 translate-y-0' : 'opacity-0 translate-y-4',
      )}
    >
      <div
        className={clsx(
          'h-11 pl-4 pr-0 flex items-center bg-surface border-b border-border shrink-0',
          framelessDesktop && 'ace-desktop-frameless-topbar',
        )}
        onMouseDown={onHeaderMouseDown}
      >
        <button
          type="button"
          onClick={close}
          className="px-3 h-7 rounded-md text-fg-2 text-[13px] hover:bg-surface-hi transition flex items-center gap-1.5"
        ><VsIcon name="back" size={13} />返回</button>
        <span className="flex-1 text-center text-[15px] font-semibold">设置</span>
        {/* 占位:让标题居中,与 TopBar 视觉对齐;frameless 模式下右侧由 WindowControls 占据 */}
        <div className={clsx(framelessDesktop ? 'flex items-center pr-0' : 'w-16 pr-4')}>
          {framelessDesktop && <WindowControls isMaximized={isMaximized} />}
        </div>
      </div>
      <div className="flex-1 flex overflow-hidden">
        <nav className="w-14 sm:w-[200px] bg-surface-alt border-r border-border py-2 overflow-y-auto shrink-0">
          {SETTINGS_NAV_GROUPS.map((group, groupIndex) => {
            const headingId = `settings-nav-group-${group.key}`;
            return (
              <div
                key={group.key}
                role="group"
                aria-labelledby={headingId}
              >
                <div
                  id={headingId}
                  className={clsx(
                    'sr-only sm:not-sr-only sm:block sm:px-4 sm:pb-1 text-[11px] font-medium text-fg-mute opacity-75',
                    groupIndex === 0 ? 'pt-1' : 'pt-4',
                  )}
                >
                  {group.label}
                </div>
                {group.items.map((item) => {
                  const itemIndex = settingsNavIndexForKey(item.key);
                  const active = activeNav === itemIndex;
                  return (
                    <button
                      key={item.key}
                      type="button"
                      aria-current={active ? 'page' : undefined}
                      aria-label={item.label}
                      onClick={() => setActiveNav(itemIndex)}
                      className={clsx(
                        'w-full px-0 sm:px-4 py-2 text-[13px] transition border-l-[3px] flex items-center justify-center sm:justify-start gap-2 text-left',
                        active
                          ? 'text-accent font-semibold bg-accent-bg border-accent'
                          : 'text-fg hover:bg-surface-hi border-transparent',
                      )}
                    >
                      <VsIcon name={item.icon} size={15} className="shrink-0 opacity-80" />
                      <span className="hidden sm:inline truncate">{item.label}</span>
                    </button>
                  );
                })}
              </div>
            );
          })}
        </nav>
        <div className="flex-1 overflow-y-auto px-3 py-4 sm:px-6 md:px-12 md:py-6">
          {activeNavKey === 'general' && (
            <SectionGeneral
              health={health}
              activeSessionId={activeSessionId}
              onPermissionModeChanged={onPermissionModeChanged}
              onDesktopNotificationsChanged={onDesktopNotificationsChanged}
              onReplayGuidedTour={onReplayGuidedTour}
            />
          )}
          {activeNavKey === 'appearance' && (
            <SectionAppearance
              theme={theme}
              setTheme={setTheme}
              colorTheme={colorTheme}
              setColorTheme={setColorTheme}
              fontSize={fontSize}
              onFontSizeChange={onFontSizeChange}
            />
          )}
          {activeNavKey === 'config' && <SectionConfig />}
          {activeNavKey === 'personalization' && <SectionPersonalization />}
          {activeNavKey === 'skills' && <SectionSkills />}
          {activeNavKey === 'mcp' && <SectionMCP />}
          {activeNavKey === 'connectors' && <SectionConnectors />}
          {activeNavKey === 'models' && (
            <SectionModel onModelProfileUpdated={onModelProfileUpdated} />
          )}
          {activeNavKey === 'tools' && <SectionTools />}
          {activeNavKey === 'hooks' && <SectionHooks />}
          {activeNavKey === 'archived' && <SectionArchived />}
          {activeNavKey === 'usage' && <SectionUsage />}
          {activeNavKey === 'feedback' && <SectionFeedback />}
          {activeNavKey === 'about' && <SectionAbout health={health} />}
        </div>
      </div>
    </div>
  );
}

// ─── 常规 ──────────────────────────────────────────────────────────────────
// 真实接入:默认权限模式(api.getDefaultPermissionMode / setDefaultPermissionMode)、
// Daemon 状态(/api/health 透传 health prop)。其余字段(工作模式 / 默认打开目标 /
// 最大轮次)目前是 UI 占位,本地 state。

function SectionGeneral({
  health,
  activeSessionId = '',
  onPermissionModeChanged,
  onDesktopNotificationsChanged,
  onReplayGuidedTour,
}) {
  const { t } = useTranslation();
  const [uiLocale, setUiLocale] = useState(() => localePreference());
  const [uiLocaleBusy, setUiLocaleBusy] = useState(false);
  const [permMode, setPermMode] = useState('default');
  const [permBusy, setPermBusy] = useState(false);
  const [notificationsEnabled, setNotificationsEnabled] = useState(
    () => health?.notifications?.enabled !== false,
  );
  const [notificationsBusy, setNotificationsBusy] = useState(false);
  const [notificationAuthorization, setNotificationAuthorization] = useState(
    () => notificationAuthorizationPresentation(null),
  );
  const [notificationAuthorizationBusy, setNotificationAuthorizationBusy] =
    useState(false);
  const macAuthorizationAvailable = macNotificationAuthorizationAvailable();
  const backgroundProcessAvailable = desktopBackgroundProcessAvailable();
  const [backgroundProcessEnabled, setBackgroundProcessEnabled] =
    useState(false);
  const [backgroundProcessBusy, setBackgroundProcessBusy] = useState(
    backgroundProcessAvailable,
  );
  const closeBehaviorAvailable = desktopCloseBehaviorAvailable();
  const [closeBehavior, setCloseBehavior] = useState(DESKTOP_CLOSE_BEHAVIORS.ASK);
  const [closeBehaviorBusy, setCloseBehaviorBusy] = useState(closeBehaviorAvailable);
  const [closeBehaviorTrayAvailable, setCloseBehaviorTrayAvailable] = useState(true);
  const [maxTurns, setMaxTurns] = useState(50);
  const [workMode, setWorkMode] = useState('coding');
  const [openTarget, setOpenTarget] = useState('vscode');
  const [remoteWeb, setRemoteWeb] = useState(
    () => normalizeRemoteWebState(null),
  );
  const [remoteWebLoaded, setRemoteWebLoaded] = useState(false);
  const [remoteWebBusy, setRemoteWebBusy] = useState(false);
  const remoteWebBusyRef = useRef(false);
  const [remoteWebError, setRemoteWebError] = useState('');
  const [remoteWebConnectionUrl, setRemoteWebConnectionUrl] = useState('');

  const applyRemoteWebState = useCallback((value) => {
    const normalized = normalizeRemoteWebState(value);
    setRemoteWeb(normalized);
    setRemoteWebConnectionUrl((current) => (
      selectRemoteWebConnection(normalized, current)?.url || ''
    ));
    return normalized;
  }, []);

  useEffect(() => {
    let cancelled = false;
    loadUiLocale(api)
      .then((state) => {
        if (!cancelled) setUiLocale(state.preference);
      })
      .catch(() => {
        // Startup injection/cache remains usable when the daemon is offline.
      });
    return () => { cancelled = true; };
  }, []);

  useEffect(() => {
    let cancelled = false;
    setPermBusy(false);
    api.getDefaultPermissionMode()
      .then((state) => {
        if (!cancelled) setPermMode(normalizePermissionMode(state?.mode));
      })
      .catch(() => {
        if (!cancelled) setPermMode('default');
      });
    return () => { cancelled = true; };
  }, []);

  useEffect(() => {
    let cancelled = false;
    api.getDesktopNotifications()
      .then((state) => {
        if (!cancelled) setNotificationsEnabled(state?.enabled !== false);
      })
      .catch(() => {
        if (!cancelled) {
          setNotificationsEnabled(health?.notifications?.enabled !== false);
        }
      });
    return () => { cancelled = true; };
  }, [health?.notifications?.enabled]);

  useEffect(() => {
    let cancelled = false;
    api.getRemoteWeb()
      .then((state) => {
        if (cancelled) return;
        applyRemoteWebState(state);
        setRemoteWebError('');
      })
      .catch((error) => {
        if (!cancelled) {
          setRemoteWebError(error?.message || '远程 Web 状态读取失败');
        }
      })
      .finally(() => {
        if (!cancelled) setRemoteWebLoaded(true);
      });
    return () => { cancelled = true; };
  }, [applyRemoteWebState]);

  useEffect(() => {
    if (!backgroundProcessAvailable) return undefined;
    let cancelled = false;
    setBackgroundProcessBusy(true);
    getDesktopBackgroundProcess()
      .then((state) => {
        if (cancelled) return;
        if (state?.ok) {
          setBackgroundProcessEnabled(state.enabled === true);
        }
      })
      .finally(() => {
        if (!cancelled) setBackgroundProcessBusy(false);
      });
    return () => { cancelled = true; };
  }, [backgroundProcessAvailable]);

  useEffect(() => {
    if (!closeBehaviorAvailable) return undefined;
    let cancelled = false;
    setCloseBehaviorBusy(true);
    getDesktopCloseBehavior()
      .then((state) => {
        if (cancelled) return;
        if (state?.ok) setCloseBehavior(state.behavior);
        setCloseBehaviorTrayAvailable(state?.trayAvailable !== false);
      })
      .finally(() => {
        if (!cancelled) setCloseBehaviorBusy(false);
      });
    return () => { cancelled = true; };
  }, [closeBehaviorAvailable]);

  useEffect(() => {
    if (!macAuthorizationAvailable) return undefined;
    let cancelled = false;
    const refreshAuthorization = () => getMacNotificationAuthorization().then((state) => {
      if (!cancelled) {
        setNotificationAuthorization(
          notificationAuthorizationPresentation(state),
        );
      }
    });
    refreshAuthorization();
    const unsubscribe = subscribeMacNotificationAuthorization((state) => {
      if (cancelled) return;
      setNotificationAuthorization(
        notificationAuthorizationPresentation(state),
      );
      setNotificationAuthorizationBusy(false);
    });
    // Returning from System Settings does not emit a UserNotifications
    // callback, so refresh when ACECode becomes active again.
    window.addEventListener('focus', refreshAuthorization);
    return () => {
      cancelled = true;
      unsubscribe();
      window.removeEventListener('focus', refreshAuthorization);
    };
  }, [macAuthorizationAvailable]);

  const requestSystemNotificationAuthorization = async () => {
    if (!macAuthorizationAvailable || notificationAuthorizationBusy) return;
    setNotificationAuthorizationBusy(true);
    const state = await requestMacNotificationAuthorization();
    const presented = notificationAuthorizationPresentation(state);
    setNotificationAuthorization(presented);
    if (presented.status !== 'requesting') {
      setNotificationAuthorizationBusy(false);
    }
  };

  const openSystemNotificationSettings = async () => {
    if (notificationAuthorizationBusy) return;
    setNotificationAuthorizationBusy(true);
    const opened = await openMacNotificationSettings();
    setNotificationAuthorizationBusy(false);
    if (!opened) {
      toast({ kind: 'err', text: '无法打开 macOS 通知设置' });
    }
  };

  const switchDesktopNotifications = async (enabled) => {
    const next = !!enabled;
    const previous = notificationsEnabled;
    if (notificationsBusy || next === previous) return;
    setNotificationsEnabled(next);
    setNotificationsBusy(true);
    try {
      const state = await api.setDesktopNotifications(next);
      const confirmed = state?.enabled !== false;
      setNotificationsEnabled(confirmed);
      onDesktopNotificationsChanged?.(state || { enabled: confirmed });
      let authorizationAfterEnable = notificationAuthorization;
      if (confirmed && macAuthorizationAvailable
          && !['authorized', 'provisional', 'requesting'].includes(
            notificationAuthorization.status,
          )) {
        const nativeState = await requestMacNotificationAuthorization();
        authorizationAfterEnable =
          notificationAuthorizationPresentation(nativeState);
        setNotificationAuthorization(authorizationAfterEnable);
      }
      toast({
        kind: confirmed && authorizationAfterEnable.status === 'denied'
          ? 'err'
          : 'ok',
        text: !confirmed
          ? '任务完成通知已关闭'
          : (authorizationAfterEnable.status === 'denied'
            ? '任务完成通知已打开，但 macOS 系统权限已拒绝'
            : (authorizationAfterEnable.status === 'requesting'
              ? '任务完成通知已打开，请确认 macOS 系统授权'
              : '任务完成通知已打开')),
      });
    } catch (e) {
      setNotificationsEnabled(previous);
      toast({ kind: 'err', text: '任务完成通知设置失败:' + (e?.message || '') });
    } finally {
      setNotificationsBusy(false);
    }
  };

  const switchPermissionMode = async (mode) => {
    const nextMode = normalizePermissionMode(mode);
    const previousMode = normalizePermissionMode(permMode);
    if (permBusy || nextMode === previousMode) return;
    setPermMode(nextMode);
    setPermBusy(true);
    try {
      const state = await api.setDefaultPermissionMode(nextMode);
      const confirmedMode = normalizePermissionMode(state?.mode || nextMode);
      setPermMode(confirmedMode);
      if (activeSessionId) {
        try {
          await api.setSessionPermissionMode(activeSessionId, confirmedMode);
          onPermissionModeChanged?.({ sessionId: activeSessionId, mode: confirmedMode });
        } catch (syncError) {
          toast({ kind: 'err', text: '默认权限模式已更新,当前会话同步失败:' + (syncError?.message || '') });
          return;
        }
      }
      toast({ kind: 'ok', text: '默认权限模式已更新' });
    } catch (e) {
      setPermMode(previousMode);
      toast({ kind: 'err', text: '默认权限模式更新失败:' + (e?.message || '') });
    } finally {
      setPermBusy(false);
    }
  };

  const switchBackgroundProcess = async (enabled) => {
    const next = !!enabled;
    const previous = backgroundProcessEnabled;
    if (!backgroundProcessAvailable || backgroundProcessBusy ||
        next === previous) return;
    setBackgroundProcessEnabled(next);
    setBackgroundProcessBusy(true);
    try {
      const state = await setDesktopBackgroundProcess(next);
      if (!state?.ok) {
        throw new Error(state?.error || '原生设置不可用');
      }
      setBackgroundProcessEnabled(state.enabled === true);
      toast({
        kind: 'ok',
        text: state.enabled
          ? '退出后将继续运行后台进程'
          : '下次退出时将停止后台进程',
      });
    } catch (error) {
      setBackgroundProcessEnabled(previous);
      toast({
        kind: 'err',
        text: '后台运行设置失败:' + (error?.message || ''),
      });
    } finally {
      setBackgroundProcessBusy(false);
    }
  };

  const switchRemoteWeb = async (enabled) => {
    const next = !!enabled;
    const previous = remoteWeb;
    const remoteOriginWillDisconnect = !next
      && !remoteWebOriginSurvivesLocalMode(window.location.hostname);
    let mutationAccepted = false;
    if (remoteWebBusyRef.current
        || remoteWebBusy
        || next === previous.configuredEnabled) {
      return;
    }
    remoteWebBusyRef.current = true;
    setRemoteWebBusy(true);
    setRemoteWebError('');
    setRemoteWeb((current) => ({
      ...current,
      enabled: next,
      configuredEnabled: next,
      applying: true,
    }));
    try {
      const pending = applyRemoteWebState(await api.setRemoteWeb(next));
      mutationAccepted = true;
      const confirmed = applyRemoteWebState(
        await waitForRemoteWebMode(api, next, {
          initialState: pending,
          acceptDisconnectWhenDisabling: remoteOriginWillDisconnect,
        }),
      );
      toast({
        kind: 'ok',
        text: confirmed.effectiveEnabled
          ? '远程 Web 模式已开启'
          : '远程 Web 模式已关闭，仅允许本机访问',
      });
    } catch (error) {
      if (remoteOriginWillDisconnect
          && !Number.isInteger(error?.status)
          && !error?.state?.error) {
        applyRemoteWebState({
          ...previous,
          enabled: false,
          configuredEnabled: false,
          effectiveEnabled: false,
          effectiveBind: previous.daemonBind || '127.0.0.1',
          proxyBind: '',
          proxyState: 'stopped',
          proxyPid: 0,
          error: '',
          applying: false,
          port: 0,
          connections: [],
        });
        toast({ kind: 'ok', text: '远程 Web 模式已关闭，仅允许本机访问' });
        return;
      }
      applyRemoteWebState(
        mutationAccepted
          ? (error?.state || {
            ...previous,
            enabled: next,
            configuredEnabled: next,
            applying: true,
          })
          : previous,
      );
      const message = error?.code === 'DANGEROUS_MODE_REMOTE_WEB_FORBIDDEN'
        ? '危险模式下不能开启远程 Web 模式'
        : (error?.message || '设置失败');
      setRemoteWebError(message);
      toast({ kind: 'err', text: `远程 Web 模式设置失败：${message}` });
    } finally {
      remoteWebBusyRef.current = false;
      setRemoteWebBusy(false);
    }
  };

  const copyRemoteWebConnection = async () => {
    const connection = selectRemoteWebConnection(
      remoteWeb,
      remoteWebConnectionUrl,
    );
    if (!connection) {
      toast({ kind: 'err', text: '当前没有可复制的远程连接' });
      return;
    }
    const result = await copyTextToSystemClipboard(connection.url);
    toast({
      kind: result?.ok ? 'ok' : 'err',
      text: result?.ok
        ? '连接已复制到剪贴板'
        : `复制连接失败：${result?.error || '剪贴板不可用'}`,
    });
  };

  const switchUiLocale = async (next) => {
    const previous = uiLocale;
    if (uiLocaleBusy || next === previous) return;
    setUiLocale(next);
    setUiLocaleBusy(true);
    try {
      const state = await persistUiLocale(next, api);
      setUiLocale(state.preference);
    } catch {
      setUiLocale(previous);
      toast({ kind: 'err', text: t('locale.saveFailed') });
    } finally {
      setUiLocaleBusy(false);
    }
  };

  const switchCloseBehavior = async (nextBehavior) => {
    const previous = closeBehavior;
    if (!closeBehaviorAvailable || closeBehaviorBusy || nextBehavior === previous) return;
    setCloseBehavior(nextBehavior);
    setCloseBehaviorBusy(true);
    try {
      const state = await setDesktopCloseBehavior(nextBehavior);
      if (!state?.ok) throw new Error(state?.error || '原生设置不可用');
      setCloseBehavior(state.behavior);
      setCloseBehaviorTrayAvailable(state?.trayAvailable !== false);
      const label = DESKTOP_CLOSE_BEHAVIOR_OPTIONS.find(
        (option) => option.value === state.behavior,
      )?.label || '每次询问';
      toast({ kind: 'ok', text: `关闭窗口时将${label}` });
    } catch (error) {
      setCloseBehavior(previous);
      toast({
        kind: 'err',
        text: '关闭窗口设置失败:' + (error?.message || ''),
      });
    } finally {
      setCloseBehaviorBusy(false);
    }
  };

  const selectedRemoteWebConnection = selectRemoteWebConnection(
    remoteWeb,
    remoteWebConnectionUrl,
  );
  const remoteWebStatus = !remoteWebLoaded
    ? '正在读取状态'
    : (remoteWebBusy || remoteWeb.applying
      ? '正在启动或停止反向代理'
      : (remoteWeb.effectiveEnabled
        ? `反向代理已监听 ${remoteWeb.effectiveBind}:${remoteWeb.port}`
        : `仅本机访问 · daemon ${remoteWeb.daemonBind}:${remoteWeb.daemonPort}`));
  const remoteWebProblem = remoteWebError || remoteWeb.error;

  return (
    <>
      <h2 className="text-xl font-bold mb-5">常规</h2>

      <div className="flex items-center justify-between gap-4 px-3.5 py-2.5 rounded-md bg-surface border border-border mb-2">
        <div>
          <label htmlFor="settings-ui-locale" className="text-[13px] font-medium">
            {t('locale.label')}
          </label>
        </div>
        <select
          id="settings-ui-locale"
          value={uiLocale}
          disabled={uiLocaleBusy}
          onChange={(event) => switchUiLocale(event.target.value)}
          className="h-8 min-w-36 px-2 text-[12px] rounded-md border border-border bg-surface-alt text-fg outline-none focus:border-accent transition disabled:opacity-60"
        >
          <option value="auto">{t('locale.auto')}</option>
          <option value="zh-CN">{t('locale.zhCN')}</option>
          <option value="en-US">{t('locale.enUS')}</option>
        </select>
      </div>

      <div className="h-px bg-border my-5" />

      <div className="text-[14px] font-semibold mb-1">工作模式</div>
      <p className="text-[12px] text-fg-mute mb-3">选择 Agent 显示多少技术细节</p>
      <div className="grid grid-cols-2 gap-3 max-w-md mb-5">
        {[
          { key: 'coding', label: '用于编程', desc: '更专业的回复与控制' },
          { key: 'daily',  label: '适合日常工作', desc: '同样强大,技术细节更少' },
        ].map((opt) => {
          const active = workMode === opt.key;
          return (
            <button
              key={opt.key}
              type="button"
              onClick={() => setWorkMode(opt.key)}
              className={clsx(
                'relative p-3 rounded-lg border text-left transition',
                active ? 'border-accent border-2 bg-accent-bg' : 'border-border bg-surface hover:border-accent/50',
              )}
            >
              <div className="text-[13px] font-semibold">{opt.label}</div>
              <div className="text-[11px] text-fg-mute mt-1">{opt.desc}</div>
              {active && <span className="absolute top-2 right-2 w-2.5 h-2.5 rounded-full bg-accent" />}
            </button>
          );
        })}
      </div>

      <div className="h-px bg-border my-5" />

      <div
        role="switch"
        aria-checked={notificationsEnabled}
        tabIndex={0}
        onClick={() => switchDesktopNotifications(!notificationsEnabled)}
        onKeyDown={(e) => {
          if (e.key === ' ' || e.key === 'Enter') {
            e.preventDefault();
            switchDesktopNotifications(!notificationsEnabled);
          }
        }}
        className={clsx(
          'flex items-center justify-between px-3.5 py-2.5 rounded-md bg-surface border border-border mb-2 transition',
          'cursor-pointer hover:bg-surface-hi',
          (notificationsBusy || notificationAuthorizationBusy)
            && 'opacity-75 cursor-wait',
        )}
      >
        <div>
          <div className="text-[13px] font-medium">打开任务完成通知</div>
          <div className="text-[11px] text-fg-mute mt-0.5">仅在 ACECode 窗口失去焦点且主任务完成时发送系统通知</div>
        </div>
        <div onClick={(e) => e.stopPropagation()}>
          <Toggle
            on={notificationsEnabled}
            disabled={notificationsBusy || notificationAuthorizationBusy}
            onChange={switchDesktopNotifications}
          />
        </div>
      </div>

      {macAuthorizationAvailable && (
        <div className="flex items-center justify-between gap-4 px-3.5 py-2.5 rounded-md bg-surface border border-border mb-2">
          <div>
            <div className="text-[13px] font-medium">macOS 系统通知权限</div>
            <div className="text-[11px] text-fg-mute mt-0.5">
              {notificationAuthorization.description}
            </div>
          </div>
          <div className="flex items-center gap-2 shrink-0">
            <span
              aria-live="polite"
              className={clsx(
                'flex items-center gap-1.5 text-[12px]',
                (NOTIFICATION_AUTHORIZATION_TONE[
                  notificationAuthorization.tone
                ] || NOTIFICATION_AUTHORIZATION_TONE.muted).text,
              )}
            >
              <span
                className={clsx(
                  'w-2 h-2 rounded-full',
                  (NOTIFICATION_AUTHORIZATION_TONE[
                    notificationAuthorization.tone
                  ] || NOTIFICATION_AUTHORIZATION_TONE.muted).dot,
                )}
              />
              {notificationAuthorization.label}
            </span>
            {notificationAuthorization.canRequest
              && notificationAuthorization.status !== 'requesting' && (
                <button
                  type="button"
                  disabled={notificationAuthorizationBusy}
                  onClick={requestSystemNotificationAuthorization}
                  className="px-2.5 py-1 text-[11px] rounded-md border border-border bg-surface-alt hover:bg-surface-hi transition disabled:opacity-60"
                >
                  授权
                </button>
              )}
            {notificationAuthorization.status === 'denied'
              && notificationAuthorization.canOpenSettings && (
                <button
                  type="button"
                  disabled={notificationAuthorizationBusy}
                  onClick={openSystemNotificationSettings}
                  className="px-2.5 py-1 text-[11px] rounded-md border border-border bg-surface-alt hover:bg-surface-hi transition disabled:opacity-60"
                >
                  打开系统设置
                </button>
              )}
          </div>
        </div>
      )}

      <div className="h-px bg-border my-5" />

      {onReplayGuidedTour && (
        <>
          <div className="flex items-center justify-between gap-4 px-3.5 py-3 rounded-md bg-surface border border-border mb-2">
            <div>
              <div className="text-[13px] font-medium">新手指引</div>
              <div className="text-[11px] text-fg-mute mt-0.5">从添加项目、开始新对话到模型设置</div>
            </div>
            <button
              type="button"
              onClick={onReplayGuidedTour}
              className="h-8 shrink-0 px-3 rounded-md bg-accent text-white text-[12px] font-semibold hover:opacity-90 transition"
            >
              重新查看新手指引
            </button>
          </div>
          <div className="h-px bg-border my-5" />
        </>
      )}

      <div className="text-[14px] font-semibold mb-1">权限模式</div>
      <p className="text-[12px] text-fg-mute mb-3">
        {activeSessionId ? '新建会话默认使用此模式,当前会话会同步切换' : '新建会话默认使用此模式'}
      </p>
      {PERMISSION_MODES.map((p, i) => (
        <div
          key={i}
          role="radio"
          aria-checked={permMode === p.id}
          tabIndex={0}
          onClick={() => switchPermissionMode(p.id)}
          onKeyDown={(e) => { if (e.key === ' ' || e.key === 'Enter') { e.preventDefault(); switchPermissionMode(p.id); } }}
          className={clsx(
            'flex items-center justify-between px-3.5 py-2.5 rounded-md bg-surface border border-border mb-2 transition',
            'cursor-pointer hover:bg-surface-hi',
            permBusy && 'opacity-75 cursor-wait',
          )}
        >
          <div>
            <div className="text-[13px] font-medium">{p.label}</div>
            <div className="text-[11px] text-fg-mute mt-0.5">{p.hint}</div>
          </div>
          <div onClick={(e) => e.stopPropagation()}>
            <Toggle
              on={permMode === p.id}
              disabled={permBusy}
              onChange={(v) => { if (v) switchPermissionMode(p.id); }}
            />
          </div>
        </div>
      ))}

      <div className="h-px bg-border my-5" />

      <div className="flex items-center justify-between px-3.5 py-2.5 rounded-md bg-surface border border-border mb-2">
        <div>
          <div className="text-[13px] font-medium">默认打开目标</div>
          <div className="text-[11px] text-fg-mute mt-0.5">默认打开文件和文件夹的位置</div>
        </div>
        <select
          value={openTarget}
          onChange={(e) => setOpenTarget(e.target.value)}
          className="h-7 px-2 text-[12px] rounded-md border border-border bg-surface-alt text-fg outline-none focus:border-accent transition"
        >
          <option value="vscode">VS Code</option>
          <option value="vim">Vim</option>
          <option value="terminal">终端</option>
        </select>
      </div>
      <div className="flex items-center justify-between px-3.5 py-2.5 rounded-md bg-surface border border-border mb-2">
        <div>
          <div className="text-[13px] font-medium">最大轮次</div>
          <div className="text-[11px] text-fg-mute mt-0.5">单次 agent loop 的最大迭代数</div>
        </div>
        <input
          type="number"
          value={maxTurns}
          onChange={(e) => setMaxTurns(Number(e.target.value) || 50)}
          className="w-20 h-7 px-2 text-[12px] text-center rounded-md border border-border bg-surface-alt text-fg outline-none focus:border-accent transition"
        />
      </div>
      <div className="flex items-center justify-between px-3.5 py-2.5 rounded-md bg-surface border border-border mb-2">
        <div>
          <div className="text-[13px] font-medium">后台进程状态</div>
          <div className="text-[11px] text-fg-mute mt-0.5">{health?.cwd || '—'}</div>
        </div>
        <span className="flex items-center gap-1.5 text-[12px] text-ok">
          <span className="w-2 h-2 rounded-full bg-ok shadow-[0_0_4px_var(--ace-ok)]" />
          {t('settings.backgroundRunning', { port: health?.port || 28080 })}
        </span>
      </div>
      {closeBehaviorAvailable && (
        <div className="flex items-center justify-between gap-4 px-3.5 py-2.5 rounded-md bg-surface border border-border mb-2">
          <div>
            <div className="text-[13px] font-medium">关闭窗口时</div>
            <div className="text-[11px] text-fg-mute mt-0.5">
              点击窗口右上角关闭按钮时执行的操作
            </div>
          </div>
          <select
            value={closeBehavior}
            disabled={closeBehaviorBusy}
            onChange={(event) => switchCloseBehavior(event.target.value)}
            className="h-7 shrink-0 px-2 text-[12px] rounded-md border border-border bg-surface-alt text-fg outline-none focus:border-accent transition disabled:opacity-60"
          >
            {DESKTOP_CLOSE_BEHAVIOR_OPTIONS.map((option) => (
              <option
                key={option.value}
                value={option.value}
                disabled={option.value === DESKTOP_CLOSE_BEHAVIORS.MINIMIZE_TO_TRAY
                  && !closeBehaviorTrayAvailable}
              >
                {option.label}
              </option>
            ))}
          </select>
        </div>
      )}
      {backgroundProcessAvailable && (
        <div
          role="switch"
          aria-checked={backgroundProcessEnabled}
          tabIndex={0}
          onClick={() => switchBackgroundProcess(!backgroundProcessEnabled)}
          onKeyDown={(event) => {
            if (event.key === ' ' || event.key === 'Enter') {
              event.preventDefault();
              switchBackgroundProcess(!backgroundProcessEnabled);
            }
          }}
          className={clsx(
            'flex items-center justify-between gap-4 px-3.5 py-2.5 rounded-md bg-surface border border-border mb-2 transition',
            'cursor-pointer hover:bg-surface-hi',
            backgroundProcessBusy && 'opacity-75 cursor-wait',
          )}
        >
          <div>
            <div className="text-[13px] font-medium">
              退出 ACECode 后继续运行后台进程
            </div>
            <div className="text-[11px] text-fg-mute mt-0.5">
              开启后，真正退出桌面应用时，后台任务和待处理交互仍会继续运行
            </div>
          </div>
          <div onClick={(event) => event.stopPropagation()}>
            <Toggle
              on={backgroundProcessEnabled}
              disabled={backgroundProcessBusy}
              onChange={switchBackgroundProcess}
            />
          </div>
        </div>
      )}

      <div className="h-px bg-border my-5" />

      <div
        data-remote-web-settings
        className="px-3.5 py-2.5 rounded-md bg-surface border border-border mb-2"
      >
        <div className="flex items-center justify-between gap-4">
          <div>
            <div className="text-[13px] font-medium">远程 Web 模式</div>
            <div className="text-[11px] text-fg-mute mt-0.5 max-w-lg">
              开启后会启动独立的反向代理进程，daemon 仍仅监听 127.0.0.1
            </div>
          </div>
          <Toggle
            on={remoteWeb.configuredEnabled}
            disabled={!remoteWebLoaded || remoteWebBusy}
            onChange={switchRemoteWeb}
            ariaLabel="远程 Web 模式"
          />
        </div>

        <div className="h-px bg-border my-3" />

        <div
          aria-live="polite"
          className={clsx(
            'flex items-center gap-1.5 text-[12px]',
            remoteWebProblem
              ? 'text-danger'
              : ((remoteWebBusy || remoteWeb.applying)
                ? 'text-warn'
                : (remoteWeb.effectiveEnabled ? 'text-ok' : 'text-fg-mute')),
          )}
        >
          <span
            className={clsx(
              'w-2 h-2 rounded-full shrink-0',
              remoteWebProblem
                ? 'bg-danger'
                : ((remoteWebBusy || remoteWeb.applying)
                  ? 'bg-warn'
                  : (remoteWeb.effectiveEnabled ? 'bg-ok' : 'bg-fg-mute')),
            )}
          />
          {remoteWebProblem || remoteWebStatus}
        </div>

        {remoteWeb.effectiveEnabled && (
          <div className="mt-3">
            {remoteWeb.connections.length > 1 && (
              <label className="block mb-2">
                <span className="block text-[11px] text-fg-mute mb-1">
                  连接网络
                </span>
                <select
                  value={selectedRemoteWebConnection?.url || ''}
                  onChange={(event) => setRemoteWebConnectionUrl(
                    event.target.value,
                  )}
                  className="h-8 w-full px-2 text-[12px] rounded-md border border-border bg-surface-alt text-fg outline-none focus:border-accent transition"
                >
                  {remoteWeb.connections.map((connection) => (
                    <option key={connection.url} value={connection.url}>
                      {connection.kind === 'computer_name'
                        ? `${connection.host}（计算机名）`
                        : connection.host}
                    </option>
                  ))}
                </select>
              </label>
            )}

            {selectedRemoteWebConnection ? (
              <>
                <div className="flex items-center gap-2">
                  <input
                    aria-label="远程 Web 连接"
                    readOnly
                    value={selectedRemoteWebConnection.url}
                    className="h-8 min-w-0 flex-1 px-2 text-[12px] rounded-md border border-border bg-surface-alt text-fg outline-none"
                  />
                  <button
                    type="button"
                    onClick={copyRemoteWebConnection}
                    className="h-8 shrink-0 px-3 rounded-md border border-border bg-surface-alt text-[12px] font-medium hover:bg-surface-hi transition"
                  >
                    复制连接
                  </button>
                </div>
                <div className="mt-2 px-2.5 py-2 rounded-md border border-warn/30 bg-warn/10 text-[11px] text-warn">
                  此连接包含访问 Token，请勿将此连接公开给别人。
                </div>
              </>
            ) : (
              <div className="text-[11px] text-warn">
                未发现可用的非本机网络地址，请检查网卡连接。
              </div>
            )}
          </div>
        )}

        <div className="text-[11px] text-fg-mute mt-3 leading-relaxed">
          系统防火墙、路由器或云安全组可能仍需放行代理端口。公网访问仍建议使用可信 VPN，并在上游配置 HTTPS。
        </div>
      </div>
    </>
  );
}

// ─── 外观 ──────────────────────────────────────────────────────────────────

function SectionAppearance({
  theme,
  setTheme,
  colorTheme,
  setColorTheme,
  fontSize,
  onFontSizeChange,
}) {
  return (
    <>
      <h2 className="text-xl font-bold mb-5">外观</h2>

      <div className="text-[14px] font-semibold mb-1">主题</div>
      <p className="text-[12px] text-fg-mute mb-3">选择界面的主色风格</p>
      <div className="grid grid-cols-2 gap-3 max-w-md">
        {COLOR_THEME_OPTIONS.map((opt) => {
          const active = colorTheme === opt.key;
          return (
            <button
              key={opt.key}
              type="button"
              aria-pressed={active}
              onClick={() => setColorTheme(opt.key)}
              className={clsx(
                'relative p-3 rounded-lg border text-left transition',
                active ? 'border-accent border-2 bg-accent-bg' : 'border-border bg-surface hover:border-accent/50',
              )}
            >
              <div className={clsx('flex gap-1 mb-2', `ace-theme-preview-${opt.key}`)}>
                <span className="ace-theme-preview-bg w-6 h-6 rounded border border-border" />
                <span className="ace-theme-preview-surface w-6 h-6 rounded border border-border" />
                <span className="ace-theme-preview-accent w-6 h-6 rounded border border-border" />
              </div>
              <div className="text-[13px] font-semibold">{opt.label}</div>
              {active && <span className="absolute top-2 right-2 w-2.5 h-2.5 rounded-full bg-accent" />}
            </button>
          );
        })}
      </div>

      <div className="h-px bg-border my-5" />
      <div className="flex items-center justify-between px-3.5 py-2.5 rounded-md bg-surface border border-border mb-2 max-w-md">
        <div>
          <div className="text-[13px] font-medium">暗黑模式</div>
          <div className="text-[11px] text-fg-mute mt-0.5">使用深色背景，关闭后使用浅色背景</div>
        </div>
        <Toggle
          on={theme === 'dark'}
          onChange={(enabled) => setTheme(enabled ? 'dark' : 'light')}
        />
      </div>

      <div className="h-px bg-border my-5" />
      <div className="text-[14px] font-semibold mb-1">字体大小</div>
      <div className="grid grid-cols-3 gap-1 p-1 rounded-lg border border-border bg-surface max-w-[240px]">
        {FONT_SIZE_OPTIONS.map((opt) => {
          const active = fontSize === opt.key;
          return (
            <button
              key={opt.key}
              type="button"
              aria-pressed={active}
              onClick={() => onFontSizeChange(opt.key)}
              className={clsx(
                'h-8 rounded-md text-[13px] font-medium transition',
                active
                  ? 'bg-accent text-white shadow-sm'
                  : 'text-fg-2 hover:bg-surface-hi hover:text-fg',
              )}
            >
              {opt.label}
            </button>
          );
        })}
      </div>
    </>
  );
}

// ─── 关于 ──────────────────────────────────────────────────────────────────

function SectionAbout({ health }) {
  const [webCoreInfo, setWebCoreInfo] = useState(null);

  useEffect(() => {
    let cancelled = false;
    getCurrentWebCoreInfo()
      .then((info) => {
        if (!cancelled) setWebCoreInfo(info);
      })
      .catch(() => {
        if (!cancelled) setWebCoreInfo(null);
      });
    return () => { cancelled = true; };
  }, []);

  const programVersionLabel = formatProgramVersion(health?.version);
  const webCoreLabel = formatWebCoreLabel(webCoreInfo);
  const webCoreDetail = formatWebCoreDetail(webCoreInfo);

  return (
    <>
      <h2 className="text-xl font-bold mb-5">关于</h2>

      {/* 程序版本 */}
      <div className="flex items-center justify-between px-3.5 py-2.5 rounded-md bg-surface border border-border mb-2">
        <div>
          <div className="text-[13px] font-medium">当前版本</div>
          <div className="text-[11px] text-fg-mute mt-0.5">ACECode 桌面 / TUI / Daemon 同版本号</div>
        </div>
        <span className="text-[12px] text-fg-2">{programVersionLabel}</span>
      </div>

      <div className="flex items-center justify-between gap-3 px-3.5 py-2.5 rounded-md bg-surface border border-border mb-2">
        <div className="min-w-0">
          <div className="text-[13px] font-medium">Web 核心</div>
          <div className="text-[11px] text-fg-mute mt-0.5">当前桌面 WebView / 浏览器渲染核心</div>
        </div>
        <div className="min-w-0 max-w-[62%] text-right">
          <div
            className="text-[12px] text-fg-2 truncate"
            title={webCoreLabel}
          >
            {webCoreLabel}
          </div>
          {webCoreDetail && (
            <div
              className="text-[10px] text-fg-mute truncate"
              title={webCoreDetail}
            >
              {webCoreDetail}
            </div>
          )}
        </div>
      </div>
    </>
  );
}

function SectionConfig() {
  const [upgradeUrl, setUpgradeUrl] = useState(DEFAULT_UPGRADE_SERVICE_URL);
  const [upgradeLoading, setUpgradeLoading] = useState(true);
  const [upgradeSaving, setUpgradeSaving] = useState(false);
  const [upgradeSaved, setUpgradeSaved] = useState(false);
  const [upgradeError, setUpgradeError] = useState('');
  const [depPython, setDepPython] = useState(true);
  const [depNode, setDepNode] = useState(true);
  const [depCsharp, setDepCsharp] = useState(false);
  const [diagRunning, setDiagRunning] = useState(false);
  const [resetRunning, setResetRunning] = useState(false);

  useEffect(() => {
    let cancelled = false;
    setUpgradeLoading(true);
    setUpgradeError('');
    api.getUpgradeConfig()
      .then((cfg) => {
        if (!cancelled) setUpgradeUrl(cfg?.base_url || DEFAULT_UPGRADE_SERVICE_URL);
      })
      .catch((e) => {
        if (!cancelled) setUpgradeError(e?.message || String(e));
      })
      .finally(() => {
        if (!cancelled) setUpgradeLoading(false);
      });
    return () => { cancelled = true; };
  }, []);

  const saveUpgradeUrl = async () => {
    const baseUrl = upgradeUrl.trim();
    if (!baseUrl || !/^https?:\/\//i.test(baseUrl)) {
      setUpgradeError('升级服务 URL 必须使用 http 或 https');
      return;
    }
    setUpgradeSaving(true);
    setUpgradeSaved(false);
    setUpgradeError('');
    try {
      const saved = await api.setUpgradeConfig({ base_url: baseUrl });
      setUpgradeUrl(saved?.base_url || baseUrl);
      setUpgradeSaved(true);
      toast({ kind: 'ok', text: '升级服务 URL 已保存' });
      setTimeout(() => setUpgradeSaved(false), 1500);
    } catch (e) {
      const message = e?.message || String(e);
      setUpgradeError(message);
      toast({ kind: 'err', text: message });
    } finally {
      setUpgradeSaving(false);
    }
  };

  const runDiag = () => {
    setDiagRunning(true);
    setTimeout(() => {
      setDiagRunning(false);
      toast({ kind: 'ok', text: '诊断完成(占位)' });
    }, 1600);
  };
  const runReset = () => {
    setResetRunning(true);
    setTimeout(() => {
      setResetRunning(false);
      toast({ kind: 'ok', text: '重置完成(占位)' });
    }, 2400);
  };

  const dependencies = [
    { key: 'python', label: 'Python 工具', desc: 'uv / ruff / mypy 等',     checked: depPython, toggle: () => setDepPython((v) => !v) },
    { key: 'node',   label: 'Node.js 工具', desc: 'pnpm / npm / tsx 等',     checked: depNode,   toggle: () => setDepNode((v) => !v) },
    { key: 'csharp', label: 'C# 工具',     desc: 'dotnet SDK / Roslyn 等',  checked: depCsharp, toggle: () => setDepCsharp((v) => !v) },
  ];

  return (
    <>
      <h2 className="text-xl font-bold mb-5">配置</h2>

      <div className="text-[14px] font-semibold mb-1">升级服务</div>
      <div className="rounded-md bg-surface border border-border px-3.5 py-3 mb-5">
        <label htmlFor="upgrade-service-url" className="text-[13px] font-medium mb-2 block">
          升级服务 URL
        </label>
        <div className="flex gap-2">
          <input
            id="upgrade-service-url"
            type="url"
            value={upgradeUrl}
            onChange={(e) => {
              setUpgradeUrl(e.target.value);
              setUpgradeSaved(false);
              setUpgradeError('');
            }}
            disabled={upgradeLoading || upgradeSaving}
            spellCheck={false}
            className={clsx(
              'flex-1 min-w-0 h-8 px-2.5 rounded-md border bg-bg text-fg text-[12px] outline-none transition',
              upgradeError ? 'border-danger' : 'border-border focus:border-accent',
            )}
            placeholder={DEFAULT_UPGRADE_SERVICE_URL}
          />
          <button
            type="button"
            onClick={() => {
              setUpgradeUrl(DEFAULT_UPGRADE_SERVICE_URL);
              setUpgradeSaved(false);
              setUpgradeError('');
            }}
            disabled={upgradeLoading || upgradeSaving}
            className="shrink-0 px-3 py-1.5 rounded-md text-[12px] border border-border text-fg-2 hover:bg-surface-hi disabled:opacity-50 transition"
          >
            默认
          </button>
          <button
            type="button"
            onClick={saveUpgradeUrl}
            disabled={upgradeLoading || upgradeSaving}
            className={clsx(
              'shrink-0 inline-flex items-center gap-1.5 px-3 py-1.5 rounded-md text-[12px] font-medium transition disabled:opacity-50 disabled:cursor-not-allowed',
              upgradeSaved ? 'bg-ok text-white' : 'bg-accent text-white hover:opacity-90',
            )}
          >
            {upgradeSaving ? (
              <>
                <span className="ace-spinner" style={{ width: 12, height: 12 }} />
                保存中...
              </>
            ) : (
              <>
                <VsIcon
                  name={upgradeSaved ? 'ok' : 'save'}
                  size={13}
                  mono={false}
                  className="ace-icon-on-accent"
                />
                {upgradeSaved ? '已保存' : '保存'}
              </>
            )}
          </button>
        </div>
        {upgradeError && <div className="mt-2 text-[12px] text-danger">{upgradeError}</div>}
      </div>

      <div className="text-[14px] font-semibold mb-1">工作空间依赖项</div>
      <p className="text-[12px] text-fg-mute mb-3">管理 ACECode 安装并提供给 Agent 使用的开发工具</p>

      {/* 依赖项 checkbox 组 */}
      <div className="rounded-md bg-surface border border-border px-3.5 py-3 mb-2">
        <div className="text-[13px] font-medium mb-0.5">ACECode 依赖项</div>
        <div className="text-[11px] text-fg-mute mb-2.5">选择捆绑安装的语言工具链</div>
        <div className="space-y-0.5">
          {dependencies.map((dep) => (
            <button
              key={dep.key}
              type="button"
              onClick={dep.toggle}
              aria-checked={dep.checked}
              role="checkbox"
              className="w-full flex items-center gap-2.5 px-2 py-1.5 rounded text-left hover:bg-surface-hi transition"
            >
              <span
                className={clsx(
                  'w-[18px] h-[18px] rounded flex items-center justify-center text-white text-[11px] font-bold leading-none transition shrink-0',
                  dep.checked ? 'bg-accent border-2 border-accent' : 'border-2 border-border bg-transparent',
                )}
              >
                {dep.checked && <span>✓</span>}
              </span>
              <span className="flex-1 min-w-0">
                <span className="text-[13px] font-medium block">{dep.label}</span>
                <span className="text-[11px] text-fg-mute block">{dep.desc}</span>
              </span>
            </button>
          ))}
        </div>
      </div>

      <div className="h-px bg-border my-5" />

      <div className="flex items-center justify-between px-3.5 py-2.5 rounded-md bg-surface border border-border mb-2">
        <div className="min-w-0 pr-3">
          <div className="text-[13px] font-medium">诊断 ACECode 工作空间</div>
          <div className="text-[11px] text-fg-mute mt-0.5">检查当前捆绑包并记录诊断日志</div>
        </div>
        <button
          type="button"
          onClick={runDiag}
          disabled={diagRunning}
          className="shrink-0 inline-flex items-center gap-1.5 px-3 py-1 rounded-md text-[12px] text-fg-2 bg-surface-hi hover:bg-surface-alt border border-border transition disabled:opacity-60"
        >
          {diagRunning ? (
            <>
              <span className="ace-spinner" style={{ width: 12, height: 12 }} />
              诊断中…
            </>
          ) : '诊断'}
        </button>
      </div>

      <div className="flex items-center justify-between px-3.5 py-2.5 rounded-md bg-surface border border-border mb-2">
        <div className="min-w-0 pr-3">
          <div className="text-[13px] font-medium">重置并重装工作空间</div>
          <div className="text-[11px] text-fg-mute mt-0.5">删除本地捆绑包,重新下载后再加载工具</div>
        </div>
        <button
          type="button"
          onClick={runReset}
          disabled={resetRunning}
          className="shrink-0 inline-flex items-center gap-1.5 px-3 py-1 rounded-md text-[12px] text-danger bg-danger-bg hover:opacity-80 transition disabled:opacity-60"
        >
          {resetRunning ? (
            <>
              <span
                className="inline-block w-3 h-3 rounded-full border-2 border-danger border-t-transparent"
                style={{ animation: 'ace-spin 0.8s linear infinite' }}
              />
              重置中…
            </>
          ) : '重新安装'}
        </button>
      </div>
    </>
  );
}

// ─── 个性化 ────────────────────────────────────────────────────────────────
function SectionPersonalization() {
  const [text, setText] = useState('');
  const [loading, setLoading] = useState(true);
  const [saving, setSaving] = useState(false);
  const [saved, setSaved] = useState(false);

  useEffect(() => {
    let cancelled = false;
    setLoading(true);
    api.getCustomInstructions()
      .then((cfg) => {
        if (!cancelled) setText(typeof cfg?.text === 'string' ? cfg.text : '');
      })
      .catch((e) => {
        if (!cancelled) {
          toast({ kind: 'err', text: '加载自定义指令失败:' + (e?.message || '') });
        }
      })
      .finally(() => {
        if (!cancelled) setLoading(false);
      });
    return () => { cancelled = true; };
  }, []);

  const save = async () => {
    if (saving || loading) return;
    setSaving(true);
    setSaved(false);
    try {
      const result = await api.setCustomInstructions({ text });
      if (typeof result?.text === 'string') setText(result.text);
      setSaved(true);
      toast({ kind: 'ok', text: '自定义指令已保存' });
      setTimeout(() => setSaved(false), 1500);
    } catch (e) {
      toast({ kind: 'err', text: '保存自定义指令失败:' + (e?.message || '') });
    } finally {
      setSaving(false);
    }
  };

  return (
    <>
      <h2 className="text-xl font-bold mb-5">个性化</h2>

      <div className="text-[14px] font-semibold mb-1">自定义指令</div>
      <p className="text-[12px] text-fg-mute mb-3">为你的项目向 ACECode 提供额外说明和上下文</p>
      <p className="text-[12px] text-fg-mute mb-3">
        提示:自定义指令会参与每次请求的提示词上下文,频繁修改可能降低提示词缓存命中率。
      </p>

      <textarea
        value={text}
        onChange={(e) => { setText(e.target.value); setSaved(false); }}
        disabled={loading || saving}
        placeholder="例如:这个项目使用 React 18 + Vite,组件库选 Tailwind 风格,提交信息用中文..."
        rows={10}
        className="w-full px-3 py-2.5 text-[13px] rounded-md border border-border bg-surface text-fg outline-none focus:border-accent transition leading-relaxed resize-y disabled:opacity-70"
        style={{ minHeight: 240 }}
      />

      <div className="flex justify-end mt-3">
        <button
          type="button"
          onClick={save}
          disabled={loading || saving}
          className={clsx(
            'px-4 py-1.5 rounded-md text-[12px] font-medium transition',
            saved ? 'bg-ok text-white' : 'bg-accent text-white hover:opacity-90',
            (loading || saving) && 'opacity-60 cursor-not-allowed',
          )}
        >
          {saving ? '保存中...' : saved ? '已保存' : '保存'}
        </button>
      </div>
    </>
  );
}

// ─── 技能 ──────────────────────────────────────────────────────────────────
// 真实接入:GET /api/skills(?workspace= 可选,带 source/enabled 全量元数据)、
// PUT /api/skills/:name(?workspace= 供跨工作区校验)、GET /api/skills/root
// (path / global_path)、GET /api/workspaces。
// 结构:顶部全局技能(响应式卡片网格 + 打开全局目录按钮),下方「工作区 Skill 目录」
// 每个已注册工作区一个折叠组,默认折叠;mount 后台预取各工作区技能做计数,
// 展开即渲染缓存,避免一次性渲染全部工作区的技能行。
// 过滤 / 分组 / 计数逻辑在 lib/skillsSettings.js(有 Node 单测)。

function parseDesktopBridgeResult(value) {
  // webview native binding 通常返回已解析的 JS 值;调试 shim 可能给原始字符串。
  if (value == null) return value;
  if (typeof value !== 'string') return value;
  const text = value.trim();
  if (!text || text === 'null') return null;
  return JSON.parse(text);
}

function SkillCard({ skill, busyName, onToggle }) {
  return (
    <article
      data-skill-card="true"
      className={clsx(
        'flex min-h-[148px] flex-col rounded-lg border p-3.5 transition',
        skill.enabled
          ? 'border-accent/40 bg-accent-bg'
          : 'border-border bg-surface hover:border-accent/50 hover:bg-surface-hi',
      )}
    >
      <div className="flex items-start gap-3">
        <div
          className={clsx(
            'flex h-9 w-9 shrink-0 items-center justify-center rounded-md border transition',
            skill.enabled
              ? 'border-accent/40 bg-surface text-accent'
              : 'border-border bg-surface-alt text-fg-mute',
          )}
        >
          <VsIcon name="lightbulb" size={18} />
        </div>
        <div className="min-w-0 flex-1">
          <div className="break-words text-[13px] font-semibold leading-5 text-fg">{skill.name}</div>
          <div className="mt-0.5 text-[10px] text-fg-mute">
            {skill.source === 'project' ? '工作区' : '全局'}
          </div>
        </div>
        <Toggle
          on={skill.enabled}
          disabled={busyName === skill.name}
          onChange={(value) => onToggle(skill.name, value)}
          ariaLabel={`切换技能 ${skill.name}`}
        />
      </div>
      <p
        className="mt-3 line-clamp-4 text-[11px] leading-[18px] text-fg-mute"
        title={skill.description || ''}
      >
        {skill.description || '—'}
      </p>
    </article>
  );
}

function SkillCardGrid({ skills, busyName, onToggle }) {
  return (
    <div
      data-skill-card-grid="true"
      className="grid grid-cols-1 gap-3 lg:grid-cols-2 xl:grid-cols-3 2xl:grid-cols-4"
    >
      {skills.map((skill) => (
        <SkillCard key={skill.name} skill={skill} busyName={busyName} onToggle={onToggle} />
      ))}
    </div>
  );
}

// 单个工作区的折叠组:头行(展开箭头 + 名称/cwd + N/M 计数 + 打开目录)+
// 展开后的技能行列表。skills 为 null/undefined 表示尚未加载完成。
function WorkspaceSkillGroup({
  ws,
  skills,
  expanded,
  onToggleExpand,
  query,
  busyName,
  onToggleSkill,
  onOpenDir,
  openingDir,
}) {
  const loaded = Array.isArray(skills);
  const shown = loaded ? filterSkills(skills, query) : [];
  const hasQuery = !!String(query || '').trim();
  return (
    <div className="mb-2">
      <div
        role="button"
        tabIndex={0}
        aria-expanded={expanded}
        onClick={onToggleExpand}
        onKeyDown={(e) => {
          if (e.key === ' ' || e.key === 'Enter') { e.preventDefault(); onToggleExpand(); }
        }}
        className="flex items-center gap-2.5 px-3.5 py-2.5 rounded-md bg-surface border border-border cursor-pointer hover:bg-surface-hi transition"
      >
        <VsIcon name={expanded ? 'expandDown' : 'expandRight'} size={12} className="shrink-0 text-fg-mute" />
        <VsIcon name="folder" size={15} className="shrink-0 text-fg-2" />
        <div className="flex-1 min-w-0">
          <div className="text-[13px] font-medium truncate">{ws.name}</div>
          <div className="text-[11px] text-fg-mute truncate">{ws.cwd}</div>
        </div>
        <span className="text-[11px] text-fg-mute tabular-nums shrink-0">
          {loaded ? enabledRatioLabel(skills) : '…'}
        </span>
        <button
          type="button"
          title="打开该工作区的 Skill 目录"
          onClick={(e) => { e.stopPropagation(); onOpenDir(); }}
          disabled={!!openingDir}
          className="w-7 h-7 inline-flex items-center justify-center rounded text-fg-mute hover:text-fg hover:bg-surface-alt transition shrink-0 disabled:opacity-50"
        >
          <VsIcon name="folderOpen" size={14} />
        </button>
      </div>
      {expanded && (
        <div className="mt-2 pl-6">
          {!loaded && (
            <div className="px-3.5 py-3 rounded-md bg-surface border border-border text-[12px] text-fg-mute text-center mb-2">
              <span className="ace-spinner mr-2" /> 加载中
            </div>
          )}
          {loaded && shown.length === 0 && (
            <div className="px-3.5 py-3 rounded-md border border-dashed border-border text-[12px] text-fg-mute text-center mb-2">
              {hasQuery ? '无匹配技能' : '该工作区没有技能;放到项目的 .acecode/skills 目录即可被发现'}
            </div>
          )}
          {shown.length > 0 && (
            <SkillCardGrid skills={shown} busyName={busyName} onToggle={onToggleSkill} />
          )}
        </div>
      )}
    </div>
  );
}

function SectionSkills() {
  const [globalSkills, setGlobalSkills] = useState([]);
  const [workspaces, setWorkspaces] = useState([]);
  // hash -> 该工作区的项目技能数组;键缺失 = 还没加载完。
  const [wsSkills, setWsSkills] = useState({});
  const [expanded, setExpanded] = useState({});
  const [loading, setLoading] = useState(true);
  const [refreshing, setRefreshing] = useState(false);
  const [search, setSearch] = useState('');
  const [savingName, setSavingName] = useState('');
  const [openingDir, setOpeningDir] = useState('');
  const slashCommandsCtx = useSlashCommands();

  const load = useCallback(async (refresh = false) => {
    if (refresh) setRefreshing(true); else setLoading(true);
    try {
      const [skillsRes, wsRes] = await Promise.allSettled([
        api.listSkills(),
        api.listWorkspaces(),
      ]);
      if (skillsRes.status === 'fulfilled') {
        setGlobalSkills(groupSkillsBySource(normalizeSkillList(skillsRes.value)).global);
      } else if (!refresh) {
        toast({ kind: 'err', text: '加载技能失败:' + (skillsRes.reason?.message || '') });
      }
      let wsList = wsRes.status === 'fulfilled' ? normalizeWorkspaceList(wsRes.value) : [];
      if (wsList.length === 0 && typeof window.aceDesktop_listWorkspaces === 'function') {
        try {
          wsList = normalizeWorkspaceList(
            parseDesktopBridgeResult(await window.aceDesktop_listWorkspaces()),
          );
        } catch { /* bridge 兜底失败就当没有工作区 */ }
      }
      setWorkspaces(wsList);
      if (refresh) setWsSkills({});
      // 后台预取各工作区的项目技能:折叠行的 N/M 计数需要它,展开时也能
      // 即刻渲染。逐个到达逐个填充,失败置空数组避免计数一直显示省略号。
      wsList.forEach((ws) => {
        api.listSkills(ws.hash)
          .then((list) => {
            const project = groupSkillsBySource(normalizeSkillList(list)).project;
            setWsSkills((prev) => ({ ...prev, [ws.hash]: project }));
          })
          .catch(() => {
            setWsSkills((prev) => ({ ...prev, [ws.hash]: [] }));
          });
      });
    } finally {
      if (refresh) setRefreshing(false); else setLoading(false);
    }
  }, []);

  useEffect(() => { load(); }, [load]);

  const summary = useMemo(() => skillsEnabledSummary(globalSkills), [globalSkills]);
  const filteredGlobal = useMemo(
    () => filterSkills(globalSkills, search),
    [globalSkills, search],
  );
  const hasQuery = !!search.trim();

  // 启停对 disabled 是全局生效的:同名技能出现在全局列表和多个工作区列表时
  // 一起翻转,失败整体回滚。
  const toggle = async (name, next, wsHash = '') => {
    if (savingName) return;
    const beforeGlobal = globalSkills;
    const beforeWs = wsSkills;
    const flip = (list) => list.map((s) => (s.name === name ? { ...s, enabled: next } : s));
    setGlobalSkills(flip(globalSkills));
    setWsSkills((prev) => {
      const out = {};
      for (const [hash, list] of Object.entries(prev)) out[hash] = flip(list);
      return out;
    });
    setSavingName(name);
    try {
      await api.setSkillEnabled(name, next, wsHash);
      slashCommandsCtx.invalidate?.();
    } catch (e) {
      setGlobalSkills(beforeGlobal);
      setWsSkills(beforeWs);
      toast({ kind: 'err', text: '切换技能失败:' + (e?.message || '') });
    } finally {
      setSavingName('');
    }
  };

  // 打开技能目录:desktop bridge 优先;webapp 兼容模式走 REST;都不可用时复制路径。
  const openDir = async (scope, wsHash = '') => {
    if (openingDir) return;
    setOpeningDir(scope + wsHash);
    try {
      const root = await api.getSkillRoot(wsHash);
      const path = scope === 'global' ? (root?.global_path || '') : (root?.path || '');
      if (!path) throw new Error('目录路径为空');
      if (typeof window.aceDesktop_openInExplorer === 'function') {
        const result = parseDesktopBridgeResult(await window.aceDesktop_openInExplorer(path));
        if (!result?.ok) throw new Error(result?.error || 'open failed');
        toast({ kind: 'ok', text: '已打开技能目录' });
        return;
      }
      try {
        const result = await api.openInExplorer(path);
        if (!result?.ok) throw new Error(result?.error || 'open failed');
        toast({ kind: 'ok', text: '已打开技能目录' });
      } catch {
        // REST 不可用(非 desktop 环境)→ 复制路径;复制也失败(如页面失焦)
        // 时直接把路径显示出来,不要报"失败"把路径吞掉。
        try {
          await navigator.clipboard.writeText(path);
          toast({ kind: 'ok', text: '技能目录路径已复制:' + path });
        } catch {
          toast({ kind: 'info', text: path });
        }
      }
    } catch (e) {
      toast({ kind: 'err', text: '打开技能目录失败:' + (e?.message || '') });
    } finally {
      setOpeningDir('');
    }
  };

  return (
    <>
      <div className="flex items-start justify-between gap-4 mb-5">
        <div>
          <h2 className="text-xl font-bold mb-2">技能</h2>
          <p className="text-[12px] text-fg-mute">
            管理 ACECode 可调用的技能模块。启用后 Agent 在任务中可自动使用。
          </p>
        </div>
        <div className="flex items-center gap-3 shrink-0">
          <span className="text-[12px] text-fg-mute tabular-nums">{summary.label}</span>
          <button
            type="button"
            onClick={() => load(true)}
            disabled={loading || refreshing}
            title="刷新技能列表"
            className="h-8 w-8 inline-flex items-center justify-center rounded-md border border-border bg-surface text-fg-2 hover:bg-surface-hi transition disabled:opacity-50"
          >
            <RefreshIcon size={15} className={clsx(refreshing && 'animate-spin')} />
          </button>
        </div>
      </div>

      <div className="relative mb-5">
        <VsIcon
          name="search"
          size={14}
          className="absolute left-3 top-1/2 -translate-y-1/2 text-fg-mute pointer-events-none"
        />
        <input
          type="text"
          value={search}
          onChange={(e) => setSearch(e.target.value)}
          placeholder="搜索技能名称或描述..."
          className="w-full h-9 pl-9 pr-3 text-[13px] rounded-md border border-border bg-surface text-fg outline-none focus:border-accent transition"
        />
      </div>

      {loading ? (
        <div className="px-3.5 py-8 rounded-md bg-surface border border-border text-[12px] text-fg-mute text-center">
          <span className="ace-spinner mr-2" /> 加载中
        </div>
      ) : (
        <>
          {filteredGlobal.length === 0 && (
            <div className="px-3.5 py-3 rounded-md border border-dashed border-border text-[12px] text-fg-mute text-center mb-2">
              {hasQuery ? '无匹配的全局技能' : '暂无全局技能'}
            </div>
          )}
          {filteredGlobal.length > 0 && (
            <SkillCardGrid skills={filteredGlobal} busyName={savingName} onToggle={toggle} />
          )}
          <button
            type="button"
            onClick={() => openDir('global')}
            disabled={!!openingDir}
            className="w-full h-10 mt-2 inline-flex items-center justify-center gap-2 rounded-md border border-border bg-surface text-[13px] text-fg hover:bg-surface-hi transition disabled:opacity-60"
          >
            <VsIcon name="folder" size={15} />
            打开全局 Skill 目录
          </button>

          <div className="h-px bg-border my-5" />

          <div className="text-[14px] font-semibold mb-1">工作区 Skill 目录</div>
          <p className="text-[12px] text-fg-mute mb-3">
            每个工作区可拥有独立的 Skill,仅在该工作区的会话中生效。
          </p>
          {workspaces.length === 0 && (
            <div className="px-3.5 py-3 rounded-md border border-dashed border-border text-[12px] text-fg-mute text-center">
              暂无已注册的工作区
            </div>
          )}
          {workspaces.map((ws) => {
            const skills = wsSkills[ws.hash];
            const isExpanded = hasQuery
              ? workspaceAutoExpand(skills, search)
              : !!expanded[ws.hash];
            return (
              <WorkspaceSkillGroup
                key={ws.hash}
                ws={ws}
                skills={skills}
                expanded={isExpanded}
                onToggleExpand={() =>
                  setExpanded((prev) => ({ ...prev, [ws.hash]: !prev[ws.hash] }))
                }
                query={search}
                busyName={savingName}
                onToggleSkill={(name, v) => toggle(name, v, ws.hash)}
                onOpenDir={() => openDir('workspace', ws.hash)}
                openingDir={openingDir}
              />
            );
          })}
        </>
      )}
    </>
  );
}

function SectionMCP() {
  const [text, setText] = useState('');
  const [error, setError] = useState('');
  const [loading, setLoading] = useState(true);
  const [saving, setSaving] = useState(false);
  const [saved, setSaved] = useState(false);
  const [togglingName, setTogglingName] = useState('');

  // 从 JSON 编辑器文本派生开关列表:文本合法且是对象时才有内容,否则空数组。
  // 直接读文本(而非独立请求)保证开关与 JSON 编辑器永远同步。
  const { serverList, parsedOk } = useMemo(() => {
    try {
      const parsed = JSON.parse(text);
      if (!parsed || typeof parsed !== 'object' || Array.isArray(parsed)) {
        return { serverList: [], parsedOk: false };
      }
      return { serverList: buildMcpServerList(parsed), parsedOk: true };
    } catch {
      return { serverList: [], parsedOk: false };
    }
  }, [text]);

  const load = async () => {
    setLoading(true);
    setError('');
    try {
      const cfg = await api.getMcp();
      setText(JSON.stringify(cfg || {}, null, 2));
    } catch (e) {
      setError('加载 MCP 失败:' + (e?.message || ''));
      toast({ kind: 'err', text: '加载 MCP 失败:' + (e?.message || '') });
    } finally {
      setLoading(false);
    }
  };

  useEffect(() => {
    let cancelled = false;
    setLoading(true);
    api.getMcp()
      .then((cfg) => {
        if (!cancelled) setText(JSON.stringify(cfg || {}, null, 2));
      })
      .catch((e) => {
        if (!cancelled) {
          setError('加载 MCP 失败:' + (e?.message || ''));
          toast({ kind: 'err', text: '加载 MCP 失败:' + (e?.message || '') });
        }
      })
      .finally(() => {
        if (!cancelled) setLoading(false);
      });
    return () => { cancelled = true; };
  }, []);

  const onChange = (value) => {
    setText(value);
    setSaved(false);
    try {
      JSON.parse(value);
      setError('');
    } catch (e) {
      setError('JSON 格式错误:' + e.message);
    }
  };

  const format = () => {
    try {
      const parsed = JSON.parse(text);
      setText(JSON.stringify(parsed, null, 2));
      setError('');
    } catch (e) {
      setError('JSON 格式错误:' + e.message);
    }
  };
  const save = async () => {
    try {
      const parsed = JSON.parse(text);
      if (!parsed || typeof parsed !== 'object' || Array.isArray(parsed)) {
        setError('JSON 必须是对象');
        return;
      }
      setError('');
      setSaving(true);
      await api.putMcp(parsed);
      setSaved(true);
      toast({ kind: 'ok', text: 'MCP 配置已保存;重启 daemon 后生效' });
      setTimeout(() => setSaved(false), 1500);
    } catch (e) {
      const msg = e instanceof SyntaxError ? 'JSON 格式错误:' + e.message : '保存失败:' + (e?.message || '');
      setError(msg);
      toast({ kind: 'err', text: msg });
    } finally {
      setSaving(false);
    }
  };
  const reload = async () => {
    try {
      const result = await api.reloadMcp();
      toast({ kind: 'ok', text: 'Reload: ' + JSON.stringify(result) });
    } catch {
      toast({ kind: 'err', text: '当前 daemon 需要重启后加载 MCP 配置' });
    }
  };

  // 开关某个 server:先把 disabled 写回 JSON 文本(编辑器同步),再调 toggle
  // 端点落盘 + 运行时热切换。失败回滚文本。applied=false 说明 daemon 未热应用,
  // 提示需重启。
  const toggleServer = async (name, enabled) => {
    if (togglingName) return;
    let nextText = text;
    try {
      const parsed = JSON.parse(text);
      nextText = JSON.stringify(applyMcpToggle(parsed, name, enabled), null, 2);
    } catch {
      toast({ kind: 'err', text: 'JSON 无效,无法切换' });
      return;
    }
    const prevText = text;
    setText(nextText);
    setSaved(false);
    setTogglingName(name);
    try {
      const res = await api.toggleMcpServer(name, enabled);
      if (res && res.applied === false) {
        toast({ kind: 'ok', text: `${name} 已${enabled ? '启用' : '关闭'};重启 daemon 后生效` });
      } else {
        toast({ kind: 'ok', text: `${name} 已${enabled ? '启用' : '关闭'}` });
      }
    } catch (e) {
      setText(prevText);
      toast({ kind: 'err', text: '切换失败:' + (e?.message || '') });
    } finally {
      setTogglingName('');
    }
  };

  const enabledCount = countEnabledMcp(serverList);

  return (
    <>
      <h2 className="text-xl font-bold mb-5">MCP 服务器</h2>

      <div className="text-[14px] font-semibold mb-1">服务器配置</div>
      <p className="text-[12px] text-fg-mute mb-3">
        直接编辑 JSON 配置 MCP 服务器连接(stdio / sse / http)
      </p>

      <textarea
        value={text}
        onChange={(e) => onChange(e.target.value)}
        spellCheck={false}
        disabled={loading}
        rows={18}
        className={clsx(
          'w-full px-4 py-3 text-[12px] rounded-md border bg-code-bg text-code-fg font-mono outline-none transition leading-relaxed resize-y',
          error ? 'border-danger' : 'border-border focus:border-accent',
        )}
        placeholder={loading ? '加载中...' : '{\n  \"filesystem\": { ... }\n}'}
        style={{ minHeight: 380, tabSize: 2 }}
      />
      {error && (
        <div className="mt-2 text-[12px] text-danger">{error}</div>
      )}

      <div className="flex justify-end gap-2 mt-3">
        <button
          type="button"
          onClick={format}
          disabled={loading}
          className="px-3 py-1.5 rounded-md text-[12px] border border-border text-fg-2 hover:bg-surface-hi transition"
        >
          格式化
        </button>
        <button
          type="button"
          onClick={load}
          disabled={loading || saving}
          className="px-3 py-1.5 rounded-md text-[12px] border border-border text-fg-2 hover:bg-surface-hi disabled:opacity-50 transition"
        >
          重新加载
        </button>
        <button
          type="button"
          onClick={reload}
          disabled={loading || saving}
          className="px-3 py-1.5 rounded-md text-[12px] border border-border text-fg-2 hover:bg-surface-hi disabled:opacity-50 transition"
        >
          Reload
        </button>
        <button
          type="button"
          onClick={save}
          disabled={loading || saving || !!error}
          className={clsx(
            'px-4 py-1.5 rounded-md text-[12px] font-medium transition disabled:opacity-50 disabled:cursor-not-allowed',
            saved ? 'bg-ok text-white' : 'bg-accent text-white hover:opacity-90',
          )}
        >
          {saving ? '保存中...' : (saved ? '✓ 已保存' : '保存')}
        </button>
      </div>

      <div className="mt-8 pt-6 border-t border-border">
        <div className="flex items-center justify-between mb-4">
          <div className="text-[15px] font-bold">启用服务器</div>
          {parsedOk && serverList.length > 0 && (
            <div className="text-[12px] text-fg-mute">
              {enabledCount} / {serverList.length} 已启用
            </div>
          )}
        </div>

        {loading ? (
          <div className="rounded-lg border border-border bg-surface px-4 py-4 text-[12px] text-fg-mute">
            <span className="ace-spinner mr-2" /> 加载中
          </div>
        ) : !parsedOk ? (
          <div className="rounded-lg border border-border bg-surface px-4 py-4 text-[12px] text-fg-mute">
            JSON 无效,修正后可在此逐个开关服务器。
          </div>
        ) : serverList.length === 0 ? (
          <div className="rounded-lg border border-border bg-surface px-4 py-4 text-[12px] text-fg-mute">
            暂无已配置的 MCP 服务器。
          </div>
        ) : (
          <div className="space-y-2 max-w-3xl">
            {serverList.map((server) => (
              <div
                key={server.name}
                className="rounded-lg border border-border bg-surface px-4 py-3 flex items-center gap-3"
              >
                <div className="h-9 w-9 rounded-md border border-border bg-surface-alt flex items-center justify-center text-fg-2 shrink-0">
                  <VsIcon name="mcp" size={18} />
                </div>
                <div className="flex-1 min-w-0">
                  <div className="flex items-center gap-2 min-w-0">
                    <div className="text-[13px] font-semibold text-fg truncate">
                      {server.name}
                    </div>
                    <span className="text-[10px] px-1.5 py-0.5 rounded border border-border text-fg-mute shrink-0">
                      {server.transportLabel}
                    </span>
                  </div>
                  {server.commandLine && (
                    <div className="mt-0.5 text-[11px] text-fg-mute font-mono break-all">
                      {server.commandLine}
                    </div>
                  )}
                </div>
                <Toggle
                  on={server.enabled}
                  onChange={(next) => toggleServer(server.name, next)}
                  disabled={!!togglingName || saving}
                />
              </div>
            ))}
          </div>
        )}
      </div>
    </>
  );
}

function SectionConnectors() {
  const [connectors, setConnectors] = useState([]);
  const [loading, setLoading] = useState(true);
  const [savingId, setSavingId] = useState('');
  const [error, setError] = useState('');

  const load = useCallback(async () => {
    setLoading(true);
    setError('');
    try {
      const data = await api.getConnectors();
      setConnectors(normalizeConnectorList(data));
    } catch (e) {
      const message = e?.message || String(e);
      setError(message);
      toast({ kind: 'err', text: '加载连接器失败:' + message });
    } finally {
      setLoading(false);
    }
  }, []);

  useEffect(() => {
    let cancelled = false;
    setLoading(true);
    setError('');
    api.getConnectors()
      .then((data) => {
        if (!cancelled) setConnectors(normalizeConnectorList(data));
      })
      .catch((e) => {
        if (!cancelled) {
          const message = e?.message || String(e);
          setError(message);
          toast({ kind: 'err', text: '加载连接器失败:' + message });
        }
      })
      .finally(() => {
        if (!cancelled) setLoading(false);
      });
    return () => { cancelled = true; };
  }, []);

  const toggleConnector = async (connector, enabled) => {
    if (!connector?.id || savingId) return;
    const before = connectors;
    const next = applyConnectorToggle(connectors, connector.id, enabled);
    setConnectors(next);
    setSavingId(connector.id);
    setError('');
    try {
      const result = await api.setConnectors({ connectors: next });
      setConnectors(normalizeConnectorList(result));
      toast({ kind: 'ok', text: enabled ? '连接器已启用' : '连接器已关闭' });
    } catch (e) {
      const message = e?.message || String(e);
      setConnectors(before);
      setError(message);
      toast({ kind: 'err', text: '连接器保存失败:' + message });
    } finally {
      setSavingId('');
    }
  };

  return (
    <>
      <div className="flex items-start justify-between gap-4 mb-5">
        <div>
          <h2 className="text-xl font-bold mb-2">连接器</h2>
          <p className="text-[12px] text-fg-mute">config.json 中配置的连接器</p>
        </div>
        <button
          type="button"
          onClick={load}
          disabled={loading || !!savingId}
          title="刷新连接器"
          className="h-8 w-8 inline-flex items-center justify-center rounded-md border border-border bg-surface text-fg-2 hover:bg-surface-hi transition disabled:opacity-50"
        >
          <RefreshIcon size={15} className={clsx(loading && 'animate-spin')} />
        </button>
      </div>

      {error && (
        <div className="mb-3 px-3 py-2 rounded-md border border-danger bg-surface text-danger text-[12px]">
          {error}
        </div>
      )}

      {loading ? (
        <div className="px-3.5 py-8 rounded-md bg-surface border border-border text-[12px] text-fg-mute text-center">
          <span className="ace-spinner mr-2" /> 加载中
        </div>
      ) : connectors.length === 0 ? (
        <div className="rounded-lg border border-border bg-surface px-4 py-4 max-w-3xl">
          <div className="text-[14px] font-semibold text-fg mb-1">暂无已配置连接器</div>
          <div className="text-[12px] text-fg-mute">没有可显示的连接器。</div>
        </div>
      ) : (
        <div className="space-y-3 max-w-5xl">
          {connectors.map((connector) => (
            <div
              key={connector.id}
              className="rounded-lg border border-border bg-surface px-4 py-3"
            >
              <div className="flex items-start gap-3">
                <div className="h-9 w-9 rounded-md border border-border bg-surface-alt flex items-center justify-center text-fg-2 shrink-0">
                  <VsIcon name="extension" size={18} />
                </div>
                <div className="flex-1 min-w-0">
                  <div className="flex items-center gap-2 min-w-0">
                    <div className="text-[13px] font-semibold text-fg truncate">
                      {connector.name || '未命名连接器'}
                    </div>
                    <span
                      className={clsx(
                        'text-[10px] px-1.5 py-0.5 rounded border shrink-0',
                        connector.enabled
                          ? 'border-ok-border bg-ok-bg text-ok'
                          : 'border-border text-fg-mute',
                      )}
                    >
                      {connector.enabled ? '已启用' : '已关闭'}
                    </span>
                  </div>
                  <div className="mt-1 text-[11px] text-fg-mute break-words">
                    {connector.description || '无描述'}
                  </div>
                </div>
                <Toggle
                  on={connector.enabled}
                  onChange={(next) => toggleConnector(connector, next)}
                  disabled={loading || !!savingId}
                />
              </div>
            </div>
          ))}
        </div>
      )}
    </>
  );
}

// ─── 工具 ──────────────────────────────────────────────────────────────────

function SectionTools() {
  const nativeBrowserAvailable = typeof globalThis?.aceDesktop_agentBrowserGetState === 'function';

  return (
    <>
      <h2 className="text-xl font-bold mb-5">工具</h2>

      <div className="text-[14px] font-semibold mb-1">内置工具</div>
      <p className="text-[12px] text-fg-mute mb-3">
        Agent 浏览器工具由 Windows Desktop 原生提供，模型需要浏览器时会自动打开并操作同一个可见页面。
      </p>

      <div className="flex items-center gap-3 px-3.5 py-3 rounded-md bg-surface border border-border mb-2">
        <div className="w-10 h-10 rounded-md bg-surface-alt border border-border flex items-center justify-center shrink-0 text-fg">
          <VsIcon name="globe" size={20} />
        </div>
        <div className="flex-1 min-w-0">
          <div className="text-[13px] font-medium">Agent 浏览器</div>
          <div className="text-[11px] text-fg-mute mt-0.5">
            这是一个内嵌浏览器。
          </div>
        </div>
        <span className={`text-[11px] px-2 py-1 rounded-full border border-border bg-surface-alt ${nativeBrowserAvailable ? 'text-success' : 'text-fg-mute'}`}>
          {nativeBrowserAvailable ? '可用' : '仅 Windows Desktop'}
        </span>
      </div>

      {/* 占位:更多工具即将加入 */}
      <div className="px-3.5 py-3 rounded-md border border-dashed border-border text-[12px] text-fg-mute text-center mt-2">
        更多内置工具即将加入
      </div>
    </>
  );
}

// ─── 钩子 ──────────────────────────────────────────────────────────────────

function SectionHooks() {
  const [snapshot, setSnapshot] = useState(() => normalizeHookSnapshot({ hooks: [] }));
  const [loading, setLoading] = useState(true);
  const [refreshing, setRefreshing] = useState(false);
  const [busyId, setBusyId] = useState('');
  const [error, setError] = useState('');

  const applySnapshot = useCallback((data) => {
    setSnapshot(normalizeHookSnapshot(data || {}));
  }, []);

  const load = useCallback(async (refresh = false) => {
    if (refresh) setRefreshing(true); else setLoading(true);
    setError('');
    try {
      const data = refresh ? await api.refreshHooks() : await api.listHooks();
      applySnapshot(data);
    } catch (e) {
      const message = hookSettingsErrorMessage(e);
      setError(message);
      toast({ kind: 'err', text: '加载钩子失败:' + message });
    } finally {
      if (refresh) setRefreshing(false); else setLoading(false);
    }
  }, [applySnapshot]);

  useEffect(() => {
    let cancelled = false;
    setLoading(true);
    setError('');
    api.listHooks()
      .then((data) => {
        if (!cancelled) applySnapshot(data);
      })
      .catch((e) => {
        if (!cancelled) {
          const message = hookSettingsErrorMessage(e);
          setError(message);
          toast({ kind: 'err', text: '加载钩子失败:' + message });
        }
      })
      .finally(() => {
        if (!cancelled) setLoading(false);
      });
    return () => { cancelled = true; };
  }, [applySnapshot]);

  const runAction = async (hook, action) => {
    if (!hook?.id) return;
    const token = `${hook.id}:${action}`;
    setBusyId(token);
    setError('');
    try {
      let data;
      if (action === 'trust') data = await api.trustHook(hook.id);
      else if (action === 'disable') data = await api.disableHook(hook.id);
      else data = await api.enableHook(hook.id);
      applySnapshot(data);
      const label = action === 'trust' ? '已信任钩子' : (action === 'disable' ? '已禁用钩子' : '已启用钩子');
      toast({ kind: 'ok', text: label });
    } catch (e) {
      const message = hookSettingsErrorMessage(e);
      setError(message);
      toast({ kind: 'err', text: '钩子操作失败:' + message });
    } finally {
      setBusyId('');
    }
  };

  const empty = hookEmptyState(snapshot);

  return (
    <>
      <div className="flex items-start justify-between gap-4 mb-5">
        <div>
          <h2 className="text-xl font-bold mb-2">钩子</h2>
          <p className="text-[12px] text-fg-mute">
            通过配置和已启用的插件管理生命周期钩子。
            <button
              type="button"
              onClick={() => openExternalUrl('https://developers.openai.com/codex/hooks')}
              className="ml-2 text-accent hover:underline"
            >
              了解更多
            </button>
          </p>
        </div>
        <button
          type="button"
          onClick={() => load(true)}
          disabled={loading || refreshing}
          title="刷新钩子"
          className="h-8 w-8 inline-flex items-center justify-center rounded-md border border-border bg-surface text-fg-2 hover:bg-surface-hi transition disabled:opacity-50"
        >
          <RefreshIcon size={15} className={clsx(refreshing && 'animate-spin')} />
        </button>
      </div>

      {error && (
        <div className="mb-3 px-3 py-2 rounded-md border border-danger bg-surface text-danger text-[12px]">
          {error}
        </div>
      )}

      {loading ? (
        <div className="px-3.5 py-8 rounded-md bg-surface border border-border text-[12px] text-fg-mute text-center">
          <span className="ace-spinner mr-2" /> 加载中
        </div>
      ) : snapshot.isEmpty ? (
        <div className="rounded-lg border border-border bg-surface px-4 py-4 max-w-3xl">
          <div className="text-[14px] font-semibold text-fg mb-1">{empty.title}</div>
          <div className="text-[12px] text-fg-mute">{empty.body}</div>
        </div>
      ) : (
        <div className="space-y-3 max-w-5xl">
          {snapshot.hooks.map((hook) => (
            <HookListItem
              key={hook.id}
              hook={hook}
              busyId={busyId}
              onTrust={() => runAction(hook, 'trust')}
              onDisable={() => runAction(hook, 'disable')}
              onEnable={() => runAction(hook, 'enable')}
            />
          ))}
        </div>
      )}

      {!loading && snapshot.diagnostics.length > 0 && (
        <div className="mt-4 rounded-md border border-border bg-surface px-3.5 py-3">
          <div className="text-[12px] font-semibold text-fg-2 mb-2">发现诊断</div>
          <div className="space-y-1">
            {snapshot.diagnostics.slice(0, 8).map((diag, index) => (
              <div key={`${diag.code}-${index}`} className="text-[11px] text-fg-mute">
                <span className="text-fg-2">{diag.code || diag.severity}</span>
                {diag.message ? ` · ${diag.message}` : ''}
              </div>
            ))}
          </div>
        </div>
      )}
    </>
  );
}

function HookListItem({ hook, busyId, onTrust, onDisable, onEnable }) {
  const actions = hookActionState(hook);
  const busyTrust = busyId === `${hook.id}:trust`;
  const busyDisable = busyId === `${hook.id}:disable`;
  const busyEnable = busyId === `${hook.id}:enable`;
  const sourceLabel = hook.sourcePath || hook.sourceId || '未知来源';
  const commandText = hook.commandWindows
    ? `${hook.command} | Windows: ${hook.commandWindows}`
    : hook.command;

  return (
    <div className="rounded-lg border border-border bg-surface px-4 py-3">
      <div className="flex items-start gap-3">
        <div className="h-9 w-9 rounded-md border border-border bg-surface-alt flex items-center justify-center text-fg-2 shrink-0">
          <VsIcon name="hook" size={18} />
        </div>
        <div className="flex-1 min-w-0">
          <div className="flex items-center gap-2 min-w-0">
            <div className="text-[13px] font-semibold text-fg truncate">{hook.eventName || 'Hook'}</div>
            <HookBadge hook={hook} />
            {hook.managed && (
              <span className="text-[10px] px-1.5 py-0.5 rounded border border-border text-fg-mute">managed</span>
            )}
          </div>
          <div className="mt-1 flex flex-wrap gap-x-3 gap-y-1 text-[11px] text-fg-mute">
            <span>匹配: <span className="text-fg-2">{hook.matcher}</span></span>
            <span>来源: <span className="text-fg-2 break-all">{sourceLabel}</span></span>
            {hook.timeoutSeconds > 0 && <span>超时: {hook.timeoutSeconds}s</span>}
          </div>
          {commandText && (
            <div className="mt-2 rounded-md border border-border bg-code-bg px-2.5 py-1.5 font-mono text-[11px] text-code-fg break-all">
              {commandText}
            </div>
          )}
          {hook.statusMessage && (
            <div className="mt-1 text-[11px] text-fg-mute">状态消息: {hook.statusMessage}</div>
          )}
          {hook.skipReason && (
            <div className="mt-1 text-[11px] text-warn">跳过原因: {hook.skipReason}</div>
          )}
          {hook.diagnostics.length > 0 && (
            <div className="mt-2 space-y-1">
              {hook.diagnostics.map((diag, index) => (
                <div key={`${diag.code}-${index}`} className="text-[11px] text-fg-mute">
                  <span className="text-warn">{diag.code || diag.severity}</span>
                  {diag.message ? ` · ${diag.message}` : ''}
                </div>
              ))}
            </div>
          )}
        </div>
        <div className="shrink-0 flex items-center gap-2">
          {actions.canTrust && (
            <button
              type="button"
              onClick={onTrust}
              disabled={busyTrust || busyDisable || busyEnable}
              className="inline-flex items-center gap-1.5 px-3 py-1.5 rounded-md bg-accent text-white text-[12px] font-medium hover:opacity-90 transition disabled:opacity-50"
            >
              {busyTrust ? <span className="ace-spinner" /> : <VsIcon name="check" size={12} />}
              信任
            </button>
          )}
          {actions.canEnable && (
            <button
              type="button"
              onClick={onEnable}
              disabled={busyTrust || busyDisable || busyEnable}
              className="inline-flex items-center gap-1.5 px-3 py-1.5 rounded-md border border-border text-fg-2 bg-surface-alt text-[12px] hover:bg-surface-hi transition disabled:opacity-50"
            >
              {busyEnable ? <span className="ace-spinner" /> : <VsIcon name="run" size={12} />}
              启用
            </button>
          )}
          {actions.canDisable && (
            <button
              type="button"
              onClick={onDisable}
              disabled={busyTrust || busyDisable || busyEnable}
              className="inline-flex items-center gap-1.5 px-3 py-1.5 rounded-md border border-border text-fg-2 bg-surface-alt text-[12px] hover:bg-surface-hi transition disabled:opacity-50"
            >
              {busyDisable ? <span className="ace-spinner" /> : <VsIcon name="stop" size={12} />}
              禁用
            </button>
          )}
        </div>
      </div>
    </div>
  );
}

function HookBadge({ hook }) {
  const label = hookStatusLabel(hook);
  const cls = hook.disabled
    ? 'border-danger text-danger'
    : hook.pendingReview
      ? 'border-warn text-warn'
      : hook.trusted || hook.managed
        ? 'border-ok text-ok'
        : 'border-border text-fg-mute';
  return (
    <span className={clsx('text-[10px] px-1.5 py-0.5 rounded border shrink-0', cls)}>
      {label}
    </span>
  );
}

// ─── 已归档会话 ────────────────────────────────────────────────────────────
function SectionArchived() {
  const [list, setList] = useState([]);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState('');
  const [selectedKeys, setSelectedKeys] = useState(() => new Set());
  const [unarchivingKeys, setUnarchivingKeys] = useState(() => new Set());
  const [deletingKeys, setDeletingKeys] = useState(() => new Set());
  const [purgeConfirmation, setPurgeConfirmation] = useState(null);

  useEffect(() => {
    let cancelled = false;
    setLoading(true);
    setError('');
    api.listAllArchivedSessions()
      .then(async (result) => {
        let sessions = Array.isArray(result?.sessions) ? result.sessions : [];
        if (sessions.length === 0 && Array.isArray(result?.errors) && result.errors.length === 0) {
          const workspaces = await api.listWorkspaces().catch(() => []);
          if (!Array.isArray(workspaces) || workspaces.length === 0) {
            const local = await api.listSessions({ archived: true }).catch(() => []);
            sessions = (Array.isArray(local) ? local : []).map((item) => ({
              ...item,
              workspace_hash: item.workspace_hash || '__local__',
              workspaceName: item.workspaceName || '当前会话',
            }));
          }
        }
        if (!cancelled) setList(sessions);
      })
      .catch((e) => {
        if (!cancelled) setError(e.message || String(e));
      })
      .finally(() => {
        if (!cancelled) setLoading(false);
      });
    return () => { cancelled = true; };
  }, []);

  const selectedItems = useMemo(
    () => selectedArchivedSessions(list, selectedKeys),
    [list, selectedKeys],
  );
  const allSelected = useMemo(
    () => allArchivedSessionsSelected(list, selectedKeys),
    [list, selectedKeys],
  );
  const operationBusy = unarchivingKeys.size > 0 || deletingKeys.size > 0;

  const removeSelectionKeys = (removedKeys) => {
    setSelectedKeys((previous) => {
      const next = new Set(previous);
      for (const key of removedKeys) next.delete(key);
      return next;
    });
  };

  const toggleAllSelected = () => {
    if (operationBusy) return;
    setSelectedKeys((previous) => (
      toggleAllArchivedSessionSelection(list, previous)
    ));
  };

  const toggleSelected = (item) => {
    const key = archivedSessionKey(item);
    if (!key || unarchivingKeys.has(key) || deletingKeys.has(key)) return;
    setSelectedKeys((previous) => {
      const next = new Set(previous);
      if (next.has(key)) next.delete(key);
      else next.add(key);
      return next;
    });
  };

  const unarchiveArchivedItems = async (items, { batch = false } = {}) => {
    const targets = items
      .map((item) => ({ item, ...archivedSessionTarget(item) }))
      .filter((target) => (
        target.id
        && target.key
        && !unarchivingKeys.has(target.key)
        && !deletingKeys.has(target.key)
      ));
    if (targets.length === 0) return;

    const targetKeys = targets.map((target) => target.key);
    setUnarchivingKeys((previous) => {
      const next = new Set(previous);
      for (const key of targetKeys) next.add(key);
      return next;
    });

    try {
      const results = await Promise.allSettled(targets.map((target) => {
        if (target.workspaceHash && target.workspaceHash !== '__local__') {
          return api.unarchiveWorkspaceSession(target.workspaceHash, target.id);
        }
        return api.unarchiveSession(target.id);
      }));
      const succeeded = new Set();
      results.forEach((result, index) => {
        if (result.status === 'fulfilled') succeeded.add(targets[index].key);
      });
      const successCount = succeeded.size;
      const failedCount = targets.length - successCount;

      if (successCount > 0) {
        setList((previous) => removeArchivedSessionsByKey(previous, succeeded));
        removeSelectionKeys(succeeded);
        window.dispatchEvent(new Event('ace-session-archive-changed'));
      }

      if (batch) {
        if (failedCount === 0) {
          toast({ kind: 'ok', text: `已取消 ${successCount} 个会话的归档` });
        } else {
          toast({
            kind: 'err',
            text: `已取消 ${successCount} 个会话的归档，${failedCount} 个失败`,
          });
        }
      } else if (failedCount === 0) {
        toast({ kind: 'ok', text: '已取消归档' });
      } else {
        const failure = results.find((result) => result.status === 'rejected');
        toast({
          kind: 'err',
          text: '取消归档失败:' + (failure?.reason?.message || ''),
        });
      }
    } finally {
      setUnarchivingKeys((previous) => {
        const next = new Set(previous);
        for (const key of targetKeys) next.delete(key);
        return next;
      });
    }
  };

  const unarchive = (item) => {
    void unarchiveArchivedItems([item]);
  };

  const unarchiveSelected = () => {
    if (selectedItems.length === 0 || operationBusy) return;
    void unarchiveArchivedItems([...selectedItems], { batch: true });
  };

  const purgeArchivedItems = async (items, { batch = false } = {}) => {
    const targets = items
      .map((item) => ({ item, ...archivedSessionTarget(item) }))
      .filter((target) => (
        target.id
        && target.key
        && !unarchivingKeys.has(target.key)
        && !deletingKeys.has(target.key)
      ));
    if (targets.length === 0) return;

    const targetKeys = targets.map((target) => target.key);
    setDeletingKeys((previous) => {
      const next = new Set(previous);
      for (const key of targetKeys) next.add(key);
      return next;
    });

    try {
      const results = await Promise.allSettled(targets.map((target) => {
        if (target.workspaceHash && target.workspaceHash !== '__local__') {
          return api.purgeArchivedWorkspaceSession(target.workspaceHash, target.id);
        }
        return api.purgeArchivedSession(target.id);
      }));
      const succeeded = new Set();
      results.forEach((result, index) => {
        if (result.status === 'fulfilled') succeeded.add(targets[index].key);
      });
      const successCount = succeeded.size;
      const failedCount = targets.length - successCount;

      if (successCount > 0) {
        setList((previous) => removeArchivedSessionsByKey(previous, succeeded));
        removeSelectionKeys(succeeded);
        window.dispatchEvent(new Event('ace-session-archive-changed'));
      }

      if (batch) {
        if (failedCount === 0) {
          toast({ kind: 'ok', text: `已删除 ${successCount} 个会话` });
        } else {
          toast({
            kind: 'err',
            text: `已删除 ${successCount} 个会话，${failedCount} 个删除失败`,
          });
        }
      } else if (failedCount === 0) {
        toast({ kind: 'ok', text: '会话已彻底删除' });
      } else {
        const failure = results.find((result) => result.status === 'rejected');
        toast({
          kind: 'err',
          text: '彻底删除失败:' + (failure?.reason?.message || ''),
        });
      }
    } finally {
      setDeletingKeys((previous) => {
        const next = new Set(previous);
        for (const key of targetKeys) next.delete(key);
        return next;
      });
    }
  };

  const purgeOne = (item) => {
    const title = sessionDisplayTitle(item, item?.name || '') || '未命名会话';
    setPurgeConfirmation({
      title: '彻底删除会话',
      message: `彻底删除会话“${title}”？此操作不可撤销。`,
      items: [item],
      batch: false,
    });
  };

  const purgeSelected = () => {
    if (selectedItems.length === 0 || operationBusy) return;
    setPurgeConfirmation({
      title: '彻底删除选中的会话',
      message: `彻底删除选中的 ${selectedItems.length} 个会话？此操作不可撤销。`,
      items: [...selectedItems],
      batch: true,
    });
  };

  const confirmPurge = () => {
    const pending = purgeConfirmation;
    if (!pending) return;
    setPurgeConfirmation(null);
    void purgeArchivedItems(pending.items, { batch: pending.batch });
  };

  return (
    <>
      <h2 className="text-xl font-bold mb-5">已归档会话</h2>

      <div className="text-[14px] font-semibold mb-1">归档列表</div>
      <p className="text-[12px] text-fg-mute mb-3">已归档的会话不会出现在侧栏,可随时取消归档恢复</p>

      {loading ? (
        <div className="px-3.5 py-8 rounded-md bg-surface border border-border text-[12px] text-fg-mute text-center">
          <span className="ace-spinner mr-2" /> 加载中
        </div>
      ) : error ? (
        <div className="px-3.5 py-8 rounded-md bg-surface border border-border text-[12px] text-danger text-center">
          加载失败:{error}
        </div>
      ) : list.length === 0 ? (
        <div className="px-3.5 py-8 rounded-md bg-surface border border-border text-[12px] text-fg-mute text-center">
          暂无已归档会话
        </div>
      ) : (
        <>
          {list.map((item) => {
            const target = archivedSessionTarget(item);
            const title = sessionDisplayTitle(item, item.name || '');
            const selected = selectedKeys.has(target.key);
            const unarchiving = unarchivingKeys.has(target.key);
            const deleting = deletingKeys.has(target.key);
            const busy = unarchiving || deleting;
            return (
              <div
                key={target.key || item.id}
                onClick={(event) => {
                  if (shouldToggleArchivedSessionRow(event.target)) toggleSelected(item);
                }}
                className={clsx(
                  'flex items-center gap-3 px-3.5 py-2.5 rounded-md bg-surface border border-border mb-2 transition',
                  busy
                    ? 'cursor-wait opacity-60'
                    : 'cursor-pointer hover:bg-surface-hi',
                )}
              >
                <input
                  type="checkbox"
                  checked={selected}
                  disabled={busy}
                  onChange={() => toggleSelected(item)}
                  aria-label={`选择会话 ${title || '未命名会话'}`}
                  className="h-4 w-4 shrink-0 accent-accent disabled:opacity-60"
                />
                <div className="min-w-0 flex-1">
                  <div className="text-[13px] font-medium truncate">{title}</div>
                  <div className="text-[11px] text-fg-mute mt-0.5 truncate">
                    {relativeTime(item.updated_at || item.created_at)} · {item.workspaceName || item.cwd || item.workspace_hash || 'workspace'}
                  </div>
                </div>
                <div className="shrink-0 flex items-center gap-2">
                  <button
                    type="button"
                    onClick={() => unarchive(item)}
                    disabled={busy}
                    className="px-3 py-1 rounded-md text-[12px] text-fg-2 bg-surface-hi hover:bg-surface-alt border border-border transition disabled:opacity-60"
                  >
                    {unarchiving ? '取消中' : '取消归档'}
                  </button>
                  <button
                    type="button"
                    onClick={() => purgeOne(item)}
                    disabled={busy}
                    className="px-3 py-1 rounded-md text-[12px] text-danger bg-danger-bg hover:opacity-80 border border-danger/40 transition disabled:opacity-60"
                  >
                    {deleting ? '删除中' : '彻底删除'}
                  </button>
                </div>
              </div>
            );
          })}
          <div className="mt-3 flex flex-wrap items-center gap-2">
            <button
              type="button"
              onClick={toggleAllSelected}
              disabled={operationBusy}
              className="inline-flex w-fit items-center justify-center px-3 py-1 rounded-md text-[12px] text-fg-2 bg-surface-hi hover:bg-surface-alt border border-border transition disabled:opacity-50 disabled:cursor-not-allowed"
            >
              {allSelected ? '全不选' : '全选'}
            </button>
            <button
              type="button"
              onClick={unarchiveSelected}
              disabled={selectedItems.length === 0 || operationBusy}
              className="inline-flex w-fit items-center justify-center px-3 py-1 rounded-md text-[12px] text-fg-2 bg-surface-hi hover:bg-surface-alt border border-border transition disabled:opacity-50 disabled:cursor-not-allowed"
            >
              取消选中会话的归档
            </button>
            <button
              type="button"
              onClick={purgeSelected}
              disabled={selectedItems.length === 0 || operationBusy}
              className="inline-flex w-fit items-center justify-center px-3 py-1 rounded-md text-[12px] text-danger bg-danger-bg hover:opacity-80 border border-danger/40 transition disabled:opacity-50 disabled:cursor-not-allowed"
            >
              删除选中会话
            </button>
          </div>
        </>
      )}
      {purgeConfirmation && (
        <Modal onClose={() => setPurgeConfirmation(null)} width={440}>
          {({ close }) => (
            <div className="p-4">
              <div className="text-[14px] font-semibold mb-2">
                {purgeConfirmation.title}
              </div>
              <div className="text-[12.5px] text-fg-mute leading-relaxed mb-4">
                {purgeConfirmation.message}
              </div>
              <div className="flex justify-end gap-2">
                <button
                  type="button"
                  className="px-3 py-1.5 text-[12.5px] rounded-lg border border-border hover:bg-surface-hi"
                  onClick={close}
                >
                  取消
                </button>
                <button
                  type="button"
                  className="px-3 py-1.5 text-[12.5px] rounded-lg border border-danger/40 bg-danger-bg text-danger hover:opacity-80"
                  onClick={confirmPurge}
                >
                  彻底删除
                </button>
              </div>
            </div>
          )}
        </Modal>
      )}
    </>
  );
}

// ─── 使用情况 ──────────────────────────────────────────────────────────────

const USAGE_COLORS = [
  'var(--ace-accent)',
  'var(--ace-ok)',
  'var(--ace-warn)',
  'var(--ace-danger)',
  '#8b5cf6',
  '#06b6d4',
];

function shortUsageDate(date) {
  const text = String(date || '');
  const m = text.match(/^(\d{4})-(\d{2})-(\d{2})$/);
  return m ? `${Number(m[2])}/${Number(m[3])}` : text;
}

function UsageEmptyState({ text }) {
  return (
    <div className="px-3.5 py-8 rounded-md bg-surface border border-border text-[12px] text-fg-mute text-center">
      {text}
    </div>
  );
}

function SectionUsage() {
  const [raw, setRaw] = useState(null);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState('');
  const [reloadKey, setReloadKey] = useState(0);

  useEffect(() => {
    let cancelled = false;
    setLoading(true);
    setError('');
    api.getUsageStats({ days: 30, timezoneOffsetMinutes: new Date().getTimezoneOffset() })
      .then((data) => {
        if (!cancelled) setRaw(data || {});
      })
      .catch((e) => {
        if (!cancelled) setError(e.message || String(e));
      })
      .finally(() => {
        if (!cancelled) setLoading(false);
      });
    return () => { cancelled = true; };
  }, [reloadKey]);

  const stats = useMemo(() => normalizeUsageStats(raw || {}), [raw]);
  const note = usageDataNote(stats);
  const summary = [
    {
      label: `${stats.metadata.days} 天 Tokens`,
      value: formatUsageTokens(stats.summary.totals.totalTokens),
      sub: `${formatUsageTokens(stats.summary.totals.promptTokens)} 输入 / ${formatUsageTokens(stats.summary.totals.completionTokens)} 输出`,
    },
    {
      label: '用量记录',
      value: String(stats.summary.records),
      sub: stats.hasEstimates
        ? formatCount(stats.summary.estimatedRecords, 'estimates')
        : 'provider usage',
    },
    {
      label: '会话',
      value: String(stats.summary.sessionCount),
      sub: formatCount(stats.models.length, 'models'),
    },
  ];
  const tokenDetails = [
    ['输入', stats.summary.totals.promptTokens],
    ['输出', stats.summary.totals.completionTokens],
    ['缓存读', stats.summary.totals.cacheReadTokens],
    ['缓存写', stats.summary.totals.cacheWriteTokens],
    ['推理', stats.summary.totals.reasoningTokens],
    ['总计', stats.summary.totals.totalTokens],
  ];

  return (
    <>
      <div className="flex items-center justify-between mb-5">
        <h2 className="text-xl font-bold">使用情况</h2>
        <button
          type="button"
          onClick={() => setReloadKey((v) => v + 1)}
          disabled={loading}
          className="px-2.5 h-7 rounded-md text-[12px] border border-border bg-surface hover:bg-surface-hi disabled:opacity-60 transition flex items-center gap-1.5"
        >
          {loading ? <span className="ace-spinner" /> : <RefreshIcon size={13} />}
          刷新
        </button>
      </div>

      {loading && !raw ? (
        <UsageEmptyState text="加载中" />
      ) : error ? (
        <UsageEmptyState text={`加载失败:${error}`} />
      ) : !stats.hasData ? (
        <>
          <div className="grid grid-cols-1 xl:grid-cols-3 gap-3 mb-6">
            {summary.map((c) => (
              <div key={c.label} className="px-4 py-3.5 rounded-md bg-surface border border-border">
                <div className="text-[10px] text-fg-mute uppercase tracking-wider mb-1.5">{c.label}</div>
                <div className="text-[24px] font-bold text-fg leading-none mb-1">{c.value}</div>
                <div className="text-[11px] text-fg-mute">{c.sub}</div>
              </div>
            ))}
          </div>
          <UsageEmptyState text={note} />
        </>
      ) : (
        <>
          <div className="grid grid-cols-1 xl:grid-cols-3 gap-3 mb-6">
            {summary.map((c) => (
              <div key={c.label} className="px-4 py-3.5 rounded-md bg-surface border border-border">
                <div className="text-[10px] text-fg-mute uppercase tracking-wider mb-1.5">{c.label}</div>
                <div className="text-[24px] font-bold text-fg leading-none mb-1">{c.value}</div>
                <div className="text-[11px] text-fg-mute truncate">{c.sub}</div>
              </div>
            ))}
          </div>

          <div className="text-[14px] font-semibold mb-1">每日用量趋势</div>
          <p className="text-[12px] text-fg-mute mb-3">近 {stats.metadata.days} 天 token 消耗</p>
          <div className="px-4 pt-4 pb-2 rounded-md bg-surface border border-border mb-6">
            <div className="flex items-stretch gap-1.5 h-[150px]">
              {stats.daily.map((d) => {
                const h = stats.maxDailyTokens > 0 ? (d.tokens / stats.maxDailyTokens) * 100 : 0;
                return (
                  <div key={d.date} className="flex-1 min-w-[24px] h-full flex flex-col items-center gap-1.5">
                    <div className="text-[9px] text-fg-mute opacity-80 whitespace-nowrap">
                      {d.tokens > 0 ? formatUsageTokens(d.tokens) : ''}
                    </div>
                    <div className="w-full flex-1 flex items-end">
                      <div
                        className="w-full rounded-sm bg-accent transition-all"
                        style={{ height: `${h}%`, minHeight: d.tokens > 0 ? 6 : 2, opacity: d.tokens > 0 ? 0.9 : 0.18 }}
                      />
                    </div>
                    <div className="text-[10px] text-fg-mute whitespace-nowrap">{shortUsageDate(d.date)}</div>
                  </div>
                );
              })}
            </div>
          </div>

          <div className="grid grid-cols-2 xl:grid-cols-6 gap-2 mb-6">
            {tokenDetails.map(([label, value]) => (
              <div key={label} className="px-3 py-2.5 rounded-md bg-surface border border-border">
                <div className="text-[11px] text-fg-mute mb-1">{label}</div>
                <div className="text-[14px] font-semibold">{formatUsageTokens(value)}</div>
              </div>
            ))}
          </div>

          <div className="text-[14px] font-semibold mb-1">模型用量明细</div>
          <p className="text-[12px] text-fg-mute mb-3">每个模型的输入 / 输出 token 与用量记录</p>
          <div className="rounded-md bg-surface border border-border overflow-hidden mb-6">
            {stats.models.length === 0 ? (
              <div className="px-3.5 py-6 text-[12px] text-fg-mute text-center">暂无模型用量</div>
            ) : stats.models.map((m, i) => {
              const total = m.totals.totalTokens;
              const barWidth = stats.maxModelTokens > 0 ? (total / stats.maxModelTokens) * 100 : 0;
              const inputPct = total > 0 ? (m.totals.promptTokens / total) * 100 : 0;
              const color = USAGE_COLORS[i % USAGE_COLORS.length];
              return (
                <div
                  key={`${m.provider}:${m.model}:${m.modelPreset}:${i}`}
                  className={clsx(
                    'px-4 py-3.5',
                    i < stats.models.length - 1 && 'border-b border-border',
                  )}
                >
                  <div className="flex items-center justify-between mb-2 gap-3">
                    <div className="flex items-center gap-2 min-w-0">
                      <span className="w-2.5 h-2.5 rounded-sm shrink-0" style={{ background: color }} />
                      <span className="text-[13px] font-semibold truncate">{m.label}</span>
                    </div>
                    <div className="flex items-center gap-4 shrink-0">
                      <span className="text-[12px] text-fg-mute">{formatCount(m.records, 'records')}</span>
                      <span className="text-[13px] font-semibold">{formatUsageTokens(total)}</span>
                    </div>
                  </div>
                  <div className="h-1.5 rounded-sm bg-surface-hi overflow-hidden mb-1.5">
                    <div className="h-full flex" style={{ width: `${barWidth}%`, minWidth: total > 0 ? 6 : 0 }}>
                      <div className="h-full" style={{ width: `${inputPct}%`, background: color }} />
                      <div className="h-full flex-1" style={{ background: color, opacity: 0.38 }} />
                    </div>
                  </div>
                  <div className="flex flex-wrap gap-x-4 gap-y-1 text-[11px] text-fg-mute">
                    <span>输入 {formatUsageTokens(m.totals.promptTokens)}</span>
                    <span>输出 {formatUsageTokens(m.totals.completionTokens)}</span>
                    <span>{m.sessionCount} 会话</span>
                    {m.estimatedRecords > 0 && <span>{m.estimatedRecords} 估算</span>}
                  </div>
                </div>
              );
            })}
          </div>

          <div className="text-[14px] font-semibold mb-1">工作区用量</div>
          <p className="text-[12px] text-fg-mute mb-3">按工作区汇总 token 消耗</p>
          <div className="rounded-md bg-surface border border-border overflow-hidden">
            {stats.workspaces.length === 0 ? (
              <div className="px-3.5 py-6 text-[12px] text-fg-mute text-center">暂无工作区用量</div>
            ) : stats.workspaces.map((w, i) => {
              const total = w.totals.totalTokens;
              const width = stats.maxWorkspaceTokens > 0 ? (total / stats.maxWorkspaceTokens) * 100 : 0;
              return (
                <div
                  key={`${w.workspaceHash}:${i}`}
                  className={clsx('px-4 py-3', i < stats.workspaces.length - 1 && 'border-b border-border')}
                >
                  <div className="flex items-center justify-between gap-3 mb-2">
                    <div className="min-w-0">
                      <div className="text-[13px] font-medium truncate">{w.workspaceName || 'workspace'}</div>
                      <div className="text-[11px] text-fg-mute truncate">{w.cwd}</div>
                    </div>
                    <div className="text-[13px] font-semibold shrink-0">{formatUsageTokens(total)}</div>
                  </div>
                  <div className="h-1.5 rounded-sm bg-surface-hi overflow-hidden">
                    <div className="h-full bg-accent" style={{ width: `${width}%`, minWidth: total > 0 ? 6 : 0, opacity: 0.85 }} />
                  </div>
                </div>
              );
            })}
          </div>

          <div className="mt-4 px-3.5 py-2.5 rounded-md border border-dashed border-border text-[12px] text-fg-mute text-center">
            {note}
          </div>
        </>
      )}
    </>
  );
}

// ─── 问题反馈 ──────────────────────────────────────────────────────────────

function feedbackSessionOptionLabel(item) {
  const title = sessionDisplayTitle(item, item?.title || item?.summary || item?.id || '');
  const when = relativeTime(item?.updated_at || item?.created_at);
  const workspace = item?.workspaceName || item?.cwd || item?.workspace_hash || '';
  return [title, when, workspace].filter(Boolean).join(' · ');
}

function SectionFeedback() {
  const [feedbackText, setFeedbackText] = useState('');
  const [sessionsRaw, setSessionsRaw] = useState(null);
  const [selectedKey, setSelectedKey] = useState(NO_FEEDBACK_SESSION_KEY);
  const [loadingSessions, setLoadingSessions] = useState(true);
  const [sessionsError, setSessionsError] = useState('');
  const [submitting, setSubmitting] = useState(false);
  const [status, setStatus] = useState(null);

  const sessions = useMemo(
    () => normalizeDesktopFeedbackSessions(sessionsRaw || {}),
    [sessionsRaw],
  );
  const selectedSession = useMemo(
    () => selectedFeedbackSessionFromKey(sessions, selectedKey),
    [sessions, selectedKey],
  );

  const loadSessions = useCallback(() => {
    let cancelled = false;
    setLoadingSessions(true);
    setSessionsError('');
    api.listDesktopFeedbackSessions(20)
      .then((data) => {
        if (!cancelled) setSessionsRaw(data || {});
      })
      .catch((e) => {
        if (!cancelled) setSessionsError(e?.message || String(e));
      })
      .finally(() => {
        if (!cancelled) setLoadingSessions(false);
      });
    return () => { cancelled = true; };
  }, []);

  useEffect(() => loadSessions(), [loadSessions]);

  const submit = async () => {
    if (submitting) return;
    setSubmitting(true);
    setStatus(null);
    try {
      const payload = buildDesktopFeedbackPayload({
        feedbackText,
        selectedSession,
      });
      const result = await api.submitDesktopFeedback(payload);
      const filename = result?.package_filename || 'feedback package';
      setStatus({
        kind: 'ok',
        text: `反馈已上传:${filename}`,
      });
      toast({ kind: 'ok', text: '问题反馈已上传' });
      setFeedbackText('');
      setSelectedKey(NO_FEEDBACK_SESSION_KEY);
    } catch (e) {
      const body = e?.body && typeof e.body === 'object' ? e.body : {};
      const message = lookupErrorMessage(e?.code, body.message || e?.message || String(e));
      setStatus({
        kind: 'err',
        text: `上传失败:${message}`,
        packagePath: body.package_path || '',
      });
      toast({ kind: 'err', text: '问题反馈上传失败' });
    } finally {
      setSubmitting(false);
    }
  };

  return (
    <>
      <h2 className="text-xl font-bold mb-5">问题反馈</h2>

      <div className="rounded-md bg-surface border border-border overflow-hidden">
        <div className="px-4 py-3.5 border-b border-border">
          <div className="text-[14px] font-semibold mb-1">提交反馈</div>
          <p className="text-[12px] text-fg-mute">
            默认附带最近的 desktop 与 daemon 日志。关联某个具体的会话记录将更有助于我们帮您排查问题。
          </p>
        </div>

        <div className="px-4 py-4 space-y-4">
          <label className="block">
            <span className="block text-[12px] font-medium text-fg-2 mb-1.5">反馈内容</span>
            <textarea
              value={feedbackText}
              onChange={(e) => setFeedbackText(e.target.value)}
              rows={5}
              placeholder="描述你遇到的问题"
              className="w-full resize-y min-h-[112px] rounded-md bg-bg border border-border px-3 py-2 text-[13px] text-fg outline-none focus:border-accent focus:ring-1 focus:ring-accent"
            />
          </label>

          <div>
            <div className="flex items-center justify-between mb-1.5">
              <span className="text-[12px] font-medium text-fg-2">最近会话记录</span>
              <button
                type="button"
                onClick={loadSessions}
                disabled={loadingSessions || submitting}
                className="h-6 px-2 rounded-md text-[11px] text-fg-2 bg-surface-hi hover:bg-surface-alt border border-border disabled:opacity-60 transition flex items-center gap-1"
              >
                {loadingSessions ? <span className="ace-spinner" /> : <RefreshIcon size={12} />}
                刷新
              </button>
            </div>
            <div className="flex gap-2">
              <select
                value={selectedKey}
                onChange={(e) => setSelectedKey(e.target.value)}
                disabled={loadingSessions || submitting}
                className="min-w-0 flex-1 h-8 rounded-md bg-bg border border-border px-2 text-[13px] text-fg outline-none focus:border-accent"
              >
                <option value={NO_FEEDBACK_SESSION_KEY}>不附带会话</option>
                {sessions.map((item) => (
                  <option key={feedbackSessionKey(item)} value={feedbackSessionKey(item)}>
                    {feedbackSessionOptionLabel(item)}
                  </option>
                ))}
              </select>
              {selectedKey !== NO_FEEDBACK_SESSION_KEY && (
                <button
                  type="button"
                  onClick={() => setSelectedKey(NO_FEEDBACK_SESSION_KEY)}
                  disabled={submitting}
                  className="shrink-0 h-8 px-2.5 rounded-md text-[12px] border border-border bg-surface hover:bg-surface-hi disabled:opacity-60 transition"
                >
                  清除
                </button>
              )}
            </div>
            {sessionsError ? (
              <div className="mt-2 text-[12px] text-danger">加载会话失败:{sessionsError}</div>
            ) : (
              <div className="mt-2 text-[12px] text-fg-mute">
                {selectedSession
                  ? `将附带:${feedbackSessionOptionLabel(selectedSession)}`
                  : '不会附带会话数据库或会话记录。'}
              </div>
            )}
          </div>

          {status && (
            <div
              className={clsx(
                'rounded-md border px-3 py-2 text-[12px]',
                status.kind === 'ok'
                  ? 'border-ok-border bg-ok-bg text-ok'
                  : 'border-danger bg-surface text-danger',
              )}
            >
              <div>{status.text}</div>
              {status.packagePath && (
                <div className="mt-1 text-[11px] break-all">{status.packagePath}</div>
              )}
            </div>
          )}

          <div className="flex items-center justify-end">
            <button
              type="button"
              onClick={submit}
              disabled={submitting}
              className="h-8 px-3 rounded-md text-[13px] font-medium bg-accent text-white hover:opacity-95 disabled:opacity-60 transition flex items-center gap-1.5"
            >
              {submitting ? <span className="ace-spinner" /> : <VsIcon name="send" size={13} />}
              提交反馈
            </button>
          </div>
        </div>
      </div>
    </>
  );
}

// ─── 模型 ────────────────────────────────────────────────────────────────
// 列表优先的模型管理页面保留在聚焦组件中，设置导航本身保持稳定。
function SectionModel({ onModelProfileUpdated }) {
  return <ModelSettingsSection onModelProfileUpdated={onModelProfileUpdated} />;
}
