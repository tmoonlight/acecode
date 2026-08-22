// 全局会话搜索面板:服务端增量目录 + 有界正文短批次。
// 每次打开/查询都有独立 request_id；本地 AbortController 与服务端取消
// 同时执行，因此 Esc、关闭按钮、遮罩、查询替换和卸载都能真正停止工作。

import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { api } from '../lib/api.js';
import { connection } from '../lib/connection.js';
import {
  buildSearchResultSequence,
  mergeSessionContentMatches,
  rankSessions,
  rankWorkspaces,
  searchRelativeTime,
  shouldSearchUserMessages,
  workspaceDisplayName,
} from '../lib/searchSessions.js';
import { sessionDisplayTitle, withNewSessionDisplayTitles } from '../lib/sessionTitle.js';
import { SESSION_LIST_CHANGED_EVENT } from '../lib/sessionListEvents.js';
import { clsx } from '../lib/format.js';
import { VsIcon } from './Icon.jsx';

const CACHE_TTL_MS = 60_000;
const PAGE_SIZE = 8;
const MAX_EMPTY_RESULTS = 50;
const CATALOG_POLL_MS = 80;
const CONTENT_POLL_MS = 15;

const emptyProgress = Object.freeze({
  scanned_projects: 0,
  total_projects: 0,
  generation: 0,
  complete: false,
  paused: false,
});

// 仅缓存空查询最近结果；查询结果不会污染下一次打开的首页。
const cache = {
  ts: 0,
  data: {
    sessions: [],
    workspaces: [],
    errors: [],
    progress: emptyProgress,
    next_cursor: null,
  },
};

let requestSequence = 0;

function createSearchRequestId() {
  requestSequence += 1;
  const uuid = globalThis.crypto?.randomUUID?.();
  return uuid || `search-${Date.now()}-${requestSequence}`;
}

function isAbortError(error) {
  return error?.name === 'AbortError';
}

function waitForPoll(ms, signal) {
  return new Promise((resolve) => {
    if (signal?.aborted) {
      resolve();
      return;
    }
    const finish = () => {
      clearTimeout(timer);
      signal?.removeEventListener?.('abort', onAbort);
      resolve();
    };
    const onAbort = () => finish();
    const timer = setTimeout(finish, ms);
    signal?.addEventListener?.('abort', onAbort, { once: true });
  });
}

function mergeUniqueSessions(previous = [], next = []) {
  const byKey = new Map();
  const keyFor = (session) => `${session?.no_workspace ? 'no' : (session?.workspace_hash || '')}::${session?.id || ''}`;
  for (const session of previous) byKey.set(keyFor(session), session);
  for (const session of next) byKey.set(keyFor(session), session);
  return [...byKey.values()];
}

function isCacheFresh(now = Date.now()) {
  return cache.ts > 0 && now - cache.ts < CACHE_TTL_MS;
}

function invalidateCache() {
  cache.ts = 0;
}

function searchMatchContext(match) {
  if (!match || match.kind !== 'user_message') return '';
  const attachments = Array.isArray(match.attachments) ? match.attachments.filter(Boolean) : [];
  if (attachments.length > 0) return `附件: ${attachments.slice(0, 2).join(', ')}`;
  return String(match.snippet || '');
}

function progressLabel(progress, prefix) {
  if (!progress || progress.complete) return '';
  const scanned = Number(progress.scanned_projects) || 0;
  const total = Number(progress.total_projects) || 0;
  return total > 0 ? `${prefix} ${scanned}/${total}` : `${prefix}准备中`;
}

