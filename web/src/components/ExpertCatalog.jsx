import { useCallback, useEffect, useMemo, useState } from 'react';
import { api } from '../lib/api.js';
import {
  EXPERT_PRIMARY_TABS,
  EXPERT_SORTS,
  collectExpertTags,
  expertiseSummary,
  filterExperts,
  normalizeExperts,
  sortExperts,
} from '../lib/expertComponents.js';
import { clsx } from '../lib/format.js';
import { Modal } from './Modal.jsx';
import { VsIcon } from './Icon.jsx';

export function ExpertAvatar({ expert, size = 36, className = '' }) {
  const [imageFailed, setImageFailed] = useState(false);
  const image = String(expert?.avatar_url || '');
  useEffect(() => setImageFailed(false), [image]);
  const name = String(expert?.display_name || expert?.id || '专');
  const initial = [...name][0] || '专';

  return (
    <span
      className={clsx(
        'relative inline-flex shrink-0 items-center justify-center overflow-hidden rounded-lg border border-border bg-accent-bg text-accent',
        className,
      )}
      style={{ width: size, height: size }}
      aria-hidden="true"
    >
      {image && !imageFailed ? (
        <img
          src={image}
          alt=""
          className="h-full w-full object-cover"
          onError={() => setImageFailed(true)}
        />
      ) : (
        <span className="text-[13px] font-semibold">
          {expert?.type === 'team' ? <VsIcon name="extension" size={Math.max(15, size / 2)} /> : initial}
        </span>
      )}
    </span>
  );
}

export function useExpertCatalogData(workspaceHash = '') {
  const effectiveWorkspace = workspaceHash || '__local__';
  const [state, setState] = useState({
    experts: [],
    diagnostics: [],
    loading: true,
    error: '',
  });

  const refresh = useCallback(async () => {
    setState((current) => ({ ...current, loading: true, error: '' }));
    try {
      const result = await api.listExperts(effectiveWorkspace);
      setState({
        experts: normalizeExperts(result),
        diagnostics: Array.isArray(result?.diagnostics) ? result.diagnostics : [],
        loading: false,
        error: '',
      });
      return result;
    } catch (error) {
      setState((current) => ({
        ...current,
        loading: false,
        error: error?.message || '无法读取专家组件',
      }));
      return null;
    }
  }, [effectiveWorkspace]);

  useEffect(() => { refresh(); }, [refresh]);
  return { ...state, refresh, effectiveWorkspace };
}

function tagLabel(expert) {
  return expert.type === 'team' ? '专家团' : '专家';
}

function teamLead(expert) {
  if (expert.type !== 'team') return null;
  return expert.agents.find((agent) => agent.id === expert.lead_expert_id)
    || expert.agents[0]
    || null;
}

