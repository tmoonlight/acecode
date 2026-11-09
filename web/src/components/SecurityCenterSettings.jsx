// 安全中心(Settings > 编码 > 安全中心,openspec add-security-center)。
// 四个分页:概览(沙箱开关 / 网络 / 默认禁止名单 / 平台状态 / 审计摘要)、
// 文件安全(三张清单 + 最近被拦路径一键加入)、命令安全(托管规则表 + 非托管文件只读)、
// 审计中心(筛选 / 导出 / 清空)。数据层全在 lib/securityCenter.js,这里只渲染与请求。
import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { api } from '../lib/api.js';
import { lookupErrorMessage } from '../lib/errors.js';
import { clsx, relativeTime } from '../lib/format.js';
import {
  AUDIT_CATEGORY_OPTIONS,
  AUDIT_DECISION_OPTIONS,
  AUDIT_PAGE_SIZE,
  AUDIT_TIME_OPTIONS,
  EXEC_RULE_DECISIONS,
  SECURITY_TABS,
  appendEntryLine,
  auditExportFilename,
  auditQueryString,
  auditRowPresentation,
  auditSummaryCounts,
  blockedPathToWritableRoot,
  defaultAuditFilters,
  execRuleRows,
  execRulesHaveChanges,
  execRulesPayload,
  mergeAuditEntries,
  newExecRuleRow,
  otherRuleFiles,
  platformNotes,
  sandboxDraft,
  sandboxDraftHasChanges,
  sandboxPayload,
  sandboxStatusLabel,
  securityErrorCode,
  validateExecRuleRows,
  validateSandboxDraft,
} from '../lib/securityCenter.js';
import { Modal, Toggle } from './Modal.jsx';
import { VsIcon } from './Icon.jsx';
import { toast } from './Toast.jsx';

const inputClass = 'w-full h-7 px-2 text-[12px] rounded-md border border-border bg-surface-alt text-fg outline-none focus:border-accent transition disabled:opacity-50';
const monoInputClass = `${inputClass} font-mono`;
const selectClass = 'h-7 px-2 text-[12px] rounded-md border border-border bg-surface-alt text-fg outline-none focus:border-accent transition disabled:opacity-50';
const textareaClass = 'w-full min-h-[88px] px-2 py-1.5 text-[12px] font-mono leading-5 rounded-md border border-border bg-surface-alt text-fg outline-none focus:border-accent transition disabled:opacity-50 resize-y';
const primaryButtonClass = 'px-3 py-1 text-[12px] bg-accent text-white rounded disabled:opacity-60';
const secondaryButtonClass = 'px-3 py-1 text-[12px] rounded border border-border bg-surface hover:bg-surface-hi transition disabled:opacity-50';
const linkButtonClass = 'px-1.5 py-0.5 text-[11px] hover:underline disabled:opacity-50';
const AUDIT_SUMMARY_POLL_MS = 15000;

const TONE_TEXT = { ok: 'text-ok', danger: 'text-danger', warn: 'text-warn', mute: 'text-fg-mute' };
const TONE_DOT = {
  ok: 'bg-ok shadow-[0_0_4px_var(--ace-ok)]',
  danger: 'bg-danger shadow-[0_0_4px_var(--ace-danger)]',
  warn: 'bg-warn shadow-[0_0_4px_var(--ace-warn)]',
  mute: 'bg-fg-mute',
};

function StatusPill({ tone = 'mute', text }) {
  return (
    <span className={clsx('flex items-center gap-1.5 text-[12px] shrink-0', TONE_TEXT[tone] || TONE_TEXT.mute)}>
      <span className={clsx('w-2 h-2 rounded-full', TONE_DOT[tone] || TONE_DOT.mute)} />
      {text}
    </span>
  );
}

function RowCard({ title, desc, children, onClick, className }) {
  const interactive = typeof onClick === 'function';
  return (
    <div
      role={interactive ? 'button' : undefined}
      tabIndex={interactive ? 0 : undefined}
      onClick={onClick}
      onKeyDown={interactive ? (e) => { if (e.key === ' ' || e.key === 'Enter') { e.preventDefault(); onClick(); } } : undefined}
      className={clsx(
        'flex items-center justify-between gap-3 px-3.5 py-2.5 rounded-md bg-surface border border-border mb-2',
        interactive && 'cursor-pointer hover:bg-surface-hi transition',
        className,
      )}
    >
      <div className="min-w-0">
        <div className="text-[13px] font-normal">{title}</div>
        {desc && <div className="text-[11px] text-fg-mute mt-0.5">{desc}</div>}
      </div>
      {children}
    </div>
  );
}

