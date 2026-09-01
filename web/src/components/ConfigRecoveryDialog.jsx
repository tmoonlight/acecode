import { useTranslation } from 'react-i18next';
import { Modal } from './Modal.jsx';

export function ConfigRecoveryDialog({
  open,
  notice,
  busy = false,
  onAcknowledge,
}) {
  const { t } = useTranslation();
  if (!open || !notice?.pending) return null;

  return (
    <Modal
      onClose={busy ? undefined : onAcknowledge}
      width={460}
      dismissOnBackdrop={false}
      dismissOnEscape={false}
      layerClassName="z-[420]"
      labelledBy="config-recovery-dialog-title"
    >
      <div className="p-4">
        <div
          id="config-recovery-dialog-title"
          className="text-[14px] font-semibold mb-2"
        >
          {t('configRecovery.title')}
        </div>
        <div className="text-[12.5px] text-fg-mute leading-relaxed">
          {t('configRecovery.message')}
        </div>
        {notice.invalidBackupDir && (
          <div className="mt-3">
            <div className="text-[12px] text-fg-mute mb-1">
              {t('configRecovery.backupLocation')}
            </div>
            <div
              className="rounded-lg border border-border bg-surface-hi px-2.5 py-2 text-[12px] text-fg-2 break-all select-text"
              title={notice.invalidBackupDir}
            >
              {notice.invalidBackupDir}
            </div>
          </div>
        )}
        <div className="flex justify-end gap-2 mt-4">
          <button
            type="button"
            autoFocus
            disabled={busy}
            onClick={onAcknowledge}
            className="px-3 py-1.5 text-[12.5px] rounded-lg bg-accent text-white hover:opacity-90 transition-opacity disabled:cursor-wait disabled:opacity-60"
          >
            {t('configRecovery.acknowledge')}
          </button>
        </div>
      </div>
    </Modal>
  );
}
