// 顶栏右侧的会话操作按钮组。所有页面共用这一份(可用性见 lib/sessionTitleActions.js),
// 由 SessionTitleBar 挂进顶栏;按钮必须是 .ace-session-title-actions 的直接子元素,
// 顶栏尺寸与悬停样式按 `> button` 选择。

import { clsx } from '../lib/format.js';
import { VsIcon } from './Icon.jsx';
import { IosSimulatorIcon } from './IosSimulatorPanel.jsx';

function TitleActionButton({ label, state, pressed = null, buttonRef, onClick, children, ...buttonProps }) {
  const enabled = state.enabled;
  const toggle = typeof pressed === 'boolean';
  const active = toggle && enabled && pressed;
  return (
    <button
      {...buttonProps}
      ref={buttonRef}
      type="button"
      title={label}
      aria-label={label}
      aria-pressed={toggle ? active : undefined}
      disabled={!enabled}
      onClick={onClick}
      className={clsx(
        'w-7 h-7 rounded-md flex items-center justify-center shrink-0 transition focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-accent/25',
        !enabled
          ? 'text-fg-mute opacity-35 cursor-not-allowed'
          : active
            ? 'bg-accent-bg text-accent hover:bg-accent-bg'
            : 'text-fg-mute hover:bg-surface-hi hover:text-fg',
      )}
    >
      {children}
    </button>
  );
}

export function SessionTitleActions({
  states,
  summaryPanelOpen = false,
  onToggleSummaryPanel,
  trajectoryOpen = false,
  onToggleTrajectory,
  lspIndicator = null,
  sideChatOpen = false,
  sideChatButtonRef,
  onToggleSideChat,
  onOpenBrowser,
  onOpenIosSimulator,
  sessionMenuButtonRef,
  sessionMenuAttributes,
  onOpenSessionMenu,
}) {
  return (
    <>
      {states.summary.visible && (
        <TitleActionButton
          label={summaryPanelOpen ? '收起摘要面板' : '展开摘要面板'}
          state={states.summary}
          pressed={summaryPanelOpen}
          onClick={onToggleSummaryPanel}
          data-sidebar-toggle="summary"
        >
          <VsIcon name="properties" size={16} />
        </TitleActionButton>
      )}
      {states.summary.visible && (
        // 摘要面板开关是全局布局开关,与右边这些只作用于当前会话的按钮分开。
        <span aria-hidden="true" className="ace-summary-toggle-divider w-px h-4 mx-1 shrink-0 bg-border" />
      )}
      <TitleActionButton
        label={trajectoryOpen ? 'Conversation' : 'Trajectory'}
        state={states.trajectory}
        pressed={trajectoryOpen}
        onClick={onToggleTrajectory}
      >
        <VsIcon name="trajectory" size={16} />
      </TitleActionButton>
      {states.session && lspIndicator}
      <TitleActionButton
        label="侧边聊天"
        state={states.sideChat}
        pressed={sideChatOpen}
        buttonRef={sideChatButtonRef}
        onClick={onToggleSideChat}
      >
        <VsIcon name="chat" size={16} />
      </TitleActionButton>
      {states.browser.visible && (
        <TitleActionButton label="打开浏览器" state={states.browser} onClick={() => onOpenBrowser?.()}>
          <VsIcon name="globe" size={16} />
        </TitleActionButton>
      )}
      {states.iosSimulator.visible && (
        <TitleActionButton label="打开 iOS 模拟器" state={states.iosSimulator} onClick={onOpenIosSimulator}>
          <IosSimulatorIcon size={16} />
        </TitleActionButton>
      )}
      <TitleActionButton
        label="会话菜单"
        state={states.sessionMenu}
        buttonRef={sessionMenuButtonRef}
        onClick={onOpenSessionMenu}
        aria-haspopup="menu"
        {...(states.sessionMenu.enabled ? sessionMenuAttributes : null)}
      >
        <VsIcon name="ellipsisVertical" size={16} />
      </TitleActionButton>
    </>
  );
}
