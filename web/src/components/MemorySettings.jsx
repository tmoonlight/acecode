// 设置 > 个性化 > 记忆(openspec unify-memory-system)。
//
// 三块内容:
//   1. 设置行:使用记忆 / 记忆摘要 / 摘要模型。改动即保存(乐观更新,失败回滚并提示),
//      与个性化页其它设置一致;记忆摘要两项在「使用记忆」关闭或 daemon 不能运行摘要时禁用。
//   2. 记忆摘要状态:摘要开启时显示待整合观察数、最近提炼 / 整合时间与最近错误。
//   3. 条目浏览:「全局 / 当前工作区」页签,点条目打开编辑框(保存 / 取消 / 删除);
//      「重置」清空所选作用域,必须先勾选确认才能执行。
// 载荷规整、PATCH 构造、排序、校验与错误文案都在 lib/memorySettings.js(有 Node 单测),
// 这里只做渲染与请求编排。

import { useCallback, useEffect, useRef, useState } from 'react';
import { api } from '../lib/api.js';
import { clsx, formatDateTime, relativeTime } from '../lib/format.js';
import {
  MEMORY_SCOPE_GLOBAL,
  MEMORY_SCOPE_WORKSPACE,
  MEMORY_TYPE_OPTIONS,
  applyMemorySettingsPatch,
  memoryEntryDraft,
  memoryEntryDraftChanged,
  memoryEntryTimeMs,
  memoryEntryUpdateBody,
  memoryErrorCode,
  memoryErrorMessage,
  memoryScopeTabs,
  memorySettingsControls,
  memorySettingsPatch,
  memoryStatusView,
  memorySummaryModelNames,
  memorySummaryModelOptions,
  memoryTimeMs,
  memoryTypeLabel,
  memoryWorkspaceHash,
  normalizeMemoryEntryDetail,
  normalizeMemoryOverview,
  normalizeMemorySettings,
  resolveMemoryScope,
  shouldRetryMemoryOverviewWithoutWorkspace,
  validateMemoryEntryDraft,
} from '../lib/memorySettings.js';
import { Modal, Toggle } from './Modal.jsx';
import { RefreshIcon, VsIcon } from './Icon.jsx';
import { toast } from './Toast.jsx';

const rowClass = 'flex items-center justify-between gap-4 px-3.5 py-2.5 rounded-md bg-surface border border-border mb-2';
const selectClass = 'h-7 min-w-36 max-w-[16rem] px-2 text-[12px] rounded-md border border-border bg-surface-alt text-fg outline-none focus:border-accent transition disabled:opacity-50';
const fieldLabelClass = 'mb-1.5 block text-[11px] font-normal text-fg-2';
const inputClass = 'w-full h-8 px-2 text-[12px] rounded-md border border-border bg-surface-alt text-fg outline-none focus:border-accent transition disabled:opacity-50';
const textareaClass = 'w-full min-h-[220px] px-2.5 py-2 text-[12px] font-mono leading-5 rounded-md border border-border bg-surface-alt text-fg outline-none focus:border-accent transition disabled:opacity-50 resize-y';
const secondaryButtonClass = 'h-8 rounded-md border border-border bg-surface px-3.5 text-[12px] text-fg-2 transition hover:bg-surface-hi disabled:opacity-50';
const primaryButtonClass = 'inline-flex h-8 items-center gap-1.5 rounded-md bg-accent px-4 text-[12px] text-white transition hover:opacity-90 disabled:cursor-not-allowed disabled:opacity-50';
const dangerButtonClass = 'inline-flex h-8 items-center gap-1.5 rounded-md border border-danger/40 bg-danger-bg px-3.5 text-[12px] text-danger transition hover:opacity-80 disabled:cursor-not-allowed disabled:opacity-50';
const badgeClass = 'shrink-0 rounded-full border px-1.5 text-[10px] leading-4';

// 相对时间 + 悬停显示本地绝对时间;ms 为 0 时显示 empty。
function TimeText({ ms, empty = '' }) {
  if (!ms) return <span className="text-fg-mute">{empty}</span>;
  return (
    <time dateTime={new Date(ms).toISOString()} title={formatDateTime(ms, { dateStyle: 'medium', timeStyle: 'short' })}>
      {relativeTime(ms)}
    </time>
  );
}

