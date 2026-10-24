import { VsIcon } from './Icon.jsx';
import { useEffect, useRef, useState } from 'react';
import { Modal } from './Modal.jsx';
import { toast } from './Toast.jsx';
import { api } from '../lib/api.js';
import { observeThemeWorkshopUrl, validateThemeImportFile, themeImportError } from '../lib/themeImports.js';
import { themePackageSize } from '../lib/themePackages.js';

const actionClass = 'inline-flex items-center justify-center gap-1.5 px-3 py-2 rounded-md border border-border bg-surface text-[12px] hover:bg-surface-hi disabled:opacity-50';
function LibraryIcon({ workshop = false }) {
  return <VsIcon name={workshop ? 'world' : 'Upload'} size={16} />;
}

export function ThemeLibraryActions({ downloads }) {
  const [open, setOpen] = useState(false);
  const [url, setUrl] = useState('');
  const [linkError, setLinkError] = useState('');
  const [retry, setRetry] = useState(0);
  useEffect(() => observeThemeWorkshopUrl({
    loadConfig: () => api.getUpgradeConfig(), onUrl: setUrl, onError: setLinkError,
  }), [retry]);
  return <>
    <div className="flex flex-wrap items-start gap-2">
      <button className={actionClass} type="button" onClick={() => setOpen(true)}><LibraryIcon />本地导入</button>
      {url ? <a className={actionClass} href={url} target="_blank" rel="noopener noreferrer"><LibraryIcon workshop />主题工坊</a>
        : <button className={actionClass} type="button" disabled={!linkError} onClick={() => setRetry((value) => value + 1)} title={linkError || undefined}><LibraryIcon workshop />{linkError ? '重试主题工坊' : '主题工坊'}</button>}
    </div>
    {open && <ThemeImportDialog downloads={downloads} onClose={() => setOpen(false)} />}
  </>;
}

function ThemeImportDialog({ downloads, onClose }) {
  const input = useRef(null);
  const current = useRef({ revision: 0, abort: null, mounted: true });
  const [file, setFile] = useState(null);
  const [preview, setPreview] = useState(null);
  const [busy, setBusy] = useState('');
  const [failure, setFailure] = useState('');
  const [applyAfter, setApplyAfter] = useState(true);
  useEffect(() => { const state = current.current; state.mounted = true; return () => { state.mounted = false; ++state.revision; state.abort?.abort(); }; }, []);
  const choose = async (selected) => {
    const state = current.current;
    state.abort?.abort();
    const revision = ++state.revision;
    setPreview(null); setFailure(''); setFile(selected || null); setBusy('');
    try { validateThemeImportFile(selected); } catch (error) { setFailure(error.message); return; }
    state.abort = new AbortController(); setBusy('preview');
    try {
      const result = await api.previewThemeImport(selected, { signal: state.abort.signal });
      if (state.mounted && revision === state.revision) setPreview(result);
    } catch (error) { if (state.mounted && revision === state.revision && error.name !== 'AbortError') setFailure(themeImportError(error)); }
    finally { if (state.mounted && revision === state.revision) setBusy(''); }
  };
  const install = async () => {
    if (!preview || !file || busy) return;
    setBusy('install'); setFailure('');
    try {
      const result = await downloads.controller.importLocal(file, preview.package_sha256, applyAfter);
      if (current.current.mounted) { toast({ kind: 'ok', text: result.applied ? '主题已导入并应用' : '主题已导入' }); onClose(); }
    } catch (error) { if (current.current.mounted) setFailure(themeImportError(error)); }
    finally { if (current.current.mounted) setBusy(''); }
  };
  const definition = preview?.theme;
  const appearance = definition?.appearance || {};
  const installing = busy === 'install';
  return <Modal width={500} layerClassName="z-[400]" labelledBy="theme-import-title" onClose={onClose} dismissOnBackdrop={!installing} dismissOnEscape={!installing}>
    <div className="p-5">
      <div className="flex items-center justify-between gap-3"><h3 id="theme-import-title" className="text-base font-semibold">本地导入</h3><button type="button" className="text-fg-2 p-1 rounded hover:bg-surface-hi disabled:opacity-50" aria-label="关闭导入" disabled={installing} onClick={onClose}><VsIcon name="close" size={18} /></button></div>
      <input type="file" ref={input} accept=".zip,application/zip" className="hidden" onChange={(event) => { const selected = event.target.files?.[0]; event.target.value = ''; if (selected) void choose(selected); }} />
      <p className="mt-2 text-[12px] text-fg-mute">导入从 ACECode 导出或主题工坊下载的完整主题 ZIP</p>
      <button type="button" className="mt-4 w-full rounded-lg border border-dashed border-border py-5 px-3 text-sm text-accent hover:bg-surface-hi disabled:opacity-50" disabled={!!busy} onClick={() => input.current?.click()}>{file ? '更换文件' : '选择主题 ZIP'}</button>
      {file && <p className="mt-2 text-xs text-fg-2 break-all">{file.name} · {themePackageSize(file.size)}</p>}
      {busy && <p className="mt-4 text-sm text-fg-2" role="status">{installing ? '正在导入主题…' : '正在校验主题包…'}</p>}
      {definition && <div className="mt-4">
        <img src={preview.thumbnail_url} alt="待导入主题预览" className="w-full max-h-56 object-contain rounded-lg bg-surface" />
        <h4 className="mt-3 text-sm font-normal break-words">{definition.name}</h4>
        <p className="mt-1 text-xs text-fg-mute">{definition.mode === 'dark' ? '深色主题' : '浅色主题'} · v{definition.version}</p>
        <div className="flex gap-2 mt-3" aria-label="主题配色">{['accent', 'bg', 'surface', 'fg', 'send-bg'].map((key) => <span key={key} className="h-6 w-6 rounded border border-border" title={definition.colors[key]} style={{ backgroundColor: definition.colors[key] }} />)}</div>
        <dl className="grid grid-cols-[1fr_auto] gap-x-4 gap-y-2 mt-4 text-xs"><dt className="text-fg-mute">ACECode 图标</dt><dd>{appearance.logo_color || '原始配色'}</dd><dt className="text-fg-mute">首页标题</dt><dd>{appearance.home_title_color || definition.colors.fg}</dd><dt className="text-fg-mute">背景通顶</dt><dd>{appearance.extend_to_titlebar ? '开启' : '关闭'}</dd></dl>
      </div>}
      {failure && <p className="mt-4 text-xs text-danger break-words" role="alert">{failure}</p>}
      <label className="mt-5 flex items-center gap-2 text-sm"><input type="checkbox" checked={applyAfter} disabled={installing} onChange={(event) => setApplyAfter(event.target.checked)} />导入后立即应用</label>
      <div className="mt-5 flex justify-end gap-2"><button type="button" className={actionClass} disabled={installing} onClick={onClose}>取消</button><button type="button" data-ace-dialog-primary="true" className="px-3 py-2 rounded-md bg-accent text-white text-sm disabled:opacity-50" disabled={!preview || !!busy} onClick={() => void install()}>{installing ? '正在导入…' : applyAfter ? '导入并应用' : '导入主题'}</button></div>
    </div>
  </Modal>;
}
