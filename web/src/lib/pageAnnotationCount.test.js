import assert from 'node:assert/strict';
import fs from 'node:fs';
import { pageAnnotationPendingCount } from './pageAnnotationCount.js';

assert.equal(pageAnnotationPendingCount({
  pending_count: 2,
  records: [{ status: 'pending' }, { status: 'pending' }],
}), 2, 'the toolbar uses the current page snapshot');
assert.equal(pageAnnotationPendingCount({
  records: [{ status: 'pending' }, { status: 'sent' }],
}), 1, 'sent records do not inflate the current-page count');
assert.equal(pageAnnotationPendingCount({
  pending_count: 0,
  records: [{ status: 'pending' }],
}), 0, 'explicit native counts are authoritative');

const sendBar = fs.readFileSync(new URL('../components/PageAnnotationSendBar.jsx', import.meta.url), 'utf8');
assert.match(sendBar, /pageAnnotationPendingCount\(snapshot\)/);
assert.doesNotMatch(sendBar, /pendingTotal/,
  'the in-page toolbar must not display the session-wide pending total');

const panel = fs.readFileSync(new URL('../components/AgentBrowserPanel.jsx', import.meta.url), 'utf8');
assert.doesNotMatch(panel, /pendingTotal=\{outboxSnapshot\.pendingCount\}/,
  'the page toolbar receives only its own annotation snapshot');

console.log('[pass] annotation toolbar counts only the current browser page');
