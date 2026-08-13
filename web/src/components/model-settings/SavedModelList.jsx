import { clsx } from '../../lib/format.js';
import { filterSavedModels } from '../../lib/modelManager.js';
import { RefreshIcon, VsIcon } from '../Icon.jsx';
import { ProviderIcon } from './ProviderIcon.jsx';

function providerLabel(model) {
  if (model.provider === 'copilot') return 'GitHub Copilot';
  if (model.provider === 'anthropic') return 'Anthropic';
  return model.models_dev_provider_id || 'OpenAI 兼容';
}

function SavedModelRow({
  model,
  isDefault,
  busy,
  deleteBlocked,
  onSetDefault,
  onEdit,
  onDelete,
}) {
  const disabled = !!busy;
  return (
    <article role="listitem" className="flex min-w-0 flex-wrap items-center gap-3 rounded-md border border-border bg-surface px-3.5 py-2.5">
      <ProviderIcon provider={model} />
      <div className="min-w-[180px] flex-1">
        <div className="flex min-w-0 flex-wrap items-center gap-1.5">
          <span className="truncate text-[12px] font-semibold text-fg">{model.name}</span>
          {isDefault && (
            <span className="rounded border border-accent-soft bg-accent-bg px-1.5 py-0.5 text-[10px] font-medium text-accent">
              默认
            </span>
          )}
          {deleteBlocked && (
            <span className="rounded border border-warn bg-warn-bg px-1.5 py-0.5 text-[10px] text-warn">
              会话使用中
            </span>
          )}
        </div>
        <div className="mt-0.5 flex min-w-0 flex-wrap items-center gap-x-1.5 text-[11px] text-fg-mute">
          <span>{providerLabel(model)}</span>
          <span aria-hidden="true">·</span>
          <span className="min-w-0 truncate" title={model.model}>{model.model}</span>
        </div>
      </div>
      {model.capabilities?.length > 0 && (
        <div className="hidden max-w-[220px] flex-wrap justify-end gap-1 2xl:flex">
          {model.capabilities.slice(0, 4).map((capability) => (
            <span
              key={capability}
              className="rounded border border-border bg-surface-alt px-1.5 py-0.5 text-[10px] text-fg-mute"
            >
              {capability}
            </span>
          ))}
        </div>
      )}
      <div className="ml-auto flex shrink-0 items-center gap-1">
        {!isDefault && (
          <button
            type="button"
            onClick={() => onSetDefault?.(model)}
            disabled={disabled}
            className="h-7 rounded-md px-2 text-[11px] text-fg-2 transition hover:bg-surface-hi focus:outline-none focus:ring-1 focus:ring-accent disabled:opacity-50"
            aria-label={`将 ${model.name} 设为默认模型`}
          >
            设为默认
          </button>
        )}
        <button
          type="button"
          onClick={() => onEdit?.(model)}
          disabled={disabled}
          className="flex h-7 w-7 items-center justify-center rounded-md text-fg-mute transition hover:bg-surface-hi hover:text-fg focus:outline-none focus:ring-1 focus:ring-accent disabled:opacity-50"
          aria-label={`编辑模型 ${model.name}`}
          title="编辑"
        >
          <VsIcon name="edit" size={13} />
        </button>
        <button
          type="button"
          onClick={() => onDelete?.(model)}
          disabled={disabled || deleteBlocked}
          className="flex h-7 w-7 items-center justify-center rounded-md text-fg-mute transition hover:bg-danger-bg hover:text-danger focus:outline-none focus:ring-1 focus:ring-danger disabled:cursor-not-allowed disabled:opacity-40"
          aria-label={deleteBlocked
            ? `模型 ${model.name} 正在被会话使用，暂时不能删除`
            : `删除模型 ${model.name}`}
          title={deleteBlocked ? '运行中的会话正在使用该模型' : '删除'}
        >
          <VsIcon name="delete" size={13} />
        </button>
      </div>
    </article>
  );
}

export function SavedModelList({
  models,
  defaultName,
  query,
  onQueryChange,
  loading = false,
  busy = '',
  blockedDeletes = new Set(),
  onRefresh,
  onAdd,
  onSetDefault,
  onEdit,
  onDelete,
}) {
  const filtered = filterSavedModels(models, query);
  return (
    <section aria-labelledby="saved-models-title">
      <div className="mb-2 flex flex-wrap items-end justify-between gap-2">
        <div>
          <h3 id="saved-models-title" className="text-[13px] font-semibold text-fg">
            已保存模型
          </h3>
          <p className="mt-0.5 text-[11px] leading-5 text-fg-mute">
            聊天输入区的模型选择器使用这里的预设。
          </p>
        </div>
        <div className="flex min-w-0 flex-1 flex-wrap items-center justify-end gap-2 sm:flex-nowrap">
          <label className="relative min-w-[170px] max-w-[260px] flex-1 sm:flex-none">
            <span className="sr-only">搜索已保存模型</span>
            <VsIcon
              name="search"
              size={13}
              className="pointer-events-none absolute left-2.5 top-1/2 -translate-y-1/2 text-fg-mute"
            />
            <input
              type="search"
              value={query}
              onChange={(event) => onQueryChange?.(event.target.value)}
              placeholder="搜索名称、Provider 或模型 ID"
              className="h-8 w-full rounded-md border border-border bg-surface pl-8 pr-2.5 text-[11px] text-fg outline-none transition placeholder:text-fg-mute focus:border-accent focus:ring-1 focus:ring-accent-soft"
            />
          </label>
          <button
            type="button"
            onClick={onRefresh}
            disabled={loading || !!busy}
            className="flex h-8 w-8 shrink-0 items-center justify-center rounded-md border border-border bg-surface text-fg-2 transition hover:bg-surface-hi focus:outline-none focus:ring-1 focus:ring-accent disabled:opacity-50"
            aria-label="刷新已保存模型"
            title="刷新"
          >
            <RefreshIcon size={13} className={clsx(loading && 'animate-spin')} />
          </button>
          <button
            type="button"
            onClick={onAdd}
            disabled={!!busy}
            className="inline-flex h-8 shrink-0 items-center gap-1.5 rounded-md bg-accent px-3 text-[11px] font-semibold text-white transition hover:opacity-90 focus:outline-none focus:ring-2 focus:ring-accent-soft disabled:opacity-50"
          >
            <VsIcon name="add" size={13} />
            新增模型
          </button>
        </div>
      </div>

      {loading ? (
        <div role="status" className="rounded-md border border-border bg-surface px-3.5 py-5 text-center text-[12px] text-fg-mute">
          正在加载已保存模型…
        </div>
      ) : filtered.length > 0 ? (
        <div className="space-y-2" role="list">
          {filtered.map((model) => (
            <SavedModelRow
              key={model.name}
              model={model}
              isDefault={model.name === defaultName}
              busy={!!busy}
              deleteBlocked={!!(
                model.in_use || model.deletion_blocked || blockedDeletes.has(model.name)
              )}
              onSetDefault={onSetDefault}
              onEdit={onEdit}
              onDelete={onDelete}
            />
          ))}
        </div>
      ) : (
        <div className="rounded-md border border-dashed border-border bg-surface px-3.5 py-6 text-center">
          <div className="text-[12px] font-medium text-fg-2">
            {query ? '没有匹配的已保存模型' : '还没有保存模型'}
          </div>
          <div className="mt-1 text-[11px] text-fg-mute">
            {query ? '调整搜索词，或清空搜索查看全部预设。' : '新增一个 Provider 配置后即可在这里管理。'}
          </div>
        </div>
      )}
    </section>
  );
}
