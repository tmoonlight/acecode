import assert from 'node:assert/strict';
import {
  agentBrowserContentState,
  agentBrowserShowsNativePage,
  agentBrowserSurfacePresentation,
} from './agentBrowserSurface.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('Agent Browser empty tabs use the React welcome surface', () => {
  const state = {
    supported: true,
    ready: true,
    content_state: 'empty',
    url: 'about:blank',
  };
  assert.equal(agentBrowserContentState(state), 'empty');
  assert.equal(agentBrowserShowsNativePage(state), false);
  assert.deepEqual(
    agentBrowserSurfacePresentation(state),
    {
      kind: 'empty',
      canGoBack: false,
      canRetry: false,
      icon: 'globe',
      role: 'status',
      tone: 'neutral',
      title: '浏览器',
      detail: '使用“将元素添加到聊天”在聊天提示中引用 UI 元素。',
    },
  );
});

run('Agent Browser exposes native pixels only for successfully loaded pages', () => {
  assert.equal(agentBrowserShowsNativePage({
    supported: true, ready: true, content_state: 'loading', url: 'https://example.com',
  }), false);
  assert.equal(agentBrowserShowsNativePage({
    supported: true, ready: true, content_state: 'navigation_error',
  }), false);
  assert.equal(agentBrowserShowsNativePage({
    supported: true, ready: true, content_state: 'process_failed',
  }), false);
  assert.equal(agentBrowserShowsNativePage({
    supported: true, ready: true, content_state: 'live', url: 'https://example.com',
  }), true);
});

run('Agent Browser maps navigation failures to concise recovery states', () => {
  const missing = agentBrowserSurfacePresentation({
    supported: true,
    ready: true,
    content_state: 'navigation_error',
    failure_kind: 'name_not_resolved',
    can_go_back: true,
  });
  assert.equal(missing.title, '找不到此网站');
  assert.equal(missing.canRetry, true);
  assert.equal(missing.canGoBack, true);
  assert.equal(missing.role, 'alert');

  const generic = agentBrowserSurfacePresentation({
    supported: true,
    ready: true,
    content_state: 'navigation_error',
    failure_kind: 'unexpected',
    diagnostic: 'NSError domain=NSURLErrorDomain code=-1013\nDescription: Authentication required',
  });
  assert.equal(generic.title, '无法打开此页面');
  assert.doesNotMatch(generic.detail, /WebView2|status|ERR_/i);
  assert.match(generic.diagnostic, /NSURLErrorDomain code=-1013/);

  const ats = agentBrowserSurfacePresentation({
    supported: true,
    ready: true,
    content_state: 'navigation_error',
    failure_kind: 'app_transport_security',
    diagnostic: '  NSError domain=NSURLErrorDomain code=-1022  ',
  });
  assert.equal(ats.title, 'macOS 已阻止此连接');
  assert.match(ats.detail, /App Transport Security/);
  assert.equal(ats.diagnostic, 'NSError domain=NSURLErrorDomain code=-1022');
  assert.equal('diagnostic' in missing, false);
});

run('Agent Browser gives OOM and renderer failures dedicated surfaces', () => {
  const oom = agentBrowserSurfacePresentation({
    supported: true,
    ready: true,
    content_state: 'process_failed',
    failure_kind: 'out_of_memory',
  });
  assert.equal(oom.title, '页面内存不足');
  assert.match(oom.detail, /关闭不需要的标签页/);
  assert.equal(oom.canRetry, true);

  const renderer = agentBrowserSurfacePresentation({
    supported: true,
    ready: true,
    content_state: 'process_failed',
    failure_kind: 'render_process_exited',
  });
  assert.equal(renderer.title, '页面进程已停止');
});

run('Agent Browser infers content state for an older native snapshot', () => {
  assert.equal(agentBrowserContentState({ ready: true, url: 'about:blank' }), 'empty');
  assert.equal(agentBrowserContentState({ ready: true, url: 'https://example.com' }), 'live');
  assert.equal(agentBrowserContentState({ ready: true, loading: true }), 'loading');
});
