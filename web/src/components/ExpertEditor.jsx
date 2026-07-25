import { useCallback, useEffect, useMemo, useState } from 'react';
import { api } from '../lib/api.js';
import {
  CAPABILITY_KINDS,
  capabilityOptionsWithSaved,
  collectExpertTags,
  expertPayloadFromForm,
  filterExperts,
  normalizeCapabilityCatalog,
  normalizeStringList,
  selectedTeamExperts,
  setCapabilityScopeMode,
  singleExpertsForTeam,
  toggleCapabilitySelection,
  toggleTeamExpert,
  validateExpertFormFields,
} from '../lib/expertComponents.js';
import { clsx } from '../lib/format.js';
import { Modal, Toggle } from './Modal.jsx';
import { ExpertAvatar } from './ExpertCatalog.jsx';
import { VsIcon } from './Icon.jsx';

const inputClass = 'h-8 w-full rounded-md border border-border bg-surface-alt px-2.5 text-[12px] text-fg outline-none transition placeholder:text-fg-mute focus:border-accent';
const textareaClass = 'w-full resize-y rounded-md border border-border bg-surface-alt px-2.5 py-2 text-[12px] leading-5 text-fg outline-none transition placeholder:text-fg-mute focus:border-accent';

function Field({ label, hint = '', error = '', required = false, children, className = '' }) {
  return (
    <label className={clsx('block min-w-0', className)}>
      <span className="flex items-center gap-1 text-[12px] font-medium text-fg-2">
        {label}
        {required && <span className="text-danger" aria-hidden="true">*</span>}
      </span>
      {hint && <span className="mt-0.5 block text-[10px] text-fg-mute">{hint}</span>}
      <span className="mt-1.5 block">{children}</span>
      {error && <span className="mt-1 block text-[10px] text-danger">{error}</span>}
    </label>
  );
}

function TagEditor({ value, suggestions, onChange }) {
  const [draft, setDraft] = useState('');
  const tags = normalizeStringList(value);
  const add = (raw) => {
    const additions = normalizeStringList(String(raw || '').split(/[,，\n]/));
    if (additions.length === 0) return;
    onChange(normalizeStringList([...tags, ...additions]));
    setDraft('');
  };
  const available = suggestions.filter((tag) => !tags.includes(tag)).slice(0, 8);

  return (
    <div className="rounded-md border border-border bg-surface-alt p-2 focus-within:border-accent">
      <div className="flex min-h-6 flex-wrap gap-1.5">
        {tags.map((tag) => (
          <span key={tag} className="inline-flex h-6 items-center gap-1 rounded bg-accent-bg px-2 text-[11px] text-accent">
            <span>{tag}</span>
            <button
              type="button"
              onClick={() => onChange(tags.filter((item) => item !== tag))}
              className="text-fg-mute hover:text-accent"
              aria-label={`移除 Tag ${tag}`}
            >
              <VsIcon name="close" size={10} />
            </button>
          </span>
        ))}
        <input
          value={draft}
          onChange={(event) => setDraft(event.target.value)}
          onBlur={() => add(draft)}
          onKeyDown={(event) => {
            if (event.key === 'Enter' || event.key === ',') {
              event.preventDefault();
              add(draft);
            }
            if (event.key === 'Backspace' && !draft && tags.length > 0) {
              onChange(tags.slice(0, -1));
            }
          }}
          className="h-6 min-w-[110px] flex-1 bg-transparent px-1 text-[12px] text-fg outline-none placeholder:text-fg-mute"
          placeholder={tags.length > 0 ? '继续添加 Tag' : '输入后按 Enter 添加'}
        />
      </div>
      {available.length > 0 && (
        <div className="mt-2 flex flex-wrap gap-1 border-t border-border pt-2">
          {available.map((tag) => (
            <button
              key={tag}
              type="button"
              onClick={() => onChange([...tags, tag])}
              className="h-5 rounded px-1.5 text-[10px] text-fg-mute hover:bg-surface-hi hover:text-fg"
            >
              + {tag}
            </button>
          ))}
        </div>
      )}
    </div>
  );
}

