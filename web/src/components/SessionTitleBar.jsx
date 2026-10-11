import { isDismissShortcut } from '../lib/shortcutBindings.js';
import { useRef, useState } from 'react';
import { createPortal } from 'react-dom';
import { VsIcon } from './Icon.jsx';
import { tr } from '../i18n/index.js';
import { SESSION_HEADER_CONTEXT_MENU_DELEGATE } from '../lib/desktopContextMenu.js';
import { useSessionTitleAnimation } from '../lib/useSessionTitleAnimation.js';
import { SessionTitleText } from './SessionTitleText.jsx';

// 顶栏「重命名」就地编辑:Enter / 失焦提交,Esc 放弃;输入法合成中的 Enter 不提交。
// 挂载时全选原标题,直接输入即替换。
function SessionTitleRenameInput({ title, onCommit, onCancel }) {
  const [draft, setDraft] = useState(title || '');
  const doneRef = useRef(false);
  const finish = (commit) => {
    if (doneRef.current) return;
    doneRef.current = true;
    if (commit) onCommit?.(draft);
    else onCancel?.();
  };
  return (
    <input
      autoFocus
      value={draft}
      aria-label="会话标题"
      data-session-title-rename-input="true"
      onFocus={(e) => e.target.select()}
      onChange={(e) => setDraft(e.target.value)}
      onBlur={() => finish(true)}
      onKeyDown={(e) => {
        if (e.nativeEvent?.isComposing || e.keyCode === 229) return;
        if (e.key === 'Enter') {
          e.preventDefault();
          finish(true);
        } else if (isDismissShortcut(e)) {
          e.preventDefault();
          e.stopPropagation();
          finish(false);
        }
      }}
      className="ace-session-title-rename-input min-w-0 w-[420px] max-w-full h-6 px-1.5 rounded border border-accent bg-surface text-[13px] font-semibold text-fg outline-none"
    />
  );
}

// ChatView owns the session state; only its presentation moves into window chrome.
export function SessionTitleBar({
  titleTarget,
  actionsTarget,
  title,
  sessionRef,
  titleReady = true,
  workspaceLabel,
  remoteControlBound,
  renaming = false,
  onRenameCommit,
  onRenameCancel,
  labelRef,
  children,
}) {
  const titleAnimation = useSessionTitleAnimation(sessionRef, title, titleReady);
  const label = (
    <div ref={labelRef} className="ace-session-title flex items-center gap-2 min-w-0">
      <span className="ace-session-title-text flex min-w-0 items-center gap-1.5">
        {remoteControlBound && (
          <VsIcon name="computer" size={14} className="text-accent"
            alt={tr('remoteControl.connectedSession')} data-remote-control-session-icon="true" />
        )}
        {renaming ? (
          <SessionTitleRenameInput title={title} onCommit={onRenameCommit} onCancel={onRenameCancel} />
        ) : (
          <span className="text-[13px] font-semibold text-fg truncate" title={title}>
            <SessionTitleText title={title} animation={titleAnimation} />
          </span>
        )}
      </span>
      {workspaceLabel && (
        <span className="ace-session-workspace shrink-0 px-2.5 py-0.5 rounded-full text-[10px] font-medium border bg-surface-hi text-fg-mute border-transparent"
          title={workspaceLabel} data-session-workspace-label="true">
          {workspaceLabel}
        </span>
      )}
    </div>
  );
  const actions = <div className="ace-session-title-actions flex items-center gap-1 shrink-0">{children}</div>;
  if (titleTarget && actionsTarget) {
    return <>{createPortal(label, titleTarget)}{createPortal(actions, actionsTarget)}</>;
  }
  return (
    <div className="ace-session-header h-9 px-3 flex items-center justify-between bg-surface shrink-0 gap-2"
      data-desktop-context-menu-delegate={SESSION_HEADER_CONTEXT_MENU_DELEGATE}>
      {label}{actions}
    </div>
  );
}