function NoteList({ notes }) {
  if (!notes?.length) return null;
  return (
    <div className="mb-3 space-y-1">
      {notes.map((note, index) => (
        <div key={index} role="status" className={clsx('text-[12px]', TONE_TEXT[note.tone] || TONE_TEXT.mute)}>
          {note.text}
        </div>
      ))}
    </div>
  );
}

function ErrorBanner({ error, fallback, onRetry, busy }) {
  if (!error) return null;
  const text = lookupErrorMessage(error.code, error.message || fallback);
  return (
    <div role="alert" className="mb-3 px-3 py-2 rounded-md border border-danger bg-surface text-danger text-[12px]">
      {text}
      {onRetry && (
        <button type="button" onClick={onRetry} disabled={busy} className="ml-2 hover:underline disabled:opacity-50">重试</button>
      )}
    </div>
  );
}

// ---------------------------------------------------------------------------
// 数据 hooks
// ---------------------------------------------------------------------------

function useSandboxSettings() {
  const [snapshot, setSnapshot] = useState(null);
  const [draft, setDraft] = useState(null);
  const [loading, setLoading] = useState(true);
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState(null);

  const load = useCallback(async () => {
    setLoading(true);
    setError(null);
    try {
      const data = await api.getSandboxSettings();
      setSnapshot(data);
      setDraft(sandboxDraft(data));
    } catch (e) {
      setError(securityErrorCode(e, 'load'));
    } finally {
      setLoading(false);
    }
  }, []);

  const save = useCallback(async (next) => {
    setSaving(true);
    setError(null);
    try {
      const data = await api.setSandboxSettings(sandboxPayload(next));
      setSnapshot(data);
      setDraft(sandboxDraft(data));
      toast({ kind: 'ok', text: '沙箱设置已保存' });
      return true;
    } catch (e) {
      const failure = securityErrorCode(e, 'save');
      setError(failure);
      toast({ kind: 'err', text: '沙箱设置保存失败:' + lookupErrorMessage(failure.code, failure.message) });
      return false;
    } finally {
      setSaving(false);
    }
  }, []);

  useEffect(() => { void load(); }, [load]);

  return { snapshot, draft, setDraft, loading, saving, error, load, save };
}

function useExecRules() {
  const [snapshot, setSnapshot] = useState(null);
  const [rows, setRows] = useState([]);
  const [loading, setLoading] = useState(true);
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState(null);

  const load = useCallback(async () => {
    setLoading(true);
    setError(null);
    try {
      const data = await api.getExecRules();
      setSnapshot(data);
      setRows(execRuleRows(data));
    } catch (e) {
      setError(securityErrorCode(e, 'load'));
    } finally {
      setLoading(false);
    }
  }, []);

  const save = useCallback(async (nextRows) => {
    setSaving(true);
    setError(null);
    try {
      const data = await api.setExecRules(execRulesPayload(nextRows));
      setSnapshot(data);
      setRows(execRuleRows(data));
      toast({ kind: 'ok', text: '命令规则已保存' });
      return true;
    } catch (e) {
      const failure = securityErrorCode(e, 'save');
      setError(failure);
      toast({ kind: 'err', text: '命令规则保存失败:' + lookupErrorMessage(failure.code, failure.message) });
      return false;
    } finally {
      setSaving(false);
    }
  }, []);

  useEffect(() => { void load(); }, [load]);

  return { snapshot, rows, setRows, loading, saving, error, load, save };
}

