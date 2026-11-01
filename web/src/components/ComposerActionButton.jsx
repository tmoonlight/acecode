import { useTranslation } from 'react-i18next';
import { clsx } from '../lib/format.js';
import { withAppShortcutHint } from '../lib/appShortcuts.js';
import { getGoalStopControlState } from '../lib/goalControl.js';
import { VsIcon } from './Icon.jsx';
import { COMPOSER_ACTION_BUTTON_CLASS, COMPOSER_ACTION_ICON_SIZE } from '../lib/composerActionAppearance.js';

export function ComposerActionButton({ actionState, stopping = false, mainComposer = false, onSubmit, onStop }) {
  const { t } = useTranslation();
  const isStop = actionState.mode === 'stop';
  const stopControl = getGoalStopControlState({ busy: isStop, stopping });
  const label = t(isStop && stopping ? stopControl.label : actionState.submitLabel);
  const title = t(isStop && stopping ? stopControl.title : actionState.submitTitle);
  return (
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
      title={isStop && mainComposer ? withAppShortcutHint(title, 'stop') : title}
    >
      <VsIcon
        name={isStop ? 'stopFilled' : actionState.mode === 'queue' ? 'queue' : actionState.mode === 'resume' ? 'run' : 'send'}
        size={COMPOSER_ACTION_ICON_SIZE}
        mono={false}
        className="ace-icon-on-accent"
      />
    </button>
  );
}
