// Shared `/api/git/info` cache.
//
// The endpoint starts several Git subprocesses, so visible consumers share a
// 30-second result and one in-flight request. The key is deliberately two-part:
// effective API connection + cwd. A cwd-only singleton can route a session to
// the mutable global client or leak a result between different daemon ports.

import { apiConnectionScope } from './api.js';
import { GIT_STATE_CHANGED_EVENT } from './gitSessionPill.js';
import { SESSION_HOVER_GIT_CACHE_TTL_MS } from './sessionHoverDetails.js';

function finiteNumber(value) {
  const number = Number(value);
  return Number.isFinite(number) ? number : 0;
}

export function createGitInfoCache({
  now = () => Date.now(),
  ttlMs = SESSION_HOVER_GIT_CACHE_TTL_MS,
} = {}) {
  const entriesByScope = new WeakMap();
  // Git-state events currently carry cwd but no connection metadata. Keep the
  // small set of per-document entry maps iterable for conservative invalidation.
  const allScopeEntries = new Set();
  const safeTtlMs = Math.max(0, finiteNumber(ttlMs));

  const entriesFor = (apiClient, create = false) => {
    const scope = apiConnectionScope(apiClient);
    let entries = entriesByScope.get(scope);
    if (!entries && create) {
      entries = new Map();
      entriesByScope.set(scope, entries);
      allScopeEntries.add(entries);
    }
    return entries;
  };

  const load = (apiClient, cwd, { force = false } = {}) => {
    const key = typeof cwd === 'string' ? cwd : '';
    if (!key.trim()) return Promise.resolve(null);
    if (!apiClient || typeof apiClient.gitInfo !== 'function') {
      return Promise.reject(new TypeError('API client with gitInfo is required'));
    }

    const entries = entriesFor(apiClient, true);
    const timestamp = finiteNumber(now());
    const existing = entries.get(key);
    if (existing?.promise) return existing.promise;
    if (!force && existing && existing.expiresAt >= timestamp) {
      return Promise.resolve(existing.value);
    }

    // Start the caller synchronously after resolving its scope. A mutable
    // global client can change after this function returns, but request()
    // captures its URL and token before the first await.
    let resolveRequest;
    let rejectRequest;
    const request = new Promise((resolve, reject) => {
      resolveRequest = resolve;
      rejectRequest = reject;
    });
    // Keep a still-fresh value synchronously peekable while an explicit
    // lifecycle refresh is in flight. `get()` still joins the new request.
    entries.set(key, { ...existing, promise: request });
    try {
      Promise.resolve(apiClient.gitInfo(key)).then(
        (value) => {
          if (entries.get(key)?.promise === request) {
            entries.set(key, {
              value,
              expiresAt: finiteNumber(now()) + safeTtlMs,
            });
          }
          resolveRequest(value);
        },
        (error) => {
          if (entries.get(key)?.promise === request) entries.delete(key);
          rejectRequest(error);
        },
      );
    } catch (error) {
      if (entries.get(key)?.promise === request) entries.delete(key);
      rejectRequest(error);
    }
    return request;
  };

  const peek = (apiClient, cwd) => {
    const key = typeof cwd === 'string' ? cwd : '';
    if (!key.trim()) return undefined;
    if (!apiClient || (typeof apiClient !== 'object' && typeof apiClient !== 'function')) {
      return undefined;
    }
    const entries = entriesFor(apiClient);
    const existing = entries?.get(key);
    const expiresAt = Number(existing?.expiresAt);
    if (!existing || !Number.isFinite(expiresAt) || expiresAt < finiteNumber(now())) {
      return undefined;
    }
    return existing.value;
  };

  const invalidateEntries = (entries, cwd = '') => {
    if (!entries) return;
    const key = typeof cwd === 'string' ? cwd : '';
    if (key) entries.delete(key);
    else entries.clear();
  };

  return {
    get(apiClient, cwd) {
      return load(apiClient, cwd);
    },
    peek,
    refresh(apiClient, cwd) {
      return load(apiClient, cwd, { force: true });
    },
    invalidate(apiClient, cwd = '') {
      invalidateEntries(entriesFor(apiClient), cwd);
    },
    invalidateAll(cwd = '') {
      for (const entries of allScopeEntries) invalidateEntries(entries, cwd);
    },
  };
}

export const gitInfoCache = createGitInfoCache();

export function refreshWorkspaceGitInfo(apiClient, workspaceOrCwd) {
  const source = workspaceOrCwd && typeof workspaceOrCwd === 'object'
    ? workspaceOrCwd
    : null;
  if (source?.noWorkspace || source?.no_workspace) return Promise.resolve(null);
  const cwd = typeof workspaceOrCwd === 'string'
    ? workspaceOrCwd
    : String(source?.cwd || '');
  if (!cwd.trim()) return Promise.resolve(null);
  return gitInfoCache.refresh(apiClient, cwd);
}

if (typeof window !== 'undefined') {
  window.addEventListener(GIT_STATE_CHANGED_EVENT, (event) => {
    const changedCwd = String(event?.detail?.cwd || '');
    gitInfoCache.invalidateAll(changedCwd);
  });
}