function useAudit(active) {
  const [filters, setFilters] = useState(defaultAuditFilters);
  const [entries, setEntries] = useState([]);
  const [total, setTotal] = useState(0);
  const [hasMore, setHasMore] = useState(false);
  const [summary, setSummary] = useState(null);
  const [loading, setLoading] = useState(false);
  const [busy, setBusy] = useState('');
  const [error, setError] = useState(null);
  const requestSeq = useRef(0);

  const refreshSummary = useCallback(async () => {
    try {
      setSummary(await api.auditSummary());
    } catch (e) {
      setError(securityErrorCode(e, 'load'));
    }
  }, []);

  const load = useCallback(async (nextFilters = filters) => {
    const seq = ++requestSeq.current;
    setLoading(true);
    setError(null);
    try {
      const page = await api.listAudit(auditQueryString(nextFilters));
      if (seq !== requestSeq.current) return;
      setEntries(page?.entries || []);
      setTotal(Number(page?.total) || 0);
      setHasMore(!!page?.has_more);
    } catch (e) {
      if (seq !== requestSeq.current) return;
      setError(securityErrorCode(e, 'load'));
    } finally {
      if (seq === requestSeq.current) setLoading(false);
    }
  }, [filters]);

  const loadMore = useCallback(async () => {
    const last = entries[entries.length - 1];
    if (!last) return;
    setBusy('more');
    try {
      const page = await api.listAudit(auditQueryString(filters, { beforeId: last.id }));
      setEntries((current) => mergeAuditEntries(current, page?.entries || []));
      setHasMore(!!page?.has_more);
      setTotal(Number(page?.total) || 0);
    } catch (e) {
      setError(securityErrorCode(e, 'load'));
    } finally {
      setBusy('');
    }
  }, [entries, filters]);

  const applyFilters = useCallback((patch) => {
    const next = { ...filters, ...patch };
    setFilters(next);
    void load(next);
  }, [filters, load]);

  const exportLogs = useCallback(async (format) => {
    setBusy(`export-${format}`);
    try {
      const base = auditQueryString(filters, { limit: 0 });
      const text = await api.exportAudit(`${base}${base ? '&' : '?'}format=${format}`);
      const blob = new Blob([typeof text === 'string' ? text : JSON.stringify(text)],
        { type: format === 'csv' ? 'text/csv;charset=utf-8' : 'application/x-ndjson;charset=utf-8' });
      const url = URL.createObjectURL(blob);
      const anchor = document.createElement('a');
      anchor.href = url;
      anchor.download = auditExportFilename(format);
      document.body.appendChild(anchor);
      anchor.click();
      anchor.remove();
      setTimeout(() => URL.revokeObjectURL(url), 1000);
      toast({ kind: 'ok', text: '审计日志已导出' });
    } catch (e) {
      const failure = securityErrorCode(e, 'export');
      toast({ kind: 'err', text: '导出失败:' + lookupErrorMessage(failure.code, failure.message) });
    } finally {
      setBusy('');
    }
  }, [filters]);

  const clear = useCallback(async () => {
    setBusy('clear');
    try {
      await api.clearAudit();
      setEntries([]);
      setTotal(0);
      setHasMore(false);
      await refreshSummary();
      toast({ kind: 'ok', text: '审计日志已清空' });
      return true;
    } catch (e) {
      const failure = securityErrorCode(e, 'clear');
      toast({ kind: 'err', text: '清空失败:' + lookupErrorMessage(failure.code, failure.message) });
      return false;
    } finally {
      setBusy('');
    }
  }, [refreshSummary]);

  useEffect(() => {
    if (!active) return undefined;
    void refreshSummary();
    const timer = setInterval(() => { void refreshSummary(); }, AUDIT_SUMMARY_POLL_MS);
    return () => clearInterval(timer);
  }, [active, refreshSummary]);

  return { filters, applyFilters, entries, total, hasMore, summary, loading, busy, error, load, loadMore, refreshSummary, exportLogs, clear };
}

// ---------------------------------------------------------------------------
// 分页
// ---------------------------------------------------------------------------

