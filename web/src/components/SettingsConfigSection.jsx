import { useEffect, useRef, useState } from 'react';
import { api } from '../lib/api.js';
import {
  MIGRATION_POLL_INTERVAL_MS, TOOLCHAIN_FIELDS, applyMigrationPollOutcome, environmentError, migrationFailureMessage,
  migrationPercent, migrationPollOutcome, migrationSkippedFilesHint, pickEnvironmentPath, terminalPath, toolchainPillState,
} from '../lib/environmentSettings.js';
import { desktopUpdateRestartAvailable, requestDesktopUpdateRestart } from '../lib/updateJob.js';
import { VsIcon } from './Icon.jsx';
import { Modal } from './Modal.jsx';
import { SkillScanPathsSettings } from './SkillScanPathsSettings.jsx';
import { UpgradeSettings } from './UpgradeSettings.jsx';

function Status({ label, tone = 'muted' }) {
  return <span className={`ace-settings-status ${tone === 'ok' ? 'text-ok' : tone === 'danger' ? 'text-danger' : 'text-fg-mute'}`}>{label}</span>;
}

function PathEditor({ id, label, detail, value = '', onSave, onBrowse, status, disabled, readOnly }) {
  const [draft, setDraft] = useState(value);
  useEffect(() => setDraft(value), [value]);
  return (
    <div className="ace-settings-row">
      <div className="ace-settings-label">
        <label htmlFor={id}>{label}</label>
        {detail && <small>{detail}</small>}
      </div>
      <div className="ace-settings-value">
        <input id={id} value={draft} placeholder="未设置" readOnly={readOnly} disabled={disabled}
          onChange={(event) => setDraft(event.target.value)}
          onBlur={async () => { if (onSave && draft.trim() !== value && !await onSave(draft.trim())) setDraft(value); }}
          onKeyDown={(event) => { if (event.key === 'Enter') event.currentTarget.blur(); }} />
        {onBrowse && <button type="button" className="ace-settings-button" disabled={disabled}
          onClick={onBrowse}><VsIcon name={id === 'terminal-program-path' ? 'file' : 'folder'} size={15} />{readOnly ? '更改' : id === 'terminal-program-path' ? '浏览' : '选择文件夹'}</button>}
        {status && <Status {...status} />}
      </div>
    </div>
  );
}

