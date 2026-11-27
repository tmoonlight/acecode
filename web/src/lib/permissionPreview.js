import { buildPermissionToolPreview } from './permissionToolPreview.js';

export const PERMISSION_PREVIEW_LIMITS = Object.freeze({
  previewChars: 3200,
  stringChars: 900,
  objectKeys: 24,
  arrayItems: 16,
  depth: 4,
});

export function truncatePermissionText(text, limit = PERMISSION_PREVIEW_LIMITS.previewChars) {
  const value = String(text ?? '');
  if (value.length <= limit) return { text: value, truncated: 0 };
  return {
    text: value.slice(0, limit) + `\n... 已截断 ${value.length - limit} 个字符`,
    truncated: value.length - limit,
  };
}

export function compactPermissionValue(value, depth = 0) {
  if (typeof value === 'string') {
    return truncatePermissionText(value, PERMISSION_PREVIEW_LIMITS.stringChars).text;
  }
  if (value === null || typeof value !== 'object') return value;

  if (depth >= PERMISSION_PREVIEW_LIMITS.depth) {
    if (Array.isArray(value)) return `[Array(${value.length})]`;
    return '{...}';
  }

  if (Array.isArray(value)) {
    const items = value
      .slice(0, PERMISSION_PREVIEW_LIMITS.arrayItems)
      .map((item) => compactPermissionValue(item, depth + 1));
    if (value.length > PERMISSION_PREVIEW_LIMITS.arrayItems) {
      items.push(`... 还有 ${value.length - PERMISSION_PREVIEW_LIMITS.arrayItems} 项`);
    }
    return items;
  }

  const out = {};
  const entries = Object.entries(value).slice(0, PERMISSION_PREVIEW_LIMITS.objectKeys);
  for (const [key, item] of entries) out[key] = compactPermissionValue(item, depth + 1);
  const rest = Object.keys(value).length - entries.length;
  if (rest > 0) out['...'] = `还有 ${rest} 个字段`;
  return out;
}

export function formatPermissionArgsPreview(args) {
  if (typeof args === 'string') return truncatePermissionText(args);
  let text = '{}';
  try {
    text = JSON.stringify(compactPermissionValue(args || {}), null, 2);
  } catch {
    text = String(args ?? '');
  }
  return truncatePermissionText(text);
}

// 自动模式（试验）退回弹窗的说明(permission.auto_review)由确认框单独显示,
// 不是工具参数,不进参数预览。
function withoutReviewNote(args) {
  const permission = args && typeof args === 'object' && !Array.isArray(args) ? args.permission : null;
  if (!permission || typeof permission !== 'object' || !('auto_review' in permission)) return args;
  const { auto_review: _note, ...rest } = permission;
  const { permission: _permission, ...others } = args;
  return Object.keys(rest).length ? { ...others, permission: rest } : others;
}

export function buildPermissionPreview(request) {
  const tool = buildPermissionToolPreview(request?.tool, request?.args);
  if (tool.kind === 'command') {
    return { tool, ...truncatePermissionText(tool.command) };
  }
  if (tool.kind === 'json') {
    return { tool, ...formatPermissionArgsPreview(withoutReviewNote(request?.args)) };
  }
  return { tool, text: '', truncated: 0 };
}
