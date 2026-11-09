// 顶栏右侧那组会话操作按钮(摘要面板 / 轨迹 / 侧边聊天 / 浏览器 / iOS 模拟器 / 会话菜单)
// 的可用性判定。会话页、新建任务首页、定时任务页、专家组件页渲染的是同一组按钮,
// 位置与数量不随页面变化;没有会话可作用时按钮禁用,而不是从顶栏消失。
//
// 「显示」只取决于宿主能力(Desktop 原生浏览器、macOS 模拟器宿主)和外层是否接了
// 摘要面板开关;「可用」才看当前页面是不是一个真实会话。

export function sessionTitleActionStates({
  sessionId = '',
  auxiliaryPage = false,
  readOnly = false,
  summaryToggle = false,
  agentBrowser = false,
  iosSimulator = false,
} = {}) {
  // 定时任务页 / 专家组件页(ChatView 的 children)即使带着上一次的会话 ref,也不是会话视图。
  const session = !!sessionId && !auxiliaryPage;
  return {
    session,
    summary: { visible: !!summaryToggle, enabled: session },
    trajectory: { visible: true, enabled: session },
    sideChat: { visible: true, enabled: session && !readOnly },
    browser: { visible: !!agentBrowser, enabled: session },
    iosSimulator: { visible: !!iosSimulator, enabled: session },
    sessionMenu: { visible: true, enabled: session },
  };
}
