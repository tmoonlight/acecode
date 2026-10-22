// Agent Browser 新标签页「推荐」(lib/agentBrowserTopSites.js)的纯逻辑测试。
//
// 场景:
// 1. 地址归一:只认 http(s),去掉 #锚点;标题去掉 Desktop 的默认「新标签页」;
//    超大 / 非法图标不上报。
// 2. 访问跟踪器:地址变化才算新访问,标题 / 图标后到只做补充,刷新同一地址不计数。
// 3. 前端加载前就已打开的页面(启动时第一次见到就已 live)不重复计数;
//    对账完成后新出现的页面照常计数。
// 4. 推荐接口响应归一、移除单项。
// 5. App 级记录器:只装一次、用 Desktop 页面列表打底、上报失败不抛出。
import assert from 'node:assert/strict';
import {
  AGENT_BROWSER_TOP_SITE_FAVICON_MAX_LENGTH,
  createAgentBrowserVisitTracker,
  installAgentBrowserVisitRecorder,
  normalizeTopSitesResponse,
  topSiteFavicon,
  topSiteHost,
  topSiteTitle,
  topSiteUrlKey,
  withoutTopSite,
} from './agentBrowserTopSites.js';
import { AGENT_BROWSER_STATE_EVENT } from './agentBrowser.js';

