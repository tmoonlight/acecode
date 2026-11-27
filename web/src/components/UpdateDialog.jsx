import { useEffect, useRef, useState, useSyncExternalStore } from 'react';
import { api } from '../lib/api.js';
import { formatBytes } from '../lib/format.js';
import {
  formatUpdatePublishedDate,
  nondecreasingUpdateProgress,
  normalizeUpdateReleases,
  updateDialogAllowsRollback,
  updateDialogMode,
  updateJobCanCancel,
  updateJobIsActive,
  updateJobIsRollback,
  updateJobPhaseLabel,
  updateJobProgress,
  updateRestartMessage,
} from '../lib/updateJob.js';
import { upgradeSettingsStore } from '../lib/upgradeSettings.js';
import { Modal } from './Modal.jsx';

function UpdateProgressBar({ phaseLabel, progress }) {
  const renderedProgressRef = useRef(0);
  const renderedProgress = nondecreasingUpdateProgress(
    renderedProgressRef.current,
    progress,
  );
  renderedProgressRef.current = renderedProgress;

  return (
    <>
      <div className="flex items-center justify-between text-[12px]">
        <span className="text-fg">{phaseLabel}</span>
        <span className="text-fg-mute tabular-nums">{renderedProgress}%</span>
      </div>
      <div
        className="ace-update-progress-track mt-2 h-2 overflow-hidden rounded-full bg-surface-hi"
        role="progressbar"
        aria-label="ACECode 升级"
        aria-valuemin={0}
        aria-valuemax={100}
        aria-valuenow={renderedProgress}
        aria-valuetext={`${phaseLabel}，${renderedProgress}%`}
      >
        <div
          className="ace-update-progress-fill h-full rounded-full"
          style={{ width: `${renderedProgress}%` }}
        />
      </div>
    </>
  );
}

function versionText(value) {
  const text = String(value || '').trim();
  if (!text) return '未知';
  return text.startsWith('v') ? text : `v${text}`;
}

function rollbackRunningNote(job) {
  if (job?.cancel_requested) return '正在安全停止回滚，请稍候。';
  if (job?.can_cancel === false) return '已开始替换程序文件，为保证安装完整，此阶段不能取消。';
  return '可取消本次回滚；关闭窗口只会让任务转到后台继续。';
}

// 「自动升级」与设置页的「自动更新」是同一个配置项(upgrade.auto_update),共用同一个 store。
function UpdateAutoSwitch({ settings, store }) {
  const enabled = settings.draft?.auto_update === true;
  return (
    <div className="mr-auto flex min-w-0 items-center gap-2">
      <button
        id="update-dialog-auto-update"
        type="button"
        role="switch"
        aria-checked={enabled}
        disabled={!settings.draft || settings.loading}
        onClick={() => { store.update('auto_update', !enabled); void store.save(); }}
        className={`relative h-5 w-9 shrink-0 rounded-full transition-colors focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-accent disabled:opacity-50 ${enabled ? 'bg-accent' : 'bg-border'}`}
      >
        <span className={`absolute left-0.5 top-0.5 h-4 w-4 rounded-full bg-white transition-transform ${enabled ? 'translate-x-4' : ''}`} />
      </button>
      <label htmlFor="update-dialog-auto-update" className="shrink-0 text-[12px] text-fg-2">
        自动升级
      </label>
      {settings.error && (
        <span role="alert" title={settings.error} className="min-w-0 truncate text-[11px] text-danger">
          {settings.error}
        </span>
      )}
    </div>
  );
}

function RollbackConfirmDialog({ version, autoUpdateEnabled, onCancel, onConfirm }) {
  return (
    <Modal onClose={onCancel} width={460}>
      <div className="px-5 py-5 text-[13px] leading-6 text-fg">
        {autoUpdateEnabled
          ? `您确定您要回退到 ${versionText(version)} 版本吗？自动更新功能将被关闭。`
          : `您确定您要回退到 ${versionText(version)} 版本吗？`}
      </div>
      <div className="flex items-center justify-end gap-2 border-t border-border bg-surface-alt px-5 py-3">
        <button
          type="button"
          onClick={onCancel}
          className="rounded-md px-3 py-1.5 text-[12px] text-fg-mute hover:bg-surface-hi hover:text-fg"
        >
          取消
        </button>
        <button
          type="button"
          data-ace-dialog-primary="true"
          onClick={onConfirm}
          className="rounded-md bg-accent px-4 py-1.5 text-[12px] font-medium text-white hover:opacity-90"
        >
          回退
        </button>
      </div>
    </Modal>
  );
}

