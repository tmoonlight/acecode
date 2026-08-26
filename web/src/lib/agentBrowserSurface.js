const CONTENT_STATES = new Set([
  'empty',
  'loading',
  'live',
  'navigation_error',
  'process_failed',
]);

const NAVIGATION_FAILURES = Object.freeze({
  certificate: {
    title: '无法安全打开此网站',
    detail: '网站的安全证书无效或与当前地址不匹配。',
  },
  server_unreachable: {
    title: '无法连接到此网站',
    detail: '请检查网站地址和网络连接，然后重试。',
  },
  timeout: {
    title: '页面响应超时',
    detail: '网站等待时间过长，请稍后重试。',
  },
  invalid_response: {
    title: '网站返回了无效响应',
    detail: '此网站暂时无法正确响应，请稍后重试。',
  },
  connection_aborted: {
    title: '连接已中断',
    detail: '网页连接意外中断，请检查网络后重试。',
  },
  connection_reset: {
    title: '连接已重置',
    detail: '网站关闭了当前连接，请重新加载页面。',
  },
  disconnected: {
    title: '网络连接已断开',
    detail: '请恢复网络连接，然后重试。',
  },
  cannot_connect: {
    title: '无法连接到此网站',
    detail: '请检查网站地址和网络连接，然后重试。',
  },
  name_not_resolved: {
    title: '找不到此网站',
    detail: '请检查地址是否正确，然后重试。',
  },
  redirect_failed: {
    title: '页面重定向失败',
    detail: '网站无法完成跳转，请稍后重试。',
  },
  app_transport_security: {
    title: 'macOS 已阻止此连接',
    detail: '此页面不符合 App Transport Security 的安全连接要求。',
  },
  secure_connection_failed: {
    title: '无法建立安全连接',
    detail: 'TLS 安全连接失败，请检查网站和网络安全配置。',
  },
  authentication_required: {
    title: '此页面需要身份验证',
    detail: '请确认登录信息后重新加载页面。',
  },
  proxy_authentication_required: {
    title: '代理服务器需要身份验证',
    detail: '请检查系统代理设置后重新加载页面。',
  },
});

const PROCESS_FAILURES = Object.freeze({
  out_of_memory: {
    title: '页面内存不足',
    detail: '请关闭不需要的标签页或其他占用内存的应用，然后重试。',
  },
  unresponsive: {
    title: '页面没有响应',
    detail: '此页面已停止响应，你可以重试或返回上一页。',
  },
  browser_process_exited: {
    title: '浏览器组件已停止',
    detail: '浏览器组件意外停止，请重试加载此页面。',
  },
  render_process_exited: {
    title: '页面进程已停止',
    detail: '此页面意外退出，请重新加载。',
  },
});

export function agentBrowserContentState(state = {}) {
  if (state.supported === false || (!state.ready && state.error)) return 'unavailable';
  if (!state.ready) return 'initializing';
  if (CONTENT_STATES.has(state.content_state)) return state.content_state;
  if (state.loading) return 'loading';
  if (state.error) return 'navigation_error';
  return state.url && state.url !== 'about:blank' ? 'live' : 'empty';
}

export function agentBrowserShowsNativePage(state = {}) {
  return agentBrowserContentState(state) === 'live';
}

export function agentBrowserSurfacePresentation(state = {}) {
  const kind = agentBrowserContentState(state);
  const common = {
    kind,
    canGoBack: !!state.can_go_back,
    canRetry: false,
    icon: 'globe',
    role: 'status',
    tone: 'neutral',
  };

  if (kind === 'live') return { ...common, hidden: true, title: '', detail: '' };
  if (kind === 'empty') {
    return {
      ...common,
      title: '浏览器',
      detail: '使用“将元素添加到聊天”在聊天提示中引用 UI 元素。',
    };
  }
  if (kind === 'loading') {
    return {
      ...common,
      title: '正在打开页面',
      detail: '请稍候，网页正在加载。',
    };
  }
  if (kind === 'initializing') {
    return {
      ...common,
      title: '正在启动浏览器',
      detail: '正在连接 Windows WebView2。',
    };
  }
  if (kind === 'unavailable') {
    return {
      ...common,
      icon: 'warning',
      role: 'alert',
      tone: 'error',
      title: '浏览器不可用',
      detail: 'Windows WebView2 暂时无法启动，请重新启动 ACECode 后再试。',
    };
  }
  if (kind === 'navigation_error') {
    const failure = NAVIGATION_FAILURES[state.failure_kind] || {
      title: '无法打开此页面',
      detail: '请检查网站地址和网络连接，然后重试。',
    };
    const diagnostic = String(state.diagnostic || '').trim();
    return {
      ...common,
      ...failure,
      ...(diagnostic ? { diagnostic } : {}),
      canRetry: true,
      icon: 'warning',
      role: 'alert',
      tone: 'error',
    };
  }

  const failure = PROCESS_FAILURES[state.failure_kind] || {
    title: '页面暂时不可用',
    detail: '页面组件意外停止，请重新加载。',
  };
  return {
    ...common,
    ...failure,
    canRetry: true,
    icon: 'warning',
    role: 'alert',
    tone: 'error',
  };
}
