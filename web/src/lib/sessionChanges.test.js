import assert from 'node:assert/strict';
import {
  aggregateHunksFromMessages,
  changeGroupsSignature,
  changeSetSignature,
  collectHunkMessagesFromItems,
  collectTurnChangeSetsFromItems,
  latestTurnSuccessfulChangedFiles,
  summarizeChangeGroups,
} from './sessionChanges.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('collectHunkMessagesFromItems 只抽取带 hunks 的 tool item', () => {
  const hunk = { old_start: 1, old_count: 1, new_start: 1, new_count: 1, lines: [] };
  const messages = collectHunkMessagesFromItems([
    { kind: 'msg', content: 'hello' },
    { kind: 'tool', tool: { hunks: [] } },
    {
      kind: 'tool',
      tool: {
        summary: { object: 'a.js', metrics: [{ label: '+', value: '1' }, { label: '-', value: '0' }] },
        hunks: [hunk],
      },
    },
  ]);
  assert.deepEqual(messages, [{
    file: 'a.js',
    additions: 1,
    deletions: 0,
    hunks: [{ ...hunk, file: 'a.js' }],
  }]);
});

run('latestTurnSuccessfulChangedFiles 只提取最后一轮成功工具并去重', () => {
  const items = [
    { kind: 'msg', role: 'user', id: 1, content: 'old' },
    { kind: 'tool', tool: { isDone: true, success: true, tool: 'file_edit', hunks: [{ file: 'old.js' }] } },
    { kind: 'msg', role: 'user', id: 3, content: 'current' },
    { kind: 'tool', tool: { isDone: true, success: true, tool: 'file_edit', hunks: [{ file: 'src\\a.js' }] } },
    { kind: 'tool', tool: { isDone: true, success: false, tool: 'file_write', hunks: [{ file: 'failed.js' }] } },
    { kind: 'tool', tool: { isDone: false, success: null, tool: 'file_write', args: { path: 'pending.js' }, hunks: [] } },
    { kind: 'tool', tool: { isDone: true, success: true, tool: 'file_write', hunks: [{ file: 'src/a.js' }] } },
  ];
  assert.deepEqual(latestTurnSuccessfulChangedFiles(items), ['src/a.js']);
});

run('latestTurnSuccessfulChangedFiles falls back to file tool arguments without hunks', () => {
  assert.deepEqual(latestTurnSuccessfulChangedFiles([
    { kind: 'msg', role: 'user', id: 1 },
    { kind: 'tool', tool: { isDone: true, success: true, tool: 'file_write', args: { path: 'empty.txt' }, hunks: [] } },
    { kind: 'tool', tool: { isDone: true, success: true, tool: 'file_edit', summary: { object: 'src/a.js' }, hunks: [] } },
    { kind: 'tool', tool: { isDone: true, success: true, tool: 'bash', summary: { object: 'not-a-safe-path' }, hunks: [] } },
  ]), ['empty.txt', 'src/a.js']);
});

run('latestTurnSuccessfulChangedFiles requires a user-turn boundary', () => {
  assert.deepEqual(latestTurnSuccessfulChangedFiles([
    { kind: 'tool', tool: { isDone: true, success: true, tool: 'file_write', args: { path: 'a.js' }, hunks: [] } },
  ]), []);
});

run('历史 tool item 中的 file_write / file_edit hunks 会进入聚合，file_read 被忽略', () => {
  const writeHunk = { old_start: 1, old_count: 0, new_start: 1, new_count: 1, lines: [] };
  const editHunk = { old_start: 4, old_count: 1, new_start: 4, new_count: 2, lines: [] };
  const messages = collectHunkMessagesFromItems([
    {
      kind: 'tool',
      tool: {
        summary: { object: 'src/new.js', metrics: [{ label: '+', value: '1' }, { label: '-', value: '0' }] },
        hunks: [writeHunk],
      },
    },
    {
      kind: 'tool',
      tool: {
        summary: { object: 'src/read.js', metrics: [{ label: '+', value: '0' }, { label: '-', value: '0' }] },
        hunks: [],
      },
    },
    {
      kind: 'tool',
      tool: {
        summary: { object: 'src/edit.js', metrics: [{ label: '+', value: '2' }, { label: '-', value: '1' }] },
        hunks: [editHunk],
      },
    },
  ]);
  const groups = aggregateHunksFromMessages(messages);
  assert.deepEqual(groups.map((g) => g.file), ['src/new.js', 'src/edit.js']);
  assert.deepEqual(summarizeChangeGroups(groups), {
    fileCount: 2,
    totalAdditions: 3,
    totalDeletions: 1,
    hasChanges: true,
  });
});

