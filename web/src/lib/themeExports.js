import { isAiColorTheme } from './colorTheme.js';
import { desktopUiMode } from './desktopShellMode.js';

const EXPORT_STATES = new Set(['preparing', 'compressing', 'saving', 'completed', 'cancelled', 'failed']);
export const EMPTY_THEME_EXPORT = Object.freeze({ status: 'idle', entry: null, job: null, filename: '', hadCompression: false, cancelling: false, canCancel: true, error: null });

export function canManageTheme(entry) {
  return isAiColorTheme(entry?.id) && ['local', 'workshop'].includes(entry.source) && entry.installed === true;
}

export function themeExportFilename(entry) {
  let name = String(entry?.name || entry?.id || 'theme').replace(/[<>:"/\\|?*\u0000-\u001f]/g, '-').trim().replace(/[. ]+$/g, '').replace(/\.zip$/i, '');
  name = Array.from(name).slice(0, 100).join('') || 'theme';
  if (/^(con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\.|$)/i.test(name)) name = `theme-${name}`;
  return `${name}.zip`;
}

export function themeExportBusy(state) {
  return ['choosing', 'preparing', 'compressing', 'saving'].includes(state?.status);
}

export function themeExportProgress(job) {
  return typeof job?.progress === 'number' && Number.isFinite(job.progress)
    ? Math.max(0, Math.min(1, job.progress)) : null;
}

export function themeManagementFailure(error, fallback = '主题操作失败，请重试') {
  const body = error?.body || error;
  const code = error?.code || body?.error;
  const messages = {
    THEME_BUILTIN_PROTECTED: '内置主题不能导出或删除',
    THEME_BUSY: '主题正在处理中，请稍后重试',
    THEME_CHANGED: '主题内容已更新，请重新导出',
    THEME_NOT_INSTALLED: '主题资源不存在或已损坏，请刷新后重试',
    THEME_EXPORT_NOT_FOUND: '导出任务已过期，请重新导出',
    THEME_EXPORT_NOT_READY: '主题包尚未准备好，请重新导出',
    THEME_INVALID_PACKAGE: '主题资源校验失败，无法导出',
    THEME_UNSAFE_PATH: '主题资源路径无法安全访问',
    THEME_SAVE_FAILED: '无法保存主题包，请检查目标位置和写入权限',
    THEME_CONFIG_UNAVAILABLE: '外观配置暂不可用，请稍后重试',
    THEME_DELETE_ROLLBACK_FAILED: '删除主题时恢复文件失败，请检查主题资源目录',
    PERSIST_FAILED: '外观配置保存失败，主题未删除',
  };
  const detail = typeof body?.message === 'string' ? body.message : typeof error?.message === 'string' ? error.message : '';
  const message = messages[code] || detail || fallback;
  return { message, detail: detail !== message ? detail : '', path: typeof body?.error_path === 'string' ? body.error_path : '' };
}

// Called in the click handler before its first await: the browser's save picker
// requires transient user activation. Cancelling it must never start a job.
export function chooseThemeSaveDestination(entry, win = globalThis.window) {
  if (desktopUiMode(win) !== 'browser') return Promise.resolve({ kind: 'native' });
  if (win?.isSecureContext === false || typeof win?.showSaveFilePicker !== 'function') {
    return Promise.resolve({ kind: 'download' });
  }
  try {
    return Promise.resolve(win.showSaveFilePicker({
      id: 'acecode-theme-export',
      suggestedName: themeExportFilename(entry),
      types: [{ description: '主题包', accept: { 'application/zip': ['.zip'] } }],
      excludeAcceptAllOption: true,
    })).then((handle) => ({ kind: 'file', handle })).catch((error) => {
      if (error?.name === 'AbortError') return { kind: 'cancelled' };
      if (['SecurityError', 'NotSupportedError'].includes(error?.name)) return { kind: 'download' };
      throw error;
    });
  } catch (error) {
    if (error?.name === 'AbortError') return Promise.resolve({ kind: 'cancelled' });
    if (['SecurityError', 'NotSupportedError'].includes(error?.name)) return Promise.resolve({ kind: 'download' });
    return Promise.reject(error);
  }
}

export function downloadThemeBlob(blob, filename, {
  document: doc = globalThis.document,
  URLApi = globalThis.URL,
  schedule = globalThis.setTimeout?.bind(globalThis),
} = {}) {
  if (!blob || !doc?.createElement || !URLApi?.createObjectURL) throw new Error('当前浏览器无法下载主题包');
  let url = '', anchor;
  try {
    url = URLApi.createObjectURL(blob);
    anchor = doc.createElement('a');
    anchor.href = url;
    anchor.download = filename;
    anchor.style.display = 'none';
    (doc.body || doc.documentElement).append(anchor);
    anchor.click();
    anchor.remove();
    // Give the browser time to acquire the Blob before releasing its URL.
    if (schedule) schedule(() => URLApi.revokeObjectURL(url), 60000);
    else URLApi.revokeObjectURL(url);
  } catch (error) {
    anchor?.remove?.();
    if (url) URLApi.revokeObjectURL(url);
    throw error;
  }
}

export function createThemeExportController({
  api,
  chooseSave = chooseThemeSaveDestination,
  download = downloadThemeBlob,
  onChange,
  onComplete,
  wait = () => new Promise((resolve) => setTimeout(resolve, 200)),
}) {
  let state = { ...EMPTY_THEME_EXPORT }, active = true, operation = null;
  const patch = (change) => { state = { ...state, ...change }; if (active) onChange?.(state); };
  const consume = (job, current) => {
    if (!job || job.id !== current.entry.id || typeof job.job_id !== 'string' || !job.job_id || !EXPORT_STATES.has(job.state)) {
      throw new Error('导出任务返回了无效结果，请重试');
    }
    current.job = job;
    patch({ job, status: job.state, filename: themeExportFilename({ name: job.filename || current.entry.name }), hadCompression: state.hadCompression || job.state === 'compressing' });
  };
  const cancelRemote = (current) => {
    if (!current.job?.job_id) return Promise.resolve(null);
    if (!current.cancelPromise) current.cancelPromise = api.cancelThemeExport(current.job.job_id);
    return current.cancelPromise;
  };
  const finishNativeSave = (job) => {
    patch({ status: 'completed', job, cancelling: false, error: null });
    if (active) onComplete?.({ filename: state.filename, saved: true });
    return true;
  };
  const finishCancelled = async (current) => {
    if (current.job?.state === 'completed' && current.job.native_saved === true) return finishNativeSave(current.job);
    let cancelled;
    try { cancelled = await cancelRemote(current); }
    catch (error) {
      if (current.job?.state === 'completed' && current.job.native_saved === true) return finishNativeSave(current.job);
      throw error;
    }
    // Cancellation can race the atomic native file commit. Its first response
    // may still say "saving"; a later completed/native_saved result wins.
    let latest = current.job?.state === 'completed' && current.job.native_saved === true ? current.job : cancelled || current.job;
    while (current.native && latest && ['preparing', 'compressing', 'saving'].includes(latest.state)) {
      await wait();
      consume(await api.getThemeExport(latest.job_id), current);
      latest = current.job;
    }
    if (latest?.state === 'completed' && latest.native_saved === true) {
      return finishNativeSave(latest);
    } else if (latest?.state === 'failed') throw latest;
    else patch({ status: 'cancelled', ...(latest ? { job: latest } : {}), cancelling: false });
    return false;
  };
  return {
    state: () => state,
    activate() { active = true; },
    dispose() {
      active = false;
      if (!operation || operation.committing) return;
      operation.cancelled = true;
      operation.abort.abort();
      void cancelRemote(operation).catch(() => {});
    },
    dismissError() { if (!operation) patch({ error: null, status: 'idle' }); },
    async cancel() {
      const current = operation;
      if (!current || current.committing || current.cancelled) return false;
      current.cancelled = true;
      current.abort.abort();
      patch({ cancelling: true });
      try { await cancelRemote(current); }
      catch (error) {
        if (!(current.job?.state === 'completed' && current.job.native_saved === true)) patch({ error: themeManagementFailure(error, '取消导出失败，请重试') });
      }
      return true;
    },
    async start(entry) {
      if (!active || operation || !canManageTheme(entry)) return false;
      const current = { entry, job: null, cancelled: false, committing: false, abort: new AbortController(), writable: null, cancelPromise: null };
      operation = current;
      patch({ ...EMPTY_THEME_EXPORT, entry, status: 'choosing', filename: themeExportFilename(entry) });
      try {
        let destination = await chooseSave(entry);
        if (destination?.kind === 'cancelled' || current.cancelled || !active) return await finishCancelled(current);
        if (!['native', 'file', 'download'].includes(destination?.kind)) throw new Error('无法获取主题包保存位置');
        current.native = destination.kind === 'native';
        patch({ status: destination.kind === 'native' ? 'choosing' : 'preparing' });
        let job;
        try { job = await api.exportTheme(entry.id, { native_save: destination.kind === 'native' }); }
        catch (error) {
          if (destination.kind !== 'native' || error?.code !== 'THEME_NATIVE_SAVE_UNAVAILABLE') throw error;
          if (current.cancelled || !active) return await finishCancelled(current);
          destination = { kind: 'download' };
          current.native = false;
          job = await api.exportTheme(entry.id, { native_save: false });
        }
        if (job?.cancelled === true || job?.state === 'cancelled') { patch({ status: 'cancelled' }); return false; }
        consume(job, current);
        while (['preparing', 'compressing', 'saving'].includes(current.job.state)) {
          if (current.cancelled || !active) return await finishCancelled(current);
          await wait();
          if (current.cancelled || !active) return await finishCancelled(current);
          consume(await api.getThemeExport(current.job.job_id), current);
        }
        if (current.cancelled || !active) return await finishCancelled(current);
        if (current.job.state === 'cancelled') return false;
        if (current.job.state === 'failed') throw current.job;
        let saved = current.job.native_saved === true;
        if (!saved) {
          if (destination.kind === 'native') throw new Error('主题包未保存，请重新导出');
          patch({ status: 'saving' });
          const blob = await api.readThemeExport(current.job.job_id, { signal: current.abort.signal });
          if (current.cancelled || !active) return await finishCancelled(current);
          if (destination.kind === 'file') {
            current.writable = await destination.handle.createWritable();
            if (current.cancelled || !active) return await finishCancelled(current);
            await current.writable.write(blob);
            if (current.cancelled || !active) return await finishCancelled(current);
            current.committing = true;
            patch({ canCancel: false });
            await current.writable.close();
            current.writable = null;
            saved = true;
          } else {
            current.committing = true;
            patch({ canCancel: false });
            download(blob, state.filename);
          }
        }
        patch({ status: 'completed', cancelling: false });
        if (active) onComplete?.({ filename: state.filename, saved });
        return true;
      } catch (error) {
        if (current.cancelled && error?.name === 'AbortError') {
          try { return await finishCancelled(current); } catch (cancelError) { error = cancelError; }
        }
        patch({ status: 'failed', cancelling: false, error: themeManagementFailure(error, '导出主题失败，请重试') });
        // A broken polling request must not leave a native export silently saving.
        if (!current.committing) void cancelRemote(current).catch(() => {});
        return false;
      } finally {
        if (current.writable) { try { await current.writable.abort(); } catch { /* The destination may already be closed. */ } }
        if (operation === current) operation = null;
      }
    },
  };
}