function OverviewTab({ sandbox, rules, audit, onNavigate }) {
  const { snapshot, draft, loading, saving, error, load, save } = sandbox;
  const status = sandboxStatusLabel(snapshot);
  const notes = useMemo(() => platformNotes(snapshot?.platform), [snapshot]);
  const counts = auditSummaryCounts(audit.summary);
  const denyCount = draft ? sandboxPayload(draft).filesystem.deny.length : 0;
  const ruleCount = rules.rows.length;
  const platform = snapshot?.platform;
  const toggle = (patch) => {
    if (!draft) return;
    const next = { ...draft, ...patch };
    sandbox.setDraft(next);
    void save(next);
  };
  const networkDesc = !platform ? '沙箱内命令是否可以访问网络'
    : platform.network_enforced ? '关闭后沙箱内命令无法联网(macOS / Linux 真隔离)'
    : platform.network_best_effort ? '关闭后只注入离线环境(代理指向无效端口、pip / npm 离线、ssh 桩),不走代理的程序仍能联网'
    : '当前后端不隔离网络,此开关不生效';

  return (
    <>
      <ErrorBanner error={error} fallback="加载沙箱设置失败" onRetry={load} busy={loading} />
      <div className="text-[14px] font-semibold mb-1">沙箱安全</div>
      <p className="text-[12px] text-fg-mute mb-3">AI 执行的命令运行在隔离沙箱里,文件、命令、网络策略按下面的规则生效。</p>
      <RowCard title="沙箱安全" desc="关闭后所有命令直接在当前用户权限下执行,自动模式的未知命令改为逐条确认">
        <div className="flex items-center gap-3">
          <StatusPill tone={status.tone} text={status.text} />
          <Toggle on={!!draft?.enabled} disabled={!draft || loading || saving} ariaLabel="沙箱安全"
            onChange={(value) => toggle({ enabled: value })} />
        </div>
      </RowCard>
      <RowCard title="网络访问" desc={networkDesc}>
        <Toggle on={!!draft?.networkAccess} disabled={!draft || loading || saving || !draft?.enabled} ariaLabel="网络访问"
          onChange={(value) => toggle({ networkAccess: value })} />
      </RowCard>
      <RowCard title="内置默认禁止名单"
        desc={`读写都拒绝:${(snapshot?.defaults?.deny || []).join('、') || '~/.ssh、~/.aws、~/.gnupg、~/.netrc、~/.docker/config.json、~/.kube'}`}>
        <Toggle on={!!draft?.denyDefaults} disabled={!draft || loading || saving || !draft?.enabled} ariaLabel="内置默认禁止名单"
          onChange={(value) => toggle({ denyDefaults: value })} />
      </RowCard>
      {platform && (
        <div className="px-3.5 py-2.5 rounded-md bg-surface border border-border mb-2">
          <div className="flex items-center justify-between gap-3">
            <div className="text-[13px] font-normal">平台状态</div>
            <span className="text-[11px] text-fg-mute font-mono">{platform.os} · {platform.backend}</span>
          </div>
          <div className="text-[11px] text-fg-mute mt-0.5">
            {[
              platform.available ? '沙箱后端可用' : `沙箱后端不可用:${platform.reason || '未知原因'}`,
              platform.read_isolation ? '读隔离生效' : '仅拦截写入',
              platform.network_enforced ? '可断网' : platform.network_best_effort ? '准断网' : '不隔离网络',
            ].join(' · ')}
          </div>
          <div className="mt-2"><NoteList notes={notes} /></div>
        </div>
      )}

      <div className="h-px bg-border my-5" />
      <div className="text-[14px] font-semibold mb-1">策略</div>
      <p className="text-[12px] text-fg-mute mb-3">文件与命令策略对所有会话生效,保存后活跃会话立即采用。</p>
      <RowCard title="文件安全" desc={`为沙箱配置可读、可写与禁止访问的路径 · 黑名单 ${denyCount} 条`} onClick={() => onNavigate('files')}>
        <VsIcon name="arrowRight" size={14} alt="" />
      </RowCard>
      <RowCard title="命令安全" desc={`为命令前缀配置放行、询问与禁止 · 规则 ${ruleCount} 条`} onClick={() => onNavigate('commands')}>
        <VsIcon name="arrowRight" size={14} alt="" />
      </RowCard>

      <div className="h-px bg-border my-5" />
      <div className="text-[14px] font-semibold mb-1">审计中心</div>
      <p className="text-[12px] text-fg-mute mb-3">拦截 / 放行记录与日志导出。</p>
      <div className="px-3.5 py-2.5 rounded-md bg-surface border border-border mb-2">
        <div className="flex items-center justify-between gap-3">
          <div className="text-[13px] font-normal">{`日志(${counts.total} 条)`}</div>
          <button type="button" className="text-[12px] text-accent hover:underline" onClick={() => onNavigate('audit')}>查看全部</button>
        </div>
        <div className="flex flex-wrap gap-4 mt-1 text-[11px] text-fg-mute">
          <span>{`放行 ${counts.allowed}`}</span>
          <span>{`拒绝 ${counts.denied}`}</span>
          <span>{`沙箱拦截 ${counts.blocked}`}</span>
          {audit.summary?.last_ts_ms > 0 && <span>{`最近:${relativeTime(audit.summary.last_ts_ms)}`}</span>}
        </div>
      </div>
    </>
  );
}

function ListEditor({ label, desc, value, onChange, error, disabled, placeholder }) {
  return (
    <div className="px-3.5 py-2.5 rounded-md bg-surface border border-border mb-2">
      <div className="text-[13px] font-normal">{label}</div>
      <div className="text-[11px] text-fg-mute mt-0.5 mb-2">{desc}</div>
      <textarea className={textareaClass} value={value} disabled={disabled} placeholder={placeholder}
        spellCheck={false} aria-label={label} aria-invalid={!!error}
        onChange={(e) => onChange(e.target.value)} />
      {error && <div className="text-[11px] text-danger mt-1">{error}</div>}
    </div>
  );
}