function BasicEditor({ form, errors, tagSuggestions, update }) {
  const isTeam = form.type === 'team';
  return (
    <div data-expert-editor-basic="true" className="grid grid-cols-1 gap-4 lg:grid-cols-2">
      <Field label={isTeam ? '专家团名称' : '专家名称'} error={errors.displayName} required>
        <input
          autoFocus
          className={inputClass}
          value={form.displayName}
          onChange={(event) => update('displayName', event.target.value)}
          placeholder={isTeam ? '产品交付专家团' : '高级开发工程师'}
        />
      </Field>
      <Field label={isTeam ? '创建者或团队称呼' : '姓名或称呼'} error={errors.author} required>
        <input
          className={inputClass}
          value={form.author}
          onChange={(event) => update('author', event.target.value)}
          placeholder={isTeam ? 'ACECode' : '吴八哥'}
        />
      </Field>
      <Field label="职业 / 定位">
        <input
          className={inputClass}
          value={form.profession}
          onChange={(event) => update('profession', event.target.value)}
          placeholder={isTeam ? '研发交付' : '资深开发工程师'}
        />
      </Field>
      <Field label="Tag" hint="Tag 可多选；同一专家会出现在每个已选 Tag 下">
        <TagEditor value={form.tags} suggestions={tagSuggestions} onChange={(value) => update('tags', value)} />
      </Field>
      <Field label="能力介绍" hint="简要说明什么时候适合派遣这个组件" className="lg:col-span-2">
        <textarea
          className={clsx(textareaClass, 'min-h-[76px]')}
          value={form.description}
          onChange={(event) => update('description', event.target.value)}
          placeholder="用一段话介绍能够解决的问题和交付方式"
        />
      </Field>
      <Field label="擅长领域" hint="一行一个；显示在卡片与详情中">
        <textarea
          className={clsx(textareaClass, 'min-h-[112px]')}
          value={form.expertiseText}
          onChange={(event) => update('expertiseText', event.target.value)}
          placeholder={'系统架构\n代码质量\n高并发'}
        />
      </Field>
      <Field label="开场白" hint="一行一个；只在详情中显示，可填入真实聊天框">
        <textarea
          className={clsx(textareaClass, 'min-h-[112px]')}
          value={form.quickPromptsText}
          onChange={(event) => update('quickPromptsText', event.target.value)}
          placeholder={'审查当前改动\n帮我设计这项功能'}
        />
      </Field>
      {!isTeam && (
        <Field
          label="工作方式 / 系统提示"
          hint="描述思考方法、工作步骤和交付格式"
          error={errors.instructions}
          required
          className="lg:col-span-2"
        >
          <textarea
            className={clsx(textareaClass, 'min-h-[148px]')}
            value={form.instructions}
            onChange={(event) => update('instructions', event.target.value)}
            placeholder="先理解目标和现有实现，再给出可验证的方案；说明关键取舍与风险。"
          />
        </Field>
      )}
    </div>
  );
}

function capabilityStatus(option) {
  const labels = {
    available: '可用',
    connected: '已连接',
    starting: '连接中',
    disabled: '已禁用',
    disconnected: '未连接',
    failed: '连接失败',
    cancelled: '已取消',
    timed_out: '连接超时',
    missing: '已缺失',
    unavailable: '不可用',
  };
  const warning = ['starting', 'disconnected', 'failed', 'timed_out'].includes(option.status);
  const danger = option.status === 'missing';
  return {
    label: labels[option.status] || (option.available ? '可用' : '不可用'),
    className: danger ? 'text-danger' : warning ? 'text-warn' : option.available ? 'text-ok' : 'text-fg-mute',
  };
}

function capabilityDisabledReason(option) {
  const reasons = {
    globally_disabled: '已在全局设置中禁用',
    not_allowed_by_global_policy: '不在全局允许范围内',
    runtime_unavailable: '当前运行环境不可用',
    starting: '正在建立连接',
    connection_failed: '连接失败，请检查全局 MCP 设置',
    connection_cancelled: '连接已取消',
    connection_timed_out: '连接超时',
    unavailable: '当前不可用',
  };
  return reasons[option.disabled_reason] || option.disabled_reason;
}

