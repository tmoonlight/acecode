import assert from 'node:assert/strict';
import {
  DEFAULT_TRANSCRIPT_CAPABILITIES,
  READ_ONLY_TRANSCRIPT_CAPABILITIES,
  normalizeTranscriptCapabilities,
  transcriptItemRole,
  transcriptMessageContextAttrs,
  transcriptRenderKind,
  transcriptRowAttrs,
  transcriptRowClassName,
} from './transcriptItemPresentation.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('default and read-only transcript capabilities have explicit interaction boundaries', () => {
  assert.equal(DEFAULT_TRANSCRIPT_CAPABILITIES.forkMessages, true);
  assert.equal(DEFAULT_TRANSCRIPT_CAPABILITIES.showMessageFooters, true);
  assert.equal(DEFAULT_TRANSCRIPT_CAPABILITIES.navigateFiles, true);
  assert.equal(DEFAULT_TRANSCRIPT_CAPABILITIES.showSelectionAnnotations, true);
  assert.equal(READ_ONLY_TRANSCRIPT_CAPABILITIES.forkMessages, false);
  assert.equal(READ_ONLY_TRANSCRIPT_CAPABILITIES.showMessageFooters, false);
  assert.equal(READ_ONLY_TRANSCRIPT_CAPABILITIES.navigateFiles, false);
  assert.equal(READ_ONLY_TRANSCRIPT_CAPABILITIES.showSelectionAnnotations, false);
  assert.equal(READ_ONLY_TRANSCRIPT_CAPABILITIES.openSubagentTranscripts, false);
  assert.equal(normalizeTranscriptCapabilities({ forkMessages: false }).forkMessages, false);
  assert.equal(normalizeTranscriptCapabilities({ forkMessages: false }).navigateFiles, true);
});

run('render kind covers every projected transcript item and safely falls back to message', () => {
  for (const kind of [
    'activity_summary',
    'completion_summary',
    'media_group',
    'subagent_group',
    'termination_notice',
    'tool',
  ]) {
    assert.equal(transcriptRenderKind({ kind }), kind);
  }
  assert.equal(transcriptRenderKind({ kind: 'msg' }), 'message');
  assert.equal(transcriptRenderKind({ kind: 'future_item' }), 'message');
});

run('top-level row attributes preserve the main transcript DOM contract', () => {
  const item = {
    kind: 'msg',
    id: 42,
    messageId: 'message-42',
    messageOrdinal: 7,
    role: 'user',
    content: 'normalized',
    metadata: { display_text: 'original' },
  };
  assert.equal(transcriptItemRole(item), 'user');
  assert.equal(transcriptRowClassName(item), 'ace-chat-row flex flex-col');
  assert.deepEqual(transcriptRowAttrs(item, { continuation: true }), {
    'data-chat-row': 'true',
    'data-chat-item-id': '42',
    'data-chat-kind': 'msg',
    'data-chat-role': 'user',
    'data-desktop-message-id': 'message-42',
    'data-desktop-message-role': 'user',
    'data-desktop-message-text': 'original',
    'data-desktop-message-can-fork': 'true',
    'data-chat-user-message': 'true',
    'data-chat-message-ordinal': '7',
    'data-chat-assistant-continuation': 'true',
  });
});

run('system rows keep the assistant gutter and nested rows do not claim top-level identity', () => {
  const item = { kind: 'msg', id: 'system-1', role: 'system', content: 'working' };
  assert.equal(
    transcriptRowClassName(item),
    'ace-chat-row flex flex-col ace-chat-row-assistant-gutter',
  );
  assert.equal(transcriptRowClassName(item, { nested: true }), 'flex flex-col');
  const attrs = transcriptRowAttrs(item, { nested: true, canFork: false });
  assert.equal(attrs['data-chat-kind'], 'msg');
  assert.equal(attrs['data-chat-role'], 'system');
  assert.equal(attrs['data-chat-row'], undefined);
  assert.equal(attrs['data-chat-item-id'], undefined);
  assert.equal(attrs['data-desktop-message-can-fork'], undefined);
});

run('completion summary exposes assistant desktop context while respecting read-only fork state', () => {
  const item = {
    kind: 'completion_summary',
    messageId: 'done-1',
    summary: 'all done',
  };
  assert.deepEqual(transcriptMessageContextAttrs(item, { canFork: false }), {
    'data-desktop-message-id': 'done-1',
    'data-desktop-message-role': 'assistant',
    'data-desktop-message-text': 'all done',
    'data-desktop-message-can-fork': undefined,
  });
});