run('collectTurnChangeSetsFromItems 将工具变更归到前一个用户消息', () => {
  const hunkA = { old_start: 1, old_count: 1, new_start: 1, new_count: 1, lines: [] };
  const hunkB = { old_start: 2, old_count: 0, new_start: 2, new_count: 1, lines: [] };
  const sets = collectTurnChangeSetsFromItems([
    { kind: 'msg', id: 1, messageId: 'u1', role: 'user', content: 'first turn' },
    {
      kind: 'tool',
      id: 2,
      tool: { summary: { object: 'a.js', metrics: [{ label: '+', value: '1' }, { label: '-', value: '1' }] }, hunks: [hunkA] },
    },
    { kind: 'msg', id: 3, messageId: 'a1', role: 'assistant', content: 'done' },
    { kind: 'msg', id: 4, messageId: 'u2', role: 'user', content: 'second turn' },
    {
      kind: 'tool',
      id: 5,
      tool: { summary: { object: 'b.js', metrics: [{ label: '+', value: '1' }, { label: '-', value: '0' }] }, hunks: [hunkB] },
    },
  ]);
  assert.equal(sets.length, 2);
  assert.equal(sets[0].userMessageId, 'u1');
  assert.equal(sets[0].afterItemId, 2);
  assert.equal(sets[0].groups[0].file, 'a.js');
  assert.equal(sets[1].userMessageId, 'u2');
  assert.equal(sets[1].afterItemId, 5);
  assert.equal(sets[1].groups[0].file, 'b.js');
});

run('collectTurnChangeSetsFromItems 从本轮文件摘要排除工作区临时文件', () => {
  const scratchHunk = { old_start: 0, old_count: 0, new_start: 1, new_count: 3, lines: [] };
  const sourceHunk = { old_start: 1, old_count: 1, new_start: 1, new_count: 2, lines: [] };
  const sets = collectTurnChangeSetsFromItems([
    { kind: 'msg', id: 1, messageId: 'u1', role: 'user', content: 'change files' },
    {
      kind: 'tool',
      id: 2,
      tool: {
        summary: { object: 'N:/repo/.acecode/tmp/session-1/helper.py', metrics: [{ label: '+', value: '3' }] },
        hunks: [scratchHunk],
        metadata: { exclude_from_turn_change_summary: true },
      },
    },
    {
      kind: 'tool',
      id: 3,
      tool: {
        summary: { object: 'N:/repo/src/main.cpp', metrics: [{ label: '+', value: '2' }, { label: '-', value: '1' }] },
        hunks: [sourceHunk],
      },
    },
  ]);

  assert.equal(sets.length, 1);
  assert.deepEqual(sets[0].groups.map((group) => group.file), ['N:/repo/src/main.cpp']);
  assert.deepEqual(sets[0].summary, {
    fileCount: 1,
    totalAdditions: 2,
    totalDeletions: 1,
    hasChanges: true,
  });
});

run('collectTurnChangeSetsFromItems 本轮仅修改临时文件时不生成摘要', () => {
  const sets = collectTurnChangeSetsFromItems([
    { kind: 'msg', id: 1, messageId: 'u1', role: 'user', content: 'write helper' },
    {
      kind: 'tool',
      id: 2,
      tool: {
        summary: { object: 'N:/repo/.acecode/tmp/session-1/helper.ps1', metrics: [{ label: '+', value: '1' }] },
        hunks: [{ old_start: 0, old_count: 0, new_start: 1, new_count: 1, lines: [] }],
        metadata: { exclude_from_turn_change_summary: true },
      },
    },
  ]);

  assert.deepEqual(sets, []);
});

run('collectTurnChangeSetsFromItems 用完整轮次净 diff 替代重叠工具 patch', () => {
  const operationCreate = {
    old_start: 0, old_count: 0, new_start: 1, new_count: 2,
    lines: [{ kind: 'added', text: 'old' }, { kind: 'added', text: 'keep' }],
  };
  const operationEdit = {
    old_start: 1, old_count: 1, new_start: 1, new_count: 1,
    lines: [{ kind: 'removed', text: 'old' }, { kind: 'added', text: 'new' }],
  };
  const netHunk = {
    old_start: 0, old_count: 0, new_start: 1, new_count: 2,
    lines: [{ kind: 'added', text: 'new' }, { kind: 'added', text: 'keep' }],
  };
  const sets = collectTurnChangeSetsFromItems([
    {
      kind: 'msg', id: 1, messageId: 'u-net', role: 'user', content: 'create and edit',
      turnNetDiff: {
        userMessageUuid: 'u-net', complete: true, errors: [],
        files: [{ file: 'img_test.py', additions: 2, deletions: 0, hunks: [netHunk] }],
      },
    },
    { kind: 'tool', id: 2, tool: {
      summary: { object: 'img_test.py', metrics: [{ label: '+', value: '2' }] },
      hunks: [operationCreate],
    } },
    { kind: 'tool', id: 3, tool: {
      summary: { object: 'img_test.py', metrics: [{ label: '+', value: '1' }, { label: '-', value: '1' }] },
      hunks: [operationEdit],
    } },
  ]);

  assert.equal(sets.length, 1);
  assert.equal(sets[0].groups.length, 1);
  assert.equal(sets[0].groups[0].hunks.length, 1);
  assert.deepEqual(sets[0].groups[0].hunks[0].lines.map((line) => line.text), ['new', 'keep']);
  assert.deepEqual(sets[0].summary, {
    fileCount: 1,
    totalAdditions: 2,
    totalDeletions: 0,
    hasChanges: true,
  });
});