export function UpdateDialog({ open, ...props }) {
  if (!open) return null;
  return <UpdateDialogContent {...props} />;
}

function UpdateDialogContent({
  updateStatus,
  job,
  starting = false,
  startingRollback = '',
  cancelling = false,
  restarting = false,
  restartAvailable = false,
  onConfirm,
  onCancel,
  onRetry,
  onRollback,
  onRestart,
  onClose,
}) {
  const settingsStore = upgradeSettingsStore(api);
  const settings = useSyncExternalStore(settingsStore.subscribe, settingsStore.getSnapshot);
  useEffect(() => { void settingsStore.load(); }, [settingsStore]);
  const [rollbackTarget, setRollbackTarget] = useState('');

  // 回滚请求已发出、任务还没回来:按回滚任务的进行中状态显示。
  const rollbackStarting = starting && !job && Boolean(startingRollback);
  const mode = rollbackStarting ? 'running' : updateDialogMode(job, updateStatus);
  const rollback = rollbackStarting || updateJobIsRollback(job);
  const storeManaged = mode === 'store_managed';
  const active = !storeManaged && (updateJobIsActive(job) || starting || restarting);
  const canCancel = updateJobCanCancel(job);
  const currentVersion = storeManaged
    ? updateStatus?.current_version
    : job?.current_version || updateStatus?.current_version;
  const targetVersion = mode === 'up_to_date'
    ? currentVersion
    : job?.target_version || (rollbackStarting ? startingRollback : updateStatus?.latest_version);
  const packageSize = rollback
    ? job?.bytes_total
    : job?.bytes_total ?? updateStatus?.package_size;
  const canRollback = !storeManaged && typeof onRollback === 'function'
    && updateDialogAllowsRollback(mode, { starting, restarting });
  const retry = rollback && job?.target_version
    ? () => onRollback?.(job.target_version)
    : onRetry;
  const confirmRollback = () => {
    const version = rollbackTarget;
    setRollbackTarget('');
    if (version) onRollback?.(version);
  };
  const releaseHistory = normalizeUpdateReleases(updateStatus?.releases);
  const visibleReleaseHistory = releaseHistory.slice(0, Math.ceil(releaseHistory.length / 2));
  const progress = updateJobProgress(job);
  const phaseLabel = starting && !job
    ? (rollbackStarting ? '正在启动回滚任务' : '正在启动升级任务')
    : updateJobPhaseLabel(job);
  const restartMessage = updateRestartMessage(job);
  const title = storeManaged
    ? '由 Microsoft Store 管理更新'
    : mode === 'success'
    ? (rollback ? '回滚完成' : '升级安装完成')
    : mode === 'cancelled'
      ? (rollback ? '回滚已取消' : '升级已取消')
    : mode === 'failure'
      ? (rollback ? '回滚失败' : '升级失败')
      : mode === 'up_to_date'
        ? '已是最新版本'
        : rollback ? 'ACECode 回滚' : 'ACECode 升级';
  const subtitle = storeManaged
    ? '请在 Microsoft Store 的“库”中获取 ACECode 更新。'
    : '';

  return (
    <Modal onClose={onClose} width={560} dismissOnBackdrop={!active}>
      <div className="px-5 py-4 border-b border-border">
        <div className="text-[15px] font-semibold text-fg">{title}</div>
        {subtitle && <div className="mt-1 text-[12px] text-fg-mute">
          {subtitle}
        </div>}
      </div>

      <div className="max-h-[calc(100vh-10rem)] overflow-y-auto px-5 py-5">
        <div className="grid grid-cols-[88px_1fr] gap-y-2 text-[12px]">
          <span className="text-fg-mute">当前版本</span>
          <span className="text-fg font-medium tabular-nums">{versionText(currentVersion)}</span>
          {!storeManaged && (
            <>
              <span className="text-fg-mute">
                {mode === 'up_to_date' ? '最新版本' : '目标版本'}
              </span>
              <span className="text-fg font-medium tabular-nums">{versionText(targetVersion)}</span>
            </>
          )}
          {!storeManaged && Number(packageSize) > 0 && (
            <>
              <span className="text-fg-mute">安装包</span>
              <span className="text-fg tabular-nums">{formatBytes(Number(packageSize))}</span>
            </>
          )}
        </div>

        {mode === 'up_to_date' && (
          <div className="mt-5 rounded-lg border border-ok bg-surface-alt px-4 py-3 text-[12px] leading-5 text-fg">
            当前已是最新版本，无需升级。
          </div>
        )}

        {!storeManaged && releaseHistory.length > 0 && (
          <section className="mt-5" aria-label="版本更新记录">
            <div className="text-[14px] font-semibold text-fg">版本更新记录</div>

            <div className="mt-3 max-h-64 overflow-y-auto rounded-lg border border-border bg-surface-alt">
              {visibleReleaseHistory.map((release, index) => {
                const publishedDate = formatUpdatePublishedDate(release.published_at);
                const latest = release.version === updateStatus?.latest_version;
                return (
                  <article
                    key={`${release.version}-${index}`}
                    className={[
                      'group px-3.5 py-3',
                      index > 0 ? 'border-t border-border' : '',
                    ].join(' ')}
                  >
                    <div className="flex items-center gap-2">
                      <span className="text-[12px] font-semibold text-fg tabular-nums">
                        {versionText(release.version)}
                      </span>
                      {latest && (
                        <span className="rounded bg-accent-bg px-1.5 py-0.5 text-[10px] font-medium text-accent">
                          最新
                        </span>
                      )}
                      {canRollback && release.rollback_available && (
                        <button
                          type="button"
                          onClick={() => setRollbackTarget(release.version)}
                          className="rounded px-1.5 py-0.5 text-[11px] font-medium text-accent opacity-0 transition-opacity hover:bg-accent-bg focus-visible:opacity-100 focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-accent group-hover:opacity-100 [@media(hover:none)]:opacity-100"
                        >
                          回滚到此版本
                        </button>
                      )}
                      {publishedDate && (
                        <span className="ml-auto text-[10px] text-fg-mute tabular-nums">
                          {publishedDate}
                        </span>
                      )}
                    </div>
                    <div className="mt-1.5 whitespace-pre-wrap break-words text-[12px] leading-5 text-fg-2">
                      {release.notes}
                    </div>
                  </article>
                );
              })}
            </div>
          </section>
        )}

        {(mode === 'running' || starting) && (
          <div className="mt-5">
            <UpdateProgressBar
              key={job?.job_id || 'starting'}
              phaseLabel={phaseLabel}
              progress={progress}
            />
            {job?.phase === 'downloading' && Number(job?.bytes_total) > 0 && (
              <div className="mt-2 text-[11px] text-fg-mute tabular-nums">
                {formatBytes(Number(job.bytes_downloaded || 0))} / {formatBytes(Number(job.bytes_total))}
              </div>
            )}
            <div className="mt-3 text-[11px] leading-5 text-fg-mute">
              {rollback ? rollbackRunningNote(job) : job?.cancel_requested
                ? '正在安全停止升级，请稍候。'
                : job?.can_cancel === false
                  ? '已开始替换程序文件，为保证安装完整，此阶段不能取消。'
                  : '可取消本次升级；关闭窗口只会让任务转到后台继续。'}
            </div>
          </div>
        )}

        {mode === 'success' && (
          <div className="mt-5 rounded-lg border border-ok bg-surface-alt px-4 py-3 text-[12px] leading-5 text-fg">
            {rollback && restartAvailable
              ? '回滚已安装，是否立即重启 ACECode 以使用该版本？'
              : restartAvailable
              ? '升级已安装，是否立即重启 ACECode 以使用新版本？'
              : (restartMessage || '升级已安装，请重新启动 ACECode。')}
          </div>
        )}

        {mode === 'failure' && (
          <div className="mt-5 rounded-lg border border-danger bg-surface-alt px-4 py-3">
            <div className="text-[12px] font-medium text-danger">
              {rollback ? '回滚没有完成' : '升级没有完成'}
            </div>
            <div className="mt-1 break-words text-[11px] leading-5 text-fg-mute">
              {job?.error || '未知升级错误'}
            </div>
          </div>
        )}

        {mode === 'cancelled' && (
          <div className="mt-5 rounded-lg border border-border bg-surface-alt px-4 py-3 text-[12px] leading-5 text-fg">
            {rollback
              ? '回滚任务已安全停止，ACECode 仍保持当前版本。'
              : '升级任务已安全停止，ACECode 仍保持当前版本。'}
          </div>
        )}
      </div>

      <div className="flex items-center justify-end gap-2 border-t border-border bg-surface-alt px-5 py-3">
        {!storeManaged && <UpdateAutoSwitch settings={settings} store={settingsStore} />}
        {mode === 'confirm' && (
          <>
            <button
              type="button"
              onClick={onClose}
              disabled={starting}
              className="rounded-md px-3 py-1.5 text-[12px] text-fg-mute hover:bg-surface-hi hover:text-fg disabled:opacity-50"
            >
              取消
            </button>
            <button
              type="button"
              data-ace-dialog-primary="true"
              onClick={onConfirm}
              disabled={starting}
              className="rounded-md bg-accent px-4 py-1.5 text-[12px] font-medium text-white hover:opacity-90 disabled:opacity-60"
            >
              {starting ? '正在启动…' : '立即升级'}
            </button>
          </>
        )}
        {(mode === 'up_to_date' || storeManaged) && (
          <button
            type="button"
            data-ace-dialog-primary="true"
            onClick={onClose}
            className="rounded-md bg-accent px-4 py-1.5 text-[12px] font-medium text-white hover:opacity-90"
          >
            关闭
          </button>
        )}
        {mode === 'running' && (
          <>
            <button
              type="button"
              onClick={onCancel}
              disabled={!canCancel || cancelling}
              className="rounded-md px-3 py-1.5 text-[12px] text-danger hover:bg-surface-hi disabled:cursor-not-allowed disabled:opacity-45"
            >
              {cancelling || job?.cancel_requested ? '正在取消…' : rollback ? '取消回滚' : '取消升级'}
            </button>
            <button
              type="button"
              onClick={onClose}
              className="rounded-md px-4 py-1.5 text-[12px] text-fg hover:bg-surface-hi"
            >
              后台运行
            </button>
          </>
        )}
        {mode === 'success' && (
          restartAvailable ? (
            <>
              <button
                type="button"
                onClick={onClose}
                disabled={restarting}
                className="rounded-md px-3 py-1.5 text-[12px] text-fg-mute hover:bg-surface-hi hover:text-fg disabled:opacity-50"
              >
                稍后重启
              </button>
              <button
                type="button"
                data-ace-dialog-primary="true"
                onClick={onRestart}
                disabled={restarting}
                className="rounded-md bg-accent px-4 py-1.5 text-[12px] font-medium text-white hover:opacity-90 disabled:opacity-60"
              >
                {restarting ? '正在重启…' : '立即重启'}
              </button>
            </>
          ) : (
            <button
              type="button"
              data-ace-dialog-primary="true"
              onClick={onClose}
              className="rounded-md bg-accent px-4 py-1.5 text-[12px] font-medium text-white hover:opacity-90"
            >
              我知道了
            </button>
          )
        )}
        {mode === 'failure' && (
          <>
            <button
              type="button"
              onClick={onClose}
              className="rounded-md px-3 py-1.5 text-[12px] text-fg-mute hover:bg-surface-hi hover:text-fg"
            >
              关闭
            </button>
            <button
              type="button"
              data-ace-dialog-primary="true"
              onClick={retry}
              disabled={starting}
              className="rounded-md bg-accent px-4 py-1.5 text-[12px] font-medium text-white hover:opacity-90 disabled:opacity-60"
            >
              {starting ? '正在重试…' : '重试'}
            </button>
          </>
        )}
        {mode === 'cancelled' && (
          <>
            <button
              type="button"
              onClick={onClose}
              className="rounded-md px-3 py-1.5 text-[12px] text-fg-mute hover:bg-surface-hi hover:text-fg"
            >
              关闭
            </button>
            <button
              type="button"
              data-ace-dialog-primary="true"
              onClick={retry}
              disabled={starting}
              className="rounded-md bg-accent px-4 py-1.5 text-[12px] font-medium text-white hover:opacity-90 disabled:opacity-60"
            >
              {starting ? '正在重试…' : rollback ? '重新回滚' : '重新升级'}
            </button>
          </>
        )}
      </div>
      {rollbackTarget && (
        <RollbackConfirmDialog
          version={rollbackTarget}
          autoUpdateEnabled={settings.draft?.auto_update !== false}
          onCancel={() => setRollbackTarget('')}
          onConfirm={confirmRollback}
        />
      )}
    </Modal>
  );
}
