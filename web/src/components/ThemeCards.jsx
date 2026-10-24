import { VsIcon } from './Icon.jsx';
import { useEffect, useMemo, useState } from 'react';
import { Modal } from './Modal.jsx';
import { toast } from './Toast.jsx';
import { clsx } from '../lib/format.js';
import { api } from '../lib/api.js';
import { BUILTIN_THEME_CARDS, EVA_THEME_ID, NATIONAL_DAY_THEME_ID, themeDownloadPercent, themeJobActive, themePackageSize } from '../lib/themePackages.js';
import { canManageTheme, createThemeExportController, EMPTY_THEME_EXPORT, themeExportBusy, themeExportProgress, themeManagementFailure } from '../lib/themeExports.js';

export function LocalThemeCard({ entry, selected, onSelect, onExport, onDelete, actionsDisabled, deleting, exporting, job, onCancel, busy = false }) {
  const [thumbnail, setThumbnail] = useState('');
  useEffect(() => {
    let active = true, url = '';
    setThumbnail('');
    const request = entry.source === 'workshop' && !entry.installed
      ? api.readWorkshopThumbnail(entry.key)
      : api.readThemeImage(entry.id, 'thumbnail', entry.installed_version || entry.version);
    request.then((blob) => {
      if (!active) return;
      url = URL.createObjectURL(blob);
      setThumbnail(url);
    }).catch(() => {});
    return () => { active = false; if (url) URL.revokeObjectURL(url); };
  }, [entry.id, entry.version, entry.installed_version, entry.source, entry.key, entry.installed]);
  const downloading = themeJobActive(job) && job.id === entry.id;
  const canDownload = entry.source === 'workshop' && (!entry.installed || entry.update_available) && !busy;
  return (
    <div className={clsx('ace-theme-card ace-local-theme', selected && 'is-selected', (deleting || exporting) && 'has-active-operation')} data-theme-id={entry.id}>
      {thumbnail && <img className="ace-theme-card-background" src={thumbnail} alt="" draggable="false" />}
      <button type="button" className={clsx('ace-theme-card-choice', canDownload && 'can-download')} aria-pressed={selected}
        aria-busy={downloading} onClick={() => onSelect(entry.id)} disabled={deleting || (busy && !entry.installed)}>
        <div className="ace-theme-card-swatches" aria-hidden="true">
          {(Array.isArray(entry.swatches) ? entry.swatches : []).slice(0, 3).map((color, index) => <span key={index} style={{ backgroundColor: /^#[0-9a-f]{6}$/i.test(color) ? color : undefined }} />)}
        </div>
        <span className="ace-theme-card-name" title={entry.name}>{entry.name}</span>
        <span className="ace-theme-card-state">{entry.update_available ? '可更新' : selected ? '使用中' : entry.installed ? (entry.source === 'workshop' ? '已安装' : '已生成') : '未安装'}</span>
        {canDownload && <span className="ace-theme-card-download-cover" aria-hidden="true"><VsIcon name="Download" size={52} /></span>}
      </button>
      {downloading && <ThemeDownloadProgress job={job} onCancel={onCancel} />}
      {canManageTheme(entry) && (
        <div className="ace-theme-card-actions">
          <button type="button" className="ace-theme-export-link" disabled={actionsDisabled} onClick={(event) => { event.stopPropagation(); onExport(entry); }}>
            {exporting ? '正在导出…' : '导出主题'}
          </button>
          <button type="button" className="ace-theme-delete-link" disabled={actionsDisabled} onClick={(event) => { event.stopPropagation(); onDelete(entry); }}>
            {deleting ? '正在删除…' : '删除主题'}
          </button>
        </div>
      )}
    </div>
  );
}

export function ThemeCards({ options, selected, onSelect, downloads, onCreateAiTheme }) {
  const [confirmation, setConfirmation] = useState(null);
  const [deleteTarget, setDeleteTarget] = useState(null);
  const [deleteError, setDeleteError] = useState(null);
  const [deleting, setDeleting] = useState(false);
  const [exportState, setExportState] = useState(EMPTY_THEME_EXPORT);
  const exports = useMemo(() => createThemeExportController({
    api,
    onChange: setExportState,
    onComplete: ({ saved }) => toast({ kind: 'ok', text: saved ? '主题包已保存' : '主题包已交给浏览器下载' }),
  }), []);
  useEffect(() => { exports.activate(); return () => exports.dispose(); }, [exports]);
  const exportBusy = themeExportBusy(exportState);
  const exportProgress = themeExportProgress(exportState.job);
  const deleteTheme = async () => {
    if (!deleteTarget || deleting) return;
    setDeleting(true);
    setDeleteError(null);
    try {
      const result = await downloads.controller.remove(deleteTarget.id);
      setDeleteTarget(null);
      toast({ kind: result.cleanup_pending ? 'info' : 'ok', text: result.cleanup_pending ? '主题已删除，部分缓存待清理' : '主题已删除' });
    } catch (error) { setDeleteError(themeManagementFailure(error, '删除主题失败，请重试')); }
    finally { setDeleting(false); }
  };
  const job = downloads?.job;
  const busy = themeJobActive(job);
  const entryFor = (id) => downloads?.entries?.find((entry) => entry.id === id)
    || downloads?.workshopEntries?.find((entry) => entry.id === id)
    || (id === EVA_THEME_ID ? downloads?.entry : null);
  const workshopEntries = downloads?.workshopEntries || [];
  const customEntries = [...(downloads?.localEntries || []).filter((entry) => !workshopEntries.some((remote) => remote.id === entry.id)), ...workshopEntries];

  const selectBuiltin = async (id) => {
    let current = entryFor(id);
    if (!current) {
      current = await downloads?.controller.refresh({ notify: true, id });
    }
    if (!current || (current.available === false && !current.installed)) return;
    if (current.installed && (!current.update_available || busy)) void onSelect(id);
    else setConfirmation(current);
  };

  return (
    <>
      <div className="ace-theme-cards">
        {options.map((option) => (
          <div key={option.key} className={clsx('ace-theme-card', selected === option.key && 'is-selected')}>
            <button type="button" aria-pressed={selected === option.key}
              onClick={() => onSelect(option.key)} className="ace-theme-card-choice">
              <div className={clsx('ace-theme-card-swatches', `ace-theme-preview-${option.key}`)} aria-hidden="true">
                <span className="ace-theme-preview-bg" /><span className="ace-theme-preview-surface" /><span className="ace-theme-preview-accent" />
              </div>
              <span className="ace-theme-card-name">{option.label}</span>
            </button>
          </div>
        ))}
        {BUILTIN_THEME_CARDS.map((builtin) => {
          const entry = entryFor(builtin.id);
          const downloading = busy && job?.id === builtin.id;
          const canDownload = entry?.available !== false && (!entry?.installed || entry?.update_available) && !busy;
          return <div key={builtin.id} data-theme-id={builtin.id} className={clsx('ace-theme-card ace-downloadable-theme', selected === builtin.id && 'is-selected')}>
          <img className="ace-theme-card-background" src={builtin.thumbnail} alt="" draggable="false" />
          <button type="button" className={clsx('ace-theme-card-choice', canDownload && 'can-download')} aria-pressed={selected === builtin.id}
            aria-label={builtin.name} aria-busy={downloading || downloads?.loading} onClick={() => selectBuiltin(builtin.id)} disabled={(busy && !entry?.installed) || downloads?.loading || (entry?.available === false && !entry?.installed)}>
            <div className="ace-theme-card-swatches" aria-hidden="true">
              {builtin.swatches.map((fallback, index) => <span key={index} style={{ backgroundColor: /^#[0-9a-f]{6}$/i.test(entry?.swatches?.[index]) ? entry.swatches[index] : fallback }} />)}
            </div>
            <span className="ace-theme-card-name">{builtin.name}</span>
            <span className="ace-theme-card-state">{entry?.update_available ? '可更新' : selected === builtin.id && entry?.installed ? '使用中' : entry?.installed ? '已下载' : entry?.available === false ? '暂时无法下载' : '未下载'}</span>
            {canDownload && (
              <span className="ace-theme-card-download-cover" aria-hidden="true">
                <VsIcon name="Download" size={52} />
              </span>
            )}
          </button>
          {downloading && <ThemeDownloadProgress job={job} onCancel={() => downloads.controller.cancel()} />}
        </div>;
        })}
        <div className="ace-theme-card ace-ai-theme-action">
          <button type="button" className="ace-theme-card-choice" onClick={onCreateAiTheme}>
            <VsIcon name="MagicWand" className="ace-ai-theme-wand" size={29} />
            <span className="ace-theme-card-name">AI主题</span>
            <span className="ace-ai-theme-description">描述灵感，生成专属主题</span>
            <VsIcon name="arrowRight" className="ace-ai-theme-arrow" size={21} />
          </button>
        </div>
        {customEntries.map((local) => <LocalThemeCard key={local.id} entry={local} selected={selected === local.id}
          onSelect={local.source === 'workshop' ? selectBuiltin : onSelect}
          onExport={(theme) => { void exports.start(theme); }} onDelete={(theme) => { setDeleteError(null); setDeleteTarget(theme); }}
          actionsDisabled={busy || exportBusy || deleting || !!downloads?.deletingId} deleting={downloads?.deletingId === local.id}
          busy={busy} job={job} onCancel={() => downloads.controller.cancel()}
          exporting={exportBusy && exportState.entry?.id === local.id} />)}
      </div>
      {downloads?.workshopError && <p className="mt-2 text-[12px] text-fg-mute" role="status">{downloads.workshopError} <button type="button" className="text-accent hover:underline" onClick={() => downloads.controller.refresh()}>重试</button></p>}
      {downloads?.error && <p className="mt-2 text-[12px] text-fg-mute" role="status">{downloads.error} <button type="button" className="text-accent hover:underline" onClick={() => downloads.controller.refresh({ notify: true })}>重试</button></p>}
      {deleteTarget && (
        <Modal width={400} layerClassName="z-[400]" labelledBy="theme-delete-title" onClose={() => { if (!deleting) setDeleteTarget(null); }} dismissOnBackdrop={!deleting} dismissOnEscape={!deleting}>
          <div className="p-5">
            <h3 id="theme-delete-title" className="text-base font-semibold">删除主题</h3>
            <p className="mt-3 text-sm text-fg-2 break-words">{`将删除“${deleteTarget.name}”及其在应用内的主题包。`}</p>
            {selected === deleteTarget.id && <p className="mt-2 text-sm text-fg-2">删除后将切换为蓝色主题。</p>}
            {deleteError && <ThemeActionError failure={deleteError} />}
            <div className="flex justify-end gap-2 mt-5">
              <button type="button" disabled={deleting} className="px-3 py-1.5 rounded-md border border-border hover:bg-surface-hi disabled:opacity-50" onClick={() => setDeleteTarget(null)}>取消</button>
              <button type="button" data-ace-dialog-primary="true" disabled={deleting} className="px-3 py-1.5 rounded-md bg-danger text-white disabled:opacity-50" onClick={() => { void deleteTheme(); }}>{deleting ? '正在删除…' : '删除主题'}</button>
            </div>
          </div>
        </Modal>
      )}
      {exportBusy && exportState.hadCompression && (
        <Modal width={420} layerClassName="z-[400]" labelledBy="theme-export-title" onClose={() => { if (exportState.canCancel) void exports.cancel(); }} dismissOnBackdrop={exportState.canCancel} dismissOnEscape={exportState.canCancel}>
          <div className="p-5" aria-live="polite">
            <h3 id="theme-export-title" className="text-base font-semibold">{exportState.status === 'saving' ? '正在保存主题…' : '正在打包主题…'}</h3>
            <p className="mt-3 text-sm break-words">{exportState.filename}</p>
            {exportState.status === 'compressing' && (
              <div className="ace-theme-export-progress-row">
                <progress className="ace-theme-export-progress" max={1} value={exportProgress ?? undefined} aria-label="主题压缩进度" />
                {exportProgress !== null && <span className="text-sm text-fg-2 tabular-nums">{Math.floor(exportProgress * 100)}%</span>}
              </div>
            )}
            <p className="mt-2 text-sm text-fg-2">{exportState.cancelling ? '正在取消…' : exportState.status === 'saving' ? '正在保存主题包…' : '正在压缩主题资源…'}</p>
            <div className="flex justify-end mt-5">
              <button type="button" disabled={exportState.cancelling || !exportState.canCancel} className="px-3 py-1.5 rounded-md border border-border hover:bg-surface-hi disabled:opacity-50" onClick={() => { void exports.cancel(); }}>取消</button>
            </div>
          </div>
        </Modal>
      )}
      {exportState.error && !exportBusy && (
        <Modal width={420} layerClassName="z-[400]" labelledBy="theme-export-error-title" onClose={() => exports.dismissError()}>
          <div className="p-5">
            <h3 id="theme-export-error-title" className="text-base font-semibold">导出主题失败</h3>
            <ThemeActionError failure={exportState.error} />
            <div className="flex justify-end mt-5"><button type="button" data-ace-dialog-primary="true" className="px-3 py-1.5 rounded-md border border-border hover:bg-surface-hi" onClick={() => exports.dismissError()}>知道了</button></div>
          </div>
        </Modal>
      )}
      {confirmation && (
        <Modal width={400} layerClassName="z-[400]" labelledBy="theme-download-title" onClose={() => setConfirmation(null)} dismissOnBackdrop={false}>
          <div className="p-5">
            <h3 id="theme-download-title" className="text-base font-semibold">{`下载${confirmation.name || (confirmation.id === NATIONAL_DAY_THEME_ID ? '国庆节' : 'EVA 初号机')}主题`}</h3>
            <p className="mt-3 text-sm text-fg-2">需要下载 {themePackageSize(confirmation.package.bytes)}，下载完成后自动应用。</p>
            {confirmation.installed && (
              <button type="button" className="mt-3 text-sm text-accent hover:underline" onClick={() => {
                setConfirmation(null);
                void onSelect(confirmation.id);
              }}>使用已下载版本</button>
            )}
            <div className="flex justify-end gap-2 mt-5">
              <button type="button" className="px-3 py-1.5 rounded-md border border-border hover:bg-surface-hi" onClick={() => setConfirmation(null)}>取消</button>
              <button type="button" data-ace-dialog-primary="true" className="px-3 py-1.5 rounded-md bg-accent text-white" onClick={() => {
                const entry = confirmation;
                setConfirmation(null);
                void downloads.controller.install(entry);
              }}>下载并应用</button>
            </div>
          </div>
        </Modal>
      )}
    </>
  );
}

function ThemeDownloadProgress({ job, onCancel }) {
  return <div className="ace-theme-card-download" aria-live="polite">
    <div className="flex items-center justify-between gap-2">
      <span>{job.state === 'installing' ? '正在校验并安装…' : `下载中 ${themeDownloadPercent(job)}%`}</span>
      <button type="button" onClick={onCancel} className="opacity-80 hover:opacity-100 underline underline-offset-2">取消下载</button>
    </div>
    <div className="ace-theme-progress" role="progressbar" aria-label="主题下载进度" aria-valuemin={0} aria-valuemax={100} aria-valuenow={themeDownloadPercent(job)}>
      <span style={{ width: `${themeDownloadPercent(job)}%` }} />
    </div>
    <div>{themePackageSize(job.bytes_downloaded)} / {themePackageSize(job.bytes_total)}</div>
  </div>;
}

function ThemeActionError({ failure }) {
  return <div className="mt-3 text-sm break-words" role="alert">
    <p className="text-danger">{failure.message}</p>
    {failure.detail && <p className="mt-2 text-fg-2">{failure.detail}</p>}
    {failure.path && <p className="mt-2 text-fg-2">{failure.path}</p>}
  </div>;
}