function capabilitySourceLabel(source) {
  const labels = {
    global: '全局',
    workspace: '项目',
    project: '项目',
    packaged: '专家包',
  };
  return labels[source] || source;
}

function CapabilityList({
  title,
  kind,
  options,
  selected,
  inherited,
  query,
  onQuery,
  onToggleInherited,
  onToggle,
}) {
  const visible = options.filter((option) => {
    const needle = query.trim().toLocaleLowerCase();
    return !needle || [option.id, option.label, option.description, option.source]
      .some((entry) => String(entry || '').toLocaleLowerCase().includes(needle));
  });

  return (
    <section className="min-w-0 rounded-lg border border-border bg-surface">
      <div className="border-b border-border px-3 py-2.5">
        <div className="flex items-center justify-between gap-3">
          <div>
            <h3 className="text-[13px] font-semibold text-fg">{title}</h3>
            <p className="mt-0.5 text-[10px] text-fg-mute">
              {inherited ? '继承所有全局可用项' : `已选 ${selected.length} / ${options.length}`}
            </p>
          </div>
          <label className="flex items-center gap-2 text-[10px] text-fg-mute">
            继承全局
            <Toggle on={inherited} onChange={onToggleInherited} />
          </label>
        </div>
        <label className="relative mt-2 block">
          <span className="sr-only">搜索 {title}</span>
          <VsIcon name="search" size={12} className="pointer-events-none absolute left-2 top-1/2 -translate-y-1/2 text-fg-mute" />
          <input
            value={query}
            onChange={(event) => onQuery(event.target.value)}
            disabled={inherited}
            className="h-7 w-full rounded-md border border-border bg-surface-alt pl-7 pr-2 text-[11px] text-fg outline-none focus:border-accent disabled:opacity-50"
            placeholder={`搜索 ${title}`}
          />
        </label>
      </div>
      <div className="max-h-[248px] overflow-y-auto p-1.5">
        {visible.length > 0 ? visible.map((option) => {
          const checked = selected.includes(option.id);
          const disabled = inherited || ((!option.available || !option.configurable) && !checked);
          const status = capabilityStatus(option);
          return (
            <label
              key={option.id}
              className={clsx(
                'flex min-h-11 items-start gap-2 rounded-md px-2 py-2 transition',
                disabled ? 'cursor-not-allowed opacity-55' : 'cursor-pointer hover:bg-surface-hi',
              )}
            >
              <input
                type="checkbox"
                checked={checked}
                disabled={disabled}
                onChange={() => onToggle(option.id)}
                className="mt-0.5 h-3.5 w-3.5 accent-[var(--ace-accent)]"
              />
              <span className="min-w-0 flex-1">
                <span className="flex items-center justify-between gap-2">
                  <span className="truncate text-[11px] font-medium text-fg">{option.label}</span>
                  <span className={clsx('shrink-0 text-[9px]', status.className)}>{status.label}</span>
                </span>
                <span className="mt-0.5 block truncate text-[9px] text-fg-mute">
                  {option.description || (kind === 'mcp_servers' ? option.transport : '') || option.id}
                </span>
                {(option.source || (kind === 'mcp_servers' && option.transport)) && (
                  <span className="mt-0.5 block truncate text-[9px] text-fg-mute">
                    {option.source && capabilitySourceLabel(option.source)}
                    {option.source && kind === 'mcp_servers' && option.transport ? ' · ' : ''}
                    {kind === 'mcp_servers' && option.transport ? option.transport.toUpperCase() : ''}
                  </span>
                )}
                {capabilityDisabledReason(option) && (
                  <span className="mt-0.5 block text-[9px] text-fg-mute">{capabilityDisabledReason(option)}</span>
                )}
              </span>
            </label>
          );
        }) : (
          <div className="flex h-20 items-center justify-center text-[11px] text-fg-mute">
            没有匹配的{title}
          </div>
        )}
      </div>
    </section>
  );
}

