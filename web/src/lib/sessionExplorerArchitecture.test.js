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

test('所有共享会话右键入口都携带所属工作区目录并排除无工作区会话', () => {
  const sidebar = source('../components/Sidebar.jsx');
  const chatView = source('../components/ChatView.jsx');
  const miniSession = source('../components/MiniSession.jsx');

  assert.match(sidebar, /data-desktop-session-workspace-path=\{noWorkspace \? undefined : s\.cwd \|\| undefined\}/);
  assert.match(chatView, /data-desktop-session-workspace-path=\{sessionIsNoWorkspace \? undefined : ref\?\.cwd \|\| undefined\}/);
  assert.match(miniSession, /data-desktop-session-workspace-path=\{noWorkspace \? undefined : session\.cwd \|\| undefined\}/);
});
