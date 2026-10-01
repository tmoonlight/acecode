
import assert from 'node:assert/strict';
import { createTranscriptState, loadTranscriptHistory, mergeTranscriptHistoryPage, fetchCompletedTurnHistory } from './sessionTranscript.js';
const tail = loadTranscriptHistory(createTranscriptState(), {
  messages: [{ role: 'user', id: 'u9', content: 'ninth', message_position: '999' }],
  has_more: true, before: 'b9', after: 'a9',
}).state;
assert.equal(tail.historyBefore, 'b9');
const live = { ...tail, busy: true, lastSeq: 20, streamingId: 100 };
const combined = mergeTranscriptHistoryPage(live, {
  messages: [{ role: 'user', id: 'u8', content: 'eighth', message_position: '888' }],
  before: 'b8', has_more: true,
});
assert.equal(combined.items[1], tail.items[0]);
assert.equal(combined.lastSeq, 20);
assert.equal(combined.streamingId, 100);
assert.equal(combined.historyAfter, 'a9');
assert.equal(combined.items[0].messagePosition, '888');
assert.notEqual(combined.items[0].id, combined.items[1].id);
const requests = [];
const api = { getMessages: async (sid, query) => {
  requests.push(query);
  return requests.length === 1
    ? { messages: [{ role: 'assistant', content: 'done' }], before: 'b', has_more: true }
    : { messages: [{ role: 'user', id: 'u9', content: 'ninth' }], has_more: false };
} };
const canonical = await fetchCompletedTurnHistory(api, 'session', tail);
assert.equal(requests.length, 2);
assert.equal(requests[0].limit, 200);
assert.equal(requests[1].before, 'b');
assert.equal(canonical.messages[0].id, 'u9');
assert.equal(canonical.messages[1].content, 'done');
console.log('[pass] paged metadata, stable item identities and bounded self-heal history');