function ToolScope({
  options,
  selected,
  inherited,
  onToggleInherited,
  onToggle,
}) {
  return (
    <section className="rounded-lg border border-border bg-surface">
      <div className="flex items-center justify-between gap-4 border-b border-border px-3 py-2.5">
        <div>
          <h3 className="text-[13px] font-semibold text-fg">ACECode 本地工具</h3>
          <p className="mt-0.5 text-[10px] text-fg-mute">
            {inherited ? '继承所有全局可用工具' : `已开启 ${selected.length} / ${options.length}`}
          </p>
        </div>
        <label className="flex items-center gap-2 text-[10px] text-fg-mute">
          继承全局
          <Toggle on={inherited} onChange={onToggleInherited} />
        </label>
      </div>
      <div className="grid grid-cols-1 gap-px bg-border sm:grid-cols-2">
        {options.map((option) => {
          const checked = selected.includes(option.id);
          const canRemove = checked && !inherited;
          const disabled = inherited || (!canRemove && (!option.available || !option.configurable));
          const status = capabilityStatus(option);
          return (
            <div key={option.id} className="flex min-h-[54px] items-center justify-between gap-3 bg-surface px-3 py-2">
              <div className="min-w-0">
                <div className="truncate text-[11px] font-medium text-fg">{option.label}</div>
                <div className={clsx('mt-0.5 truncate text-[9px]', status.className)}>
                  {capabilityDisabledReason(option) || status.label}
                </div>
              </div>
              <Toggle on={checked} disabled={disabled} onChange={() => onToggle(option.id)} />
            </div>
          );
        })}
      </div>
    </section>
  );
}

function AdvancedEditor({
  form,
  updateCapabilities,
  catalog,
  loading,
  error,
  onRetry,
}) {
  const [queries, setQueries] = useState({ skills: '', mcp_servers: '' });
  const capabilities = form.capabilities || {};
  const optionGroups = useMemo(() => Object.fromEntries(CAPABILITY_KINDS.map((kind) => [
    kind,
    capabilityOptionsWithSaved(catalog[kind], capabilities[kind], kind),
  ])), [capabilities, catalog]);

  const inherited = (kind) => !Object.prototype.hasOwnProperty.call(capabilities, kind);
  const selected = (kind) => normalizeStringList(capabilities[kind]);
  const setInherited = (kind, on) => {
    updateCapabilities(setCapabilityScopeMode(capabilities, kind, on ? 'inherit' : 'custom'));
  };
  const toggle = (kind, id) => updateCapabilities(toggleCapabilitySelection(capabilities, kind, id));
  const total = CAPABILITY_KINDS.reduce((sum, kind) => sum + selected(kind).length, 0);

  return (
    <div data-expert-editor-advanced="true">
      <div className="mb-4 rounded-lg border border-border bg-surface-alt px-3.5 py-3">
        <div className="text-[13px] font-semibold text-fg">该专家的能力范围</div>
        <p className="mt-1 text-[11px] leading-5 text-fg-mute">
          已明确选择 {total} 项。每一类都可选择继承全局，或改为只允许已勾选项；专家设置不会突破全局权限、安全策略或沙箱。
        </p>
      </div>

      {loading ? (
        <div className="flex h-40 items-center justify-center text-[12px] text-fg-mute">
          <span className="ace-spinner mr-2 h-4 w-4" />
          正在读取运行时能力…
        </div>
      ) : error ? (
        <div className="flex h-40 flex-col items-center justify-center rounded-lg border border-danger bg-surface text-center">
          <p className="text-[12px] text-danger">能力目录加载失败</p>
          <p className="mt-1 max-w-[420px] text-[10px] text-fg-mute">{error}</p>
          <button type="button" onClick={onRetry} className="mt-3 h-7 rounded-md border border-border px-3 text-[11px] text-fg-2 hover:bg-surface-hi">
            重新加载
          </button>
        </div>
      ) : (
        <div className="space-y-4">
          <div className="grid grid-cols-1 gap-4 lg:grid-cols-2">
            <CapabilityList
              title="Skill"
              kind="skills"
              options={optionGroups.skills}
              selected={selected('skills')}
              inherited={inherited('skills')}
              query={queries.skills}
              onQuery={(value) => setQueries((current) => ({ ...current, skills: value }))}
              onToggleInherited={(on) => setInherited('skills', on)}
              onToggle={(id) => toggle('skills', id)}
            />
            <CapabilityList
              title="MCP"
              kind="mcp_servers"
              options={optionGroups.mcp_servers}
              selected={selected('mcp_servers')}
              inherited={inherited('mcp_servers')}
              query={queries.mcp_servers}
              onQuery={(value) => setQueries((current) => ({ ...current, mcp_servers: value }))}
              onToggleInherited={(on) => setInherited('mcp_servers', on)}
              onToggle={(id) => toggle('mcp_servers', id)}
            />
          </div>
          <ToolScope
            options={optionGroups.tools}
            selected={selected('tools')}
            inherited={inherited('tools')}
            onToggleInherited={(on) => setInherited('tools', on)}
            onToggle={(id) => toggle('tools', id)}
          />
        </div>
      )}
    </div>
  );
}

