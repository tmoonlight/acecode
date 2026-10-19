import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import {
  createPendingNewSessionFirstUserMessage,
  withPendingNewSessionFirstUserMessage,
} from './newSessionFirstUserMessage.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

function assistant(content = 'answer') {
  return {
    kind: 'msg',
    id: 9,
    messageId: 'assistant-1',
    role: 'assistant',
    content,
  };
}

function canonicalUser({
  content = 'expanded prompt',
  displayText = '测试',
  metadata = {},
} = {}) {
  return {
    kind: 'msg',
    id: 1,
    messageId: 'user-1',
    role: 'user',
    content,
    metadata: {
      display_text: displayText,
      ...metadata,
    },
  };
}

run('新 session 空快照仍把临时首条 user 放在 AI 之前', () => {
  const pending = createPendingNewSessionFirstUserMessage({
    sessionId: 'session-1',
    text: '测试',
    timestampMs: 100,
  });
  const result = withPendingNewSessionFirstUserMessage(
    [assistant()],
    pending,
    'session-1',
  );

  assert.deepEqual(result.map((item) => item.role), ['user', 'assistant']);
  assert.equal(result[0].content, '测试');
  assert.equal(result[0].metadata.optimistic_new_session_input, true);
  assert.equal(result[0].ts, 100);
});

run('权威 user 到达时隐藏临时副本且保留权威身份', () => {
  const pending = createPendingNewSessionFirstUserMessage({
    sessionId: 'session-1',
    text: '测试',
  });
  const canonical = canonicalUser();
  const source = [canonical, assistant()];
  const result = withPendingNewSessionFirstUserMessage(source, pending, 'session-1');

  assert.equal(result, source);
  assert.equal(result.length, 2);
  assert.equal(result[0].messageId, 'user-1');
  assert.equal(result[0].content, 'expanded prompt');
});

run('旧 REST 快照再次覆盖权威 user 后临时行会重新补位', () => {
  const pending = createPendingNewSessionFirstUserMessage({
    sessionId: 'session-1',
    text: '测试',
  });
  const live = withPendingNewSessionFirstUserMessage(
    [canonicalUser(), assistant()],
    pending,
    'session-1',
  );
  assert.equal(live[0].messageId, 'user-1');

  const overwritten = withPendingNewSessionFirstUserMessage(
    [assistant()],
    pending,
    'session-1',
  );
  assert.deepEqual(overwritten.map((item) => item.role), ['user', 'assistant']);
  assert.equal(overwritten[0].metadata.optimistic_new_session_input, true);
});

run('合成 user 即使文本相同也不能冒充权威首条输入', () => {
  const pending = createPendingNewSessionFirstUserMessage({
    sessionId: 'session-1',
    text: '测试',
  });
  const synthetic = canonicalUser({
    content: '测试',
    displayText: '测试',
    metadata: { synthetic_user_prompt: true },
  });
  const result = withPendingNewSessionFirstUserMessage(
    [synthetic, assistant()],
    pending,
    'session-1',
  );

  assert.equal(result.length, 3);
  assert.equal(result[0].metadata.optimistic_new_session_input, true);
  assert.equal(result[1].metadata.synthetic_user_prompt, true);
});

run('临时首条消息不会串入其他 session', () => {
  const pending = createPendingNewSessionFirstUserMessage({
    sessionId: 'session-1',
    text: '测试',
  });
  const source = [assistant('other answer')];
  const result = withPendingNewSessionFirstUserMessage(source, pending, 'session-2');

  assert.equal(result, source);
});

run('缺少 session 或非空文本时不创建临时行', () => {
  assert.equal(createPendingNewSessionFirstUserMessage({ sessionId: '', text: '测试' }), null);
  assert.equal(createPendingNewSessionFirstUserMessage({ sessionId: 'session-1', text: '  ' }), null);
});

