import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

function source(relativePath) {
  return fs.readFileSync(path.join(srcRoot, relativePath), 'utf8').replace(/\r\n?/g, '\n');
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

test('SessionRow restores the binding icon without animating initial mount', () => {
  const sidebar = source('components/Sidebar.jsx');
  assert.match(sidebar, /const remoteControlBound = Boolean\(s\.remote_control_bound \?\? s\.remoteControlBound\);/);
  assert.match(sidebar, /useRef\(remoteControlBound\)/);
  assert.match(
    sidebar,
    /shouldStartRemoteControlSurge\(wasBound, remoteControlBound\)/,
  );
  assert.match(sidebar, /remoteControlBound && 'is-remote-control-bound'/);
  assert.match(sidebar, /data-remote-control-bound=\{remoteControlBound \? 'true' : undefined\}/);
  assert.match(sidebar, /remoteControlBound && \([\s\S]*name="computer"[\s\S]*data-remote-control-session-icon="true"/);
});

test('SessionRow removes the one-shot overlay after the surge completes', () => {
  const sidebar = source('components/Sidebar.jsx');
  assert.match(sidebar, /remoteControlSurging && \(/);
  assert.match(sidebar, /className="ace-session-remote-control-surge"/);
  assert.match(
    sidebar,
    /onAnimationEnd=\{\(\) => finishRemoteControlSurge\(activeRemoteControlSurgeSequenceRef\.current\)\}/,
  );
  assert.match(sidebar, /REMOTE_CONTROL_SURGE_FALLBACK_MS = 950/);
  assert.match(sidebar, /onRemoteControlSurgeCompleted\?\.\(sequence\)/);
  assert.match(
    sidebar,
    /else if \(!remoteControlBound\) \{\s*setRemoteControlSurging\(false\);/,
  );
});

test('bound sessions use title icons instead of a persistent row background', () => {
  const styles = source('styles/globals.css');
  const chatView = source('components/ChatView.jsx');
  const icon = source('../public/vs-icons/Computer.svg');
  assert.doesNotMatch(
    styles,
    /\.ace-sidebar-session-row\.is-remote-control-bound\s*\{[\s\S]*background-color:[\s\S]*background-image:[\s\S]*box-shadow:/,
  );
  assert.match(chatView, /const remoteControlBound = Boolean\(ref\?\.remote_control_bound \?\? ref\?\.remoteControlBound\);/);
  assert.match(chatView, /remoteControlBound && \([\s\S]*name="computer"[\s\S]*data-remote-control-session-icon="true"/);
  assert.match(
    chatView,
    /notifySessionListChanged\(\{[\s\S]*workspaceHash: noWorkspace \? '' : commandWorkspaceHash,[\s\S]*noWorkspace,/,
  );
  assert.match(icon, /<svg[\s\S]*<rect[\s\S]*<path/);
  assert.match(styles, /@keyframes ace-session-remote-control-surge\s*\{/);
  assert.match(
    styles,
    /\.ace-session-remote-control-surge\s*\{[\s\S]*pointer-events: none;/,
  );
  assert.match(
    styles,
    /@media \(prefers-reduced-motion: reduce\) \{\s*\.ace-session-remote-control-surge \{\s*display: none;/,
  );
  assert.doesNotMatch(
    styles,
    /@media \(prefers-reduced-motion: reduce\)[\s\S]*?\.ace-sidebar-session-row\.is-remote-control-bound\s*\{\s*(?:display|background):\s*none/,
  );
});

test('active header binding state follows session navigation and list refreshes', () => {
  const app = source('App.jsx');
  const sidebar = source('components/Sidebar.jsx');
  const jump = source('lib/sessionJump.js');
  assert.match(app, /SESSION_LIST_CHANGED_EVENT/);
  assert.match(app, /syncActiveRemoteControlBound/);
  assert.match(app, /onActiveRemoteControlBoundChange=\{syncActiveRemoteControlBound\}/);
  assert.match(sidebar, /onActiveRemoteControlBoundChange\(\{[\s\S]*remoteControlBound:/);
  assert.match(sidebar, /detail\.reason === 'remote-control-bound'[\s\S]*applyRemoteControlSessionSelection/);
  assert.match(sidebar, /detail\.reason === 'remote-control-unbound'[\s\S]*clearRemoteControlSessionBindings/);
  assert.match(jump, /\['remote_control_bound', \['remote_control_bound', 'remoteControlBound'\]\]/);
});
