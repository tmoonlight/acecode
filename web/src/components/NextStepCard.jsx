import { useEffect, useRef } from 'react';
import { clsx } from '../lib/format.js';
import { appShortcutContextAllows } from '../lib/appShortcuts.js';
import { matchesShortcutBinding } from '../lib/shortcutBindings.js';
import { VsIcon } from './Icon.jsx';

const CONFIRM_SHORTCUT = { code: 'Enter', key: 'Enter', control: true };

// 回合末尾的「下一步」框(仿 Claude 桌面版 wrap_up):模型在 task_complete 里给出的
// 问题与 1–2 个回复按钮,第一个是推荐项。点按钮把按钮文字作为用户回复发出;
// 关闭只收起这一次的框,问题以静态文字留在完成总结里。
export function NextStepCard({ step, onReply, onDismiss, sending = false }) {
  const primaryRef = useRef(null);
  useEffect(() => {
    let composing = false;
    let settleTimer;
    const startComposition = () => { clearTimeout(settleTimer); composing = true; };
    const endComposition = () => { settleTimer = setTimeout(() => { composing = false; }, 0); };
    const confirm = (event) => {
      const button = primaryRef.current;
      if (event.defaultPrevented || composing || !matchesShortcutBinding(event, CONFIRM_SHORTCUT)
          || !button || button.disabled || !button.getClientRects().length
          || !appShortcutContextAllows('nextStep', event.target)) return;
      // Consume before the main composer interprets Ctrl+Enter as editing/sending.
      event.preventDefault();
      event.stopImmediatePropagation();
      if (!event.repeat) button.click();
    };
    window.addEventListener('compositionstart', startComposition, true);
    window.addEventListener('compositionend', endComposition, true);
    window.addEventListener('keydown', confirm, true);
    return () => {
      clearTimeout(settleTimer);
      window.removeEventListener('compositionstart', startComposition, true);
      window.removeEventListener('compositionend', endComposition, true);
      window.removeEventListener('keydown', confirm, true);
    };
  }, []);
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
              ref={index === 0 ? primaryRef : undefined}
              data-next-step-primary={index === 0 ? 'true' : undefined}
              aria-keyshortcuts={index === 0 ? 'Control+Enter' : undefined}
              onClick={() => onReply?.(label)}
              className={clsx(
                'inline-flex h-8 min-w-0 max-w-full items-center gap-2 rounded-md px-3 text-[12px] font-medium transition',
                'disabled:cursor-wait disabled:opacity-50',
                index === 0
                  ? 'bg-accent text-white hover:opacity-90'
                  : 'bg-surface-hi text-fg-2 hover:bg-border',
              )}
            >
              <span className="min-w-0 truncate">{label}</span>
              {index === 0 && (
                <span aria-hidden="true" className="inline-flex shrink-0 items-center gap-1">
                  <kbd className="inline-flex h-5 items-center rounded border border-current px-1 font-sans text-[11px] leading-none">Ctrl</kbd>
                  <kbd className="inline-flex h-5 w-5 items-center justify-center rounded border border-current">
                    <svg viewBox="0 0 16 16" width="14" height="14" fill="none" stroke="currentColor" strokeWidth="1.25" strokeLinecap="round" strokeLinejoin="round">
                      <path d="M12.5 3v5a2 2 0 0 1-2 2h-7m3-3-3 3 3 3" />
                    </svg>
                  </kbd>
                </span>
              )}
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
