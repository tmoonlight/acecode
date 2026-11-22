// 会话级待发批注箱。
//
// 记录归会话，页签只是它的渲染端点（Q33）。因此这里按 session id 建容器，
// 页签把快照上报进来，页签关掉时只把记录标成 orphan，不删记录——用户在别的
// 页面点「发送」时，这些记录要跟着一起发出去（Q35：提交范围是会话全量）。
//
// 四态（技术设计 §4.3）：
//   live       目标在当前文档里有效
//   replayable 文档已替换（刷新/跳转），等在新文档上重放
//   stale      目标已失效或被移除
//   orphan     页签已关闭，没有 DOM 可操作，但记录与图都还在

import { apiConnectionScope } from './api.js';

const EMPTY_SNAPSHOT = Object.freeze({
  revision: 0, records: Object.freeze([]), pages: Object.freeze([]), pendingCount: 0,
});

export function annotationState(record, page) {
  if (!page || page.closed) return 'orphan';
  const documentId = page.documentId || '';
  if (record?.document_id && documentId && record.document_id !== documentId) return 'replayable';
  if (record?.valid !== true) return 'stale';
  return 'live';
}

// 卡片明细里显示的元素类型。多目标时取主目标（PRD §6.7）。
export function annotationElement(record) {
  const target = Array.isArray(record?.targets) ? record.targets[0] : null;
  const tag = String(target?.tag || '').trim().toLowerCase();
  return tag && tag.length <= 32 ? tag : 'div';
}

export function normalizeAnnotationRecord(record, page) {
  return {
    id: String(record?.id || ''),
    pageId: page.id,
    pageTitle: page.title,
    pageUrl: page.url,
    number: Number.isInteger(record?.number) && record.number > 0 ? record.number : 0,
    description: String(record?.description || ''),
    element: annotationElement(record),
    differences: Array.isArray(record?.differences) ? record.differences : [],
    targets: Array.isArray(record?.targets) ? record.targets : [],
    region: record?.region ?? null,
    // 保存时按目标矩形裁的局部图，只用于待发卡片的缩略图。
    thumbnail: typeof record?.thumbnail === 'string' ? record.thumbnail : '',
    state: annotationState(record, page),
    documentId: String(record?.document_id || ''),
    sent: record?.status === 'sent',
  };
}

export function createAnnotationOutbox() {
  // pageId -> { id, url, title, closed, documentId, pageShot, records: [] }
  const pages = new Map();
  const listeners = new Set();
  let snapshot = EMPTY_SNAPSHOT;

  const pageOrder = (pageId) => {
    let index = 0;
    for (const key of pages.keys()) {
      if (key === pageId) return index;
      index += 1;
    }
    return index;
  };

  const recompute = () => {
    const records = [];
    const pageList = [];
    for (const page of pages.values()) {
      const pending = page.records.filter((record) => !record.sent);
      for (const record of pending) records.push(record);
      pageList.push({
        id: page.id, url: page.url, title: page.title, closed: page.closed,
        pageShot: page.pageShot, count: pending.length,
      });
    }
    records.sort((a, b) => {
      const left = pageOrder(a.pageId);
      const right = pageOrder(b.pageId);
      return left === right ? a.number - b.number : left - right;
    });
    snapshot = Object.freeze({
      revision: snapshot.revision + 1,
      records: Object.freeze(records),
      pages: Object.freeze(pageList),
      pendingCount: records.length,
    });
    for (const listener of listeners) listener();
  };

  return {
    getSnapshot: () => snapshot,
    subscribe(listener) {
      listeners.add(listener);
      return () => listeners.delete(listener);
    },
    // 页签上报自己的快照。`closed` 为真时保留记录，只把状态降级为 orphan。
    observe({ pageId, url = '', title = '', closed = false, snapshot: pageSnapshot = null }) {
      if (!pageId) return;
      const previous = pages.get(pageId);
      const records = pageSnapshot && Array.isArray(pageSnapshot.records)
        ? pageSnapshot.records.map((record) => normalizeAnnotationRecord(record, {
            id: pageId,
            title: title || previous?.title || '',
            url: url || previous?.url || '',
          }))
        : (previous?.records || []);
      const next = {
        id: pageId,
        url: url || previous?.url || '',
        title: title || previous?.title || '',
        closed: closed || (previous?.closed && !pageSnapshot),
        documentId: pageSnapshot?.document_id || '',
        pageShot: typeof pageSnapshot?.page_shot === 'string' && pageSnapshot.page_shot
          ? pageSnapshot.page_shot
          : (previous?.pageShot || ''),
        records,
      };
      if (closed) {
        next.records = records.map((record) => ({ ...record, state: 'orphan', sent: record.sent }));
      }
      const unchanged = previous &&
        previous.closed === next.closed &&
        previous.url === next.url &&
        previous.title === next.title &&
        previous.documentId === next.documentId &&
        previous.pageShot === next.pageShot &&
        previous.records.length === next.records.length &&
        previous.records.every((record, index) => (
          record.id === next.records[index].id &&
          record.state === next.records[index].state &&
          record.description === next.records[index].description &&
          record.thumbnail === next.records[index].thumbnail &&
          record.sent === next.records[index].sent
        ));
      if (unchanged) return;
      pages.set(pageId, next);
      recompute();
    },
    closePage(pageId) {
      const page = pages.get(pageId);
      if (!page || page.closed) return;
      pages.set(pageId, {
        ...page,
        closed: true,
        records: page.records.map((record) => ({ ...record, state: 'orphan' })),
      });
      recompute();
    },
    discardPage(pageId) {
      const page = pages.get(pageId);
      if (!page) return;
      pages.set(pageId, { ...page, records: [] });
      recompute();
    },
    // 只改本地这条的文字。页签已关闭时原生那边已经没有可写的记录，
    // 待发箱就是它唯一的家；活页签的编辑走回页面那条路径（#217）。
    updateDescription(id, description) {
      for (const [pageId, page] of pages) {
        const index = page.records.findIndex((record) => record.id === id);
        if (index < 0) continue;
        const records = page.records.slice();
        records[index] = { ...records[index], description: String(description ?? '') };
        pages.set(pageId, { ...page, records });
        recompute();
        return true;
      }
      return false;
    },
    markSent(ids) {
      const target = new Set((ids || []).map(String));
      if (!target.size) return;
      let changed = false;
      for (const [pageId, page] of pages) {
        const next = page.records.filter((record) => !target.has(record.id));
        if (next.length !== page.records.length) {
          pages.set(pageId, { ...page, records: next });
          changed = true;
        }
      }
      if (changed) recompute();
    },
    pendingRecords: () => snapshot.records,
    pendingCount: () => snapshot.pendingCount,
    pagesWithPending: () => snapshot.pages.filter((page) => page.count > 0),
    reset() {
      pages.clear();
      recompute();
    },
  };
}

const outboxes = new WeakMap();

// 按「连接作用域 + 会话」取容器。作用域隔离避免切换服务器后把上一台的
// 待发批注串到这一台。
export function annotationOutboxFor(sessionId, api) {
  if (!sessionId) return null;
  let scope;
  try { scope = apiConnectionScope(api); } catch { scope = api || globalThis; }
  let sessions = outboxes.get(scope);
  if (!sessions) { sessions = new Map(); outboxes.set(scope, sessions); }
  if (!sessions.has(sessionId)) sessions.set(sessionId, createAnnotationOutbox());
  return sessions.get(sessionId);
}
