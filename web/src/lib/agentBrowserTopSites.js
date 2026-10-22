// Agent Browser 新标签页「推荐」:常访问网站。
//
// 对照 Codex 桌面版(thread-app-shell-chrome 的 Suggested 区):它读 Chromium 内核的
// TopSites,取前 4 个排成一行,悬停出现 × 可移除并能撤销。ACECode 的 Agent Browser
// 是 WebView2 / WKWebView,没有浏览历史接口,所以由这里在页面加载完成时把访问
// 上报给 daemon(`POST /api/agent-browser/visits`),daemon 存在数据目录里排序。
// Desktop 每次启动 daemon 端口会变,localStorage 跨启动保不住,不能存在前端。
import {
  AGENT_BROWSER_STATE_EVENT,
  hasNativeAgentBrowser,
  listAgentBrowserPages,
} from './agentBrowser.js';

export const AGENT_BROWSER_TOP_SITES_LIMIT = 4;
// 与 daemon 的 kMaxFaviconDataUrlBytes 一致:更大的图标不上传,推荐退回地球图标。
export const AGENT_BROWSER_TOP_SITE_FAVICON_MAX_LENGTH = 64 * 1024;
const MAX_URL_LENGTH = 2048;

function text(value) {
  return value == null ? '' : String(value);
}

