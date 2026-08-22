import assert from 'node:assert/strict';
import { createdFileSource } from './createdFileSource.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('file_write 新建文件优先返回参数中的完整源码', () => {
  const source = createdFileSource({
    success: true,
    tool: 'file_write',
    args: { file_path: 'src/example.cpp', content: 'const int answer = 42;\r\n' },
    summary: { verb: 'Created', object: 'src/example.cpp' },
    hunks: [{
      old_count: 0,
      lines: [{ kind: 'added', text: 'stale', new_line_no: 1 }],
    }],
  });

  assert.deepEqual(source, {
    path: 'src/example.cpp',
    content: 'const int answer = 42;\n',
  });
});

run('file_edit 仅在空 old_string 创建文件时读取 new_string', () => {
  const source = createdFileSource({
    success: true,
    tool: 'file_edit',
    args: { file_path: 'script.py', old_string: '', new_string: 'print("ok")\n' },
    summary: { verb: 'Created', object: 'script.py' },
    hunks: [],
  });
  assert.equal(source.content, 'print("ok")\n');

  assert.equal(createdFileSource({
    success: true,
    tool: 'file_edit',
    args: { old_string: 'old', new_string: 'new' },
    summary: { verb: 'Created', object: 'script.py' },
    hunks: [],
  }), null);
});

run('历史 Created 结果可从全新增 hunk 按行号还原', () => {
  const source = createdFileSource({
    success: true,
    tool: 'file_write',
    args: null,
    summary: { verb: 'Created', object: 'notes.txt' },
    hunks: [{
      old_count: 0,
      lines: [
        { kind: 'added', text: 'first', new_line_no: 1 },
        { kind: 'added', text: 'second', new_line_no: 2 },
      ],
    }],
  });
  assert.deepEqual(source, { path: 'notes.txt', content: 'first\nsecond' });
});

run('不安全 hunk、编辑和失败结果不伪装成新建源码', () => {
  const unsafe = {
    success: true,
    tool: 'file_write',
    args: null,
    summary: { verb: 'Created', object: 'a.js' },
    hunks: [{
      old_count: 1,
      lines: [{ kind: 'removed', text: 'old', old_line_no: 1 }],
    }],
  };
  assert.equal(createdFileSource(unsafe), null);
  assert.equal(createdFileSource({ ...unsafe, summary: { verb: 'Wrote', object: 'a.js' } }), null);
  assert.equal(createdFileSource({ ...unsafe, success: false }), null);
});
