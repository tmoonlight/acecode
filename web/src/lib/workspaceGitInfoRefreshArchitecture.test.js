import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

function source(relativePath) {
  return fs.readFileSync(path.join(srcRoot, relativePath), 'utf8').replace(/\r\n?/g, '\n');
}

function section(text, startMarker, endMarker) {
  const start = text.indexOf(startMarker);
  const end = text.indexOf(endMarker, start + startMarker.length);
  assert.ok(start >= 0 && end > start, `missing section ${startMarker}`);
  return text.slice(start, end);
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

test('project selection and workspace-backed new conversations refresh Git metadata', () => {
  const app = source('App.jsx');
  const openHome = section(app, 'const openHomeForWorkspace =', 'const openLoopPage =');
  const replaceHome = section(app, 'const replaceHomeWorkspace =', 'const abortGuidedTour =');

  assert.match(app, /import \{ refreshWorkspaceGitInfo \} from '\.\/lib\/gitInfoCache\.js'/);
  assert.match(openHome, /refreshWorkspaceGitInfo\(createApi\(next\), next\)/);
  assert.match(replaceHome, /refreshWorkspaceGitInfo\(createApi\(next\), next\)/);
});

test('all workspace-backed new-task entry points refresh without changing generic navigation', () => {
  const app = source('App.jsx');
  const chat = source('components/ChatView.jsx');
  const sidebar = source('components/Sidebar.jsx');
  const expertTask = section(app, 'const dispatchExpertToNewTask =', 'const consumeInitialDraftText =');
  const trayTask = section(app, 'const createDesktopTraySession =', 'const handleSubagentTasksChange =');
  const homeTask = section(chat, 'const createHomeComposerSession =', 'const uploadMediaFilesToSession =');
  const sidebarTask = section(sidebar, 'const createSessionInWorkspace =', 'const onAddWorkspace =');
  const genericNavigation = section(app, 'const navigateToRef =', 'const replaceNavigationState =');

  assert.match(expertTask, /refreshWorkspaceGitInfo\(createApi\(base\), base\)/);
  assert.match(trayTask, /refreshWorkspaceGitInfo\(createApi\(next\), next\)/);
  assert.match(homeTask, /refreshWorkspaceGitInfo\(api, target\)/);
  assert.match(sidebarTask, /refreshWorkspaceGitInfo\(api, ws\)/);
  assert.doesNotMatch(genericNavigation, /refreshWorkspaceGitInfo/);
});

console.log('workspaceGitInfoRefreshArchitecture.test.js: all tests passed');