function FilesTab({ sandbox, audit }) {
  const { snapshot, draft, setDraft, loading, saving, error, load, save } = sandbox;
  const notes = useMemo(() => platformNotes(snapshot?.platform), [snapshot]);
  const errors = useMemo(() => (draft ? validateSandboxDraft(draft) : {}), [draft]);
  const changed = sandboxDraftHasChanges(draft, snapshot);
  const blocked = audit.summary?.blocked_paths || [];
  const update = (key, value) => setDraft((current) => ({ ...current, [key]: value }));
  const readOnlyPlatform = snapshot?.platform?.read_isolation === false;

  return (
    <>
      <ErrorBanner error={error} fallback="加载沙箱设置失败" onRetry={load} busy={loading} />
      <div className="text-[14px] font-semibold mb-1">文件安全</div>
      <p className="text-[12px] text-fg-mute mb-3">
        一行一条。~ 表示家目录,:workspace_roots 表示当前工作区根,:tmpdir 表示沙箱临时目录,其余必须是绝对路径。黑名单支持 **/.env 这类通配。
      </p>
      <NoteList notes={notes} />
      <ListEditor label="可读白名单" value={draft?.read || ''} error={errors.read} disabled={!draft || loading || saving}
        desc={readOnlyPlatform ? '当前平台不拦截读取,此清单不生效(仅 macOS / Linux)' : '非空时沙箱内只能读取这些目录与可写目录;留空表示全盘可读'}
        placeholder={'~/.cargo\n/usr/local/share'} onChange={(value) => update('read', value)} />
      <ListEditor label="可写白名单" value={draft?.write || ''} error={errors.write} disabled={!draft || loading || saving}
        desc="工作区之外额外允许写入的目录" placeholder={'D:\\shared\\out'} onChange={(value) => update('write', value)} />
      <ListEditor label="黑名单" value={draft?.deny || ''} error={errors.deny} disabled={!draft || loading || saving}
        desc={readOnlyPlatform ? '这些路径禁止写入(当前平台不拦截读取);优先级高于白名单' : '这些路径读写都拒绝;优先级高于白名单'}
        placeholder={'~/.ssh\n**/.env'} onChange={(value) => update('deny', value)} />
      <div className="flex items-center gap-2 mb-5">
        <button type="button" className={primaryButtonClass} disabled={!draft || !changed || saving || Object.keys(errors).length > 0}
          onClick={() => { void save(draft); }}>保存</button>
        <button type="button" className={secondaryButtonClass} disabled={!draft || !changed || saving}
          onClick={() => setDraft(sandboxDraft(snapshot))}>放弃修改</button>
        {changed && <span className="text-[11px] text-fg-mute">有未保存的修改</span>}
      </div>

      <div className="h-px bg-border my-5" />
      <div className="text-[14px] font-semibold mb-1">最近被沙箱拦截的路径</div>
      <p className="text-[12px] text-fg-mute mb-3">来自审计记录。加入白名单后记得保存;加入黑名单会让 AI 明确知道这条路径不可碰。</p>
      {blocked.length === 0 ? (
        <div className="px-3.5 py-4 rounded-md bg-surface border border-border text-[12px] text-fg-mute text-center mb-2">还没有被拦截的路径</div>
      ) : blocked.map((item) => (
        <div key={item.path} className="flex items-center justify-between gap-3 px-3.5 py-2 rounded-md bg-surface border border-border mb-2">
          <div className="min-w-0">
            <div className="text-[12px] font-mono truncate" title={item.path}>{item.path}</div>
            <div className="text-[11px] text-fg-mute mt-0.5">{`拦截 ${item.count} 次 · ${relativeTime(item.last_ts_ms)}`}</div>
          </div>
          <div className="flex items-center gap-1 shrink-0">
            <button type="button" className={linkButtonClass} disabled={!draft || saving}
              onClick={() => update('write', appendEntryLine(draft?.write, blockedPathToWritableRoot(item.path)))}>加入可写</button>
            <button type="button" className={clsx(linkButtonClass, 'text-danger')} disabled={!draft || saving}
              onClick={() => update('deny', appendEntryLine(draft?.deny, item.path))}>加入黑名单</button>
          </div>
        </div>
      ))}
    </>
  );
}

