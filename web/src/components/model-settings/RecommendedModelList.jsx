import { formatModelTokenLimit } from '../../lib/modelSettings.js';
import { clsx } from '../../lib/format.js';
import { VsIcon } from '../Icon.jsx';

const CAPABILITY_LABELS = {
  vision: '视觉',
  tool_use: '工具',
  reasoning: '推理',
  web_search: '联网',
};

function capabilitySummary(capabilities) {
  return (Array.isArray(capabilities) ? capabilities : [])
    .map((capability) => CAPABILITY_LABELS[capability] || capability)
    .join(' · ');
}

export function RecommendedModelList({ items, loading = false, error = '', onConfigure }) {
  return (
    <section aria-labelledby="recommended-models-title">
      <div className="mb-2 flex items-end justify-between gap-3">
        <div>
          <h3 id="recommended-models-title" className="text-[13px] font-semibold text-fg">
            热门预置
          </h3>
          <p className="mt-0.5 text-[11px] leading-5 text-fg-mute">
            配置模板不会自动保存，也不会改变默认模型。
          </p>
        </div>
      </div>

      {loading && (
        <div role="status" className="rounded-md border border-border bg-surface px-3.5 py-4 text-[12px] text-fg-mute">
          正在读取本地模型目录…
        </div>
      )}
      {!loading && error && (
        <div role="status" className="rounded-md border border-warn bg-warn-bg px-3.5 py-3 text-[12px] text-warn">
          {error}
        </div>
      )}
      {!loading && items.length > 0 && (
        <div className="grid grid-cols-1 gap-2 xl:grid-cols-2" role="list">
          {items.map((item) => {
            const unavailable = item.deprecated || item.unavailable;
            return (
              <article
                key={item.id || item.model_id}
                role="listitem"
                className="flex min-w-0 items-center gap-3 rounded-md border border-border bg-surface px-3.5 py-2.5"
              >
                <div className="flex h-8 w-8 shrink-0 items-center justify-center rounded-md border border-accent-soft bg-accent-bg text-accent">
                  <VsIcon name="lightbulb" size={15} />
                </div>
                <div className="min-w-0 flex-1">
                  <div className="flex min-w-0 items-center gap-2">
                    <span className="truncate text-[12px] font-semibold text-fg">{item.name}</span>
                    {unavailable && (
                      <span className="shrink-0 rounded border border-warn bg-warn-bg px-1.5 py-0.5 text-[10px] text-warn">
                        目录警告
                      </span>
                    )}
                  </div>
                  <div className="mt-0.5 truncate text-[11px] text-fg-mute" title={item.model_id}>
                    {formatModelTokenLimit(item.context_window) || '未知上下文'}
                    {item.capabilities?.length ? ` · ${capabilitySummary(item.capabilities)}` : ''}
                  </div>
                  {item.privacy_warning && (
                    <div className="mt-1 flex items-start gap-1 text-[10px] leading-4 text-warn">
                      <VsIcon name="warning" size={11} className="mt-0.5 shrink-0" />
                      <span>{item.privacy_warning}</span>
                    </div>
                  )}
                </div>
                <button
                  type="button"
                  onClick={() => onConfigure?.(item)}
                  className={clsx(
                    'shrink-0 rounded-md border border-border bg-surface px-2.5 py-1.5 text-[11px] font-medium text-fg-2 transition',
                    'hover:border-accent-soft hover:bg-accent-bg hover:text-accent focus:outline-none focus:ring-1 focus:ring-accent',
                  )}
                  aria-label={`配置模板 ${item.name}`}
                >
                  配置
                </button>
              </article>
            );
          })}
        </div>
      )}
    </section>
  );
}
