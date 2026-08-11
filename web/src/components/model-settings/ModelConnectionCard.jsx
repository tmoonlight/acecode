import { clsx } from '../../lib/format.js';
import { VsIcon } from '../Icon.jsx';

export function ModelConnectionCard({
  auth,
  flow,
  busy = false,
  onConnect,
  onLogout,
  onCopyCode,
  onPoll,
}) {
  const authenticated = !!auth?.authenticated;
  const loading = !!auth?.loading;
  return (
    <section aria-labelledby="model-connections-title">
      <div className="mb-2 flex items-end justify-between gap-3">
        <div>
          <h3 id="model-connections-title" className="text-[13px] font-semibold text-fg">
            模型连接
          </h3>
          <p className="mt-0.5 text-[11px] leading-5 text-fg-mute">
            受管 Provider 在这里完成登录，密钥和端点不会进入模型弹窗。
          </p>
        </div>
      </div>

      <div className="rounded-md border border-border bg-surface px-3.5 py-2.5">
        <div className="flex flex-wrap items-center gap-3">
          <span
            aria-hidden="true"
            className={clsx(
              'h-2 w-2 shrink-0 rounded-full',
              authenticated ? 'bg-ok' : loading ? 'bg-fg-mute' : 'bg-warn',
            )}
          />
          <div className="min-w-0 flex-1">
            <div className="flex flex-wrap items-center gap-2">
              <span className="text-[13px] font-medium text-fg">GitHub Copilot</span>
              <span className={clsx(
                'rounded border px-1.5 py-0.5 text-[10px] font-medium',
                authenticated
                  ? 'border-ok-border bg-ok-bg text-ok'
                  : 'border-border bg-surface-alt text-fg-mute',
              )}>
                {loading ? '正在检查' : authenticated ? '已连接' : '未连接'}
              </span>
            </div>
            <p className="mt-0.5 text-[11px] text-fg-mute">
              使用 ACECode 现有的 GitHub 设备登录与受管端点。
            </p>
          </div>
          <div className="ml-auto flex items-center gap-2">
            {authenticated ? (
              <button
                type="button"
                onClick={onLogout}
                disabled={busy}
                className="h-8 rounded-md border border-border bg-surface px-3 text-[12px] font-medium text-fg-2 transition hover:bg-surface-hi focus:outline-none focus:ring-1 focus:ring-accent disabled:opacity-50"
              >
                退出连接
              </button>
            ) : (
              <button
                type="button"
                onClick={onConnect}
                disabled={busy || loading}
                className="inline-flex h-8 items-center gap-1.5 rounded-md bg-accent px-3 text-[12px] font-medium text-white transition hover:opacity-90 focus:outline-none focus:ring-2 focus:ring-accent-soft disabled:opacity-50"
              >
                <VsIcon name="extension" size={13} />
                连接 GitHub
              </button>
            )}
          </div>
        </div>

        {flow?.device_code && (
          <div className="mt-2.5 flex flex-wrap items-center gap-2 border-t border-border pt-2.5">
            <span className="text-[11px] text-fg-mute">设备验证码</span>
            <button
              type="button"
              onClick={() => onCopyCode?.(flow.user_code)}
              className="rounded border border-border bg-surface-alt px-2 py-1 text-[12px] font-semibold tracking-wider text-fg transition hover:bg-surface-hi focus:outline-none focus:ring-1 focus:ring-accent"
              aria-label={`复制 GitHub 验证码 ${flow.user_code || ''}`}
            >
              {flow.user_code || '—'}
            </button>
            <span className="min-w-0 flex-1 text-[11px] text-fg-mute">
              {flow.message || '等待 GitHub 授权'}
            </span>
            <button
              type="button"
              onClick={onPoll}
              disabled={busy}
              className="h-7 rounded-md border border-border px-2.5 text-[11px] text-fg-2 transition hover:bg-surface-hi focus:outline-none focus:ring-1 focus:ring-accent disabled:opacity-50"
            >
              我已完成授权
            </button>
          </div>
        )}
      </div>
    </section>
  );
}
