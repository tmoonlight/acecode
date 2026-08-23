import { ApiError } from './api.js';

export function editableFileError(error, fallback = '无法编辑此文件') {
  if (!(error instanceof ApiError)) return error?.message || fallback;
  const body = error.body;
  if (body && typeof body === 'object') {
    if (body.error === 'file changed') return '磁盘内容已变化，未覆盖文件；请保留当前草稿并重新载入后再合并';
    if (body.error === 'file too large') return '文件过大，无法在详情中编辑';
    if (body.error === 'binary' || body.error === 'unsupported encoding') return '文件编码不受支持，只能只读预览';
    if (body.error === 'unknown workspace') return '该文件不在当前工作区或 worktree 中，只能只读预览';
    if (body.error === 'not found') return '文件不存在';
    if (body.error) return String(body.error);
  }
  return `${fallback} (HTTP ${error.status})`;
}

export function editableFileConflict(error) {
  return error instanceof ApiError && error.status === 409;
}

export function editableStatePatch(result, { editing = true } = {}) {
  const text = String(result?.text ?? '');
  return {
    editing,
    loading: false,
    saving: false,
    baselineText: text,
    text,
    readId: String(result?.read_id || ''),
    encoding: String(result?.encoding || ''),
    lineEnding: String(result?.line_ending || ''),
    hasBom: result?.has_bom === true,
    size: Number(result?.size || 0),
    externalChanged: false,
    error: '',
    readOnlyReason: '',
  };
}

export async function saveEditableFileDraft(api, {
  cwd = '',
  path = '',
  edit = null,
} = {}) {
  const readId = String(edit?.readId || '');
  if (!cwd || !path || !readId) {
    throw new Error('缺少保存文件所需的读取版本');
  }
  const text = String(edit?.text ?? edit?.baselineText ?? '');
  const result = await api.saveEditableFile(cwd, path, text, readId);
  return {
    text,
    result,
    patch: editableStatePatch({ ...result, text }),
  };
}

export async function saveEditableFileDraftBatch(api, {
  tabs = [],
  fallbackCwd = '',
  onSaving,
  onSaved,
} = {}) {
  let savedCount = 0;
  for (const tab of tabs) {
    onSaving?.(tab);
    try {
      const saved = await saveEditableFileDraft(api, {
        cwd: tab?.cwd || fallbackCwd,
        path: tab?.path || '',
        edit: tab?.edit,
      });
      savedCount += 1;
      onSaved?.(tab, saved);
    } catch (error) {
      return { ok: false, savedCount, tab, error };
    }
  }
  return { ok: true, savedCount };
}