// 只认 http / https,去掉 #fragment;其它返回空串(about:blank、file:、data: 都不记)。
export function topSiteUrlKey(url) {
  let value = text(url).trim();
  const hash = value.indexOf('#');
  if (hash >= 0) value = value.slice(0, hash);
  if (!value || value.length > MAX_URL_LENGTH || /[\s\u0000-\u001f\u007f]/.test(value)) return '';
  const match = /^(https?):\/\/([^/?#]*)/i.exec(value);
  if (!match) return '';
  const authority = match[2].slice(match[2].lastIndexOf('@') + 1);
  if (!authority) return '';
  return match[1].toLowerCase() + value.slice(match[1].length);
}

export function topSiteTitle(title) {
  const value = text(title).replace(/[\u0000-\u001f\u007f]+/g, ' ').trim();
  // Desktop 在页面没有标题时填的默认标题(kAgentBrowserDefaultTitle),不是网站标题。
  // 必须是比较里的字面量:i18n 编译插件会把模块级常量里的中文换成译文。
  return value === '新标签页' ? '' : value;
}

export function topSiteFavicon(favicon) {
  const value = text(favicon);
  if (!value || value.length > AGENT_BROWSER_TOP_SITE_FAVICON_MAX_LENGTH) return '';
  return /^(https?:\/\/|data:image\/)/i.test(value) ? value : '';
}

// 推荐没有标题时显示主机名(去掉用户信息,保留端口)。
export function topSiteHost(url) {
  const match = /^[a-z][a-z0-9+.-]*:\/\/([^/?#]*)/i.exec(text(url));
  if (!match) return text(url);
  return match[1].slice(match[1].lastIndexOf('@') + 1);
}

export function normalizeTopSitesResponse(response) {
  const sites = Array.isArray(response?.sites) ? response.sites : [];
  const seen = new Set();
  const out = [];
  for (const site of sites) {
    const url = topSiteUrlKey(site?.url);
    if (!url || seen.has(url)) continue;
    seen.add(url);
    out.push({
      url,
      title: topSiteTitle(site?.title) || topSiteHost(url),
      favicon: topSiteFavicon(site?.favicon),
    });
  }
  return out;
}

// 新标签页每次挂载都会重新读推荐;先用上一次的结果渲染,避免切页签时整块闪一下。
let cachedTopSites = null;

export function cachedAgentBrowserTopSites() {
  return cachedTopSites;
}

export function rememberAgentBrowserTopSites(sites) {
  cachedTopSites = Array.isArray(sites) ? sites : null;
}

export function withoutTopSite(sites, url) {
  const key = topSiteUrlKey(url);
  const list = Array.isArray(sites) ? sites : [];
  const next = list.filter((site) => site?.url !== key);
  return next.length === list.length ? list : next;
}

// 把 native 页面状态事件翻译成「要不要上报、上报什么」。规则:
//   - 只有加载完成(content_state=live)的 http(s) 页面才算访问;
//   - 地址变了才算一次新访问(new_visit=true),刷新同一地址不计数;
//   - 同一次访问后到的标题 / 图标只做补充(new_visit=false);
//   - 前端加载前就已经打开的页面(第一次见到就已是 live,且不在启动时还没
//     对账完的空窗期之后新出现)只记下,不计数 —— 否则每次刷新 / 切工作区都会
//     把所有已打开页面多算一次。
export function createAgentBrowserVisitTracker() {
  const pages = new Map();
  let seeded = false;

  const remember = (pageId, url, title, favicon) => {
    pages.set(pageId, { url, title, favicon });
  };

  return {
    // 启动时用 Desktop 的完整页面列表打底;之后第一次见到的 live 页面都是新页面。
    seed(list = []) {
      for (const page of Array.isArray(list) ? list : []) {
        const pageId = text(page?.page_id || page?.pageId).trim();
        if (!pageId || pages.has(pageId)) continue;
        const live = text(page?.content_state) === 'live';
        remember(
          pageId,
          live ? topSiteUrlKey(page?.url) : '',
          live ? topSiteTitle(page?.title) : '',
          live ? topSiteFavicon(page?.favicon) : '',
        );
      }
      seeded = true;
    },
    observe(detail) {
      if (!detail || typeof detail !== 'object') return null;
      const pageId = text(detail.page_id || detail.pageId).trim();
      if (!pageId) return null;
      if (detail.closed === true) {
        pages.delete(pageId);
        return null;
      }
      const previous = pages.get(pageId);
      if (text(detail.content_state) !== 'live') {
        // 导航中 / 失败 / 空白:记一个「不在任何地址上」的占位,下一次 live 必然计数。
        if (!previous) remember(pageId, '', '', '');
        return null;
      }
      const url = topSiteUrlKey(detail.url);
      const title = topSiteTitle(detail.title);
      const favicon = topSiteFavicon(detail.favicon);
      if (!previous) {
        remember(pageId, url, title, favicon);
        if (!seeded || !url) return null;
        return { url, title, favicon, new_visit: true };
      }
      if (previous.url !== url) {
        remember(pageId, url, title, favicon);
        return url ? { url, title, favicon, new_visit: true } : null;
      }
      if (!url) return null;
      const nextTitle = title || previous.title;
      const nextFavicon = favicon || previous.favicon;
      if (nextTitle === previous.title && nextFavicon === previous.favicon) return null;
      remember(pageId, url, nextTitle, nextFavicon);
      return {
        url,
        title: nextTitle !== previous.title ? nextTitle : '',
        favicon: nextFavicon !== previous.favicon ? nextFavicon : '',
        new_visit: false,
      };
    },
  };
}

let installedRecorder = null;

// App 挂载时装一次:所有 Agent Browser 页面的访问都上报,不依赖用户正在看哪个会话。
// record 失败(daemon 未就绪、数据目录迁移中)静默丢弃 —— 推荐只是便利功能。
export function installAgentBrowserVisitRecorder(
  record,
  win = globalThis.window,
  { listPages = listAgentBrowserPages } = {},
) {
  if (!win?.addEventListener || installedRecorder || !hasNativeAgentBrowser(win)) return () => {};
  const tracker = createAgentBrowserVisitTracker();
  const handler = (event) => {
    const visit = tracker.observe(event?.detail);
    if (!visit) return;
    Promise.resolve()
      .then(() => record(visit))
      .catch(() => {});
  };
  win.addEventListener(AGENT_BROWSER_STATE_EVENT, handler);
  installedRecorder = { win, handler };
  Promise.resolve()
    .then(() => listPages('', win))
    .then((result) => tracker.seed(result?.ok === true ? result.pages : []))
    .catch(() => tracker.seed([]));
  return () => {
    if (installedRecorder?.handler !== handler) return;
    win.removeEventListener(AGENT_BROWSER_STATE_EVENT, handler);
    installedRecorder = null;
  };
}
