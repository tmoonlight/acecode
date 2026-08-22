const CREATED_FILE_TOOLS = new Set(['file_write', 'file_edit']);

function ownString(value, key) {
  if (!value || typeof value !== 'object' || Array.isArray(value)) return null;
  if (!Object.prototype.hasOwnProperty.call(value, key)) return null;
  return typeof value[key] === 'string' ? value[key] : null;
}

function normalizeSourceText(text) {
  return String(text ?? '').replace(/\r\n?/g, '\n');
}

function sourceFromArgs(tool, args) {
  if (tool === 'file_write') return ownString(args, 'content');
  if (tool === 'file_edit') {
    const oldString = ownString(args, 'old_string');
    if (oldString !== '') return null;
    return ownString(args, 'new_string');
  }
  return null;
}

// 历史消息不会持久化 tool_start.args。新文件的结构化 diff 来自 空内容 ->
// 新内容，正常情况下只含 added 行；只有满足这个约束时才安全地还原源码。
function sourceFromAddedHunks(hunks) {
  if (!Array.isArray(hunks)) return null;
  if (hunks.length === 0) return '';

  const rows = new Map();
  let nextImplicitLine = 1;
  for (const hunk of hunks) {
    if (!hunk || Number(hunk.old_count || 0) !== 0 || !Array.isArray(hunk.lines)) {
      return null;
    }
    for (const line of hunk.lines) {
      if (!line || line.kind !== 'added') return null;
      const explicitLine = Number(line.new_line_no);
      const lineNumber = Number.isInteger(explicitLine) && explicitLine > 0
        ? explicitLine
        : nextImplicitLine;
      if (rows.has(lineNumber)) return null;
      rows.set(lineNumber, String(line.text ?? ''));
      nextImplicitLine = lineNumber + 1;
    }
  }

  if (rows.size === 0) return '';
  let lastLine = 0;
  for (const lineNumber of rows.keys()) lastLine = Math.max(lastLine, lineNumber);
  const sourceLines = [];
  for (let lineNumber = 1; lineNumber <= lastLine; lineNumber += 1) {
    if (!rows.has(lineNumber)) return null;
    sourceLines.push(rows.get(lineNumber));
  }
  return sourceLines.join('\n');
}

export function createdFileSource(entry) {
  if (!entry || entry.success === false) return null;
  const tool = String(entry.tool || '').toLowerCase();
  if (!CREATED_FILE_TOOLS.has(tool) || entry.summary?.verb !== 'Created') return null;

  const hasArgs = !!entry.args && typeof entry.args === 'object' && !Array.isArray(entry.args);
  const content = hasArgs
    ? sourceFromArgs(tool, entry.args)
    : sourceFromAddedHunks(entry.hunks);
  if (content == null) return null;

  const hunkPath = Array.isArray(entry.hunks)
    ? entry.hunks.find((hunk) => typeof hunk?.file === 'string')?.file
    : '';
  const path = entry.summary?.object
    || ownString(entry.args, 'file_path')
    || hunkPath
    || entry.displayOverride
    || '';

  return {
    path: String(path),
    content: normalizeSourceText(content),
  };
}
