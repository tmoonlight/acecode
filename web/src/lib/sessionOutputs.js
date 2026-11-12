// 摘要面板「产物」分组的纯数据整形:把 GET /api/sessions/:id/outputs 的结果
// 变成要画的行。组件 SummaryPanel.jsx 只渲染这里的结果。

// 收起时显示的条数(与 Claude 桌面版一致),其余点「显示更多」展开。
export const SESSION_OUTPUTS_COLLAPSED_LIMIT = 5;

const BACKSLASH = String.fromCharCode(92);

function splitPath(path) {
  return String(path || '').split(BACKSLASH).join('/').split('/').filter(Boolean);
}

// 接口结果 → 干净的条目。没有路径的丢掉;顺序沿用服务端(最近修改的在前)。
export function normalizeSessionOutputs(payload) {
  const list = Array.isArray(payload?.outputs) ? payload.outputs : [];
  const outputs = [];
  for (const item of list) {
    const path = typeof item?.path === 'string' ? item.path : '';
    const segments = splitPath(path);
    if (segments.length === 0) continue;
    const modifiedMs = Number(item.modified_ms);
    outputs.push({
      path,
      relative: typeof item.relative === 'string' ? item.relative : '',
      name: segments[segments.length - 1],
      modifiedMs: Number.isFinite(modifiedMs) ? modifiedMs : 0,
    });
  }
  return outputs;
}

// 轮询拿到的列表多数时候没变:没变就让组件留着旧数组,不触发重渲染。
export function sessionOutputsEqual(a, b) {
  if (a === b) return true;
  if (!Array.isArray(a) || !Array.isArray(b) || a.length !== b.length) return false;
  return a.every((item, index) => item.path === b[index].path
    && item.relative === b[index].relative
    && item.modifiedMs === b[index].modifiedMs);
}

// 所在目录的名字:工作目录里的文件用相对路径算(根目录下的文件没有),
// 工作目录外的用绝对路径算。
function parentName(output) {
  const segments = splitPath(output.relative || output.path);
  return segments.length >= 2 ? segments[segments.length - 2] : '';
}

// 要画的行。重名的文件(openspec 的三个 spec.md)后面带上所在目录名,
// 不重名的只显示文件名;完整路径放 title。
export function sessionOutputRows(outputs, { expanded = false, limit = SESSION_OUTPUTS_COLLAPSED_LIMIT } = {}) {
  const list = Array.isArray(outputs) ? outputs : [];
  const nameCounts = new Map();
  for (const output of list) {
    const key = output.name.toLowerCase();
    nameCounts.set(key, (nameCounts.get(key) || 0) + 1);
  }
  const shown = expanded ? list : list.slice(0, limit);
  return {
    rows: shown.map((output) => ({
      key: output.path,
      path: output.path,
      name: output.name,
      hint: nameCounts.get(output.name.toLowerCase()) > 1 ? parentName(output) : '',
      title: output.path,
    })),
    hiddenCount: list.length - shown.length,
    expandable: list.length > limit,
  };
}