function CommandsTab({ rules }) {
  const { snapshot, rows, setRows, loading, saving, error, load, save } = rules;
  const errors = useMemo(() => validateExecRuleRows(rows), [rows]);
  const changed = execRulesHaveChanges(rows, snapshot);
  const others = otherRuleFiles(snapshot);
  const updateRow = (key, patch) => setRows((current) => current.map((row) => (row.key === key ? { ...row, ...patch } : row)));
  const removeRow = (key) => setRows((current) => current.filter((row) => row.key !== key));

  return (
    <>
      <ErrorBanner error={error} fallback="加载命令规则失败" onRetry={load} busy={loading} />
      <div className="text-[14px] font-semibold mb-1">命令安全</div>
      <p className="text-[12px] text-fg-mute mb-3">
        按命令前缀决定放行、询问或禁止。前缀按空格分词,git status|diff 表示第二个词可以是任一个。「放行(沙箱外)」会跳过沙箱,只给确实需要的命令;解释器、shell、rm、sudo 这类前缀不能放行。
      </p>
      <div className="rounded-md bg-surface border border-border mb-2 overflow-hidden" data-testid="exec-rules-table">
        <div className="grid grid-cols-[minmax(0,1.4fr)_9rem_minmax(0,1fr)_2rem] gap-3 px-3.5 py-2 text-[11px] font-normal text-fg-mute bg-surface-alt">
          <div>命令前缀</div>
          <div>决策</div>
          <div>说明</div>
          <div />
        </div>
        {rows.length === 0 && (
          <div className="px-3.5 py-4 text-[12px] text-fg-mute text-center border-t border-border">
            {loading ? '加载中' : '还没有规则。确认框里选「以后都允许」也会自动写到这里。'}
          </div>
        )}
        {rows.map((row) => (
          <div key={row.key} className="border-t border-border px-3.5 py-2">
            <div className="grid grid-cols-[minmax(0,1.4fr)_9rem_minmax(0,1fr)_2rem] items-center gap-3">
              <input className={monoInputClass} type="text" value={row.pattern} placeholder="git push" aria-label="命令前缀"
                aria-invalid={!!errors[row.key]} autoComplete="off" spellCheck={false} disabled={loading || saving}
                onChange={(e) => updateRow(row.key, { pattern: e.target.value })} />
              <select className={selectClass} value={row.decision} aria-label="决策" disabled={loading || saving}
                onChange={(e) => updateRow(row.key, { decision: e.target.value })}>
                {EXEC_RULE_DECISIONS.map((item) => <option key={item.key} value={item.key}>{item.label}</option>)}
              </select>
              <input className={inputClass} type="text" value={row.justification} placeholder="可选" aria-label="说明"
                autoComplete="off" disabled={loading || saving}
                onChange={(e) => updateRow(row.key, { justification: e.target.value })} />
              <button type="button" className="h-7 w-7 inline-flex items-center justify-center rounded-md text-fg-mute hover:text-danger hover:bg-surface-hi transition disabled:opacity-50"
                title="删除规则" aria-label="删除规则" disabled={loading || saving} onClick={() => removeRow(row.key)}>
                <VsIcon name="delete" size={14} alt="" />
              </button>
            </div>
            {errors[row.key] && <div className="text-[11px] text-danger mt-1">{errors[row.key]}</div>}
          </div>
        ))}
        <div className="flex items-center justify-between gap-3 px-3.5 py-2 border-t border-border text-[11px] text-fg-mute">
          <span className="min-w-0 truncate" title={snapshot?.dir || ''}>{`规则目录:${snapshot?.dir || ''}`}</span>
          <button type="button" className={clsx(linkButtonClass, 'shrink-0 whitespace-nowrap')} disabled={loading || saving}
            onClick={() => setRows((current) => [...current, newExecRuleRow()])}>添加规则</button>
        </div>
      </div>
      <div className="flex items-center gap-2 mb-5">
        <button type="button" className={primaryButtonClass} disabled={!snapshot || !changed || saving || Object.keys(errors).length > 0}
          onClick={() => { void save(rows); }}>保存</button>
        <button type="button" className={secondaryButtonClass} disabled={!snapshot || !changed || saving}
          onClick={() => setRows(execRuleRows(snapshot))}>放弃修改</button>
        {changed && <span className="text-[11px] text-fg-mute">有未保存的修改</span>}
      </div>

      {others.length > 0 && (
        <>
          <div className="h-px bg-border my-5" />
          <div className="text-[14px] font-semibold mb-1">其它规则文件</div>
          <p className="text-[12px] text-fg-mute mb-3">手写的规则文件只读展示,请直接编辑文件;解析失败的文件整体不生效。</p>
          {others.map((file) => (
            <div key={file.name} className="px-3.5 py-2.5 rounded-md bg-surface border border-border mb-2">
              <div className="flex items-center justify-between gap-3">
                <div className="text-[13px] font-normal font-mono">{file.name}</div>
                <span className="text-[11px] text-fg-mute truncate" title={file.path}>{file.path}</span>
              </div>
              {file.error ? (
                <div className="text-[11px] text-danger mt-1">{`解析失败,整个文件未生效:${file.error}`}</div>
              ) : (
                <div className="mt-1 space-y-0.5">
                  {(file.rules || []).map((rule, index) => (
                    <div key={index} className="text-[11px] text-fg-mute font-mono">
                      {rule.display}
                      <span className="ml-2 text-fg">{EXEC_RULE_DECISIONS.find((item) => item.decision === rule.decision && item.file === 'default.rules')?.label || rule.decision}</span>
                      {rule.justification && <span className="ml-2">{rule.justification}</span>}
                    </div>
                  ))}
                  {(file.rules || []).length === 0 && <div className="text-[11px] text-fg-mute">(空)</div>}
                </div>
              )}
            </div>
          ))}
        </>
      )}
    </>
  );
}

