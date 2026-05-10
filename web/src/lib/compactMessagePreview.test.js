import assert from 'node:assert/strict';
import {
  buildCompactMessagePreview,
  compactLineCount,
  compactOneLinePreview,
  labelForNonAssistantRole,
} from './compactMessagePreview.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('compactOneLinePreview folds multiline text into one line', () => {
  assert.equal(compactOneLinePreview('first\n  second\n\nthird'), 'first second third');
});

run('compactOneLinePreview truncates long content', () => {
  const text = 'x'.repeat(30);
  assert.equal(compactOneLinePreview(text, 12), 'xxxxxxxxxxxx...');
});

run('compactOneLinePreview handles JSON-able values', () => {
  assert.equal(compactOneLinePreview({ tool: 'skill_view', ok: true }), '{"tool":"skill_view","ok":true}');
});

run('compactLineCount counts CRLF and LF lines', () => {
  assert.equal(compactLineCount('a\r\nb\nc'), 3);
});

run('labelForNonAssistantRole distinguishes tool calls and returns', () => {
  assert.equal(labelForNonAssistantRole('tool_call'), '工具调用');
  assert.equal(labelForNonAssistantRole('tool_result'), '工具返回');
  assert.equal(labelForNonAssistantRole('tool'), '工具返回');
  assert.equal(labelForNonAssistantRole('system'), '系统信息');
});

run('buildCompactMessagePreview returns one-line preview metadata', () => {
  const meta = buildCompactMessagePreview({ role: 'tool_result', content: 'line1\nline2' });
  assert.equal(meta.label, '工具返回');
  assert.equal(meta.preview, 'line1 line2');
  assert.equal(meta.lineCount, 2);
});
