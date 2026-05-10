const DEFAULT_PREVIEW_LIMIT = 180;

function textFromValue(value) {
  if (typeof value === 'string') return value;
  if (value == null) return '';
  try {
    return JSON.stringify(value);
  } catch {
    return String(value);
  }
}

export function compactOneLinePreview(value, limit = DEFAULT_PREVIEW_LIMIT) {
  const text = textFromValue(value);
  const normalized = text
    .replace(/\r\n/g, '\n')
    .split('\n')
    .map((line) => line.trim())
    .filter(Boolean)
    .join(' ')
    .replace(/\s+/g, ' ')
    .trim();
  if (!normalized) return '空内容';
  const chars = Array.from(normalized);
  const max = Math.max(8, Number(limit) || DEFAULT_PREVIEW_LIMIT);
  if (chars.length <= max) return normalized;
  return chars.slice(0, max).join('') + '...';
}

export function compactLineCount(value) {
  const text = textFromValue(value);
  if (!text) return 0;
  return text.split(/\r\n|\r|\n/).length;
}

export function labelForNonAssistantRole(role) {
  const normalized = String(role || '').toLowerCase();
  if (normalized === 'tool_call') return '工具调用';
  if (normalized === 'tool_result' || normalized === 'tool') return '工具返回';
  if (normalized === 'error') return '错误信息';
  return '系统信息';
}

export function buildCompactMessagePreview({ role = '', content = '', label = '' } = {}) {
  const text = textFromValue(content);
  return {
    label: label || labelForNonAssistantRole(role),
    text,
    preview: compactOneLinePreview(text),
    lineCount: compactLineCount(text),
    charCount: Array.from(text).length,
  };
}
