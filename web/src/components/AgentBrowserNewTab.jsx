import { useCallback, useEffect, useLayoutEffect, useRef, useState } from 'react';
import { api } from '../lib/api.js';
import { formatDateTime } from '../lib/format.js';
import {
  AGENT_BROWSER_TOP_SITES_LIMIT,
  cachedAgentBrowserTopSites,
  normalizeTopSitesResponse,
  rememberAgentBrowserTopSites,
  withoutTopSite,
} from '../lib/agentBrowserTopSites.js';
import {
  recentFileTimeLabel,
  recentFileTypeLabel,
} from '../lib/agentBrowserRecentFiles.js';
import { FileTypeIcon, VsIcon } from './Icon.jsx';
import { toast } from './Toast.jsx';

// Agent Browser 空白页(新标签页),版式照 Codex 桌面版的 New Tab:
//   推荐 —— 常访问网站,一行最多 4 个,悬停出现 × 移除(可撤销);
//   最近 —— 本会话改过的文件,点开后用文件预览替换这个标签页。
// Codex 页面最上方的「工具」区(变更 / 终端 / 文件 / 侧边聊天…)不做。
// 两块都没有内容时退回原来的「浏览器」提示。

function useTopSites() {
  const [sites, setSites] = useState(() => cachedAgentBrowserTopSites() || []);
  const [loaded, setLoaded] = useState(() => cachedAgentBrowserTopSites() !== null);
  const mountedRef = useRef(true);

  const refresh = useCallback(async () => {
    try {
      const next = normalizeTopSitesResponse(
        await api.getAgentBrowserTopSites(AGENT_BROWSER_TOP_SITES_LIMIT),
      );
      rememberAgentBrowserTopSites(next);
      if (mountedRef.current) setSites(next);
    } catch {
      // daemon 不可用时保留缓存;推荐只是便利功能,不打扰用户。
    } finally {
      if (mountedRef.current) setLoaded(true);
    }
  }, []);

  useEffect(() => {
    mountedRef.current = true;
    void refresh();
    return () => { mountedRef.current = false; };
  }, [refresh]);

  const dismiss = useCallback(async (site) => {
    setSites((previous) => {
      const next = withoutTopSite(previous, site.url);
      rememberAgentBrowserTopSites(next);
      return next;
    });
    try {
      await api.dismissAgentBrowserTopSite(site.url);
    } catch {
      toast({ kind: 'err', text: '无法更新推荐' });
      void refresh();
      return;
    }
    void refresh();
    toast({
      kind: 'info',
      text: '已移除推荐',
      actions: [{
        label: '撤销',
        errorText: '无法更新推荐:',
        onClick: async () => {
          await api.restoreAgentBrowserTopSite(site.url);
          await refresh();
        },
      }],
    });
  }, [refresh]);

  return { sites, loaded, dismiss };
}

function SiteFavicon({ favicon }) {
  const [failed, setFailed] = useState(false);
  useEffect(() => { setFailed(false); }, [favicon]);
  return (
    <span className="ace-agent-browser-newtab-favicon" aria-hidden="true">
      {favicon && !failed ? (
        <img
          src={favicon}
          alt=""
          draggable="false"
          referrerPolicy="no-referrer"
          onError={() => setFailed(true)}
        />
      ) : (
        <VsIcon name="globe" size={30} />
      )}
    </span>
  );
}

function SuggestedSites({ sites, onNavigate, onDismiss }) {
  if (sites.length === 0) return null;
  return (
    <section className="ace-agent-browser-newtab-section" data-section="suggested">
      <h3 className="ace-agent-browser-newtab-heading">推荐</h3>
      <ul className="ace-agent-browser-newtab-sites">
        {sites.map((site) => (
          <li key={site.url} className="ace-agent-browser-newtab-site">
            <button
              type="button"
              className="ace-agent-browser-newtab-site-link"
              title={`${site.title}\n${site.url}`}
              aria-label={site.title}
              onClick={() => onNavigate(site.url)}
            >
              <SiteFavicon favicon={site.favicon} />
              <span className="ace-agent-browser-newtab-site-title">{site.title}</span>
            </button>
            <button
              type="button"
              className="ace-agent-browser-newtab-icon-button ace-agent-browser-newtab-dismiss"
              aria-label={`移除推荐:${site.title}`}
              title="移除推荐"
              onClick={() => onDismiss(site)}
            >
              <VsIcon name="close" size={14} />
            </button>
          </li>
        ))}
      </ul>
    </section>
  );
}

