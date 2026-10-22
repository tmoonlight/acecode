import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import {
  SESSION_REFERENCE_DROP_EVENT,
  dispatchSessionReferenceDrop,
  droppableSessionReferences,
  sessionReferenceDropTargetFrom,
  sessionReferencesFromEntries,
} from './sessionReferenceDrop.js';
import { formatSessionReferenceToken, parseSessionReferenceToken } from './sessionReference.js';
import { composerSessionTagForReference, composerTextFromDocument } from './richComposerModel.js';

function test(name, fn) {
  try { fn(); console.log('ok -', name); }
  catch (error) { console.error('not ok -', name); throw error; }
}

function source(path) {
  return readFileSync(fileURLToPath(new URL(path, import.meta.url)), 'utf8');
}

const workspaceSession = { id: 's1', title: '修复侧栏', workspace_hash: 'ws1' };
const taskSession = { id: 't1', summary: '整理周报', no_workspace: true };

// 场景:从侧栏多选拖动 —— 里面有普通会话、无工作区任务、定时任务分组行、重复项和归档会话。
// 期望:只产出可被 @ 引用的会话,保持拖动顺序;标题取侧栏显示标题;工作区名优先用会话自带,
// 没有时按 hash 查;分组行(sidebar_group)、归档会话和子会话被跳过,重复项只留一个。
test('拖动条目转换成会话引用', () => {
  const references = sessionReferencesFromEntries([
    { session: workspaceSession },
    { session: { id: 'group-1', sidebar_group: true, title: '每分钟问候' } },
    { session: taskSession },
    { session: workspaceSession },
    { session: { id: 'old', title: '旧会话', workspace_hash: 'ws1', archived: true } },
    { session: { id: 'child', title: '子任务', workspace_hash: 'ws1', parent_session_id: 's1' } },
  ], { workspaceNameFor: hash => (hash === 'ws1' ? 'acecode' : '') });
  assert.deepEqual(references, [
    { id: 's1', title: '修复侧栏', workspace_hash: 'ws1', no_workspace: false, workspace_name: 'acecode' },
    { id: 't1', title: '整理周报', workspace_hash: '', no_workspace: true, workspace_name: '任务' },
  ]);
});

// 场景:把当前会话自身、以及输入框里已经 @ 过的会话再拖进来。
// 期望:这两类被过滤掉(@ 菜单同样不列当前会话),其余照常插入。
test('过滤当前会话和已引用的会话', () => {
  const references = sessionReferencesFromEntries([{ session: workspaceSession }, { session: taskSession }, { session: { id: 'cur', workspace_hash: 'ws1' } }]);
  const text = `看看 ${formatSessionReferenceToken({ id: 't1', no_workspace: true, title: '整理周报' })}`;
  assert.deepEqual(droppableSessionReferences(references, { currentSessionId: 'cur', text }).map(item => item.id), ['s1']);
});

// 场景:拖进输入框后生成的会话标签要与 @ 菜单插入的完全等价,提交时才能被 extractSessionReferences 识别。
// 期望:标签序列化出的文本就是 @ 菜单使用的 @session: 令牌,解析回来是同一个会话。
test('拖入生成的标签与 @ 菜单令牌一致', () => {
  const [reference] = sessionReferencesFromEntries([{ session: workspaceSession }], { workspaceNameFor: () => 'acecode' });
  const tag = composerSessionTagForReference(reference);
  const text = composerTextFromDocument([{ type: 'paragraph', children: [{ text: '' }, tag, { text: '' }] }]);
  assert.equal(text, formatSessionReferenceToken(reference, { trailingSpace: false }));
  assert.equal(parseSessionReferenceToken(text).session_id, 's1');
  assert.equal(composerSessionTagForReference({}), null);
});

// 场景:侧栏松手时由落点元素找到所在的输入框并派发事件。
// 期望:只有带 data-session-reference-drop="true" 的祖先才算落点;事件被输入框认领后返回 true。
test('落点查找与事件派发', () => {
  const composer = { id: 'composer' };
  const inner = { closest: selector => (selector === '[data-session-reference-drop="true"]' ? composer : null) };
  assert.equal(sessionReferenceDropTargetFrom(inner), composer);
  assert.equal(sessionReferenceDropTargetFrom(null), null);

  const listeners = [];
  const win = {
    CustomEvent: class { constructor(type, init) { this.type = type; this.detail = init.detail; } },
    dispatchEvent(event) { listeners.forEach(fn => fn(event)); return true; },
  };
  listeners.push(event => {
    assert.equal(event.type, SESSION_REFERENCE_DROP_EVENT);
    if (event.detail.target === composer) event.detail.handled = true;
  });
  assert.equal(dispatchSessionReferenceDrop(composer, [{ id: 's1' }], { clientX: 1, clientY: 2 }, win), true);
  assert.equal(dispatchSessionReferenceDrop({ id: 'other' }, [{ id: 's1' }], {}, win), false);
  assert.equal(dispatchSessionReferenceDrop(composer, [], {}, win), false);
});

// 接线守卫:侧栏拖动在输入框上方切成「引用」并在松手时派发;输入框声明落点、认领事件、
// 经 RichComposer 在落点插入标签。任何一环断开,拖进去就只会变成一次无效的排序拖动。
test('侧栏、输入框与编辑器三处接线', () => {
  const sidebar = source('../components/Sidebar.jsx');
  assert.match(sidebar, /sessionReferenceDropTargetAt\(document, moveEvent\.clientX, moveEvent\.clientY\)/);
  assert.match(sidebar, /dispatchSessionReferenceDrop\(composer, references,/);
  assert.match(sidebar, /sessionReferences: sessionReferencesFromEntries\(entries,/);
  const inputBar = source('../components/InputBar.jsx');
  assert.match(inputBar, /data-session-reference-drop=\{sessionReferenceDropEnabled \? 'true' : undefined\}/);
  assert.match(inputBar, /window\.addEventListener\(SESSION_REFERENCE_DROP_EVENT, onDrop\)/);
  assert.match(inputBar, /insertSessionReferences\?\.\(references,/);
  const composer = source('../components/RichComposer.jsx');
  assert.match(composer, /insertSessionReferences\(references = \[\], \{ clientX, clientY \} = \{\}\)/);
  assert.match(composer, /composerSessionTagForReference/);
});