function SummaryBadge() {
  return <span className={clsx(badgeClass, 'border-accent/40 bg-accent-bg text-accent')}>摘要生成</span>;
}

export function MemorySettings({ workspaceHash = '' }) {
  const hash = memoryWorkspaceHash(workspaceHash);
  const [settings, setSettings] = useState(null);
  const [settingsLoading, setSettingsLoading] = useState(true);
  const [settingsError, setSettingsError] = useState(null);
  const [settingsBusy, setSettingsBusy] = useState(false);
  const [modelNames, setModelNames] = useState([]);
  const [modelsLoaded, setModelsLoaded] = useState(false);
  const [overview, setOverview] = useState(null);
  const [overviewLoading, setOverviewLoading] = useState(true);
  const [overviewError, setOverviewError] = useState(null);
  const [workspaceUnknown, setWorkspaceUnknown] = useState(false);
  const [requestedScope, setRequestedScope] = useState(MEMORY_SCOPE_GLOBAL);
  const [editTarget, setEditTarget] = useState(null);
  const [resetScope, setResetScope] = useState('');
  const settingsRequestRef = useRef(0);
  const overviewRequestRef = useRef(0);
  const settingsBusyRef = useRef(false);

  const loadSettings = useCallback(async () => {
    const requestId = ++settingsRequestRef.current;
    setSettingsLoading(true);
    try {
      const payload = await api.getMemorySettings();
      if (requestId !== settingsRequestRef.current) return;
      setSettings(normalizeMemorySettings(payload));
      setSettingsError(null);
    } catch (error) {
      if (requestId === settingsRequestRef.current) setSettingsError(error);
    } finally {
      if (requestId === settingsRequestRef.current) setSettingsLoading(false);
    }
  }, []);

  const loadModels = useCallback(async () => {
    try {
      const names = memorySummaryModelNames(await api.listModels());
      setModelNames(names);
      setModelsLoaded(true);
    } catch {
      // 读不到模型列表时下拉只剩「当前模型」与已选项,不影响其它设置。
    }
  }, []);

  // 工作区未登记(404 UNKNOWN_WORKSPACE)时退回只读全局;其余错误原样展示。
  const loadOverview = useCallback(async () => {
    const requestId = ++overviewRequestRef.current;
    setOverviewLoading(true);
    try {
      let unknown = false;
      let payload;
      try {
        payload = await api.getMemoryOverview(hash);
      } catch (error) {
        if (!shouldRetryMemoryOverviewWithoutWorkspace(error, hash)) throw error;
        unknown = true;
        payload = await api.getMemoryOverview('');
      }
      if (requestId !== overviewRequestRef.current) return;
      setOverview(normalizeMemoryOverview(payload));
      setWorkspaceUnknown(unknown);
      setOverviewError(null);
    } catch (error) {
      if (requestId === overviewRequestRef.current) setOverviewError(error);
    } finally {
      if (requestId === overviewRequestRef.current) setOverviewLoading(false);
    }
  }, [hash]);

  useEffect(() => {
    void loadSettings();
    void loadModels();
    // 卸载后让还在路上的读取作废,不再回写状态。
    return () => { settingsRequestRef.current += 1; };
  }, [loadSettings, loadModels]);

  useEffect(() => {
    void loadOverview();
    return () => { overviewRequestRef.current += 1; };
  }, [loadOverview]);

  // 改动即保存:先乐观展示,以响应为准;失败回滚并提示。保存期间整组控件禁用。
  const changeSetting = async (field, value) => {
    if (!settings || settingsBusyRef.current) return;
    const patch = memorySettingsPatch(field, value);
    if (!patch) return;
    const previous = settings;
    settingsBusyRef.current = true;
    setSettingsBusy(true);
    setSettings(applyMemorySettingsPatch(previous, patch));
    try {
      setSettings(normalizeMemorySettings(await api.setMemorySettings(patch)));
    } catch (error) {
      setSettings(previous);
      toast({ kind: 'err', text: memoryErrorMessage(error, '保存记忆设置失败') });
    } finally {
      settingsBusyRef.current = false;
      setSettingsBusy(false);
    }
  };

  const controls = memorySettingsControls(settings, { loading: settingsLoading, busy: settingsBusy });
  const modelName = settings?.summary.model_name || '';
  const modelOptions = memorySummaryModelOptions(modelNames, modelName, { loaded: modelsLoaded });
  const modelUnavailable = modelOptions.some((option) => option.unavailable && option.value === modelName);
  const tabs = memoryScopeTabs(overview, { workspaceHash: hash, workspaceUnknown });
  const scope = resolveMemoryScope(requestedScope, tabs);
  const workspaceTab = tabs.find((tab) => tab.scope === MEMORY_SCOPE_WORKSPACE);
  const scopeData = overview?.scopes?.[scope];
  const entries = scopeData?.entries || [];
  const status = overview ? memoryStatusView(overview.status, { workspaceAvailable: !!workspaceTab?.available }) : null;
  // 旧版 daemon 没有这组接口:设置行已经报过错,条目区不再重复同一条错误。
  const unsupported = memoryErrorCode(settingsError) === 'MEMORY_UNSUPPORTED'
    && memoryErrorCode(overviewError) === 'MEMORY_UNSUPPORTED';

  return (
    <section data-memory-settings="true" aria-busy={settingsLoading || settingsBusy || overviewLoading}>
      <div className="text-[14px] font-semibold mb-1">记忆</div>
      <p className="text-[12px] text-fg-mute mb-3">
        让 ACECode 在会话之间记住你的偏好、反馈和项目背景。会话中也可以用 /memory 查看和整理记忆。
      </p>

      {settingsError && (
        <div role="alert" className="mb-2 text-[12px] text-danger">
          {memoryErrorMessage(settingsError, '加载记忆设置失败')}
          {!unsupported && (
            <button type="button" onClick={() => { void loadSettings(); }} disabled={settingsLoading}
              className="ml-2 hover:underline disabled:opacity-50">重试</button>
          )}
        </div>
      )}

      <div className={rowClass}>
        <div className="min-w-0">
          <div className="text-[13px] font-normal">使用记忆</div>
          <div className="text-[11px] text-fg-mute mt-0.5">
            {settings && !settings.enabled
              ? '已关闭：新会话不会注入记忆上下文，也不会提供记忆工具；已有记忆仍保留在磁盘上'
              : '新会话会读取已保存的记忆，并可以用记忆工具记录偏好和经验'}
          </div>
        </div>
        <Toggle on={!!settings?.enabled} disabled={controls.enabledDisabled} ariaLabel="使用记忆"
          onChange={(next) => { void changeSetting('enabled', next); }} />
      </div>

      <div className={rowClass}>
        <div className="min-w-0">
          <div className="text-[13px] font-normal">记忆摘要</div>
          <div className="text-[11px] text-fg-mute mt-0.5">
            会话闲置后自动提炼经验并整合成记忆；开启后会额外调用模型
          </div>
          {controls.summaryHint && (
            <div className={clsx('text-[11px] mt-0.5', controls.summaryHintTone === 'warn' ? 'text-warn' : 'text-fg-mute')}>
              {controls.summaryHint}
            </div>
          )}
        </div>
        <Toggle on={!!settings?.summary.enabled} disabled={controls.summaryDisabled} ariaLabel="记忆摘要"
          onChange={(next) => { void changeSetting('summary.enabled', next); }} />
      </div>

      <div className={rowClass}>
        <div className="min-w-0">
          <label htmlFor="memory-summary-model" className="block text-[13px] font-normal">摘要模型</label>
          <div className="text-[11px] text-fg-mute mt-0.5">
            提炼和整合记忆时调用的模型；「当前模型」沿用会话正在使用的模型
          </div>
          {modelUnavailable && (
            <div role="status" className="text-[11px] text-warn mt-0.5">所选摘要模型已不存在，请重新选择</div>
          )}
        </div>
        <select id="memory-summary-model" value={modelName} disabled={controls.summaryDisabled}
          aria-invalid={modelUnavailable}
          onChange={(event) => { void changeSetting('summary.model_name', event.target.value); }}
          className={selectClass}>
          {modelOptions.map((option) => (option.unavailable
            ? <option key={option.value} value={option.value} disabled>{option.label} · 模型不可用</option>
            : <option key={option.value || '__current_model__'} value={option.value}>{option.label}</option>))}
        </select>
      </div>

      {controls.showStatus && (
        <div className="mt-3 text-[12px]" aria-live="polite" data-memory-summary-status="true">
          {!status ? (
            <div className="text-fg-mute">
              {overviewLoading ? '正在读取记忆摘要状态…' : '暂时无法读取记忆摘要状态'}
            </div>
          ) : (
            <dl className="grid grid-cols-[auto_minmax(0,1fr)] gap-x-4 gap-y-1.5">
              <dt className="text-fg-mute" title="已从会话中提炼、尚未整合进记忆的观察">待整合观察</dt>
              <dd className="flex flex-wrap gap-x-3 tabular-nums">
                {status.pending.map((item) => (
                  <span key={item.scope}>
                    {item.scope === MEMORY_SCOPE_GLOBAL ? `全局 ${item.count}` : `当前工作区 ${item.count}`}
                  </span>
                ))}
              </dd>
              <dt className="text-fg-mute">最近提炼</dt>
              <dd><TimeText ms={status.lastExtractionMs} empty="尚未运行" /></dd>
              <dt className="text-fg-mute">最近整合</dt>
              <dd><TimeText ms={status.lastConsolidationMs} empty="尚未运行" /></dd>
              {status.error && (
                <>
                  <dt className="text-fg-mute">最近错误</dt>
                  <dd className={clsx('min-w-0 break-words', status.error.stale ? 'text-fg-mute' : 'text-danger')}>
                    {status.error.message}
                    {status.error.ms > 0 && (
                      <span className="ml-2 text-fg-mute"><TimeText ms={status.error.ms} /></span>
                    )}
                  </dd>
                </>
              )}
            </dl>
          )}
        </div>
      )}

      {!unsupported && (
        <>
          <div className="h-px bg-border my-5" />

          <div className="flex items-start justify-between gap-3 mb-1">
            <div className="text-[14px] font-semibold">记忆条目</div>
            <div className="flex shrink-0 items-center gap-1.5">
              <button type="button" title="刷新记忆列表" aria-label="刷新记忆列表"
                onClick={() => { void loadOverview(); }} disabled={overviewLoading}
                className="h-7 w-7 inline-flex items-center justify-center rounded-md text-fg-2 hover:bg-surface-hi transition disabled:opacity-50">
                <RefreshIcon size={14} className={clsx(overviewLoading && 'animate-spin')} />
              </button>
              <button type="button" onClick={() => setResetScope(scope)} disabled={!overview || overviewLoading}
                className="h-7 px-2.5 rounded-md border border-border bg-surface text-[12px] text-danger hover:bg-surface-hi transition disabled:opacity-50">
                重置
              </button>
            </div>
          </div>
          <p className="text-[12px] text-fg-mute mb-3">
            Agent 记下的记忆与记忆摘要整合出的记忆，按最近更新排列；点击条目可以查看和编辑。
          </p>

          <div className="flex flex-wrap items-center gap-x-3 gap-y-2 mb-3">
            <div role="tablist" aria-label="记忆作用域" className="flex items-center gap-1">
              {tabs.map((tab) => {
                const active = tab.scope === scope;
                return (
                  <button key={tab.scope} type="button" role="tab" id={`memory-scope-tab-${tab.scope}`}
                    aria-selected={active} aria-controls="memory-scope-panel"
                    disabled={!tab.available} title={tab.hint || undefined}
                    onClick={() => setRequestedScope(tab.scope)}
                    className={clsx(
                      'inline-flex items-center gap-1.5 px-3 py-1 text-[12px] rounded-md border transition disabled:cursor-not-allowed disabled:opacity-50',
                      active ? 'bg-accent text-white border-accent' : 'bg-surface border-border hover:bg-surface-hi',
                    )}>
                    <span>{tab.label}</span>
                    {overview && tab.available && (
                      <span className={clsx('text-[11px] tabular-nums', active ? 'opacity-80' : 'text-fg-mute')}>
                        {tab.count}
                      </span>
                    )}
                  </button>
                );
              })}
            </div>
            {workspaceTab?.hint && <span className="text-[11px] text-fg-mute">{workspaceTab.hint}</span>}
          </div>

          <div id="memory-scope-panel" role="tabpanel" aria-labelledby={`memory-scope-tab-${scope}`}>
            {overviewError && (
              <div role="alert" className="mb-2 text-[12px] text-danger">
                {memoryErrorMessage(overviewError, '加载记忆失败')}
                <button type="button" onClick={() => { void loadOverview(); }} disabled={overviewLoading}
                  className="ml-2 hover:underline disabled:opacity-50">重试</button>
              </div>
            )}
            {!overview && overviewLoading && (
              <div className="py-3 text-[12px] text-fg-mute"><span className="ace-spinner mr-2" />加载中</div>
            )}
            {overview && entries.length === 0 && (
              <div className="py-3 text-[12px] text-fg-mute">
                {scope === MEMORY_SCOPE_WORKSPACE ? '当前工作区还没有记忆' : '还没有全局记忆'}
              </div>
            )}
            {entries.map((entry) => (
              <MemoryEntryRow key={entry.name} entry={entry}
                onOpen={() => setEditTarget({ scope, name: entry.name, entry })} />
            ))}
            {scopeData?.dir && (
              <div className="mt-2 text-[11px] text-fg-mute break-all" title={scopeData.dir}>
                {`位置：${scopeData.dir}`}
              </div>
            )}
          </div>
        </>
      )}

      {editTarget && (
        <MemoryEntryDialog
          target={editTarget}
          workspaceHash={hash}
          onClose={() => setEditTarget(null)}
          onChanged={() => { void loadOverview(); }}
        />
      )}
      {resetScope && (
        <MemoryResetDialog
          scope={resetScope}
          workspaceHash={hash}
          entryCount={overview?.scopes?.[resetScope]?.entries?.length || 0}
          onClose={() => setResetScope('')}
          onDone={() => { void loadOverview(); }}
        />
      )}
    </section>
  );
}

