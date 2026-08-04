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

test('SessionRow restores the fixed binding state without animating initial mount', () => {
  const sidebar = source('components/Sidebar.jsx');
  assert.match(sidebar, /const remoteControlBound = Boolean\(s\.remote_control_bound \?\? s\.remoteControlBound\);/);
  assert.match(sidebar, /useRef\(remoteControlBound\)/);
  assert.match(
    sidebar,
    /shouldStartRemoteControlSurge\(wasBound, remoteControlBound\)/,
  );
  assert.match(sidebar, /remoteControlBound && 'is-remote-control-bound'/);
  assert.match(sidebar, /data-remote-control-bound=\{remoteControlBound \? 'true' : undefined\}/);
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

test('bound background persists while reduced motion suppresses only the surge', () => {
  const styles = source('styles/globals.css');
  assert.match(
    styles,
    /\.ace-sidebar-session-row\.is-remote-control-bound\s*\{[\s\S]*background-color:[\s\S]*background-image:[\s\S]*box-shadow:/,
  );
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