async function run(name, fn) {
  try {
    await fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

function state(pageId, overrides = {}) {
  return {
    page_id: pageId,
    url: 'about:blank',
    title: '新标签页',
    favicon: '',
    content_state: 'empty',
    ...overrides,
  };
}

function live(pageId, url, overrides = {}) {
  return state(pageId, { url, title: '', content_state: 'live', ...overrides });
}

await run('top site url key keeps http(s) only and drops the fragment', () => {
  assert.equal(topSiteUrlKey('HTTPS://Example.com/a?b=1#top'), 'https://Example.com/a?b=1');
  assert.equal(topSiteUrlKey('http://127.0.0.1:5173/'), 'http://127.0.0.1:5173/');
  for (const bad of ['about:blank', 'file:///C:/a.html', 'data:text/html,hi', 'https://', 'https://a b/', '']) {
    assert.equal(topSiteUrlKey(bad), '', `${bad} 不能成为推荐`);
  }
  assert.equal(topSiteHost('https://user@host.example:8443/p'), 'host.example:8443');
});

await run('titles drop the desktop default and control characters; favicons are bounded', () => {
  assert.equal(topSiteTitle('新标签页'), '', 'Desktop 在页面没有标题时填的默认标题不是网站标题');
  assert.equal(topSiteTitle(' 第一行\n第二行 '), '第一行 第二行');
  assert.equal(topSiteFavicon('javascript:alert(1)'), '');
  assert.equal(topSiteFavicon('https://cdn.example/f.ico'), 'https://cdn.example/f.ico');
  assert.equal(topSiteFavicon(`data:image/png;base64,${'A'.repeat(AGENT_BROWSER_TOP_SITE_FAVICON_MAX_LENGTH)}`), '');
});

await run('tracker counts a navigation once and sends later title/favicon as metadata', () => {
  const tracker = createAgentBrowserVisitTracker();
  tracker.seed([]);
  assert.equal(tracker.observe(state('p1')), null, '空白页不是访问');
  assert.equal(tracker.observe(state('p1', { url: 'https://a.example/', content_state: 'loading' })), null);
  assert.deepEqual(tracker.observe(live('p1', 'https://a.example/')), {
    url: 'https://a.example/', title: '', favicon: '', new_visit: true,
  });
  assert.deepEqual(tracker.observe(live('p1', 'https://a.example/', { title: 'A' })), {
    url: 'https://a.example/', title: 'A', favicon: '', new_visit: false,
  });
  assert.deepEqual(
    tracker.observe(live('p1', 'https://a.example/', { title: 'A', favicon: 'https://a.example/f.ico' })),
    { url: 'https://a.example/', title: '', favicon: 'https://a.example/f.ico', new_visit: false },
    '只带变化的字段,未变的标题不重发',
  );
  assert.equal(tracker.observe(live('p1', 'https://a.example/', { title: 'A' })), null,
    '标题没变、图标暂时为空(刷新中)不上报');
});

await run('reloading the same address does not count; a new address or SPA route does', () => {
  const tracker = createAgentBrowserVisitTracker();
  tracker.seed([]);
  tracker.observe(state('p1', { content_state: 'loading', url: 'https://a.example/' }));
  assert.equal(tracker.observe(live('p1', 'https://a.example/'))?.new_visit, true);
  tracker.observe(state('p1', { content_state: 'loading', url: 'https://a.example/' }));
  assert.equal(tracker.observe(live('p1', 'https://a.example/')), null, '刷新不计数');
  assert.equal(tracker.observe(live('p1', 'https://a.example/#section')), null, '只改锚点不计数');
  assert.equal(tracker.observe(live('p1', 'https://a.example/docs'))?.new_visit, true,
    'pushState 换路由没有 loading 也要计数');
});

await run('pages already open before the frontend loaded are seeded, not counted', () => {
  const tracker = createAgentBrowserVisitTracker();
  // 对账返回前:第一次见到就已 live 的页面视为早已打开。
  assert.equal(tracker.observe(live('early', 'https://early.example/', { title: 'Early' })), null);
  tracker.seed([
    { page_id: 'listed', url: 'https://listed.example/', content_state: 'live', title: 'Listed' },
    { page_id: 'early', url: 'https://other.example/', content_state: 'live' },
  ]);
  assert.equal(tracker.observe(live('listed', 'https://listed.example/', { title: 'Listed' })), null,
    '刷新 / 切工作区后已打开页面的状态事件不能再计一次');
  assert.equal(tracker.observe(live('early', 'https://early.example/', { title: 'Early' })), null,
    '打底不覆盖已经见过的页面');
  // 对账之后才出现的页面,即使第一次见到就已 live 也是新打开的。
  assert.equal(tracker.observe(live('fresh', 'https://fresh.example/'))?.new_visit, true);
  // 关闭后同一 page id 重新出现按新页面处理。
  tracker.observe(state('listed', { closed: true }));
  assert.equal(tracker.observe(live('listed', 'https://listed.example/'))?.new_visit, true);
});

await run('failed navigations are not visits but the retry is', () => {
  const tracker = createAgentBrowserVisitTracker();
  tracker.seed([]);
  assert.equal(tracker.observe(state('p1', {
    url: 'https://down.example/', content_state: 'navigation_error',
  })), null);
  assert.equal(tracker.observe(live('p1', 'https://down.example/'))?.new_visit, true);
});

await run('top sites response is normalized and dismissing removes one entry', () => {
  const sites = normalizeTopSitesResponse({
    sites: [
      { url: 'https://a.example/', title: 'A', favicon: 'data:image/png;base64,AA' },
      { url: 'https://a.example/#dup', title: 'dup' },
      { url: 'http://127.0.0.1:5173/', title: '' },
      { url: 'javascript:alert(1)', title: 'bad' },
    ],
  });
  assert.deepEqual(sites, [
    { url: 'https://a.example/', title: 'A', favicon: 'data:image/png;base64,AA' },
    { url: 'http://127.0.0.1:5173/', title: '127.0.0.1:5173', favicon: '' },
  ]);
  assert.deepEqual(normalizeTopSitesResponse(null), []);
  assert.deepEqual(withoutTopSite(sites, 'https://a.example/#x').map((s) => s.url), ['http://127.0.0.1:5173/']);
  assert.equal(withoutTopSite(sites, 'https://none.example/'), sites, '没有可移除的项时保持引用');
});

function fakeDesktopWindow() {
  const listeners = new Map();
  return {
    listeners,
    __ACECODE_DESKTOP_SHELL__: true,
    __ACECODE_OS__: 'windows',
    aceDesktop_agentBrowserGetState: () => '{}',
    aceDesktop_agentBrowserSetLayout: () => '{}',
    aceDesktop_agentBrowserCreatePage: () => '{}',
    addEventListener(type, fn) { listeners.set(type, fn); },
    removeEventListener(type, fn) { if (listeners.get(type) === fn) listeners.delete(type); },
  };
}

await run('app recorder installs once, seeds from the page list and swallows report failures', async () => {
  assert.equal(typeof installAgentBrowserVisitRecorder(() => {}, {}), 'function',
    '非 Desktop 环境返回空 dispose');

  const win = fakeDesktopWindow();
  const reported = [];
  const dispose = installAgentBrowserVisitRecorder(
    async (visit) => {
      reported.push(visit);
      if (visit.url.includes('fail')) throw new Error('daemon down');
    },
    win,
    {
      listPages: async () => ({
        ok: true,
        pages: [{ page_id: 'old', url: 'https://old.example/', content_state: 'live' }],
      }),
    },
  );
  const second = installAgentBrowserVisitRecorder(() => {}, win);
  assert.equal(win.listeners.size, 1, '重复安装不挂第二个监听器');
  await new Promise((resolve) => setTimeout(resolve, 0));

  const emit = (detail) => win.listeners.get(AGENT_BROWSER_STATE_EVENT)({ detail });
  emit(live('old', 'https://old.example/', { title: 'Old' }));
  emit(live('new', 'https://fail.example/'));
  emit(live('new2', 'https://ok.example/'));
  await new Promise((resolve) => setTimeout(resolve, 0));
  assert.deepEqual(reported.map((v) => [v.url, v.new_visit]), [
    ['https://old.example/', false],
    ['https://fail.example/', true],
    ['https://ok.example/', true],
  ], '已打开的页面只补标题不计数;上报失败不影响后续上报');

  second();
  assert.equal(win.listeners.size, 1);
  dispose();
  assert.equal(win.listeners.size, 0);
});