function MemoryEntryRow({ entry, onOpen }) {
  const typeLabel = memoryTypeLabel(entry.type);
  return (
    <div role="button" tabIndex={0} data-memory-entry={entry.name}
      onClick={onOpen}
      onKeyDown={(event) => {
        if (event.key === 'Enter' || event.key === ' ') { event.preventDefault(); onOpen(); }
      }}
      className="group flex items-start justify-between gap-3 px-3.5 py-2.5 rounded-md bg-surface border border-border mb-2 cursor-pointer">
      <div className="min-w-0 flex-1">
        <div className="flex flex-wrap items-center gap-1.5">
          <span className="text-[13px] font-normal font-mono break-all transition-colors group-hover:text-accent">
            {entry.name}
          </span>
          {typeLabel && <span className={clsx(badgeClass, 'border-border bg-surface-alt text-fg-mute')}>{typeLabel}</span>}
          {entry.source === 'summary' && <SummaryBadge />}
        </div>
        <div className="text-[11px] text-fg-mute mt-0.5 line-clamp-2 break-words" title={entry.description || undefined}>
          {entry.description || '—'}
        </div>
      </div>
      <span className="shrink-0 pt-0.5 text-[11px] text-fg-mute">
        <TimeText ms={memoryEntryTimeMs(entry)} />
      </span>
    </div>
  );
}