function ExpertCard({
  expert,
  onOpen,
  onDispatch,
  onEdit,
  onDelete,
  dispatching = false,
}) {
  const lead = teamLead(expert);
  const expertise = expert.expertise.slice(0, 3);
  const detailLabel = `查看${tagLabel(expert)}“${expert.display_name}”`;

  return (
    <article
      data-expert-card={expert.id}
      className="group relative flex min-h-[204px] flex-col rounded-xl border border-border bg-surface p-[15px] transition hover:border-border-hi hover:bg-surface-hi"
    >
      <button
        type="button"
        aria-label={detailLabel}
        onClick={() => onOpen(expert)}
        className="absolute inset-0 z-0 cursor-pointer rounded-xl outline-none focus-visible:ring-2 focus-visible:ring-accent"
      />
      <div className="flex items-start gap-3">
        <ExpertAvatar expert={expert} />
        <div className="min-w-0 flex-1">
          <div className="flex items-center gap-1.5">
            <h3 className="truncate text-[14px] font-semibold text-fg">{expert.display_name}</h3>
            <span className="shrink-0 rounded border border-border px-1.5 py-0.5 text-[10px] text-fg-mute">
              {tagLabel(expert)}
            </span>
          </div>
          <p className="mt-0.5 truncate text-[11px] text-fg-mute">
            {expert.author || expert.profession || (expert.source === 'workspace' ? '项目提供' : '我的专家')}
          </p>
        </div>
        <button
          type="button"
          disabled={dispatching}
          onClick={(event) => {
            event.stopPropagation();
            onDispatch(expert);
          }}
          className="relative z-10 h-[26px] shrink-0 rounded-md bg-accent px-2.5 text-[11px] font-medium text-white transition hover:opacity-90 disabled:cursor-wait disabled:opacity-50"
        >
          {dispatching ? '派遣中…' : '派遣'}
        </button>
      </div>

      <p className="mt-3 line-clamp-2 min-h-10 text-[12px] leading-5 text-fg-2">
        {expert.description || '还没有填写能力介绍。'}
      </p>

      {expert.type === 'team' && (
        <div className="mt-2 flex items-center gap-3 text-[11px] text-fg-mute">
          <span className="truncate">主理人：{lead?.display_name || '未指定'}</span>
          <span className="shrink-0">{expert.agents.length || (expert.member_expert_ids.length + 1)} 位成员</span>
        </div>
      )}

      <div className="mt-auto pt-3">
        <div className="flex min-h-[24px] flex-wrap gap-1.5" aria-label="擅长领域">
          {expertise.length > 0 ? expertise.map((item) => (
            <span key={item} className="max-w-[132px] truncate rounded-md bg-bg px-2 py-1 text-[10px] text-fg-2">
              {item}
            </span>
          )) : (
            <span className="text-[10px] text-fg-mute">尚未填写擅长领域</span>
          )}
        </div>
        <div className="mt-3 flex min-h-[24px] items-center gap-1.5 border-t border-border pt-3">
          <div className="flex min-w-0 flex-1 gap-1 overflow-hidden">
            {expert.tags.slice(0, 2).map((tag) => (
              <span key={tag} className="max-w-[110px] truncate rounded bg-accent-bg px-1.5 py-0.5 text-[10px] text-accent">
                {tag}
              </span>
            ))}
          </div>
          {expert.managed_global && (
            <>
              <button
                type="button"
                onClick={(event) => {
                  event.stopPropagation();
                  onEdit?.(expert);
                }}
                className="relative z-10 h-6 rounded px-1.5 text-[11px] text-fg-mute hover:bg-bg hover:text-fg"
              >
                编辑
              </button>
              <button
                type="button"
                onClick={(event) => {
                  event.stopPropagation();
                  onDelete?.(expert);
                }}
                className="relative z-10 h-6 rounded px-1.5 text-[11px] text-fg-mute hover:bg-danger-bg hover:text-danger"
              >
                删除
              </button>
            </>
          )}
        </div>
      </div>
    </article>
  );
}

function DetailSection({ title, children }) {
  return (
    <section>
      <h3 className="text-[12px] font-semibold text-fg">{title}</h3>
      <div className="mt-2">{children}</div>
    </section>
  );
}