function MemberPickerDialog({ form, experts, onConfirm, onClose }) {
  const [selectedIds, setSelectedIds] = useState(() => normalizeStringList(form.selectedExpertIds));
  const [query, setQuery] = useState('');
  const [tag, setTag] = useState('all');
  const candidates = useMemo(() => singleExpertsForTeam(experts, form.id), [experts, form.id]);
  const tags = useMemo(() => collectExpertTags(candidates, 'agent'), [candidates]);
  const visible = useMemo(
    () => filterExperts(candidates, { type: 'agent', query, tag }),
    [candidates, query, tag],
  );

  const toggle = (id) => setSelectedIds((current) => (
    current.includes(id) ? current.filter((item) => item !== id) : [...current, id]
  ));

  return (
    <Modal onClose={onClose} width={720} layerClassName="z-[310]" labelledBy="team-member-picker-title">
      <div data-team-expert-picker="true" className="flex max-h-[82vh] flex-col">
        <header className="flex items-start justify-between gap-3 border-b border-border px-5 py-3.5">
          <div>
            <h2 id="team-member-picker-title" className="text-[15px] font-semibold text-fg">添加专家</h2>
            <p className="mt-0.5 text-[10px] text-fg-mute">可搜索或按 Tag 筛选，一次添加多位专家。</p>
          </div>
          <button type="button" onClick={onClose} className="p-1 text-fg-mute hover:text-fg" aria-label="关闭">
            <VsIcon name="close" size={16} />
          </button>
        </header>
        <div className="border-b border-border px-5 py-3">
          <label className="relative block">
            <VsIcon name="search" size={13} className="pointer-events-none absolute left-2.5 top-1/2 -translate-y-1/2 text-fg-mute" />
            <input
              autoFocus
              value={query}
              onChange={(event) => setQuery(event.target.value)}
              className={clsx(inputClass, 'pl-8')}
              placeholder="搜索专家名称、Tag 或擅长领域"
            />
          </label>
          <div className="mt-2 flex gap-1 overflow-x-auto pb-1">
            {['all', ...tags].map((item) => (
              <button
                key={item}
                type="button"
                onClick={() => setTag(item)}
                className={clsx(
                  'h-6 shrink-0 rounded px-2 text-[10px]',
                  tag === item ? 'bg-accent-bg text-accent' : 'text-fg-mute hover:bg-surface-hi hover:text-fg',
                )}
              >
                {item === 'all' ? '全部' : item}
              </button>
            ))}
          </div>
        </div>
        <div className="flex-1 overflow-y-auto px-5 py-3">
          {visible.length > 0 ? (
            <div className="overflow-hidden rounded-lg border border-border">
              {visible.map((expert) => {
                const selected = selectedIds.includes(expert.id);
                return (
                  <button
                    key={expert.id}
                    type="button"
                    onClick={() => toggle(expert.id)}
                    className={clsx(
                      'flex w-full items-center gap-3 border-b border-border px-3 py-2.5 text-left last:border-b-0',
                      selected ? 'bg-accent-bg' : 'bg-surface hover:bg-surface-hi',
                    )}
                  >
                    <ExpertAvatar expert={expert} size={32} />
                    <span className="min-w-0 flex-1">
                      <span className="block truncate text-[12px] font-medium text-fg">{expert.display_name}</span>
                      <span className="mt-0.5 block truncate text-[10px] text-fg-mute">
                        {expert.profession || '专家'}{expert.expertise[0] ? ` · 擅长${expert.expertise[0]}` : ''}
                      </span>
                    </span>
                    <span className={clsx(
                      'flex h-5 w-5 items-center justify-center rounded border',
                      selected ? 'border-accent bg-accent text-white' : 'border-border bg-surface-alt',
                    )}>
                      {selected && <VsIcon name="check" size={11} />}
                    </span>
                  </button>
                );
              })}
            </div>
          ) : (
            <div className="flex h-36 items-center justify-center text-[11px] text-fg-mute">没有匹配的专家</div>
          )}
        </div>
        <footer className="flex items-center justify-between gap-3 border-t border-border px-5 py-3">
          <span className="text-[11px] text-fg-mute">已选 {selectedIds.length} 人</span>
          <div className="flex gap-2">
            <button type="button" onClick={onClose} className="h-8 rounded-md border border-border px-3 text-[12px] text-fg-2 hover:bg-surface-hi">取消</button>
            <button
              type="button"
              onClick={() => onConfirm(selectedIds)}
              className="h-8 rounded-md bg-accent px-3 text-[12px] font-medium text-white hover:opacity-90"
            >
              确认添加
            </button>
          </div>
        </footer>
      </div>
    </Modal>
  );
}