// 编辑框:先读全文(GET 带 body)再允许编辑,避免拿列表里不含正文的摘要把正文覆盖成空。
// 保存后若有内容被脱敏(redactions > 0)留在框里展示保存后的内容并提示;否则关闭。
function MemoryEntryDialog({ target, workspaceHash, onClose, onChanged }) {
  const { scope, name } = target;
  const [detail, setDetail] = useState(null);
  const [draft, setDraft] = useState(() => memoryEntryDraft(target.entry));
  const [loading, setLoading] = useState(true);
  const [loadError, setLoadError] = useState(null);
  const [saving, setSaving] = useState(false);
  const [actionError, setActionError] = useState('');
  const [showErrors, setShowErrors] = useState(false);
  const [redactions, setRedactions] = useState(0);
  const [confirmDelete, setConfirmDelete] = useState(false);
  const [deleting, setDeleting] = useState(false);
  const busy = saving || deleting;

  useEffect(() => {
    let cancelled = false;
    setLoading(true);
    api.getMemoryEntry(scope, name, workspaceHash)
      .then((payload) => {
        if (cancelled) return;
        const loaded = normalizeMemoryEntryDetail(payload, scope);
        if (!loaded) {
          setLoadError({ message: '无法读取这条记忆' });
          return;
        }
        setDetail(loaded);
        setDraft(memoryEntryDraft(loaded));
        setLoadError(null);
      })
      .catch((error) => {
        if (cancelled) return;
        setLoadError(error);
        // 条目已被别处删除:顺手刷新列表,关掉编辑框后不会再看到它。
        if (memoryErrorCode(error) === 'NOT_FOUND') onChanged?.();
      })
      .finally(() => { if (!cancelled) setLoading(false); });
    return () => { cancelled = true; };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [scope, name, workspaceHash]);

  const errors = validateMemoryEntryDraft(draft);
  const hasErrors = Object.keys(errors).length > 0;
  const updateDraft = (field, value) => {
    setDraft((current) => ({ ...current, [field]: value }));
    setActionError('');
  };

  const saveEntry = async () => {
    if (!detail || busy) return;
    if (hasErrors) {
      setShowErrors(true);
      return;
    }
    if (!memoryEntryDraftChanged(draft, detail)) {
      onClose();
      return;
    }
    setSaving(true);
    setActionError('');
    try {
      const saved = normalizeMemoryEntryDetail(
        await api.updateMemoryEntry(scope, name, memoryEntryUpdateBody(draft), workspaceHash),
        scope,
      );
      onChanged?.();
      if (saved && saved.redactions > 0) {
        setDetail(saved);
        setDraft(memoryEntryDraft(saved));
        setRedactions(saved.redactions);
        return;
      }
      toast({ kind: 'ok', text: '记忆已保存' });
      onClose();
    } catch (error) {
      setActionError(memoryErrorMessage(error, '保存记忆失败'));
    } finally {
      setSaving(false);
    }
  };

  const deleteEntry = async () => {
    if (deleting) return;
    setDeleting(true);
    try {
      await api.deleteMemoryEntry(scope, name, workspaceHash);
      toast({ kind: 'ok', text: '记忆已删除' });
      onChanged?.();
      onClose();
    } catch (error) {
      setConfirmDelete(false);
      setActionError(memoryErrorMessage(error, '删除记忆失败'));
      if (memoryErrorCode(error) === 'NOT_FOUND') onChanged?.();
    } finally {
      setDeleting(false);
    }
  };

  const shown = detail || target.entry || {};
  const createdMs = memoryTimeMs(shown.created_at);
  const updatedMs = memoryTimeMs(shown.updated_at);
  const sessionCount = Array.isArray(shown.source_sessions) ? shown.source_sessions.length : 0;

  return (
    <>
      <Modal
        onClose={() => { if (!busy) onClose(); }}
        width="min(680px, calc(100vw - 32px))"
        dismissOnBackdrop={false}
        dismissOnEscape={!busy}
        layerClassName="z-[310]"
        labelledBy="memory-entry-dialog-title"
      >
        <div className="flex max-h-[min(760px,calc(100vh-32px))] flex-col" aria-busy={loading || busy}>
          <header className="flex shrink-0 items-start gap-3 border-b border-border px-5 py-4">
            <div className="min-w-0 flex-1">
              <h2 id="memory-entry-dialog-title" className="text-[15px] font-semibold text-fg font-mono break-all">
                {name}
              </h2>
              <div className="mt-1 flex flex-wrap items-center gap-x-3 gap-y-1 text-[11px] text-fg-mute">
                <span>{scope === MEMORY_SCOPE_WORKSPACE ? '当前工作区记忆' : '全局记忆'}</span>
                {shown.source === 'summary' && <SummaryBadge />}
                {createdMs > 0 && <span>创建于 <TimeText ms={createdMs} /></span>}
                {updatedMs > 0 && <span>更新于 <TimeText ms={updatedMs} /></span>}
                {sessionCount > 0 && <span>{`来自 ${sessionCount} 个会话`}</span>}
              </div>
            </div>
            <button
              type="button"
              onClick={onClose}
              disabled={busy}
              aria-label="关闭记忆编辑框"
              className="flex h-8 w-8 shrink-0 items-center justify-center rounded-md text-fg-mute transition hover:bg-surface-hi hover:text-fg disabled:opacity-40"
            >
              <VsIcon name="close" size={14} />
            </button>
          </header>

          <div className="min-h-0 flex-1 space-y-4 overflow-y-auto px-5 py-4">
            {loading && !detail && (
              <div className="text-[12px] text-fg-mute"><span className="ace-spinner mr-2" />加载中</div>
            )}
            {loadError && (
              <div role="alert" className="text-[12px] text-danger">
                {memoryErrorMessage(loadError, '读取记忆失败')}
              </div>
            )}
            {detail && (
              <>
                <div>
                  <label htmlFor="memory-entry-description" className={fieldLabelClass}>描述</label>
                  <input
                    id="memory-entry-description"
                    type="text"
                    autoFocus
                    value={draft.description}
                    disabled={busy}
                    aria-invalid={showErrors && !!errors.description}
                    onChange={(event) => updateDraft('description', event.target.value)}
                    className={inputClass}
                  />
                  {showErrors && errors.description && (
                    <div className="mt-1 text-[11px] text-danger">{errors.description}</div>
                  )}
                </div>
                <div>
                  <label htmlFor="memory-entry-type" className={fieldLabelClass}>类型</label>
                  <select
                    id="memory-entry-type"
                    value={draft.type}
                    disabled={busy}
                    onChange={(event) => updateDraft('type', event.target.value)}
                    className={clsx(selectClass, 'h-8')}
                  >
                    {!draft.type && <option value="" disabled>未知类型</option>}
                    {MEMORY_TYPE_OPTIONS.map((option) => (
                      <option key={option.value} value={option.value}>{option.label}</option>
                    ))}
                  </select>
                </div>
                <div>
                  <label htmlFor="memory-entry-body" className={fieldLabelClass}>内容</label>
                  <textarea
                    id="memory-entry-body"
                    value={draft.body}
                    rows={12}
                    spellCheck={false}
                    disabled={busy}
                    onChange={(event) => updateDraft('body', event.target.value)}
                    className={textareaClass}
                  />
                  <div className="mt-1 text-[11px] text-fg-mute">
                    支持 Markdown；保存时疑似密钥会被替换为 [REDACTED]
                  </div>
                </div>
                {redactions > 0 && (
                  <div role="status" className="text-[12px] text-warn">
                    {`已保存。${redactions} 处疑似密钥已替换为 [REDACTED]，上面显示的是保存后的内容。`}
                  </div>
                )}
                {detail.path && (
                  <div className="text-[11px] text-fg-mute break-all">{`文件：${detail.path}`}</div>
                )}
              </>
            )}
            {actionError && <div role="alert" className="text-[12px] text-danger">{actionError}</div>}
          </div>

          <footer className="flex shrink-0 flex-wrap items-center gap-2 border-t border-border bg-surface-alt px-5 py-3">
            <button
              type="button"
              onClick={() => setConfirmDelete(true)}
              disabled={!detail || busy}
              className="h-8 px-2 text-[12px] text-danger hover:underline disabled:opacity-50"
            >
              删除
            </button>
            <span className="flex-1" />
            <button type="button" onClick={onClose} disabled={busy} className={secondaryButtonClass}>
              取消
            </button>
            <button
              type="button"
              data-ace-dialog-primary="true"
              onClick={() => { void saveEntry(); }}
              disabled={!detail || busy}
              className={primaryButtonClass}
            >
              {saving && <span className="ace-spinner" />}
              保存
            </button>
          </footer>
        </div>
      </Modal>

      {confirmDelete && (
        <Modal
          onClose={() => { if (!deleting) setConfirmDelete(false); }}
          width={420}
          dismissOnBackdrop={!deleting}
          dismissOnEscape={!deleting}
          layerClassName="z-[330]"
          labelledBy="memory-delete-title"
        >
          <div className="p-4">
            <div id="memory-delete-title" className="text-[14px] font-semibold mb-2">删除这条记忆？</div>
            <div className="mb-2 text-[12.5px] font-mono text-fg break-all">{name}</div>
            <div className="text-[12.5px] text-fg-mute leading-relaxed mb-4">
              删除后无法恢复；记忆摘要之后也不会再自动生成这条记忆。
            </div>
            <div className="flex justify-end gap-2">
              <button type="button" onClick={() => setConfirmDelete(false)} disabled={deleting} className={secondaryButtonClass}>
                取消
              </button>
              <button
                type="button"
                data-ace-dialog-primary="true"
                onClick={() => { void deleteEntry(); }}
                disabled={deleting}
                className={dangerButtonClass}
              >
                {deleting && <span className="ace-spinner" />}
                删除
              </button>
            </div>
          </div>
        </Modal>
      )}
    </>
  );
}

// 重置是整个作用域的不可逆清空(条目、索引、待整合观察与归档):打开时「重置」按钮禁用,
// 必须先勾选确认 —— 这是第二道确认,避免一次误点 / 误按 Enter 就清空。
function MemoryResetDialog({ scope, workspaceHash, entryCount, onClose, onDone }) {
  const [acknowledged, setAcknowledged] = useState(false);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState('');
  const workspace = scope === MEMORY_SCOPE_WORKSPACE;

  const resetScope = async () => {
    if (!acknowledged || busy) return;
    setBusy(true);
    setError('');
    try {
      await api.resetMemoryScope(scope, workspaceHash);
      toast({ kind: 'ok', text: workspace ? '已重置当前工作区记忆' : '已重置全局记忆' });
      onDone?.();
      onClose();
    } catch (resetError) {
      setError(memoryErrorMessage(resetError, '重置记忆失败'));
    } finally {
      setBusy(false);
    }
  };

  return (
    <Modal
      onClose={() => { if (!busy) onClose(); }}
      width={460}
      dismissOnBackdrop={!busy}
      dismissOnEscape={!busy}
      layerClassName="z-[310]"
      labelledBy="memory-reset-title"
    >
      <div className="p-4">
        <div id="memory-reset-title" className="text-[14px] font-semibold mb-2">
          {workspace ? '重置当前工作区记忆？' : '重置全局记忆？'}
        </div>
        <div className="text-[12.5px] text-fg-mute leading-relaxed mb-3">
          {workspace
            ? `将永久清空当前工作区的 ${entryCount} 条记忆，以及索引、待整合观察和归档。全局记忆不受影响。`
            : `将永久清空全局的 ${entryCount} 条记忆，以及索引、待整合观察和归档。工作区记忆不受影响。`}
        </div>
        <label className="mb-4 flex cursor-pointer items-start gap-2 text-[12.5px] text-fg">
          <input
            type="checkbox"
            checked={acknowledged}
            disabled={busy}
            onChange={(event) => setAcknowledged(event.target.checked)}
            className="mt-0.5 accent-danger"
          />
          <span>我了解这些记忆会被永久删除，且无法撤销</span>
        </label>
        {error && <div role="alert" className="mb-3 text-[12px] text-danger">{error}</div>}
        <div className="flex justify-end gap-2">
          <button type="button" onClick={onClose} disabled={busy} className={secondaryButtonClass}>
            取消
          </button>
          <button
            type="button"
            data-ace-dialog-primary="true"
            onClick={() => { void resetScope(); }}
            disabled={!acknowledged || busy}
            className={dangerButtonClass}
          >
            {busy && <span className="ace-spinner" />}
            重置
          </button>
        </div>
      </div>
    </Modal>
  );
}
