export const PERMISSION_MODES = [
  { id: 'default', label: '默认权限', hint: '写/执行操作前确认', color: 'ok' },
  { id: 'auto', label: '自动模式', hint: '自动编辑和运行沙盒内命令,危险操作与越界请求需确认', color: 'warn' },
  { id: 'auto-review', label: '自动模式（试验）', hint: '需要确认的操作交给模型判断,拦下时说明理由', color: 'warn' },
  { id: 'yolo', label: '完全访问权限', hint: '跳过所有工具权限确认', color: 'danger' },
];

const VALID_PERMISSION_MODES = new Set(PERMISSION_MODES.map((mode) => mode.id));

export function normalizePermissionMode(mode) {
  const value = String(mode || '').trim();
  if (value === 'acceptEdits' || value === 'accept-edits') return 'auto';
  return VALID_PERMISSION_MODES.has(value) ? value : 'default';
}

export function permissionModeOption(mode) {
  const normalized = normalizePermissionMode(mode);
  return PERMISSION_MODES.find((item) => item.id === normalized) || PERMISSION_MODES[0];
}