export function ExpertDetailDialog({
  expert,
  onClose,
  onDispatch,
  onOpeningPrompt,
  onEdit,
}) {
  const [busyKey, setBusyKey] = useState('');
  const lead = teamLead(expert);

  const invoke = async (kind, prompt = '') => {
    setBusyKey(kind);
    try {
      const result = prompt
        ? await onOpeningPrompt?.(expert, prompt)
        : await onDispatch?.(expert);
      if (result !== false) onClose();
    } finally {
      setBusyKey('');
    }
  };

  return (
    <Modal onClose={onClose} width={700} dismissOnBackdrop={!busyKey} dismissOnEscape={!busyKey} labelledBy="expert-detail-title">
      <div data-expert-detail="true" className="flex max-h-[88vh] flex-col">
        <header className="flex items-start gap-3 border-b border-border px-5 py-4">
          <ExpertAvatar expert={expert} size={48} className="rounded-xl" />
          <div className="min-w-0 flex-1">
            <h2 id="expert-detail-title" className="truncate text-[18px] font-semibold text-fg">
              {expert.display_name}
            </h2>
            <div className="mt-1 flex flex-wrap items-center gap-1.5 text-[11px] text-fg-mute">
              <span>{expert.author || expert.profession || 'ACECode'}</span>
              <span className="rounded border border-border px-1.5 py-0.5">{tagLabel(expert)}</span>
              {expert.tags.map((tag) => (
                <span key={tag} className="rounded bg-accent-bg px-1.5 py-0.5 text-accent">{tag}</span>
              ))}
            </div>
          </div>
          {expert.managed_global && onEdit && (
            <button
              type="button"
              onClick={() => onEdit(expert)}
              className="h-7 rounded-md px-2 text-[11px] text-fg-mute hover:bg-surface-hi hover:text-fg"
            >
              编辑
            </button>
          )}
          <button type="button" onClick={onClose} disabled={!!busyKey} className="p-1 text-fg-mute hover:text-fg disabled:opacity-50" aria-label="关闭">
            <VsIcon name="close" size={16} />
          </button>
        </header>

        <div className="flex-1 space-y-5 overflow-y-auto px-5 py-4">
          <DetailSection title="能力介绍">
            <p className="text-[13px] leading-6 text-fg-2">
              {expert.description || '还没有填写能力介绍。'}
            </p>
          </DetailSection>

          <DetailSection title="擅长领域">
            {expert.expertise.length > 0 ? (
              <div className="flex flex-wrap gap-2">
                {expert.expertise.map((item) => (
                  <span key={item} className="rounded-md border border-border bg-surface-alt px-2.5 py-1.5 text-[12px] text-fg-2">
                    {item}
                  </span>
                ))}
              </div>
            ) : (
              <p className="text-[12px] text-fg-mute">尚未填写擅长领域</p>
            )}
          </DetailSection>

          {expert.type === 'team' && (
            <DetailSection title={`团队成员 · ${expert.agents.length || expert.member_expert_ids.length + 1} 人`}>
              <div className="overflow-hidden rounded-lg border border-border">
                {expert.agents.map((member) => (
                  <div key={member.id} className="flex items-center gap-3 border-b border-border px-3 py-2.5 last:border-b-0">
                    <ExpertAvatar expert={{ ...member, type: 'agent' }} size={30} />
                    <div className="min-w-0 flex-1">
                      <div className="truncate text-[12px] font-medium text-fg">{member.display_name || member.id}</div>
                      <div className="truncate text-[10px] text-fg-mute">{member.profession || '专家'}</div>
                    </div>
                    {member.id === lead?.id && (
                      <span className="rounded bg-accent-bg px-2 py-0.5 text-[10px] text-accent">主理人</span>
                    )}
                  </div>
                ))}
              </div>
            </DetailSection>
          )}

          {expert.quick_prompts.length > 0 && (
            <DetailSection title="试试这样问我">
              <div className="mb-2 text-[11px] text-fg-mute">选择后填入真实聊天框，不会自动发送</div>
              <div className="space-y-2">
                {expert.quick_prompts.map((prompt, index) => (
                  <button
                    key={prompt}
                    type="button"
                    disabled={!!busyKey}
                    onClick={() => invoke(`prompt-${index}`, prompt)}
                    className="flex w-full items-center gap-3 rounded-lg border border-border bg-surface-alt px-3 py-2.5 text-left text-[12px] leading-5 text-fg-2 outline-none transition hover:border-accent hover:bg-surface-hi focus-visible:ring-2 focus-visible:ring-accent disabled:opacity-50"
                  >
                    <span className="min-w-0 flex-1">{prompt}</span>
                    {busyKey === `prompt-${index}`
                      ? <span className="ace-spinner h-3.5 w-3.5 shrink-0" />
                      : <VsIcon name="expandRight" size={13} className="shrink-0 text-fg-mute" />}
                  </button>
                ))}
              </div>
            </DetailSection>
          )}

          {expert.source === 'workspace' && (
            <div className="rounded-md border border-border bg-surface-alt px-3 py-2 text-[11px] text-fg-mute">
              该专家由当前项目提供，只能查看和派遣。
            </div>
          )}
        </div>

        <footer className="flex justify-end border-t border-border px-5 py-3">
          <button
            type="button"
            disabled={!!busyKey}
            onClick={() => invoke('dispatch')}
            className="h-9 rounded-md bg-accent px-5 text-[12px] font-medium text-white hover:opacity-90 disabled:cursor-wait disabled:opacity-50"
          >
            {busyKey === 'dispatch' ? '派遣中…' : `派遣 ${expert.display_name}`}
          </button>
        </footer>
      </div>
    </Modal>
  );
}

function CatalogSkeleton() {
  return (
    <div data-expert-catalog-loading="true" aria-label="正在加载专家组件">
      <div className="mt-4 flex gap-2 overflow-hidden">
        {[80, 112, 72, 96].map((width) => (
          <span key={width} className="h-[26px] shrink-0 animate-pulse rounded-md bg-surface-hi" style={{ width }} />
        ))}
      </div>
      <div className="mt-4 grid grid-cols-[repeat(auto-fill,minmax(min(280px,100%),1fr))] gap-[14px]">
        {Array.from({ length: 6 }, (_, index) => (
          <div key={index} className="h-[204px] animate-pulse rounded-xl border border-border bg-surface">
            <div className="m-4 h-9 w-9 rounded-lg bg-surface-hi" />
            <div className="mx-4 mt-4 h-3 rounded bg-surface-hi" />
            <div className="mx-4 mt-2 h-3 w-3/4 rounded bg-surface-hi" />
          </div>
        ))}
      </div>
    </div>
  );
}

