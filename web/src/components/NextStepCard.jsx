import { clsx } from '../lib/format.js';
import { VsIcon } from './Icon.jsx';

// 回合末尾的「下一步」框(仿 Claude 桌面版 wrap_up):模型在 task_complete 里给出的
// 问题与 1–2 个回复按钮,第一个是推荐项。点按钮把按钮文字作为用户回复发出;
// 关闭只收起这一次的框,问题以静态文字留在完成总结里。
export function NextStepCard({ step, onReply, onDismiss, sending = false }) {
  if (!step) return null;
  const actions = Array.isArray(step.actions) ? step.actions : [];
  return (
    <section
      data-next-step-box="true"
      aria-label={step.ask || undefined}
      aria-busy={sending ? 'true' : undefined}
      className="relative w-full max-w-[720px] min-w-0 rounded-lg border border-border bg-surface px-3.5 py-3 shadow-sm"
    >
      {step.ask && (
        <p className="m-0 pr-7 text-[13px] font-medium leading-relaxed text-fg break-words">
          {step.ask}
        </p>
      )}
      {actions.length > 0 && (
        <div className={clsx('flex flex-wrap gap-2', step.ask && 'mt-2.5')}>
          {actions.map((label, index) => (
            <button
              key={`${index}:${label}`}
              type="button"
              disabled={sending}
              data-next-step-primary={index === 0 ? 'true' : undefined}
              onClick={() => onReply?.(label)}
              className={clsx(
                'h-8 max-w-full truncate rounded-md px-3 text-[12px] font-medium transition',
                'disabled:cursor-wait disabled:opacity-50',
                index === 0
                  ? 'bg-accent text-white hover:opacity-90'
                  : 'bg-surface-hi text-fg-2 hover:bg-border',
              )}
            >
              {label}
            </button>
          ))}
        </div>
      )}
      <button
        type="button"
        aria-label="关闭"
        onClick={onDismiss}
        className="absolute right-2 top-2 inline-flex h-6 w-6 items-center justify-center rounded text-fg-mute transition hover:bg-surface-hi hover:text-fg"
      >
        <VsIcon name="close" size={14} />
      </button>
    </section>
  );
}