function TeamMembersEditor({ form, experts, error, onChange }) {
  const [pickerOpen, setPickerOpen] = useState(false);
  const selected = selectedTeamExperts(form, experts);

  return (
    <div data-team-members-editor="true">
      <div className="mb-3 flex items-start justify-between gap-4">
        <div>
          <h3 className="text-[14px] font-semibold text-fg">团队成员</h3>
          <p className="mt-1 text-[11px] text-fg-mute">
            已选 {selected.length} 人 · {form.leadExpertId ? '已指定 1 位主理人' : '尚未指定主理人'}
          </p>
        </div>
        <button
          type="button"
          onClick={() => setPickerOpen(true)}
          className="flex h-8 items-center gap-1.5 rounded-md border border-border bg-surface px-3 text-[12px] text-fg-2 hover:bg-surface-hi"
        >
          <VsIcon name="add" size={13} />
          添加专家
        </button>
      </div>
      {selected.length > 0 ? (
        <div className="overflow-hidden rounded-lg border border-border">
          {selected.map((expert) => {
            const lead = form.leadExpertId === expert.id;
            return (
              <div key={expert.id} className="flex items-center gap-3 border-b border-border bg-surface px-3 py-3 last:border-b-0">
                <ExpertAvatar expert={expert} size={34} />
                <div className="min-w-0 flex-1">
                  <div className="truncate text-[12px] font-medium text-fg">{expert.display_name}</div>
                  <div className="mt-0.5 truncate text-[10px] text-fg-mute">
                    {expert.profession || '专家'}{expert.expertise[0] ? ` · 擅长${expert.expertise[0]}` : ''}
                  </div>
                </div>
                {lead ? (
                  <span className="rounded bg-accent-bg px-2 py-1 text-[10px] text-accent">主理人</span>
                ) : (
                  <button
                    type="button"
                    onClick={() => onChange({ ...form, leadExpertId: expert.id })}
                    className="h-7 rounded px-2 text-[10px] text-fg-mute hover:bg-surface-hi hover:text-fg"
                  >
                    设为主理人
                  </button>
                )}
                <button
                  type="button"
                  onClick={() => onChange(toggleTeamExpert(form, expert.id))}
                  className="h-7 rounded px-2 text-[10px] text-fg-mute hover:bg-danger-bg hover:text-danger"
                >
                  移除
                </button>
              </div>
            );
          })}
        </div>
      ) : (
        <button
          type="button"
          onClick={() => setPickerOpen(true)}
          className="flex h-28 w-full flex-col items-center justify-center rounded-lg border border-dashed border-border bg-surface text-fg-mute hover:bg-surface-hi"
        >
          <VsIcon name="add" size={18} />
          <span className="mt-2 text-[11px]">添加至少两位专家</span>
        </button>
      )}
      {error && <p className="mt-2 text-[10px] text-danger">{error}</p>}
      {pickerOpen && (
        <MemberPickerDialog
          form={form}
          experts={experts}
          onClose={() => setPickerOpen(false)}
          onConfirm={(ids) => {
            onChange({
              ...form,
              selectedExpertIds: ids,
              leadExpertId: ids.includes(form.leadExpertId) ? form.leadExpertId : (ids[0] || ''),
            });
            setPickerOpen(false);
          }}
        />
      )}
    </div>
  );
}