export function SearchPalette({
  open,
  onClose,
  currentWorkspaceHash = '',
  onSelectSession,
  onSelectWorkspace,
}) {
  const [query, setQuery] = useState('');
  const [data, setData] = useState(cache.data);
  const [contentSearch, setContentSearch] = useState({
    query: '',
    matches: [],
    progress: { scanned_projects: 0, total_projects: 0, complete: true },
  });
  const [selectedIndex, setSelectedIndex] = useState(0);
  const [searchError, setSearchError] = useState('');
  const [loadMoreBusy, setLoadMoreBusy] = useState(false);
  const [searchRevision, setSearchRevision] = useState(0);
  const inputRef = useRef(null);
  const listRef = useRef(null);
  const rowRefs = useRef(new Map());
  const activeSearchRef = useRef(null);
  const workspaceAbortRef = useRef(null);

  const cancelSearch = useCallback((search = activeSearchRef.current) => {
    if (!search || search.cancelled) return;
    search.cancelled = true;
    search.controller?.abort();
    // 取消请求不能复用刚被 abort 的 signal；它独立、幂等、尽力送达。
    api.cancelSessionSearch(search.requestId).catch(() => {});
    if (activeSearchRef.current === search) activeSearchRef.current = null;
  }, []);

  const closePalette = useCallback(() => {
    cancelSearch();
    workspaceAbortRef.current?.abort();
    onClose?.();
  }, [cancelSearch, onClose]);

  // WS / 本地列表事件只使空查询缓存失效；正在显示的成功结果保留到下一批。
  useEffect(() => {
    const onMsg = (event) => {
      const type = event.detail?.type;
      if (type === 'session_status' || type === 'session_status_snapshot'
          || type === 'mark_session_read_ack') {
        invalidateCache();
      }
    };
    const onSessionListChanged = () => invalidateCache();
    connection.addEventListener('message', onMsg);
    window.addEventListener(SESSION_LIST_CHANGED_EVENT, onSessionListChanged);
    return () => {
      connection.removeEventListener('message', onMsg);
      window.removeEventListener(SESSION_LIST_CHANGED_EVENT, onSessionListChanged);
    };
  }, []);

  // 面板生命周期:立刻呈现缓存，同时单独读取项目列表；没有全局 loading 门。
  useEffect(() => {
    if (!open) return undefined;
    setQuery('');
    setSelectedIndex(0);
    setSearchError('');
    setContentSearch({
      query: '',
      matches: [],
      progress: { scanned_projects: 0, total_projects: 0, complete: true },
    });
    if (isCacheFresh()) setData(cache.data);
    else setData((previous) => ({ ...previous, sessions: cache.data.sessions }));
    requestAnimationFrame(() => inputRef.current?.focus());

    const controller = typeof AbortController === 'function' ? new AbortController() : null;
    workspaceAbortRef.current = controller;
    api.listWorkspaces({ signal: controller?.signal }).then((workspaces) => {
      if (controller?.signal.aborted) return;
      setData((previous) => ({
        ...previous,
        workspaces: Array.isArray(workspaces) ? workspaces : [],
      }));
    }).catch((error) => {
      if (!isAbortError(error)) setSearchError(error?.message || '项目列表加载失败');
    });
    return () => {
      controller?.abort();
      if (workspaceAbortRef.current === controller) workspaceAbortRef.current = null;
    };
  }, [open]);

  // 查询生命周期:元数据和正文并行渐进推进。cleanup 是查询替换、open=false
  // 和组件卸载的共同取消路径。
  useEffect(() => {
    if (!open) return undefined;
    const normalizedQuery = query.trim();
    const requestId = createSearchRequestId();
    const controller = typeof AbortController === 'function' ? new AbortController() : null;
    const search = { requestId, controller, cancelled: false, query: normalizedQuery };
    activeSearchRef.current = search;
    setSearchError('');
    setLoadMoreBusy(false);
    if (!normalizedQuery) {
      setData((previous) => ({ ...previous, sessions: cache.data.sessions }));
    }

    const runCatalog = async () => {
      try {
        while (!controller?.signal.aborted) {
          const payload = await api.listGlobalSessions({
            query: normalizedQuery,
            limit: MAX_EMPTY_RESULTS,
            requestId,
            signal: controller?.signal,
          });
          if (controller?.signal.aborted || activeSearchRef.current !== search) return;
          const sessions = Array.isArray(payload?.sessions) ? payload.sessions : [];
          const errors = Array.isArray(payload?.errors) ? payload.errors : [];
          const progress = payload?.progress || emptyProgress;
          const next = {
            sessions,
            errors,
            progress,
            next_cursor: payload?.next_cursor || null,
          };
          setData((previous) => ({ ...previous, ...next }));
          if (!normalizedQuery) {
            cache.ts = Date.now();
            cache.data = { ...cache.data, ...next };
          }
          if (progress.complete) return;
          await waitForPoll(CATALOG_POLL_MS, controller?.signal);
        }
      } catch (error) {
        if (!isAbortError(error) && activeSearchRef.current === search) {
          setSearchError(error?.message || '搜索任务加载失败');
        }
      }
    };

    const runContent = async () => {
      if (!shouldSearchUserMessages(normalizedQuery)) {
        setContentSearch({
          query: '',
          matches: [],
          progress: { scanned_projects: 0, total_projects: 0, complete: true },
        });
        return;
      }
      await waitForPoll(160, controller?.signal);
      try {
        while (!controller?.signal.aborted) {
          const payload = await api.searchSessionUserMessages(
            normalizedQuery,
            50,
            { requestId, signal: controller?.signal },
          );
          if (controller?.signal.aborted || activeSearchRef.current !== search) return;
          const matches = Array.isArray(payload?.matches) ? payload.matches : [];
          const progress = payload?.progress || {
            scanned_projects: 0,
            total_projects: 0,
            complete: true,
          };
          setContentSearch({ query: normalizedQuery, matches, progress });
          if (progress.complete) return;
          await waitForPoll(CONTENT_POLL_MS, controller?.signal);
        }
      } catch (error) {
        if (!isAbortError(error) && error?.code !== 'SESSION_SEARCH_CANCELLED'
            && activeSearchRef.current === search) {
          setSearchError(error?.message || '搜索正文失败');
        }
      }
    };

    runCatalog();
    runContent();
    return () => cancelSearch(search);
  }, [open, query, searchRevision, cancelSearch]);

  const taskItems = useMemo(() => {
    const baseSessions = withNewSessionDisplayTitles(data.sessions || []);
    const normalizedQuery = query.trim();
    const matches = shouldSearchUserMessages(normalizedQuery)
      && contentSearch.query === normalizedQuery
      ? contentSearch.matches
      : [];
    const merged = mergeSessionContentMatches(baseSessions, matches);
    const ranked = rankSessions(merged, query, Date.now());
    return normalizedQuery ? ranked : ranked.slice(0, MAX_EMPTY_RESULTS);
  }, [data.sessions, query, contentSearch]);
  const projectItems = useMemo(
    () => rankWorkspaces(data.workspaces || [], query),
    [data.workspaces, query],
  );
  const items = useMemo(
    () => buildSearchResultSequence(taskItems, projectItems),
    [projectItems, taskItems],
  );

  useEffect(() => setSelectedIndex(0), [query, data.sessions, contentSearch]);

  useEffect(() => {
    const row = rowRefs.current.get(selectedIndex);
    if (row) row.scrollIntoView({ block: 'nearest' });
  }, [selectedIndex, items.length]);

  const commit = useCallback((index) => {
    const item = items[index];
    if (!item) return;
    if (item.kind === 'project') {
      onSelectWorkspace?.(item.value);
      return;
    }
    onSelectSession?.(item.value);
  }, [items, onSelectSession, onSelectWorkspace]);

  const onRootKeyDown = useCallback((event) => {
    const total = items.length;
    if (event.key === 'Escape') {
      event.preventDefault();
      event.stopPropagation();
      closePalette();
      return;
    }
    if (event.key === 'Enter') {
      if (total === 0) return;
      event.preventDefault();
      event.stopPropagation();
      commit(selectedIndex);
      return;
    }
    if (total === 0) return;
    let next = selectedIndex;
    if (event.key === 'ArrowDown') next = Math.min(total - 1, selectedIndex + 1);
    else if (event.key === 'ArrowUp') next = Math.max(0, selectedIndex - 1);
    else if (event.key === 'PageDown') next = Math.min(total - 1, selectedIndex + PAGE_SIZE);
    else if (event.key === 'PageUp') next = Math.max(0, selectedIndex - PAGE_SIZE);
    else if (event.key === 'Home') next = 0;
    else if (event.key === 'End') next = total - 1;
    else return;
    event.preventDefault();
    event.stopPropagation();
    setSelectedIndex(next);
  }, [items, selectedIndex, commit, closePalette]);

  const loadMore = useCallback(async () => {
    const search = activeSearchRef.current;
    if (!search || !data.next_cursor || loadMoreBusy) return;
    setLoadMoreBusy(true);
    try {
      const payload = await api.listGlobalSessions({
        query: search.query,
        limit: 50,
        cursor: data.next_cursor,
        requestId: search.requestId,
        signal: search.controller?.signal,
      });
      if (activeSearchRef.current !== search) return;
      setData((previous) => ({
        ...previous,
        sessions: mergeUniqueSessions(previous.sessions, payload?.sessions || []),
        errors: Array.isArray(payload?.errors) ? payload.errors : previous.errors,
        progress: payload?.progress || previous.progress,
        next_cursor: payload?.next_cursor || null,
      }));
    } catch (error) {
      if (error?.code === 'SESSION_SEARCH_CURSOR_STALE') {
        setSearchRevision((value) => value + 1);
      } else if (!isAbortError(error)) {
        setSearchError(error?.message || '加载更多失败');
      }
    } finally {
      setLoadMoreBusy(false);
    }
  }, [data.next_cursor, loadMoreBusy]);

  if (!open) return null;

  const catalogProgressText = progressLabel(data.progress, '正在建立任务索引');
  const contentProgressText = shouldSearchUserMessages(query)
    ? progressLabel(contentSearch.progress, '正在增量搜索正文')
    : '';
  const progressText = [catalogProgressText, contentProgressText].filter(Boolean).join(' · ');
  const partialErrors = Array.isArray(data.errors) ? data.errors : [];

  return (
    <div
      data-ace-native-overlay="blocking"
      className="fixed inset-0 z-[300] flex items-start justify-center pt-[15vh] px-4"
      onKeyDown={onRootKeyDown}
      onMouseDown={(event) => { if (event.target === event.currentTarget) closePalette(); }}
      style={{ background: 'rgba(var(--ace-bg-rgb), 0.50)' }}
    >
      <div
        className="bg-surface border border-border rounded-xl ace-shadow-lg overflow-hidden flex flex-col"
        style={{ width: 'min(640px, 90vw)', maxHeight: '70vh' }}
        onMouseDown={(event) => event.stopPropagation()}
      >
        <div className="h-12 px-3 flex items-center gap-2 border-b border-border shrink-0">
          <VsIcon name="search" size={14} className="text-fg-mute shrink-0" />
          <input
            ref={inputRef}
            type="text"
            value={query}
            onChange={(event) => setQuery(event.target.value)}
            placeholder="搜索任务或项目"
            className="flex-1 bg-transparent border-0 outline-none text-[14px] text-fg placeholder:text-fg-mute"
          />
          <button
            type="button"
            onClick={closePalette}
            title="关闭 (Esc)"
            className="w-6 h-6 rounded-md text-fg-mute hover:text-fg hover:bg-surface-hi flex items-center justify-center"
          >
            <VsIcon name="close" size={12} />
          </button>
        </div>

        {progressText && (
          <div className="px-3 py-1.5 text-[11px] text-fg-mute bg-surface-alt border-b border-border shrink-0">
            {progressText}（可随时关闭）
          </div>
        )}
        {searchError && (
          <div className="px-3 py-1.5 flex items-center justify-between gap-3 text-[11px] text-danger bg-danger-bg border-b border-border shrink-0">
            <span className="truncate">搜索失败：{searchError}</span>
            <button
              type="button"
              className="shrink-0 underline hover:no-underline"
              onClick={() => setSearchRevision((value) => value + 1)}
            >
              重试
            </button>
          </div>
        )}
        {!searchError && partialErrors.length > 0 && (
          <div className="px-3 py-1.5 text-[11px] text-warning bg-warning-soft/30 border-b border-border shrink-0">
            部分搜索数据加载失败：
            {partialErrors.map((error) => error.name || error.hash || error.stage).filter(Boolean).join(', ')}
          </div>
        )}

        <div ref={listRef} role="listbox" aria-label="搜索结果" className="flex-1 overflow-y-auto">
          <div className="px-3 py-1.5 flex items-center justify-between border-b border-border bg-surface-alt text-[11px] font-semibold text-fg-mute">
            <span>任务</span>
            <span>{taskItems.length}</span>
          </div>
          {taskItems.map((session, index) => {
            const item = items[index];
            const selected = index === selectedIndex;
            const showWorkspaceName = (session.workspace_hash || '') !== currentWorkspaceHash;
            const relativeTime = searchRelativeTime(session.updated_at || session.created_at);
            const matchContext = searchMatchContext(session.search_match);
            const right = [
              selected && 'Enter',
              showWorkspaceName && (session.workspaceName || ''),
              relativeTime,
            ].filter(Boolean).join(' · ');
            return (
              <div
                key={item?.key || `task:${session.id}`}
                ref={(element) => {
                  if (element) rowRefs.current.set(index, element);
                  else rowRefs.current.delete(index);
                }}
                role="option"
                aria-selected={selected}
                onMouseEnter={() => setSelectedIndex(index)}
                onMouseDown={(event) => { event.preventDefault(); commit(index); }}
                className={clsx(
                  'min-h-12 px-3 py-2 flex items-center gap-3 cursor-pointer text-[13px]',
                  selected ? 'bg-surface-hi text-fg' : 'text-fg hover:bg-surface-hi/60',
                )}
              >
                <VsIcon name="code" size={16} className="text-fg-mute shrink-0" />
                <span className="min-w-0 flex-1 flex flex-col gap-0.5">
                  <span className="truncate">{sessionDisplayTitle(session)}</span>
                  {matchContext ? (
                    <span className="truncate text-[11px] text-fg-mute">{matchContext}</span>
                  ) : null}
                </span>
                <span className="text-[12px] text-fg-mute shrink-0">{right}</span>
              </div>
            );
          })}
          {data.next_cursor && (
            <button
              type="button"
              className="w-full px-3 py-2 text-[12px] text-accent hover:bg-surface-hi disabled:opacity-50"
              disabled={loadMoreBusy}
              onClick={loadMore}
            >
              {loadMoreBusy ? '加载中…' : '加载更多任务'}
            </button>
          )}
          <div className="px-3 py-1.5 flex items-center justify-between border-y border-border bg-surface-alt text-[11px] font-semibold text-fg-mute">
            <span>项目</span>
            <span>{projectItems.length}</span>
          </div>
          {projectItems.map((workspace, projectIndex) => {
            const index = taskItems.length + projectIndex;
            const item = items[index];
            const selected = index === selectedIndex;
            const active = (workspace.hash || '') === currentWorkspaceHash || !!workspace.active;
            const right = [selected && 'Enter', active && '当前'].filter(Boolean).join(' · ');
            const name = workspaceDisplayName(workspace);
            const path = String(workspace.cwd || '').trim();
            return (
              <div
                key={item?.key || `project:${workspace.hash || projectIndex}`}
                ref={(element) => {
                  if (element) rowRefs.current.set(index, element);
                  else rowRefs.current.delete(index);
                }}
                role="option"
                aria-selected={selected}
                onMouseEnter={() => setSelectedIndex(index)}
                onMouseDown={(event) => { event.preventDefault(); commit(index); }}
                className={clsx(
                  'min-h-12 px-3 py-2 flex items-center gap-3 cursor-pointer text-[13px]',
                  selected ? 'bg-surface-hi text-fg' : 'text-fg hover:bg-surface-hi/60',
                )}
              >
                <VsIcon name="folder" size={16} className="text-fg-mute shrink-0" />
                <span className="min-w-0 flex-1 flex flex-col gap-0.5">
                  <span className="truncate font-medium">{name}</span>
                  {path && path !== name ? (
                    <span className="truncate text-[11px] text-fg-mute">{path}</span>
                  ) : null}
                </span>
                <span className="text-[12px] text-fg-mute shrink-0">{right}</span>
              </div>
            );
          })}
          {items.length === 0 && (
            <div className="px-4 py-8 text-center text-fg-mute text-[13px]">
              {progressText ? '正在增量搜索，可随时关闭' : (query.trim() ? '无匹配结果' : '暂无任务或项目')}
            </div>
          )}
        </div>
      </div>
    </div>
  );
}
