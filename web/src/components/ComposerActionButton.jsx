import { useTranslation } from 'react-i18next';
import { clsx } from '../lib/format.js';
import { withAppShortcutHint } from '../lib/appShortcuts.js';
import { getGoalStopControlState } from '../lib/goalControl.js';
import { VsIcon } from './Icon.jsx';
import { COMPOSER_ACTION_BUTTON_CLASS, COMPOSER_ACTION_ICON_SIZE } from '../lib/composerActionAppearance.js';

// shortcutRows:[{ label, keys }],发送方式不止一种时(回复中可排队、可在分叉会话中发送)
// 悬停发送按钮显示每种方式和它的快捷键;只有一种时沿用按钮自己的提示。
export function ComposerActionButton({ actionState, stopping = false, mainComposer = false, onSubmit, onStop, shortcutRows = [] }) {
  const { t } = useTranslation();
  const isStop = actionState.mode === 'stop';
  const stopControl = getGoalStopControlState({ busy: isStop, stopping });
  const label = t(isStop && stopping ? stopControl.label : actionState.submitLabel);
  const title = t(isStop && stopping ? stopControl.title : actionState.submitTitle);
  const showShortcuts = !isStop && actionState.mode !== 'resume' && shortcutRows.length > 1;
  return (
    <span className="group/send relative inline-flex shrink-0">
      <button
        type="button"
        onClick={isStop ? onStop : onSubmit}
        disabled={isStop ? stopControl.disabled : !actionState.canSubmit}
        data-composer-action={actionState.mode}
        aria-label={label}
        className={clsx(
          COMPOSER_ACTION_BUTTON_CLASS,
          isStop && stopping ? 'disabled:cursor-wait' : 'disabled:cursor-default',
        )}
        title={showShortcuts ? undefined : (isStop && mainComposer ? withAppShortcutHint(title, 'stop') : title)}
      >
        <VsIcon
          name={isStop ? 'stopFilled' : actionState.mode === 'resume' ? 'run' : 'send'}
          size={COMPOSER_ACTION_ICON_SIZE}
          mono={false}
          className="ace-icon-on-accent"
        />
      </button>
      {showShortcuts && (
        <span
          role="tooltip"
          data-send-shortcuts="true"
          className={clsx(
            'pointer-events-none absolute bottom-full right-0 z-50 mb-2 flex min-w-[208px] flex-col gap-1.5',
            'rounded-lg border border-border bg-surface px-3 py-2 text-[12px] text-fg-2 whitespace-nowrap ace-shadow',
            'invisible opacity-0 transition-[opacity,visibility] duration-150',
            'group-hover/send:visible group-hover/send:opacity-100 group-hover/send:delay-300',
            'group-focus-within/send:visible group-focus-within/send:opacity-100',
          )}
        >
          {shortcutRows.map((row) => (
            <span key={row.label} className="flex items-center justify-between gap-4">
              <span>{t(row.label)}</span>
              <span className="inline-flex items-center gap-1">
                {row.keys.map((key) => (
                  <kbd
                    key={key}
                    className="inline-flex h-5 min-w-5 items-center justify-center rounded border border-border bg-surface-alt px-1 font-sans text-[11px] leading-none text-fg-mute"
                  >
                    {key}
                  </kbd>
                ))}
              </span>
            </span>
          ))}
        </span>
      )}
    </span>
  );
}
