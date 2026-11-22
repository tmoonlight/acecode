// 铅笔路径注册表（#217）。
//
// 编辑一条待发批注要「切到那一页 + 让它进入批注模式 + 按 locator 重新选中」，
// 这三件事只有页面面板能做。卡片在输入框上方，拿不到面板实例，所以用一个
// 按 page_id 注册的小表把它们接起来。

const editors = new Map();

export function registerAnnotationEditor(pageId, edit) {
  if (!pageId || typeof edit !== 'function') return () => {};
  editors.set(pageId, edit);
  return () => {
    if (editors.get(pageId) === edit) editors.delete(pageId);
  };
}

export function hasAnnotationEditor(pageId) {
  return editors.has(pageId);
}

export function requestAnnotationEdit(pageId, record) {
  const edit = editors.get(pageId);
  if (!edit) {
    return Promise.resolve({ ok: false, error: '这一页已经不再打开了，只能修改文字' });
  }
  return Promise.resolve(edit(record)).catch((error) => ({
    ok: false, error: String(error?.message || error || '无法回到该页面'),
  }));
}