function AuditRow({ entry }) {
  const [open, setOpen] = useState(false);
  const row = auditRowPresentation(entry);
  const absolute = row.tsMs ? new Date(row.tsMs).toLocaleString() : '';
  return (
    <div className="border-t border-border">
      <div role="button" tabIndex={0} onClick={() => setOpen((value) => !value)}
        onKeyDown={(e) => { if (e.key === ' ' || e.key === 'Enter') { e.preventDefault(); setOpen((value) => !value); } }}
        className="flex items-start gap-3 px-3.5 py-2 cursor-pointer hover:bg-surface-hi transition">
        <span className={clsx('mt-1.5 w-2 h-2 rounded-full shrink-0', TONE_DOT[row.tone] || TONE_DOT.mute)} />
        <div className="min-w-0 flex-1">
          <div className="text-[12px] font-mono break-all">{row.title}</div>
          <div className="text-[11px] text-fg-mute mt-0.5">{row.meta}</div>
        </div>
        <div className="shrink-0 text-right">
          <div className={clsx('text-[11px]', TONE_TEXT[row.tone] || TONE_TEXT.mute)}>{row.decisionLabel}</div>
          <div className="text-[11px] text-fg-mute" title={absolute}>{relativeTime(row.tsMs)}</div>
        </div>
      </div>
      {open && (
        <div className="px-3.5 pb-2 pl-9 text-[11px] text-fg-mute space-y-0.5">
          <div>{`时间:${absolute}`}</div>
          {row.sessionId && <div className="font-mono">{`会话:${row.sessionId}`}</div>}
          {row.cwd && <div className="font-mono break-all">{`工作目录:${row.cwd}`}</div>}
          {row.extraPaths.length > 0 && <div className="font-mono break-all">{`路径:${row.extraPaths.join(', ')}`}</div>}
          {row.snippet && <div className="font-mono break-all">{`输出:${row.snippet}`}</div>}
        </div>
      )}
    </div>
  );
}

