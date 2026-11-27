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

// 待发卡片上删除一条之后，「撤销」保留多久。到时原生存储里那份才真正删掉。
export const ANNOTATION_UNDO_WINDOW_MS = 5000;

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
  const descriptionOverrides = new Map();
  // 本地已删除、原生存储还没删掉的记录。页签轮询送来的旧快照不能把它们复活。
  const suppressed = new Set();
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
        ? pageSnapshot.records.filter((record) => !suppressed.has(String(record?.id || ''))).map((record) => {
            const normalized = normalizeAnnotationRecord(record, {
              id: pageId,
              title: title || previous?.title || '',
              url: url || previous?.url || '',
            });
            return descriptionOverrides.has(normalized.id)
              ? { ...normalized, description: descriptionOverrides.get(normalized.id) }
              : normalized;
          })
        : (previous?.records || []);
      if (pageSnapshot && Array.isArray(pageSnapshot.records)) {
        // 原生已经删掉这条了：不再需要抑制，避免集合无限增长。
        const live = new Set(pageSnapshot.records.map((record) => String(record?.id || '')));
        for (const id of suppressed) {
          if (!live.has(id)) suppressed.delete(id);
        }
      }
      const next = {
        id: pageId,
        url: url || previous?.url || '',
        title: title || previous?.title || '',
        closed: closed || (previous?.closed && !pageSnapshot),
        documentId: pageSnapshot?.document_id || '',
        // 快照没带图时沿用上一张（页签关闭后的上报没有快照）。但这一页已经
        // 没有任何记录时不沿用：原生在记录清空后释放了整屏图，这里跟着放掉，
        // 免得下一批批注在截图失败时配上上一批的旧图。
        pageShot: typeof pageSnapshot?.page_shot === 'string' && pageSnapshot.page_shot
          ? pageSnapshot.page_shot
          : (records.length ? (previous?.pageShot || '') : ''),
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
      for (const record of page.records) {
        descriptionOverrides.delete(record.id);
        suppressed.delete(record.id);
      }
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
        descriptionOverrides.set(id, records[index].description);
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
      for (const id of target) suppressed.delete(id);
      for (const [pageId, page] of pages) {
        const next = page.records.filter((record) => !target.has(record.id));
        if (next.length !== page.records.length) {
          for (const record of page.records) {
            if (target.has(record.id)) descriptionOverrides.delete(record.id);
          }
          pages.set(pageId, { ...page, records: next });
          changed = true;
        }
      }
      if (changed) recompute();
    },
    // 立即从待发集合里摘掉（删除按钮按下那一刻），返回被摘掉的记录供撤销。
    // 原生存储的删除由调用方延后提交，撤销窗口内不碰它。
    remove(id) {
      const key = String(id || '');
      if (!key) return null;
      for (const [pageId, page] of pages) {
        const index = page.records.findIndex((record) => record.id === key);
        if (index < 0) continue;
        const removed = page.records[index];
        const records = page.records.slice(0, index).concat(page.records.slice(index + 1));
        suppressed.add(key);
        pages.set(pageId, { ...page, records });
        recompute();
        return removed;
      }
      return null;
    },
    // 撤销：放回原来的页面并按编号插回原位。
    restore(record) {
      const key = String(record?.id || '');
      const pageId = String(record?.pageId || '');
      if (!key || !pageId) return false;
      suppressed.delete(key);
      let page = pages.get(pageId);
      if (!page) {
        page = {
          id: pageId,
          url: String(record.pageUrl || ''),
          title: String(record.pageTitle || ''),
          closed: true,
          documentId: '',
          pageShot: '',
          records: [],
        };
        pages.set(pageId, page);
      }
      if (page.records.some((item) => item.id === key)) return true;
      const restored = {
        ...record,
        id: key,
        pageId,
        pageTitle: record.pageTitle || page.title || '',
        pageUrl: record.pageUrl || page.url || '',
        number: Number.isInteger(record.number) && record.number > 0 ? record.number : 0,
        state: page.closed ? 'orphan' : (record.state || 'live'),
        sent: false,
      };
      const records = page.records.concat(restored).sort((a, b) => a.number - b.number);
      pages.set(pageId, { ...page, records });
      recompute();
      return true;
    },
    pendingRecords: () => snapshot.records,
    pendingCount: () => snapshot.pendingCount,
    pagesWithPending: () => snapshot.pages.filter((page) => page.count > 0),
    reset() {
      pages.clear();
      descriptionOverrides.clear();
      suppressed.clear();
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