run('架构: 只在主页首发接入临时行并在 session 提升前记录', () => {
  const chat = readFileSync(new URL('../components/ChatView.jsx', import.meta.url), 'utf8')
    .replace(/\r\n?/g, '\n');
  const createStart = chat.indexOf('const createHomeComposerSession = useStableCallback');
  const createEnd = chat.indexOf('const stageMediaFiles = useStableCallback', createStart);
  const createFlow = chat.slice(createStart, createEnd);
  const homeStart = chat.indexOf('if (!sid) {', chat.indexOf('const isBuiltin ='));
  const activeStart = chat.indexOf('if (composerSubmitting) return;', homeStart);
  const homeFlow = chat.slice(homeStart, activeStart);
  const activeFlow = chat.slice(activeStart, chat.indexOf('const drainQueuedInput', activeStart));

  assert.ok(createStart >= 0 && createEnd > createStart);
  assert.ok(
    createFlow.indexOf('setPendingNewSessionFirstUserMessage(pendingFirstUserMessage)')
      < createFlow.indexOf('onSessionPromoted?.(next)'),
  );
  assert.match(chat, /withPendingNewSessionFirstUserMessage\(\s*items,\s*pendingNewSessionFirstUserMessage,\s*sid/s);
  assert.match(homeFlow, /firstUserMessageText:\s*!isBuiltin \? payload\.text : ''/);
  assert.match(homeFlow, /if \(explicitHomeSend && createdSessionId\)/);
  assert.doesNotMatch(activeFlow, /setPendingNewSessionFirstUserMessage|firstUserMessageText/);
});

const mixedContent = { version: 1, parts: [
  { type: 'text', text: 'Use ' },
  { type: 'skill', name: 'review', token: '$review' },
  { type: 'text', text: ' on ' },
  { type: 'attachment', key: 'local-file', id: '', name: 'notes.md', kind: 'file' },
  { type: 'text', text: ' please' },
] };

run('optimistic first messages retain inline skills/files and hydrate local preview resources', () => {
  const pending = createPendingNewSessionFirstUserMessage({
    sessionId: 'session-1', text: 'Use $review on  please', composerContent: mixedContent,
    attachments: [{ local_id: 'local-file', name: 'notes.md', kind: 'file', preview_url: 'blob:local-preview' }],
  });
  assert.deepEqual(pending.item.composerContent, mixedContent);
  assert.equal(pending.item.contentParts[0].attachment.local_id, 'local-file');
  assert.equal(pending.item.contentParts[0].attachment.preview_url, 'blob:local-preview');
  assert.equal(withPendingNewSessionFirstUserMessage([], pending, 'session-1')[0], pending.item);
});

run('attachment-only optimistic messages render immediately and reconcile after upload IDs change', () => {
  const composerContent = { version: 1, parts: [mixedContent.parts[3]] };
  const pending = createPendingNewSessionFirstUserMessage({ sessionId: 'session-1', text: '', composerContent });
  assert.ok(pending);
  assert.equal(withPendingNewSessionFirstUserMessage([], pending, 'session-1').length, 1);
  const canonical = canonicalUser({ content: '', displayText: '' });
  canonical.composerContent = { version: 1, parts: [{ ...composerContent.parts[0], id: 'uploaded-1' }] };
  const source = [canonical];
  assert.equal(withPendingNewSessionFirstUserMessage(source, pending, 'session-1'), source);
});

run('structured canonical matching keeps distinct attachment positions and ignores upload metadata', () => {
  const pending = createPendingNewSessionFirstUserMessage({ sessionId: 'session-1', composerContent: mixedContent });
  const canonical = canonicalUser({ content: 'expanded prompt', displayText: 'Use $review on  please' });
  canonical.metadata.composer_content = { version: 1, parts: mixedContent.parts.map((part) => (
    part.type === 'attachment' ? { ...part, id: 'uploaded-1' } : part
  )) };
  const source = [canonical, assistant()];
  assert.equal(withPendingNewSessionFirstUserMessage(source, pending, 'session-1'), source);
  canonical.metadata.composer_content = { version: 1, parts: [
    { ...mixedContent.parts[3], id: 'uploaded-1' },
    ...mixedContent.parts.filter((part) => part.type !== 'attachment'),
  ] };
  assert.equal(withPendingNewSessionFirstUserMessage(source, pending, 'session-1').length, 3);
});

run('legacy canonical text and attachment-only resources still deduplicate optimistic input', () => {
  const pending = createPendingNewSessionFirstUserMessage({ sessionId: 'session-1', composerContent: mixedContent });
  const canonical = canonicalUser({ content: 'expanded prompt', displayText: 'Use $review on  please' });
  assert.equal(withPendingNewSessionFirstUserMessage([canonical], pending, 'session-1').length, 1);
  const fileOnly = createPendingNewSessionFirstUserMessage({
    sessionId: 'session-1', text: '',
    attachments: [{ local_id: 'local-file', id: 'uploaded-1', name: 'notes.md', kind: 'file' }],
  });
  const oldCanonical = canonicalUser({ content: '', displayText: '' });
  oldCanonical.contentParts = [{ type: 'file', attachment: { id: 'uploaded-1', name: 'notes.md' } }];
  assert.equal(withPendingNewSessionFirstUserMessage([oldCanonical], fileOnly, 'session-1').length, 1);
});