run('完整空轮次净 diff 不回退，不完整记录回退旧工具 hunk', () => {
  const tool = {
    kind: 'tool', id: 2,
    tool: {
      summary: { object: 'a.js', metrics: [{ label: '+', value: '1' }] },
      hunks: [{ old_start: 0, old_count: 0, new_start: 1, new_count: 1, lines: [] }],
    },
  };
  assert.deepEqual(collectTurnChangeSetsFromItems([
    { kind: 'msg', id: 1, messageId: 'u-empty', role: 'user', turnNetDiff: {
      userMessageUuid: 'u-empty', complete: true, files: [], errors: [],
    } },
    tool,
  ]), []);

  const fallback = collectTurnChangeSetsFromItems([
    { kind: 'msg', id: 1, messageId: 'u-incomplete', role: 'user', turnNetDiff: {
      userMessageUuid: 'u-incomplete', complete: false, files: [], errors: ['read failed'],
    } },
    tool,
  ]);
  assert.equal(fallback.length, 1);
  assert.equal(fallback[0].groups[0].file, 'a.js');
});

run('轮次净 diff 继续排除 absolute tool path 对应的 relative scratch path', () => {
  const sets = collectTurnChangeSetsFromItems([
    {
      kind: 'msg', id: 1, messageId: 'u-exclude', role: 'user',
      turnNetDiff: {
        userMessageUuid: 'u-exclude', complete: true, errors: [],
        files: [
          { file: '.AceCode/TMP/s/Helper.py', additions: 1, deletions: 0, hunks: [] },
          { file: 'src/main.cpp', additions: 2, deletions: 1, hunks: [] },
        ],
      },
    },
    {
      kind: 'tool', id: 2,
      tool: {
        summary: { object: 'N:\\repo\\.acecode\\tmp\\s\\helper.py', metrics: [{ label: '+', value: '1' }] },
        hunks: [],
        metadata: { exclude_from_turn_change_summary: true },
      },
    },
  ]);
  assert.deepEqual(sets[0].groups.map((group) => group.file), ['src/main.cpp']);
  assert.equal(sets[0].summary.totalAdditions, 2);
  assert.equal(sets[0].summary.totalDeletions, 1);
});

run('change signatures 对同一组变更保持稳定', () => {
  const groups = aggregateHunksFromMessages([
    { file: 'a.js', additions: 2, deletions: 1, hunks: [{ old_start: 1, old_count: 1, new_start: 1, new_count: 2, lines: [] }] },
  ]);
  const first = changeGroupsSignature(groups);
  const second = changeGroupsSignature(groups.map((group) => ({ ...group, hunks: [...group.hunks] })));
  assert.equal(first, second);
  assert.equal(changeSetSignature({ userMessageId: 'u1', groups }), `u1::${first}`);
});

run('change signatures 区分同位置同数量但内容不同的变更', () => {
  const first = changeGroupsSignature(aggregateHunksFromMessages([
    { file: 'a.js', additions: 1, deletions: 1, hunks: [{
      old_start: 1,
      old_count: 1,
      new_start: 1,
      new_count: 1,
      lines: [{ kind: 'removed', text: 'old' }, { kind: 'added', text: 'new-a' }],
    }] },
  ]));
  const second = changeGroupsSignature(aggregateHunksFromMessages([
    { file: 'a.js', additions: 1, deletions: 1, hunks: [{
      old_start: 1,
      old_count: 1,
      new_start: 1,
      new_count: 1,
      lines: [{ kind: 'removed', text: 'old' }, { kind: 'added', text: 'new-b' }],
    }] },
  ]));
  assert.notEqual(second, first);
});

run('summarizeChangeGroups 汇总文件数和加删行', () => {
  const groups = aggregateHunksFromMessages([
    { hunks: [
      { file: 'a.js', additions: 2, deletions: 1 },
      { file: 'b.js', additions: 5, deletions: 0 },
      { file: 'a.js', additions: 3, deletions: 4 },
    ] },
  ]);
  assert.deepEqual(summarizeChangeGroups(groups), {
    fileCount: 2,
    totalAdditions: 10,
    totalDeletions: 5,
    hasChanges: true,
  });
});

run('aggregateHunksFromMessages 使用 message 级文件名和统计回填 hunk', () => {
  const hunk = { old_start: 1, old_count: 1, new_start: 1, new_count: 1, lines: [] };
  const groups = aggregateHunksFromMessages([
    { file: 'a.js', additions: 3, deletions: 2, hunks: [hunk] },
  ]);
  assert.equal(groups.length, 1);
  assert.equal(groups[0].file, 'a.js');
  assert.equal(groups[0].totalAdditions, 3);
  assert.equal(groups[0].totalDeletions, 2);
  assert.deepEqual(groups[0].hunks, [{ ...hunk, file: 'a.js' }]);
});

run('summarizeChangeGroups 空列表返回无变更', () => {
  assert.deepEqual(summarizeChangeGroups([]), {
    fileCount: 0,
    totalAdditions: 0,
    totalDeletions: 0,
    hasChanges: false,
  });
});