export function ExpertCatalog({
  experts,
  loading = false,
  error = '',
  diagnostics = [],
  recentIds = [],
  onRetry,
  onDispatch,
  onOpeningPrompt,
  onEdit,
  onDelete,
  initialType = 'agent',
  picker = false,
}) {
  const [type, setType] = useState(initialType === 'team' ? 'team' : 'agent');
  const [tag, setTag] = useState('all');
  const [query, setQuery] = useState('');
  const [sort, setSort] = useState('recent');
  const [detail, setDetail] = useState(null);
  const [dispatchingId, setDispatchingId] = useState('');
  const tags = useMemo(() => collectExpertTags(experts, type), [experts, type]);
  const visible = useMemo(
    () => sortExperts(filterExperts(experts, { query, type, tag }), { sort, recentIds }),
    [experts, query, recentIds, sort, tag, type],
  );

  useEffect(() => {
    if (tag !== 'all' && !tags.includes(tag)) setTag('all');
  }, [tag, tags]);

  const dispatch = async (expert) => {
    if (!onDispatch || dispatchingId) return false;
    setDispatchingId(expert.id);
    try {
      return await onDispatch(expert);
    } finally {
      setDispatchingId('');
    }
  };

  const clearFilters = () => {
    setTag('all');
    setQuery('');
  };

  return (
    <div data-expert-catalog="true">
      <div className="flex flex-col gap-3 border-b border-border pb-3 md:flex-row md:items-center">
        <div role="tablist" aria-label="专家组件类型" className="flex h-[34px] items-end gap-5">
          {EXPERT_PRIMARY_TABS.map((item) => (
            <button
              key={item.id}
              type="button"
              role="tab"
              aria-selected={type === item.id}
              onClick={() => setType(item.id)}
              className={clsx(
                'relative h-[34px] px-0.5 text-[14px] font-semibold outline-none transition focus-visible:ring-2 focus-visible:ring-accent',
                type === item.id ? 'text-fg' : 'text-fg-mute hover:text-fg',
              )}
            >
              {item.label}
              {type === item.id && <span className="absolute inset-x-0 bottom-0 h-0.5 rounded bg-accent" />}
            </button>
          ))}
        </div>

        <div className="flex min-w-0 flex-1 flex-col gap-2 sm:flex-row sm:items-center sm:justify-end">
          <label className="relative min-w-0 flex-1 sm:max-w-[300px]">
            <span className="sr-only">搜索专家组件</span>
            <VsIcon name="search" size={14} className="pointer-events-none absolute left-2.5 top-1/2 -translate-y-1/2 text-fg-mute" />
            <input
              value={query}
              onChange={(event) => setQuery(event.target.value)}
              className="h-8 w-full rounded-md border border-border bg-surface pl-8 pr-8 text-[12px] text-fg outline-none transition placeholder:text-fg-mute focus:border-accent"
              placeholder="搜索名称、作者、Tag 或擅长领域"
            />
            {query && (
              <button
                type="button"
                onClick={() => setQuery('')}
                className="absolute right-2 top-1/2 -translate-y-1/2 text-fg-mute hover:text-fg"
                aria-label="清除搜索"
              >
                <VsIcon name="close" size={13} />
              </button>
            )}
          </label>
          <label className="flex shrink-0 items-center gap-1.5 text-[11px] text-fg-mute">
            <span>排序</span>
            <select
              value={sort}
              onChange={(event) => setSort(event.target.value)}
              className="h-8 rounded-md border border-border bg-surface px-2 text-[12px] text-fg outline-none focus:border-accent"
            >
              {EXPERT_SORTS.map((item) => <option key={item.id} value={item.id}>{item.label}</option>)}
            </select>
          </label>
        </div>
      </div>

      {loading ? <CatalogSkeleton /> : (
        <>
          <div className="mt-3 flex gap-1.5 overflow-x-auto pb-1" role="tablist" aria-label="按 Tag 筛选">
            {['all', ...tags].map((item) => {
              const active = tag === item;
              const label = item === 'all' ? '全部' : item;
              return (
                <button
                  key={item}
                  type="button"
                  role="tab"
                  aria-selected={active}
                  onClick={() => setTag(item)}
                  className={clsx(
                    'h-[26px] shrink-0 rounded-md px-2.5 text-[11px] outline-none transition focus-visible:ring-2 focus-visible:ring-accent',
                    active ? 'bg-accent-bg font-medium text-accent' : 'text-fg-mute hover:bg-surface-hi hover:text-fg',
                  )}
                >
                  {label}
                </button>
              );
            })}
          </div>

          <div className="mt-3 flex items-center justify-between gap-3 text-[11px] text-fg-mute">
            <span>{visible.length} 个可用{type === 'team' ? '专家团' : '专家'}</span>
            <span className="hidden sm:inline">同一专家可属于多个 Tag</span>
          </div>

          {diagnostics.length > 0 && (
            <div className="mt-3 rounded-md border border-warn bg-surface px-3 py-2 text-[11px] text-warn">
              有些专家暂时不可用。可重试加载或检查专家包。
            </div>
          )}

          {error ? (
            <div className="mt-4 flex min-h-[220px] flex-col items-center justify-center rounded-xl border border-danger bg-surface px-6 text-center">
              <VsIcon name="error" size={24} mono={false} className="text-danger" />
              <p className="mt-3 text-[13px] font-medium text-fg">专家组件加载失败</p>
              <p className="mt-1 max-w-[460px] text-[11px] leading-5 text-fg-mute">{error}</p>
              <button type="button" onClick={onRetry} className="mt-4 h-8 rounded-md border border-border px-3 text-[12px] text-fg-2 hover:bg-surface-hi">
                重新加载
              </button>
            </div>
          ) : visible.length > 0 ? (
            <div className="mt-4 grid grid-cols-[repeat(auto-fill,minmax(min(280px,100%),1fr))] gap-[14px]">
              {visible.map((expert) => (
                <ExpertCard
                  key={expert.id}
                  expert={expert}
                  onOpen={setDetail}
                  onDispatch={dispatch}
                  onEdit={onEdit}
                  onDelete={onDelete}
                  dispatching={dispatchingId === expert.id}
                />
              ))}
            </div>
          ) : (
            <div className="mt-4 flex min-h-[230px] flex-col items-center justify-center rounded-xl border border-dashed border-border bg-surface px-6 text-center">
              <VsIcon name={type === 'team' ? 'extension' : 'brain'} size={26} className="text-fg-mute" />
              <p className="mt-3 text-[13px] font-medium text-fg-2">
                {query || tag !== 'all' ? '没有符合筛选条件的专家组件' : `还没有${type === 'team' ? '专家团' : '专家'}`}
              </p>
              <p className="mt-1 text-[11px] text-fg-mute">
                {query || tag !== 'all' ? '清除筛选后查看全部可用组件。' : picker ? '可先在专家组件页面创建。' : '通过右上角操作开始创建。'}
              </p>
              {(query || tag !== 'all') && (
                <button type="button" onClick={clearFilters} className="mt-4 h-8 rounded-md border border-border px-3 text-[12px] text-fg-2 hover:bg-surface-hi">
                  清除筛选
                </button>
              )}
            </div>
          )}
        </>
      )}

      {detail && (
        <ExpertDetailDialog
          expert={detail}
          onClose={() => setDetail(null)}
          onDispatch={dispatch}
          onOpeningPrompt={onOpeningPrompt}
          onEdit={onEdit ? (expert) => {
            setDetail(null);
            onEdit(expert);
          } : null}
        />
      )}
    </div>
  );
}