// 列表高度按可用空间取整行(最多 6 行),再多露出半行提示还能滚动;上下箭头按整页
// 翻,与 Codex 的 Recents 一致。
function RecentFiles({ files, onOpen }) {
  const listRef = useRef(null);
  const [edges, setEdges] = useState({ top: true, bottom: true });
  const [interacted, setInteracted] = useState(false);
  const [now, setNow] = useState(() => new Date());

  const updateEdges = useCallback(() => {
    const list = listRef.current;
    if (!list) return;
    const top = list.scrollTop < 1;
    const bottom = list.scrollTop + list.clientHeight >= list.scrollHeight - 1;
    setEdges((previous) => (
      previous.top === top && previous.bottom === bottom ? previous : { top, bottom }
    ));
  }, []);

  useLayoutEffect(() => {
    const list = listRef.current;
    const root = list?.closest('[data-new-tab-scroll-root]');
    if (!list || !root || typeof ResizeObserver !== 'function') {
      updateEdges();
      return undefined;
    }
    const measure = () => {
      const row = list.firstElementChild;
      if (!(row instanceof HTMLElement) || row.offsetHeight === 0) return;
      const rowHeight = row.offsetHeight;
      const rootRect = root.getBoundingClientRect();
      const offsetTop = list.getBoundingClientRect().top - rootRect.top + root.scrollTop;
      const paddingBottom = parseFloat(getComputedStyle(root).paddingBottom) || 0;
      const rows = Math.max(2, Math.floor(
        (root.clientHeight - offsetTop - paddingBottom - rowHeight / 2) / rowHeight,
      ));
      list.style.maxHeight = `${Math.min(6, rows + 0.5) * rowHeight}px`;
      list.style.setProperty('--recent-row-height', `${rowHeight}px`);
      updateEdges();
    };
    const observer = new ResizeObserver(measure);
    observer.observe(root);
    observer.observe(list);
    measure();
    return () => observer.disconnect();
  }, [files.length, updateEdges]);

  // 时间标签里「今天」的判断跨过午夜要更新。
  useEffect(() => {
    const timer = setInterval(() => setNow(new Date()), 60_000);
    return () => clearInterval(timer);
  }, []);

  const scrollPage = (direction) => {
    setInteracted(true);
    const list = listRef.current;
    const row = list?.firstElementChild;
    if (!list || !(row instanceof HTMLElement)) return;
    const rowHeight = row.offsetHeight || list.clientHeight;
    const rows = Math.max(1, Math.floor((list.clientHeight - rowHeight / 2) / rowHeight));
    const reduceMotion = window.matchMedia?.('(prefers-reduced-motion: reduce)')?.matches;
    list.scrollBy({ top: direction * rows * rowHeight, behavior: reduceMotion ? 'instant' : 'smooth' });
  };

  if (files.length === 0) return null;
  return (
    <section className="ace-agent-browser-newtab-section" data-section="recent">
      <div className="ace-agent-browser-newtab-section-header">
        <h3 className="ace-agent-browser-newtab-heading">最近</h3>
        <div className="ace-agent-browser-newtab-scroll-buttons">
          <button
            type="button"
            className="ace-agent-browser-newtab-icon-button"
            disabled={edges.top}
            aria-label="向上滚动最近文件"
            title="向上滚动"
            onClick={() => scrollPage(-1)}
          >
            <VsIcon name="glyphUp" size={13} />
          </button>
          <button
            type="button"
            className="ace-agent-browser-newtab-icon-button"
            disabled={edges.bottom}
            aria-label="向下滚动最近文件"
            title="向下滚动"
            onClick={() => scrollPage(1)}
          >
            <VsIcon name="glyphDown" size={13} />
          </button>
        </div>
      </div>
      <ul
        ref={listRef}
        className="ace-agent-browser-newtab-recents"
        aria-label="最近文件"
        data-at-bottom={edges.bottom ? 'true' : 'false'}
        data-interacted={interacted ? 'true' : 'false'}
        onScroll={updateEdges}
        onWheel={() => setInteracted(true)}
        onTouchStart={() => setInteracted(true)}
        onKeyDown={() => setInteracted(true)}
      >
        {files.map((file) => {
          const time = recentFileTimeLabel(file.ts, now);
          return (
            <li key={file.key} className="ace-agent-browser-newtab-recent">
              <button
                type="button"
                className="ace-agent-browser-newtab-recent-button"
                title={file.path}
                aria-label={file.name}
                onClick={() => onOpen(file.path)}
              >
                <span className="ace-agent-browser-newtab-recent-icon" aria-hidden="true">
                  <FileTypeIcon path={file.path} size={16} />
                </span>
                <span className="ace-agent-browser-newtab-recent-text">
                  <span className="ace-agent-browser-newtab-recent-name">{file.name}</span>
                  <span className="ace-agent-browser-newtab-recent-kind">
                    {recentFileTypeLabel(file.kind)}
                  </span>
                </span>
                {time && (
                  <time
                    className="ace-agent-browser-newtab-recent-time"
                    dateTime={new Date(file.ts).toISOString()}
                    title={formatDateTime(file.ts, { dateStyle: 'full', timeStyle: 'short' })}
                  >
                    {time}
                  </time>
                )}
              </button>
            </li>
          );
        })}
      </ul>
    </section>
  );
}

export function AgentBrowserNewTab({ recentFiles = [], onNavigate, onOpenRecentFile, fallback }) {
  const { sites, loaded, dismiss } = useTopSites();
  const files = Array.isArray(recentFiles) ? recentFiles : [];
  const canOpenFiles = typeof onOpenRecentFile === 'function';
  const visibleFiles = canOpenFiles ? files : [];
  if (sites.length === 0 && visibleFiles.length === 0) {
    // 首次读推荐还没返回时先留白,避免「浏览器」提示一闪而过。
    return loaded ? fallback : <div className="ace-agent-browser-newtab" aria-busy="true" />;
  }
  return (
    <div className="ace-agent-browser-newtab" data-new-tab-scroll-root="true">
      <div className="ace-agent-browser-newtab-inner">
        <SuggestedSites sites={sites} onNavigate={onNavigate} onDismiss={dismiss} />
        <RecentFiles files={visibleFiles} onOpen={onOpenRecentFile} />
      </div>
    </div>
  );
}
