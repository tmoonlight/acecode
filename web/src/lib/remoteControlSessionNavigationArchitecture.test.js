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

test('App handles a selected remote-control session through the existing resume helper', () => {
  const app = source('App.jsx');
  assert.match(app, /normalizeRemoteControlSessionSelected\(event\.detail \|\| \{\}\)/);
  assert.match(app, /reason: 'remote-control-session-selected'/);
  assert.match(app, /session: selected\.session/);
  assert.match(app, /resumeAndOpenSession\(selected, \{ allowDesktopActivate: false \}\)/);
  assert.match(app, /connection\.removeEventListener\('message', handler\)/);
});

test('Sidebar keeps selection state and one-shot surge logic separate from reduced-motion CSS', () => {
  const sidebar = source('components/Sidebar.jsx');
  const styles = source('styles/globals.css');
  assert.match(sidebar, /applyRemoteControlSessionSelection\(prev, session\)/);
  assert.doesNotMatch(sidebar, /remoteControlBoundTarget|projectRemoteControlBinding\(/);
  assert.match(sidebar, /current\?\.targetKey === targetKey/);
  assert.match(sidebar, /detail\.reason === 'remote-control-unbound'/);
  assert.match(sidebar, /remote-control-session-selected/);
  assert.match(sidebar, /onRemoteControlSurgeCompleted/);
  assert.match(styles, /@media \(prefers-reduced-motion: reduce\) \{\s*\.ace-session-remote-control-surge \{\s*display: none;/);
});
