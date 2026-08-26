import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

function source(relativePath) {
  return fs.readFileSync(path.join(srcRoot, relativePath), 'utf8');
}

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

function transcriptHook() {
  const file = source('lib/sessionTranscript.js');
  const start = file.indexOf('export function useSessionTranscript');
  assert.notEqual(start, -1, '找不到 useSessionTranscript');
  return file.slice(start);
}

// 回归:曾经的 `useEffect(() => { stateRef.current = state; }, [state])` 会把某次
// 渲染保存的旧快照写回权威状态引用,吞掉这之后已到达的 token。这一条守住"渲染
// 层不得反向写状态",因为该 bug 在传输层完全不可见,只能靠结构约束防复发。
run('transcript hook 不存在把渲染快照写回状态引用的写法', () => {
  const hook = transcriptHook();
  assert.equal(/stateRef\s*\.\s*current\s*=/.test(hook), false);
  assert.equal(/useEffect\(\s*\(\)\s*=>\s*\{\s*\w*[sS]tateRef\.current\s*=\s*state/.test(hook), false);
});

// transcript 状态必须归 store 所有,不能再由渲染层的本地 state 承载 —— 一旦回到
// useState,就会重新出现"渲染快照"与"实时状态"两份真相。
run('transcript 状态由 store 拥有并经 useSyncExternalStore 订阅', () => {
  const file = source('lib/sessionTranscript.js');
  const hook = transcriptHook();

  assert.match(file, /import \{ createTranscriptStore \} from '\.\/transcriptStore\.js';/);
  assert.match(hook, /useSyncExternalStore\(subscribeStore, getStoreSnapshot, getStoreSnapshot\)/);
  // 只匹配调用形式,注释里提及历史写法不算违规。
  assert.equal(/\buseState\(/.test(hook), false, 'transcript 状态不应再由本地 state 承载');
  assert.equal(/setState\(/.test(hook), false);
});

// 唯一写入口是 commit(producer);对外暴露的 updateState 也必须原样转发,不能
// 退化成"接受一份现成的状态值"。
run('对外的 updateState 只转发 producer 给 store', () => {
  const hook = transcriptHook();
  assert.match(
    hook,
    /const updateState = useCallback\(\(producer\) => store\.commit\(producer\), \[store\]\);/,
  );
  assert.equal(/typeof producer === 'function' \? producer\(/.test(hook), false);
});

// 自愈覆写必须在一次提交内完成读取、比对与写回:拆成"先 getState 再 updateState"
// 会让这中间到达的流式增量被旧快照覆盖。
run('ChatView 的自愈覆写在单次提交内读写', () => {
  const chatView = source('components/ChatView.jsx');
  const start = chatView.indexOf('applyCanonicalHistory: (data, snapshot) =>');
  assert.notEqual(start, -1, '找不到 applyCanonicalHistory');
  const block = chatView.slice(start, chatView.indexOf('\n      },', start));

  assert.match(block, /updateState\(\(current\) => \{/);
  assert.match(block, /reconcileLatestCompletedTurn\(current, canonical, snapshot\)/);
  assert.equal(
    /selfHealTranscriptRef\.current\.getState/.test(block),
    false,
    '自愈覆写不应先单独读取状态再写回',
  );
});