export function ExpertEditor({
  initial,
  editing = false,
  experts = [],
  workspaceHash = '',
  onClose,
  onSaved,
}) {
  const [form, setForm] = useState(initial);
  const [tab, setTab] = useState('basic');
  const [saving, setSaving] = useState(false);
  const [saveError, setSaveError] = useState('');
  const [errors, setErrors] = useState({});
  const [confirmClose, setConfirmClose] = useState(false);
  const [capabilityState, setCapabilityState] = useState({
    catalog: normalizeCapabilityCatalog({}),
    loading: initial?.type !== 'team',
    error: '',
  });
  const isTeam = form.type === 'team';
  const baseline = useMemo(() => JSON.stringify(initial), [initial]);
  const dirty = JSON.stringify(form) !== baseline;
  const tagSuggestions = useMemo(() => collectExpertTags(experts, 'all'), [experts]);

  const loadCapabilities = useCallback(async () => {
    if (isTeam) return;
    setCapabilityState((current) => ({ ...current, loading: true, error: '' }));
    try {
      const result = await api.listExpertCapabilities(workspaceHash || '__local__');
      setCapabilityState({ catalog: normalizeCapabilityCatalog(result), loading: false, error: '' });
    } catch (error) {
      setCapabilityState((current) => ({
        ...current,
        loading: false,
        error: error?.message || '无法读取运行时能力',
      }));
    }
  }, [isTeam, workspaceHash]);

  useEffect(() => { loadCapabilities(); }, [loadCapabilities]);

  const update = (key, value) => {
    setForm((current) => ({ ...current, [key]: value }));
    setErrors((current) => ({ ...current, [key]: '' }));
  };
  const requestClose = useCallback(() => {
    if (saving) return;
    if (dirty) setConfirmClose(true);
    else onClose();
  }, [dirty, onClose, saving]);

  const save = async () => {
    const nextErrors = validateExpertFormFields(form);
    setErrors(nextErrors);
    if (Object.keys(nextErrors).length > 0) {
      setTab(nextErrors.members || nextErrors.leadExpertId ? 'members' : 'basic');
      return;
    }
    setSaving(true);
    setSaveError('');
    try {
      const payload = expertPayloadFromForm(form);
      const result = editing
        ? await api.updateExpert(form.id, payload, workspaceHash)
        : await api.createExpert(payload, workspaceHash);
      await onSaved?.(result);
    } catch (error) {
      setSaveError(error?.message || `保存${isTeam ? '专家团' : '专家'}失败`);
    } finally {
      setSaving(false);
    }
  };

  const tabs = isTeam
    ? [{ id: 'basic', label: '基础信息' }, { id: 'members', label: '团队成员' }]
    : [{ id: 'basic', label: '基础信息' }, { id: 'advanced', label: '高级功能' }];
  const selectedCapabilities = CAPABILITY_KINDS.reduce(
    (sum, kind) => sum + normalizeStringList(form.capabilities?.[kind]).length,
    0,
  );

  return (
    <>
      <Modal
        onClose={requestClose}
        width="min(940px, calc(100vw - 24px))"
        dismissOnBackdrop={!saving}
        dismissOnEscape={!saving}
        labelledBy="expert-editor-title"
      >
        <div data-expert-editor="true" data-expert-editor-kind={form.type} className="flex max-h-[92vh] flex-col">
          <header className="flex items-start justify-between gap-4 border-b border-border px-5 py-3.5">
            <div>
              <h2 id="expert-editor-title" className="text-[16px] font-semibold text-fg">
                {editing ? `编辑${isTeam ? '专家团' : '专家'}` : isTeam ? '组建专家团' : '新建专家'}
              </h2>
              <p className="mt-0.5 text-[10px] text-fg-mute">
                {isTeam ? '引用已有专家，指定一位主理人；成员保留各自能力范围。' : '把身份、擅长领域、开场白和运行时能力配置在同一个专家中。'}
              </p>
            </div>
            <button type="button" onClick={requestClose} disabled={saving} className="p-1 text-fg-mute hover:text-fg disabled:opacity-50" aria-label="关闭">
              <VsIcon name="close" size={16} />
            </button>
          </header>

          <div className="border-b border-border px-5">
            <div role="tablist" className="flex h-10 gap-5">
              {tabs.map((item) => (
                <button
                  key={item.id}
                  type="button"
                  role="tab"
                  aria-selected={tab === item.id}
                  onClick={() => setTab(item.id)}
                  className={clsx(
                    'relative h-10 text-[12px] font-medium outline-none focus-visible:ring-2 focus-visible:ring-accent',
                    tab === item.id ? 'text-fg' : 'text-fg-mute hover:text-fg',
                  )}
                >
                  {item.label}
                  {item.id === 'advanced' && selectedCapabilities > 0 && (
                    <span className="ml-1 rounded bg-accent-bg px-1.5 py-0.5 text-[9px] text-accent">{selectedCapabilities}</span>
                  )}
                  {tab === item.id && <span className="absolute inset-x-0 bottom-0 h-0.5 bg-accent" />}
                </button>
              ))}
            </div>
          </div>

          <div className="flex-1 overflow-y-auto px-5 py-4">
            {tab === 'basic' && (
              <BasicEditor form={form} errors={errors} tagSuggestions={tagSuggestions} update={update} />
            )}
            {tab === 'advanced' && !isTeam && (
              <AdvancedEditor
                form={form}
                updateCapabilities={(value) => update('capabilities', value)}
                catalog={capabilityState.catalog}
                loading={capabilityState.loading}
                error={capabilityState.error}
                onRetry={loadCapabilities}
              />
            )}
            {tab === 'members' && isTeam && (
              <TeamMembersEditor
                form={form}
                experts={experts}
                error={errors.members || errors.leadExpertId}
                onChange={setForm}
              />
            )}
            {saveError && (
              <div className="mt-4 rounded-md border border-danger bg-danger-bg px-3 py-2 text-[11px] text-danger">
                {saveError}
              </div>
            )}
          </div>

          <footer className="flex items-center justify-between gap-3 border-t border-border px-5 py-3">
            <span className="text-[10px] text-fg-mute">
              {dirty ? '有尚未保存的更改' : editing ? '当前内容未更改' : '填写完成后即可创建'}
            </span>
            <div className="flex gap-2">
              <button
                type="button"
                onClick={requestClose}
                disabled={saving}
                className="h-8 rounded-md border border-border px-3 text-[12px] text-fg-2 hover:bg-surface-hi disabled:opacity-50"
              >
                取消
              </button>
              <button
                type="button"
                onClick={save}
                disabled={saving}
                className="h-8 rounded-md bg-accent px-4 text-[12px] font-medium text-white hover:opacity-90 disabled:cursor-wait disabled:opacity-50"
              >
                {saving ? '保存中…' : editing ? (isTeam ? '保存专家团' : '保存专家') : (isTeam ? '创建专家团' : '创建专家')}
              </button>
            </div>
          </footer>
        </div>
      </Modal>

      {confirmClose && (
        <Modal onClose={() => setConfirmClose(false)} width={420} layerClassName="z-[330]" labelledBy="expert-unsaved-title">
          <div className="p-5">
            <h2 id="expert-unsaved-title" className="text-[15px] font-semibold text-fg">放弃未保存的更改？</h2>
            <p className="mt-2 text-[12px] leading-5 text-fg-2">关闭后，本次填写的内容和能力选择不会保存。</p>
            <div className="mt-5 flex justify-end gap-2">
              <button type="button" onClick={() => setConfirmClose(false)} className="h-8 rounded-md border border-border px-3 text-[12px] text-fg-2 hover:bg-surface-hi">继续编辑</button>
              <button type="button" onClick={onClose} className="h-8 rounded-md bg-danger px-3 text-[12px] font-medium text-white hover:opacity-90">放弃更改</button>
            </div>
          </div>
        </Modal>
      )}
    </>
  );
}
