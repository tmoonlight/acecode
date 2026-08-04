import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

function source(relativePath) {
  return fs.readFileSync(path.join(srcRoot, relativePath), 'utf8').replace(/\r\n?/g, '\n');
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

function sliceBetween(text, startMarker, endMarker) {
  const start = text.indexOf(startMarker);
  const end = text.indexOf(endMarker, start + startMarker.length);
  assert.ok(start >= 0, `missing start marker: ${startMarker}`);
  assert.ok(end > start, `missing end marker: ${endMarker}`);
  return text.slice(start, end);
}

run('completion routing resolves the owner and gates only native completion dispatch', () => {
  const app = source('App.jsx');
  const dispatch = sliceBetween(
    app,
    'const sendDesktopNotification = (type, msg, payload) => {',
    'const finishMonitoredSession = (sessionId, msg, payload) => {',
  );
  const finish = sliceBetween(
    app,
    'const finishMonitoredSession = (sessionId, msg, payload) => {',
    'const handler = (e) => {',
  );

  assert.match(dispatch, /hasFocus: isHostWindowFocused\(\)/);
  assert.doesNotMatch(dispatch, /activeRef:/);
  assert.match(finish, /conversationOwnerForSession\(sessionId, payload\)/);
  assert.match(finish, /shouldNotifySessionCompletion\(\{/);
  assert.match(
    finish,
    /parentSessionId: payload\.parent_session_id \|\| msg\.parent_session_id \|\| ''/,
  );
  assert.match(
    finish,
    /if \(completionNotificationAllowed[\s\S]*?sendDesktopNotification\('completion'/,
  );

  const notificationIndex = finish.indexOf("sendDesktopNotification('completion'");
  const cleanupIndex = finish.indexOf('context.assistantText.delete(sessionId)');
  const releaseIndex = finish.indexOf('monitor.release(sessionId)');
  assert.ok(notificationIndex >= 0, 'completion dispatch must remain present for main sessions');
  assert.ok(cleanupIndex > notificationIndex, 'assistant text cleanup must remain after the gate');
  assert.ok(releaseIndex > cleanupIndex, 'monitor release must remain after cleanup');
});

run('permission and question requests stay inline without native dispatch', () => {
  const app = source('App.jsx');
  const permission = sliceBetween(
    app,
    "if (msg.type === 'permission_request') {",
    "if (msg.type === 'permission_closed') {",
  );
  const question = sliceBetween(
    app,
    "if (msg.type === 'question_request') {",
    "if (msg.type === 'question_closed') {",
  );

  assert.match(permission, /monitor\.retain\(sessionId\)/);
  assert.match(permission, /conversationOwnerForSession\(sessionId, payload\)/);
  assert.match(permission, /pushPermissionRequest\(prev, payload/);
  assert.doesNotMatch(permission, /sendDesktopNotification/);
  assert.doesNotMatch(permission, /shouldNotifySessionCompletion/);
  assert.match(question, /monitor\.retain\(sessionId\)/);
  assert.match(question, /conversationOwnerForSession\(sessionId, payload\)/);
  assert.match(question, /addPendingQuestionRequest\(prev, payload/);
  assert.doesNotMatch(question, /sendDesktopNotification/);
  assert.doesNotMatch(question, /shouldNotifySessionCompletion/);
});

console.log('subagentCompletionNotificationArchitecture.test.js: all tests passed');
