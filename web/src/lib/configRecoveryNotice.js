export function normalizeConfigRecoveryNotice(value) {
  if (!value || value.pending !== true) return null;
  return {
    pending: true,
    recoveredAtMs: Number.isFinite(Number(value.recovered_at_ms))
      ? Number(value.recovered_at_ms)
      : 0,
    configPath: typeof value.config_path === 'string' ? value.config_path : '',
    invalidBackupPath: typeof value.invalid_backup_path === 'string'
      ? value.invalid_backup_path
      : '',
    invalidBackupDir: typeof value.invalid_backup_dir === 'string'
      ? value.invalid_backup_dir
      : '',
  };
}

export function recoveryNoticeBlocksStartup(notice, open) {
  return !!(notice?.pending && open);
}
