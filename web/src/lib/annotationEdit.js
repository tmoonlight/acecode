// 铅笔路径注册表（#217）。
//
// 编辑一条待发批注要「切到那一页 + 让它进入批注模式 + 按 locator 重新选中」，
// 这三件事只有页面面板能做。卡片在输入框上方，拿不到面板实例，所以用一个
// 按 page_id 注册的小表把它们接起来。

const editors = new Map();
const editorWaiters = new Map();

export function annotationPageMissingError() {
  return '这一页已经不再打开了，只能修改文字';
}

function recordUrlKey(value) {
  const text = String(value || '').trim();
  if (!text || text === 'about:blank') return '';
  try {
    const url = new URL(text);
    url.hash = '';
    return url.href;
  } catch {
    return text.split('#')[0];
  }
}

function resolveBrowserPage(pageId, url, pages, tabs) {
  const id = String(pageId || '').trim();
  if (id) {
    const page = (Array.isArray(pages) ? pages : [])
      .find((item) => String(item?.pageId || item?.page_id || '') === id);
    if (page) return { pageId: id, page };
    const tab = (Array.isArray(tabs) ? tabs : [])
      .find((item) => item?.type === 'browser' && String(item?.pageId || '') === id);
    if (tab) return { pageId: id, page: null };
  }
  const wanted = recordUrlKey(url);
  if (!wanted) return null;
  const page = (Array.isArray(pages) ? pages : []).find((item) => (
    String(item?.pageId || item?.page_id || '').trim()
    && recordUrlKey(item?.url) === wanted
  ));
  return page
    ? { pageId: String(page.pageId || page.page_id).trim(), page }
    : null;
}

function resolveEditorWaiters(pageId, edit) {
  const waiters = editorWaiters.get(pageId);
  if (!waiters) return;
  editorWaiters.delete(pageId);
  for (const waiter of waiters) {
    clearTimeout(waiter.timer);
    waiter.resolve(edit);
  }
}

export function registerAnnotationEditor(pageId, edit) {
  if (!pageId || typeof edit !== 'function') return () => {};
  editors.set(pageId, edit);
  resolveEditorWaiters(pageId, edit);
  return () => {
    if (editors.get(pageId) === edit) editors.delete(pageId);
  };
}

export function hasAnnotationEditor(pageId) {
  return editors.has(pageId);
}

function waitForEditor(pageId, timeoutMs) {
  const current = editors.get(pageId);
  if (current) return Promise.resolve(current);
  return new Promise((resolve) => {
    const waiters = editorWaiters.get(pageId) || new Set();
    const waiter = {
      resolve,
      timer: setTimeout(() => {
        waiters.delete(waiter);
        if (!waiters.size) editorWaiters.delete(pageId);
        resolve(null);
      }, Math.max(0, timeoutMs)),
    };
    waiters.add(waiter);
    editorWaiters.set(pageId, waiters);
  });
}

export async function requestAnnotationEdit(pageId, record, {
  url = record?.pageUrl || '',
  pages = [],
  tabs = [],
  activate = null,
  editorTimeoutMs = 2000,
} = {}) {
  const directId = String(pageId || '').trim();
  const page = resolveBrowserPage(directId, url, pages, tabs)
    || (editors.has(directId) ? { pageId: directId, page: null } : null);
  if (!page) {
    return { ok: false, reason: 'missing_page', error: annotationPageMissingError() };
  }
  try {
    if (typeof activate === 'function') {
      const activated = await activate(page.pageId);
      if (activated === false) {
        return { ok: false, reason: 'activation_failed', error: '无法回到该页面' };
      }
    }
    const edit = editors.get(page.pageId) || await waitForEditor(page.pageId, editorTimeoutMs);
    if (!edit) {
      return { ok: false, reason: 'activation_failed', error: '无法回到该页面' };
    }
    const result = await edit(record);
    return result === undefined ? { ok: true } : result;
  } catch (error) {
    return { ok: false, error: String(error?.message || error || '无法回到该页面') };
  }
}
