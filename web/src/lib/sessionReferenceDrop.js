// 把侧栏会话拖进输入框 = 在落点插入 @会话 引用,与输入框里 @ 选择会话的效果相同;
// 多选拖动时一次插入多个。侧栏拖动是自绘的指针拖动(不是 HTML5 拖放),输入框收不到
// drop 事件,所以由侧栏在松手时判断落点、派发 SESSION_REFERENCE_DROP_EVENT,
// 落点所在的输入框认领并插入。
import { extractSessionReferences } from './sessionReference.js';
import { sessionDisplayTitle } from './sessionTitle.js';

export const SESSION_REFERENCE_DROP_EVENT = 'acecode:session-reference-drop';
// 可接收会话引用的输入框根元素带这个属性(值为 "true");禁用时不带。
export const SESSION_REFERENCE_DROP_ATTR = 'data-session-reference-drop';
// 拖动经过时加在输入框根元素上的高亮 class。
export const SESSION_REFERENCE_DROP_ACTIVE_CLASS = 'is-session-reference-drop';

function stringField(value) {
  return typeof value === 'string' ? value.trim() : '';
}

function referenceKey(reference = {}) {
  const noWorkspace = !!(reference.no_workspace || reference.noWorkspace);
  const id = stringField(reference.session_id || reference.id);
  return `${noWorkspace ? 'task' : stringField(reference.workspace_hash || reference.workspaceHash)}::${id}`;
}

// entries:[{ session }](侧栏拖动 / 多选的条目)。会话组(定时任务分组行)、已归档会话、
// 子会话不能被引用,与 @ 菜单的候选规则一致。
export function sessionReferencesFromEntries(entries = [], {
  workspaceNameFor = () => '',
  noWorkspaceLabel = '任务',
} = {}) {
  const seen = new Set();
  const references = [];
  for (const entry of Array.isArray(entries) ? entries : []) {
    const session = entry?.session || entry;
    if (!session || session.sidebar_group) continue;
    if (session.archived || session.is_archived) continue;
    if (stringField(session.parent_session_id || session.parentSessionId)) continue;
    const id = stringField(session.id || session.session_id || session.sessionId);
    if (!id) continue;
    const noWorkspace = !!(session.no_workspace || session.noWorkspace);
    const workspaceHash = noWorkspace ? '' : stringField(session.workspace_hash || session.workspaceHash);
    const reference = {
      id,
      title: sessionDisplayTitle(session, ''),
      workspace_hash: workspaceHash,
      no_workspace: noWorkspace,
      workspace_name: noWorkspace
        ? noWorkspaceLabel
        : stringField(session.workspaceName || session.workspace_name) || stringField(workspaceNameFor(workspaceHash)),
    };
    const key = referenceKey(reference);
    if (seen.has(key)) continue;
    seen.add(key);
    references.push(reference);
  }
  return references;
}

// 去掉当前会话自身(@ 菜单同样不列出它)和输入框里已经引用过的会话。
export function droppableSessionReferences(references = [], { currentSessionId = '', text = '' } = {}) {
  const current = stringField(currentSessionId);
  const existing = new Set(extractSessionReferences(String(text || '')).references.map(referenceKey));
  return references.filter(reference => reference.id !== current && !existing.has(referenceKey(reference)));
}

export function sessionReferenceDropTargetFrom(element) {
  return element?.closest?.(`[${SESSION_REFERENCE_DROP_ATTR}="true"]`) || null;
}

export function sessionReferenceDropTargetAt(doc, clientX, clientY) {
  if (!doc?.elementFromPoint || !Number.isFinite(clientX) || !Number.isFinite(clientY)) return null;
  return sessionReferenceDropTargetFrom(doc.elementFromPoint(clientX, clientY));
}

// 返回 true 表示有输入框认领了这次放下。
export function dispatchSessionReferenceDrop(target, references, { clientX, clientY } = {}, win = globalThis.window) {
  if (!target || !references?.length || !win?.dispatchEvent) return false;
  const detail = { target, references, clientX, clientY, handled: false };
  win.dispatchEvent(new win.CustomEvent(SESSION_REFERENCE_DROP_EVENT, { detail }));
  return detail.handled;
}
