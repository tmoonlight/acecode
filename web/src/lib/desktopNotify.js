// 桌面端系统通知封装。
//
// Windows 与 macOS native 后端共享这里的 payload 和抑制规则。
// 浏览器直连 daemon(无 acecode-desktop 桌面壳)模式下,window.aceDesktop_notify
// 不存在 → 所有 notify 调用 no-op,这与 search palette 跨 workspace bridge 的
// 降级模式一致。
//
// 唯一弹窗规则:
//   - master enabled 开启
//   - 事件是主任务 completion
//   - 整个 ACECode 窗口失焦
// permission / question 一律留在页面内处理;窗口只要有焦点,无论当前打开
// 哪个会话、workspace 或页面都不发 native completion 通知。旧的 per-type
// 和 suppress_when_focused 配置字段仅保留后端兼容,不再参与运行时判定。
//
// payload 构造抽到 buildNotificationPayload — 长文本截断 80 字 + 省略号,
// 空文本回退到默认占位,纯函数,可测。

const NOTIFICATION_BODY_LIMIT = 80;

// WebView2 / 壳内 focus 有时会短暂落到 native 标题栏,document.hasFocus()
// 会闪 false。用 focus/blur 跟踪作兜底,hasFocus 优先。
let hostWindowFocused = true;

export function noteHostWindowFocus(focused) {
  hostWindowFocused = !!focused;
}

export function isHostWindowFocused(doc = typeof document !== 'undefined' ? document : null) {
  if (doc && typeof doc.visibilityState === 'string' && doc.visibilityState === 'hidden') {
    return false;
  }
  if (doc && typeof doc.hasFocus === 'function' && doc.hasFocus()) {
    return true;
  }
  return hostWindowFocused;
}

function defaultCompletionBody() {
  return '(空白回合)';
}

function notificationsEnabled(cfg) {
  if (!cfg || typeof cfg !== 'object') return true;
  return cfg.enabled !== false;
}

// codepoint-aware 截断(避免在多字节字符中间断开)。
export function truncateForNotification(text, limit = NOTIFICATION_BODY_LIMIT) {
  const s = String(text == null ? '' : text);
  // Array.from 按 codepoint 拆,中文 / emoji 不会按 UTF-16 半个 surrogate 截。
  const codepoints = Array.from(s);
  if (codepoints.length <= limit) return s;
  return codepoints.slice(0, limit).join('') + '…';
}

export function buildNotificationPayload({
  type,
  sessionId,
  workspaceHash = '',
  sessionTitle = '',
  bodyText = '',
}) {
  const safeType = type === 'completion'
    ? 'completion'
    : (type === 'permission' ? 'permission' : 'question');
  const titlePrefix = safeType === 'completion'
    ? '已完成'
    : (safeType === 'permission' ? '需要你授权' : '需要你回答');
  const titleSuffix = sessionTitle && String(sessionTitle).trim() ? String(sessionTitle).trim() : '会话';
  const title = `${titlePrefix} · ${titleSuffix}`;
  const trimmedBody = String(bodyText || '').trim();
  const body = trimmedBody
    ? truncateForNotification(trimmedBody)
    : (safeType === 'completion' ? defaultCompletionBody() : '');
  return {
    id: `${safeType}-${sessionId || 'unknown'}-${Date.now()}`,
    workspace_hash: workspaceHash || '',
    session_id: sessionId || '',
    title,
    body,
  };
}

export function notificationBodyFromEvent(type, payload = {}) {
  if (type === 'completion') {
    return String(
      payload.final_assistant_text
      || payload.content
      || payload.text
      || '',
    );
  }
  if (type === 'permission') {
    const tool = String(payload.tool || '').trim();
    return tool ? `工具 ${tool} 正在等待权限确认` : '正在等待权限确认';
  }
  const questions = Array.isArray(payload.questions) ? payload.questions : [];
  return String(
    questions[0]?.question
    || payload.question
    || payload.prompt
    || '正在等待你的回答',
  );
}

// Completion toasts represent user-facing main sessions. A child session is
// recognized either directly from its terminal event or through the App-owned
// subagent directory; unknown ownership remains eligible rather than guessing.
export function shouldNotifySessionCompletion({
  sessionId,
  parentSessionId = '',
  ownerSessionId = '',
} = {}) {
  const sid = String(sessionId || '').trim();
  if (!sid) return false;
  const explicitParentId = String(parentSessionId || '').trim();
  const registeredOwnerId = String(ownerSessionId || '').trim();
  const resolvedOwnerId = explicitParentId || registeredOwnerId;
  return !resolvedOwnerId || resolvedOwnerId === sid;
}

export function shouldSuppress(payload, hasFocus, cfg) {
  if (!notificationsEnabled(cfg)) return true;
  // payload.id 形如 "question-..." / "completion-...",取首段判类型。
  const type = String(payload?.id || '').split('-')[0];
  if (type !== 'completion') return true;
  return hasFocus === true;
}

function bridgeAvailable() {
  return typeof window !== 'undefined'
    && typeof window.aceDesktop_notify === 'function';
}

// 派发到 native 桥。前端必须**自己**先调 shouldSuppress 判抑制 — 这里不替你过滤,
// 让调用方明确知道事件何时被吞。返回 boolean 指示是否真投递了。
export function notify(payload) {
  if (!bridgeAvailable()) return false;
  if (!payload || (!payload.title && !payload.body)) return false;
  try {
    window.aceDesktop_notify(JSON.stringify(payload));
    return true;
  } catch {
    return false;
  }
}

// 触发"切到某 session"。toast 点击与代码主动调用走同一 native click_handler,
// UX 与 webview 内 SearchPalette 选中等场景一致。
export function focusSession(workspaceHash, sessionId) {
  if (!sessionId) return false;
  if (typeof window === 'undefined'
      || typeof window.aceDesktop_focusSession !== 'function') {
    return false;
  }
  try {
    window.aceDesktop_focusSession(JSON.stringify({
      workspace_hash: workspaceHash || '',
      session_id: sessionId,
    }));
    return true;
  } catch {
    return false;
  }
}

// 一站式入口:构造 payload + 判抑制 + 投递。应用级 WS 监听器调这一个。
// hasFocus 可由调用方传入;省略时走 isHostWindowFocused()
// (document.hasFocus + focus/blur 兜底)。
export function maybeNotify({
  type,
  sessionId,
  workspaceHash,
  sessionTitle,
  bodyText,
  hasFocus,
  cfg,
}) {
  if (!bridgeAvailable()) return false;
  const payload = buildNotificationPayload({
    type, sessionId, workspaceHash, sessionTitle, bodyText,
  });
  const focused = typeof hasFocus === 'boolean' ? hasFocus : isHostWindowFocused();
  if (shouldSuppress(payload, focused, cfg)) return false;
  return notify(payload);
}
