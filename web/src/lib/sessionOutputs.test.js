// sessionOutputs.js 的单元测试(Node + node:assert)。
// 背景:摘要面板的「产物」分组列出这次对话生成的文档 / 表格 / 图片。接口
// (GET /api/sessions/:id/outputs)给出按修改时间排好的文件列表,这里的纯函数
// 负责整理成要画的行:收起时只显示前几个、重名文件带上所在目录、轮询结果没变
// 时不触发重渲染。
import assert from 'node:assert/strict';
import {
  SESSION_OUTPUTS_COLLAPSED_LIMIT,
  normalizeSessionOutputs,
  sessionOutputRows,
  sessionOutputsEqual,
} from './sessionOutputs.js';

function run(name, test) { test(); console.log(`[pass] ${name}`); }

// Windows 路径用反斜杠拼接(源码里不直接写反斜杠字面量)。
const winPath = (...parts) => parts.join(String.fromCharCode(92));

run('接口结果整理:取文件名、保留顺序,丢掉没有路径的条目', () => {
  // 场景:接口返回里混着正反斜杠路径、缺字段的条目和非数组的异常返回。
  // 期望:文件名取路径最后一段;没有路径的条目丢掉;异常返回当作空列表而不是抛错
  // (面板每隔几秒取一次,一次坏返回不该让整个面板崩掉)。
  const outputs = normalizeSessionOutputs({
    outputs: [
      { path: winPath('C:', 'work', 'docs', 'plan.md'), relative: 'docs/plan.md', modified_ms: 2000 },
      { path: '/home/u/report.xlsx', relative: '', modified_ms: 'oops' },
      { relative: 'ghost.md' },
      null,
    ],
  });
  assert.deepEqual(outputs, [
    { path: winPath('C:', 'work', 'docs', 'plan.md'), relative: 'docs/plan.md', name: 'plan.md', modifiedMs: 2000 },
    { path: '/home/u/report.xlsx', relative: '', name: 'report.xlsx', modifiedMs: 0 },
  ]);
  assert.deepEqual(normalizeSessionOutputs(null), []);
  assert.deepEqual(normalizeSessionOutputs({ outputs: 'nope' }), []);
});

run('产物行:收起时只显示前几个,展开后全部显示', () => {
  // 场景:一次对话生成了 7 个文件。
  // 期望:收起显示前 5 个(与 Claude 桌面版一致)并报告还有 2 个没显示;展开后 7 个
  // 全在;不超过 5 个时没有「显示更多」。
  const outputs = normalizeSessionOutputs({
    outputs: Array.from({ length: 7 }, (_, index) => ({ path: `/w/file${index}.md`, relative: `file${index}.md` })),
  });
  const collapsed = sessionOutputRows(outputs);
  assert.equal(collapsed.rows.length, SESSION_OUTPUTS_COLLAPSED_LIMIT);
  assert.equal(collapsed.hiddenCount, 2);
  assert.equal(collapsed.expandable, true);
  assert.equal(collapsed.rows[0].name, 'file0.md');

  const expanded = sessionOutputRows(outputs, { expanded: true });
  assert.equal(expanded.rows.length, 7);
  assert.equal(expanded.hiddenCount, 0);
  assert.equal(expanded.expandable, true);

  const few = sessionOutputRows(outputs.slice(0, 3));
  assert.equal(few.rows.length, 3);
  assert.equal(few.expandable, false);
  assert.deepEqual(sessionOutputRows(null), { rows: [], hiddenCount: 0, expandable: false });
});

run('产物行:重名文件带上所在目录,不重名的只显示文件名', () => {
  // 场景:openspec 一次生成 specs/auth/spec.md、specs/billing/spec.md,外加一个 tasks.md
  // 和一个写到工作目录外的同名 spec.md。
  // 期望:三个 spec.md 各带所在目录名(否则列表里是三行一模一样的 spec.md,分不清);
  // tasks.md 不带;大小写不同的同名文件(Windows)也算重名;完整路径放 title。
  const outputs = normalizeSessionOutputs({
    outputs: [
      { path: '/w/specs/auth/spec.md', relative: 'specs/auth/spec.md' },
      { path: '/w/specs/billing/Spec.md', relative: 'specs/billing/Spec.md' },
      { path: '/w/tasks.md', relative: 'tasks.md' },
      { path: winPath('D:', 'export', 'spec.md'), relative: '' },
    ],
  });
  const { rows } = sessionOutputRows(outputs);
  assert.deepEqual(rows.map((row) => [row.name, row.hint]), [
    ['spec.md', 'auth'],
    ['Spec.md', 'billing'],
    ['tasks.md', ''],
    ['spec.md', 'export'],
  ]);
  assert.equal(rows[3].title, winPath('D:', 'export', 'spec.md'));
  assert.equal(rows[0].key, '/w/specs/auth/spec.md');
});

run('产物行:重名文件里放在工作目录根下的那个没有目录可带', () => {
  // 场景:工作目录根下有 README.md,子目录 docs 里也有一个。
  // 期望:根下那个不带目录(它没有上级可显示),docs 里的带 docs,两行仍能区分。
  const outputs = normalizeSessionOutputs({
    outputs: [
      { path: '/w/README.md', relative: 'README.md' },
      { path: '/w/docs/README.md', relative: 'docs/README.md' },
    ],
  });
  assert.deepEqual(sessionOutputRows(outputs).rows.map((row) => row.hint), ['', 'docs']);
});

run('轮询去重:内容没变判相等,路径或修改时间变了判不等', () => {
  // 场景:回合进行中面板每隔几秒取一次产物列表,绝大多数时候结果和上次一样。
  // 期望:一样时判相等(组件留着旧数组,不重渲染);多了文件、文件被重新生成
  // (修改时间变了,排序可能也变了)判不等。
  const payload = { outputs: [{ path: '/w/a.md', relative: 'a.md', modified_ms: 1000 }] };
  const first = normalizeSessionOutputs(payload);
  assert.equal(sessionOutputsEqual(first, normalizeSessionOutputs(payload)), true);
  assert.equal(sessionOutputsEqual(first, first), true);
  assert.equal(sessionOutputsEqual(first, []), false);
  assert.equal(sessionOutputsEqual(first, normalizeSessionOutputs({
    outputs: [{ path: '/w/a.md', relative: 'a.md', modified_ms: 2000 }],
  })), false);
  assert.equal(sessionOutputsEqual(first, null), false);
});

console.log('sessionOutputs.test.js: all tests passed');