export function ExpertPickerDialog({
  workspaceHash = '',
  recentIds = [],
  onClose,
  onDispatch,
  onOpeningPrompt,
}) {
  const catalog = useExpertCatalogData(workspaceHash);
  return (
    <Modal onClose={onClose} width="min(1120px, calc(100vw - 24px))" labelledBy="expert-picker-title">
      <div data-chat-expert-picker="true" className="flex max-h-[92vh] flex-col">
        <header className="flex items-start justify-between gap-4 border-b border-border px-5 py-4">
          <div>
            <h2 id="expert-picker-title" className="text-[17px] font-semibold text-fg">选择专家组件</h2>
            <p className="mt-1 text-[11px] text-fg-mute">派遣到当前对话；开场白只会填入输入框，不会自动发送。</p>
          </div>
          <button type="button" onClick={onClose} className="p-1 text-fg-mute hover:text-fg" aria-label="关闭">
            <VsIcon name="close" size={16} />
          </button>
        </header>
        <div className="flex-1 overflow-y-auto px-4 py-4 sm:px-5">
          <ExpertCatalog
            experts={catalog.experts}
            diagnostics={catalog.diagnostics}
            loading={catalog.loading}
            error={catalog.error}
            onRetry={catalog.refresh}
            recentIds={recentIds}
            picker
            onDispatch={async (expert) => {
              const result = await onDispatch?.(expert);
              if (result !== false) onClose();
              return result;
            }}
            onOpeningPrompt={async (expert, prompt) => {
              const result = await onOpeningPrompt?.(expert, prompt);
              if (result !== false) onClose();
              return result;
            }}
          />
        </div>
      </div>
    </Modal>
  );
}

export function compactExpertSummary(expert) {
  return expertiseSummary(expert, expert?.profession || '');
}