function AuditTab({ audit }) {
  const { filters, applyFilters, entries, total, hasMore, loading, busy, error, load, loadMore, exportLogs, clear } = audit;
  const [search, setSearch] = useState(filters.q);
  const [confirmClear, setConfirmClear] = useState(false);
  const submitSearch = () => applyFilters({ q: search });

  return (
    <>
      <ErrorBanner error={error} fallback="加载审计日志失败" onRetry={() => load()} busy={loading} />
      <div className="text-[14px] font-semibold mb-1">审计中心</div>
      <p className="text-[12px] text-fg-mute mb-3">{`每一次命令执行、文件修改、沙箱拦截与规则变更的判定记录;最多保留 ${audit.summary?.max_entries || 20000} 条。`}</p>
      <div className="flex flex-wrap items-center gap-2 mb-2">
        <select className={selectClass} value={filters.category} aria-label="类型" onChange={(e) => applyFilters({ category: e.target.value })}>
          {AUDIT_CATEGORY_OPTIONS.map((item) => <option key={item.key} value={item.key}>{item.label}</option>)}
        </select>
        <select className={selectClass} value={filters.decision} aria-label="结果" onChange={(e) => applyFilters({ decision: e.target.value })}>
          {AUDIT_DECISION_OPTIONS.map((item) => <option key={item.key} value={item.key}>{item.label}</option>)}
        </select>
        <select className={selectClass} value={filters.time} aria-label="时间" onChange={(e) => applyFilters({ time: e.target.value })}>
          {AUDIT_TIME_OPTIONS.map((item) => <option key={item.key} value={item.key}>{item.label}</option>)}
        </select>
        <input className={clsx(inputClass, 'w-48')} type="search" value={search} placeholder="搜索命令、路径或原因" aria-label="搜索"
          onChange={(e) => setSearch(e.target.value)}
          onKeyDown={(e) => { if (e.key === 'Enter') { e.preventDefault(); submitSearch(); } }}
          onBlur={() => { if (search !== filters.q) submitSearch(); }} />
        <button type="button" className={secondaryButtonClass} disabled={loading} onClick={() => load()} title="刷新">刷新</button>
        <span className="flex-1" />
        <button type="button" className={secondaryButtonClass} disabled={!!busy || total === 0} onClick={() => exportLogs('csv')}>导出 CSV</button>
        <button type="button" className={secondaryButtonClass} disabled={!!busy || total === 0} onClick={() => exportLogs('jsonl')}>导出 JSONL</button>
        <button type="button" className={clsx(secondaryButtonClass, 'text-danger')} disabled={!!busy || total === 0} onClick={() => setConfirmClear(true)}>清空记录</button>
      </div>
      <div className="rounded-md bg-surface border border-border mb-2 overflow-hidden">
        <div className="flex items-center justify-between gap-3 px-3.5 py-2 text-[11px] text-fg-mute bg-surface-alt">
          <span>{`日志(${total} 条)`}</span>
          {loading && <span className="flex items-center gap-1"><span className="ace-spinner" />加载中</span>}
        </div>
        {!loading && entries.length === 0 && (
          <div className="px-3.5 py-6 text-[12px] text-fg-mute text-center border-t border-border">没有符合条件的记录</div>
        )}
        {entries.map((entry) => <AuditRow key={entry.id} entry={entry} />)}
        {hasMore && (
          <div className="border-t border-border px-3.5 py-2 text-center">
            <button type="button" className={linkButtonClass} disabled={busy === 'more'} onClick={() => { void loadMore(); }}>
              {busy === 'more' ? '加载中' : `加载更多(每次 ${AUDIT_PAGE_SIZE} 条)`}
            </button>
          </div>
        )}
      </div>
      {confirmClear && (
        <Modal onClose={() => setConfirmClear(false)} width={420} labelledBy="ace-audit-clear-title">
          <div className="p-4">
            <div id="ace-audit-clear-title" className="text-[14px] font-semibold mb-2">清空审计记录?</div>
            <div className="text-[12px] text-fg-mute mb-4">{`将删除全部 ${total} 条记录,无法恢复。需要留档请先导出。`}</div>
            <div className="flex justify-end gap-2">
              <button type="button" className={secondaryButtonClass} onClick={() => setConfirmClear(false)}>取消</button>
              <button type="button" data-ace-dialog-primary="true" className="px-3 py-1 text-[12px] bg-danger text-white rounded disabled:opacity-60"
                disabled={busy === 'clear'} onClick={async () => { if (await clear()) setConfirmClear(false); }}>清空</button>
            </div>
          </div>
        </Modal>
      )}
    </>
  );
}

// ---------------------------------------------------------------------------

export function SecurityCenterSettings() {
  const [tab, setTab] = useState('overview');
  const sandbox = useSandboxSettings();
  const rules = useExecRules();
  const audit = useAudit(tab === 'overview' || tab === 'audit' || tab === 'files');

  useEffect(() => {
    if (tab === 'audit') void audit.load();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [tab]);

  return (
    <>
      <div className="flex items-start justify-between gap-4 mb-5">
        <div>
          <h2 className="text-xl font-bold mb-2">安全中心</h2>
          <p className="text-[12px] text-fg-mute">沙箱、文件与命令策略,以及每一次判定的审计记录。</p>
        </div>
      </div>
      <div role="tablist" aria-label="安全中心" className="flex flex-wrap items-center gap-1 mb-5">
        {SECURITY_TABS.map((item) => {
          const active = tab === item.key;
          return (
            <button key={item.key} type="button" role="tab" aria-selected={active} onClick={() => setTab(item.key)}
              className={clsx('px-3 py-1 text-[12px] rounded-md border transition',
                active ? 'bg-accent text-white border-accent' : 'bg-surface border-border hover:bg-surface-hi')}>
              {item.label}
            </button>
          );
        })}
      </div>
      {tab === 'overview' && <OverviewTab sandbox={sandbox} rules={rules} audit={audit} onNavigate={setTab} />}
      {tab === 'files' && <FilesTab sandbox={sandbox} audit={audit} />}
      {tab === 'commands' && <CommandsTab rules={rules} />}
      {tab === 'audit' && <AuditTab audit={audit} />}
    </>
  );
}
