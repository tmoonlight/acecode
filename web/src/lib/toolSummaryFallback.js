const GENERIC_TOOL_ICON = '*';
const ARGUMENT_PREVIEW_BYTES = 80;
const OBJECT_PREVIEW_BYTES = 240;
const REDACTED = '[REDACTED]';

const SENSITIVE_KEYS = new Set([
  'apikey',
  'authorization',
  'authtoken',
  'accesstoken',
  'refreshtoken',
  'bearertoken',
  'clientsecret',
  'credential',
  'credentials',
  'password',
  'passwd',
  'privatekey',
  'secret',
  'token',
]);

function utf8BytesForCodePoint(codePoint) {
  if (codePoint <= 0x7f) return 1;
  if (codePoint <= 0x7ff) return 2;
  if (codePoint <= 0xffff) return 3;
  return 4;
}

export function truncateToolSummaryUtf8(value, maxBytes, suffix = '...') {
  const text = String(value ?? '');
  let total = 0;
  for (const character of text) total += utf8BytesForCodePoint(character.codePointAt(0));
  if (total <= maxBytes) return text;

  let suffixBytes = 0;
  for (const character of suffix) suffixBytes += utf8BytesForCodePoint(character.codePointAt(0));
  if (maxBytes <= suffixBytes) return suffix.slice(0, Math.max(0, maxBytes));

  const budget = maxBytes - suffixBytes;
  let used = 0;
  let prefix = '';
  for (const character of text) {
    const bytes = utf8BytesForCodePoint(character.codePointAt(0));
    if (used + bytes > budget) break;
    prefix += character;
    used += bytes;
  }
  return prefix + suffix;
}

export function capitalizeToolName(toolName) {
  const text = String(toolName || 'Tool');
  const first = text.charCodeAt(0);
  if (first >= 97 && first <= 122) {
    return String.fromCharCode(first - 32) + text.slice(1);
  }
  return text;
}

function normalizedKey(key) {
  return String(key || '').toLowerCase().replace(/[^a-z0-9]/g, '');
}

function isSensitiveKey(key) {
  const normalized = normalizedKey(key);
  return SENSITIVE_KEYS.has(normalized)
    || normalized.endsWith('apikey')
    || normalized.endsWith('authorization')
    || normalized.endsWith('credential')
    || normalized.endsWith('credentials')
    || normalized.endsWith('password')
    || normalized.endsWith('passwd')
    || normalized.endsWith('privatekey')
    || normalized.endsWith('secret')
    || normalized.endsWith('token');
}

function redactSensitiveValues(value) {
  if (Array.isArray(value)) return value.map(redactSensitiveValues);
  if (!value || typeof value !== 'object') return value;
  const result = {};
  for (const [key, nested] of Object.entries(value)) {
    result[key] = isSensitiveKey(key) ? REDACTED : redactSensitiveValues(nested);
  }
  return result;
}

function collapseWhitespace(value) {
  return String(value ?? '').replace(/\s+/g, ' ').trim();
}

function parseArguments(argumentsValue) {
  if (typeof argumentsValue !== 'string') {
    return { value: argumentsValue, raw: '', malformed: false };
  }
  const raw = argumentsValue.trim();
  if (!raw) return { value: null, raw: '', malformed: false };
  try {
    return { value: JSON.parse(raw), raw, malformed: false };
  } catch {
    return { value: raw, raw, malformed: true };
  }
}

function argumentPreview(value) {
  const redacted = redactSensitiveValues(value);
  const preview = typeof redacted === 'string'
    ? collapseWhitespace(redacted)
    : JSON.stringify(redacted);
  return truncateToolSummaryUtf8(preview ?? '', ARGUMENT_PREVIEW_BYTES);
}

export function fallbackToolSummary(toolName, argumentsValue) {
  const parsed = parseArguments(argumentsValue);
  let values = [];
  if (parsed.malformed) {
    values = [truncateToolSummaryUtf8(collapseWhitespace(parsed.raw), OBJECT_PREVIEW_BYTES)];
  } else if (parsed.value && typeof parsed.value === 'object' && !Array.isArray(parsed.value)) {
    values = Object.entries(parsed.value).map(([key, value]) => (
      isSensitiveKey(key) ? REDACTED : argumentPreview(value)
    ));
  } else if (parsed.value !== null && parsed.value !== undefined) {
    values = [argumentPreview(parsed.value)];
  }

  return {
    verb: capitalizeToolName(toolName),
    object: truncateToolSummaryUtf8(values.join(' · '), OBJECT_PREVIEW_BYTES),
    icon: GENERIC_TOOL_ICON,
    metrics: [],
  };
}

export function parseLegacyToolCall(content, fallbackToolName = '') {
  const text = String(content || '').trim();
  const match = text.match(/^\[Tool:\s*([^\]\s]+)\s*\]\s*([\s\S]*)$/i);
  const toolName = String(match?.[1] || fallbackToolName || '').trim();
  const argumentsText = match ? String(match[2] || '').trim() : '';
  const parsed = parseArguments(argumentsText);
  return {
    toolName,
    argumentsText,
    args: argumentsText ? parsed.value : null,
    malformed: parsed.malformed,
    summary: fallbackToolSummary(toolName, argumentsText),
  };
}

export function inferLegacyToolSuccess(result) {
  const metadata = result?.metadata;
  if (typeof metadata?.tool_success === 'boolean') return metadata.tool_success;
  const output = String(result?.content || '').trim();
  if (!output) return true;
  return !/^(?:\[?\s*(?:error|failed|failure|aborted|interrupted|cancelled|canceled|denied|unknown tool|hook denied|user denied|permission denied)\b|\s*(?:错误|失败|已中止|已取消))/i.test(output);
}

export const __test__ = {
  argumentPreview,
  isSensitiveKey,
  parseArguments,
  redactSensitiveValues,
};
