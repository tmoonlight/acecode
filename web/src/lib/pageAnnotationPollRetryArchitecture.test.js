// #220 轮询容错与浮窗快捷键：结构守卫。
//
// 两条都只能从组件源码断言，因为触发点都在 PageAnnotations 的 effect/handler 里：
// 1) 轮询的 read 失败一次就必须重新排下一次 timer；早先版本失败即 return，
//    结果断网一次之后页面点击再也开不出浮窗。
// 2) 浮窗底部的快捷键提示必须与实际行为一致（Enter 只添加、Ctrl+Enter 才发送）。
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const sourceRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const component = fs.readFileSync(path.join(sourceRoot, 'components/PageAnnotations.jsx'), 'utf8')
  .replace(/\r\n/g, '\n');

const pollStart = component.indexOf('const poll = async () => {');
assert.ok(pollStart > 0, 'PageAnnotations 必须有轮询函数');
const pollBody = component.slice(pollStart, component.indexOf('timer = setTimeout(poll, 200);\n    };\n', pollStart) + 40);

const failure = pollBody.slice(pollBody.indexOf('ok === false'), pollBody.indexOf('return;', pollBody.indexOf('ok === false')));
assert.match(
  failure,
  /setTimeout\(poll,\s*1000\)/,
  'read 失败后必须退避重排下一次轮询，否则一次断网就让页面点击永久失效',
);
assert.match(failure, /setError\(/, 'read 失败要给出可见提示');
assert.match(
  pollBody,
  /setError\(current => \(current \? '' : current\)\);/,
  '读取恢复后要清掉上一次的失败提示',
);
assert.doesNotMatch(
  pollBody.replace(failure, ''),
  /if \(result\?\.ok === false\) \{\s*setError\([^)]*\);\s*return;/,
  '失败分支不能只 setError 就 return',
);

// Enter 只添加（保存当前批注），Ctrl/Cmd+Enter 才保存并发送。
assert.match(component, /if \(event\.key === 'Enter' && \(event\.ctrlKey \|\| event\.metaKey\)\)/);
assert.match(component, /sendAfterSaveRef\.current = true;/);
const hint = component.match(/showTooltip\(event, '添加批注', '([^']+)'\)/g) || [];
assert.equal(hint.length, 2, '添加批注按钮的指针与键盘路径都要给提示');
for (const call of hint) {
  assert.match(call, /Enter 添加 · Ctrl\+Enter 发送/, `提示文案要与行为一致，实际: ${call}`);
  assert.doesNotMatch(call, /(?<!Ctrl\+)Enter 发送/, 'Enter 不发送，提示不能写「Enter 发送」');
}

console.log('[pass] 批注轮询在读取失败后继续重试，浮窗快捷键提示与实际行为一致');
