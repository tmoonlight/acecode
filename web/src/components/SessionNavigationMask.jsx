import { useTranslation } from 'react-i18next';

export function SessionNavigationMask({ open = false, onCancel }) {
  const { t } = useTranslation();
  if (!open) return null;

  const label = t('sessionNavigation.opening');
  const stopInteraction = (event) => {
    event.preventDefault();
    event.stopPropagation();
  };
  // 这层遮罩吞掉全部指针与键盘输入,一旦上游没能收尾就等于界面死锁。
  // Esc 是用户手里最后一个不依赖任何网络往返的出口。
  const onKeyDown = (event) => {
    if (event.key === 'Escape' && typeof onCancel === 'function') {
      event.preventDefault();
      event.stopPropagation();
      onCancel();
      return;
    }
    stopInteraction(event);
  };

  return (
    <div
      className="fixed inset-0 z-[11000] flex cursor-wait items-center justify-center outline-none"
      style={{
        background: 'rgba(var(--ace-bg-rgb), 0.82)',
        backdropFilter: 'blur(6px)',
        WebkitBackdropFilter: 'blur(6px)',
      }}
      role="status"
      aria-live="polite"
      aria-busy="true"
      aria-label={label}
      data-ace-native-overlay="blocking"
      data-session-navigation-mask="true"
      tabIndex={0}
      autoFocus
      onKeyDown={onKeyDown}
      onPointerDown={stopInteraction}
    >
      <div className="flex min-w-44 flex-col items-center gap-3 rounded-xl border border-border bg-surface px-6 py-5 ace-shadow-lg">
        <span className="ace-spinner text-[28px]" aria-hidden="true" />
        <span className="text-[13px] font-medium text-fg">{label}</span>
      </div>
    </div>
  );
}
