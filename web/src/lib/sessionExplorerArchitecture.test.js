import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

function source(relativePath) {
  return readFileSync(new URL(relativePath, import.meta.url), 'utf8');
}

function test(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

test('所有共享会话右键入口都携带规范 JSONL 路径', () => {
  const sidebar = source('../components/Sidebar.jsx');
  const chatView = source('../components/ChatView.jsx');
  const miniSession = source('../components/MiniSession.jsx');

  assert.match(sidebar, /data-desktop-session-path=\{sessionPath \|\| undefined\}/);
  assert.match(chatView, /data-desktop-session-path=\{sessionPath \|\| undefined\}/);
  assert.match(miniSession, /data-desktop-session-path=\{sessionPath \|\| undefined\}/);
});