export function SettingsConfigSection({ skillPathsFocusRequest = 0 }) {
  const [toolchains, setToolchains] = useState([]);
  const [terminal, setTerminal] = useState(null);
  const [directory, setDirectory] = useState(null);
  const [pending, setPending] = useState(null);
  const [job, setJob] = useState(null);
  const [busy, setBusy] = useState('loading');
  const [error, setError] = useState('');
  const [saved, setSaved] = useState(false);
  const [confirmCleanup, setConfirmCleanup] = useState(false);
  const operation = useRef(false);
  const mounted = useRef(true);

  useEffect(() => {
    mounted.current = true;
    Promise.allSettled([api.getToolchainConfig(), api.getTerminalConfig(), api.getDataDirectory()])
      .then(([tools, shell, dir]) => {
        if (!mounted.current) return;
        if (tools.status === 'fulfilled') setToolchains(tools.value.toolchains || []);
        if (shell.status === 'fulfilled') setTerminal(shell.value);
        if (dir.status === 'fulfilled') {
          setDirectory(dir.value);
          setJob(dir.value.migration);
          if (dir.value.migration?.state === 'failed') setError(migrationFailureMessage(dir.value.migration));
        }
        const failed = [tools, shell, dir].find((result) => result.status === 'rejected');
        if (failed) setError(environmentError(failed.reason));
        setBusy('');
      });
    return () => { mounted.current = false; };
  }, []);

  useEffect(() => {
    if (job?.state !== 'running') return undefined;
    let cancelled = false;
    let timer;
    let failures = 0;
    // 判定全部交给 migrationPollOutcome;job 用函数式更新(本 effect 只依赖 job?.state,
    // 闭包里的 job 是旧的,直接 setJob(旧值) 会让进度条回跳)。失败即停,不再无限重试。
    const poll = async () => {
      let next = null;
      let requestError = null;
      try { next = await api.getDataDirectoryMigration(); }
      catch (err) { requestError = err; }
      if (cancelled) return;
      const outcome = migrationPollOutcome({ next, error: requestError, failures });
      failures = outcome.failures;
      setJob((prev) => applyMigrationPollOutcome(prev, outcome));
      if (outcome.message) setError(outcome.message);
      if (outcome.refreshDirectory) {
        setPending(null);
        // 目录刷新在轮询判定之外单独处理:失败只提示,不把已 done 的 job 打回 running。
        // job 变为 done 会触发本 effect 清理(cancelled=true),所以这里按组件是否挂载判断。
        try { const dir = await api.getDataDirectory(); if (mounted.current) setDirectory(dir); }
        catch (err) { if (mounted.current) setError(environmentError(err)); }
      }
      if (!outcome.stop && !cancelled) timer = setTimeout(poll, MIGRATION_POLL_INTERVAL_MS);
    };
    timer = setTimeout(poll, 400);
    return () => { cancelled = true; clearTimeout(timer); };
  }, [job?.state]);

  const perform = async (name, action) => {
    if (operation.current) return false;
    operation.current = true;
    setBusy(name); setError(''); setSaved(false);
    try { await action(); if (mounted.current && !['migrate', 'restart', 'refresh'].includes(name)) setSaved(true); return true; }
    catch (err) { if (mounted.current) setError(environmentError(err)); return false; }
    finally { operation.current = false; if (mounted.current) setBusy(''); }
  };

  const saveTool = (id, dir) => perform(id, async () => {
    const result = await api.setToolchainConfig({ [id]: dir });
    setToolchains(result.toolchains || []);
  });
  const saveTerminal = (patch) => perform('terminal', async () => {
    await api.setConsoleShellConfig(patch);
    setTerminal(await api.getTerminalConfig());
    window.dispatchEvent(new Event('ace-console-config-changed'));
  });
  const browseTool = async (id) => {
    try { const path = await pickEnvironmentPath('folder', api); if (path) await saveTool(id, path); }
    catch (err) { setError(environmentError(err)); document.getElementById(`toolchain-${id}`)?.focus(); }
  };
  const changeDirectory = async () => {
    try { const path = await pickEnvironmentPath('folder', api); if (path) { setPending(path); setError(''); } }
    catch (err) { setPending(''); setError(environmentError(err)); }
  };
  const selected = terminal?.default_shell || terminal?.resolved?.id || '';
  const resolved = terminal?.resolved;
  const fallback = !!resolved?.usable && (resolved.id !== selected || !!resolved.fallback_reason);
  const browseTerminal = async () => {
    setError('');
    setSaved(false);
    const initialFilePath = resolved?.usable && resolved.id === selected
      ? resolved.program : '';
    try {
      const path = await pickEnvironmentPath('file', api, { initialFilePath });
      if (path) await saveTerminal({ default_shell: selected, shell_path: path });
    } catch (err) {
      setError(environmentError(err));
      document.getElementById('terminal-program-path')?.focus();
    }
  };
  const migrating = job?.state === 'running';
  const restartRequired = job?.state === 'done' && job.restart_required;
  const progressUnknown = job?.state === 'unknown';
  const disabled = !!busy || migrating || restartRequired;
  const refetchMigration = () => perform('refresh', async () => {
    const dir = await api.getDataDirectory();
    if (!mounted.current) return;
    setDirectory(dir);
    setJob(dir.migration);
    if (dir.migration?.state === 'failed') setError(migrationFailureMessage(dir.migration));
  });

  return (
    <div className="ace-settings-config">
      <div className="flex items-center justify-between gap-4 mb-5">
        <h2 className="text-xl font-bold">配置</h2>
        <button className="ace-settings-button" disabled={disabled} onClick={() => perform('detect', async () => {
          const results = await Promise.allSettled([api.detectToolchains(), api.detectTerminal()]);
          if (results[0].status === 'fulfilled') setToolchains(results[0].value.toolchains || []);
          if (results[1].status === 'fulfilled') setTerminal(results[1].value);
          const failure = results.find((result) => result.status === 'rejected');
          if (failure) throw failure.reason;
          window.dispatchEvent(new Event('ace-console-config-changed'));
        })}><VsIcon name="refresh" size={15} />{busy === 'detect' ? '检测中…' : '重新检测'}</button>
      </div>
      <UpgradeSettings disabled={migrating || restartRequired} />
      <section className="ace-settings-group">
        <h3>工作空间依赖项</h3><p>为 Agent 指定工具目录，留空使用系统 PATH</p>
        {TOOLCHAIN_FIELDS.map((field) => {
          const item = toolchains.find((entry) => entry.id === field.id);
          return <PathEditor key={`${field.id}:${item?.dir || ''}`} id={`toolchain-${field.id}`} label={field.label}
            detail={field.detail} value={item?.dir || ''} disabled={disabled} status={toolchainPillState(item)}
            onSave={(path) => saveTool(field.id, path)} onBrowse={() => browseTool(field.id)} />;
        })}
      </section>
      <SkillScanPathsSettings focusRequest={skillPathsFocusRequest} disabled={migrating || restartRequired} />
      <section className="ace-settings-group">
        <h3>默认终端</h3>
        <div className="ace-settings-row">
          <label className="ace-settings-label" htmlFor="terminal-type">终端类型</label>
          <div className="ace-settings-value"><select id="terminal-type" value={selected} disabled={disabled || !terminal}
            onChange={(event) => saveTerminal({ default_shell: event.target.value })}>
            {!selected && <option value="">未找到可用终端</option>}
            {(terminal?.candidates || []).map((candidate) => <option key={candidate.id} value={candidate.id}>{candidate.label}</option>)}
          </select></div>
        </div>
        <PathEditor key={`${selected}:${terminalPath(terminal, selected)}`} id="terminal-program-path" label="终端程序路径"
          detail="留空从 PATH 查找" value={terminalPath(terminal, selected)} disabled={disabled || !selected}
          status={{ label: !resolved?.usable ? '不可用' : fallback ? '已回退' : '已找到', tone: resolved?.usable ? 'ok' : 'danger' }}
          onSave={(path) => saveTerminal({ default_shell: selected, shell_path: path })}
          onBrowse={browseTerminal} />
        {fallback && <p className="break-words text-warn">{resolved.program} · {resolved.fallback_reason}</p>}
      </section>
      <section className="ace-settings-group">
        <h3>工作空间路径</h3>
        <PathEditor id="workspace-current-path" label="当前路径" value={directory?.effective_dir || ''}
          readOnly disabled={disabled || !directory} onBrowse={changeDirectory} />
        {pending !== null && !migrating && !restartRequired && <div className="ace-settings-row items-start">
          <label className="ace-settings-label" htmlFor="workspace-new-path">新路径</label>
          <div className="ace-settings-pending">
            <input id="workspace-new-path" value={pending} disabled={!!busy} onChange={(event) => setPending(event.target.value)} autoFocus />
            <div className="flex flex-wrap items-center gap-3 mt-3">
              <button className="ace-settings-button ace-settings-primary" disabled={!!busy || !pending.trim()}
                onClick={() => perform('migrate', async () => setJob(await api.migrateDataDirectory(pending.trim())))}>确认迁移</button>
              <button className="ace-settings-button" disabled={!!busy} onClick={() => { setPending(null); setError(''); }}>取消</button>
              <span className="text-[12px] text-fg-mute">迁移会复制现有数据，完成后需重启 ACECode</span>
            </div>
          </div>
        </div>}
        {migrating && <div className="py-3" role="status"><div className="flex justify-between text-[13px] mb-2"><span>正在迁移工作空间…</span><span>{migrationPercent(job)}%</span></div>
          <progress className="w-full h-1.5 accent-accent" max="100" value={job.total_bytes ? migrationPercent(job) : undefined} /></div>}
        {progressUnknown && <div className="ace-settings-row"><span className="text-[13px]">无法获取迁移进度</span>
          <button className="ace-settings-button" disabled={!!busy} onClick={refetchMigration}>
            <VsIcon name="refresh" size={15} />重新获取</button></div>}
        {restartRequired && <div className="ace-settings-row"><div className="min-w-0 text-[13px]"><span>迁移完成，重启后使用新路径</span>
          {migrationSkippedFilesHint(job) && <small className="block mt-1 text-[12px] text-warn">{migrationSkippedFilesHint(job)}</small>}</div>
          {desktopUpdateRestartAvailable() ? <button className="ace-settings-button ace-settings-primary" disabled={!!busy}
            onClick={() => perform('restart', () => requestDesktopUpdateRestart())}>立即重启</button>
            : <span className="text-[12px] text-fg-mute">请完全退出并重新启动 ACECode</span>}</div>}
        {directory?.redirect_active && directory.previous_dir && <div className="ace-settings-row">
          <div className="min-w-0 text-[12px] text-fg-mute"><span>旧工作空间</span><div className="break-all mt-1">{directory.previous_dir}</div></div>
          <button className="ace-settings-button text-danger" disabled={disabled} onClick={() => setConfirmCleanup(true)}>删除旧数据</button>
        </div>}
      </section>
      <div className="min-h-6 text-[12px]" role="status" aria-live="polite">
        {error ? <span className="text-danger">{error}</span> : busy === 'loading' ? '加载中…' : saved ? <span className="text-fg-mute">已保存</span> : null}
      </div>
      {confirmCleanup && <Modal onClose={() => !busy && setConfirmCleanup(false)} layerClassName="z-[420]" labelledBy="delete-old-workspace">
        <div className="p-5"><h3 id="delete-old-workspace" className="font-semibold">删除旧工作空间数据？</h3>
          <p className="text-[13px] text-fg-mute break-all my-4">{directory.previous_dir}</p>
          <div className="flex justify-end gap-3"><button className="ace-settings-button" disabled={!!busy} onClick={() => setConfirmCleanup(false)}>取消</button>
            <button data-ace-dialog-primary="true" className="ace-settings-button text-danger" disabled={!!busy} onClick={() => perform('cleanup', async () => {
              setDirectory(await api.cleanupDataDirectory('delete')); setConfirmCleanup(false);
            })}>删除旧数据</button></div>
          {error && <p role="alert" className="text-danger text-[12px] mt-3">{error}</p>}
        </div>
      </Modal>}
    </div>
  );
}
