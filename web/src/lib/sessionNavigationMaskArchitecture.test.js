import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

function source(relativePath) {
  return fs.readFileSync(path.join(srcRoot, relativePath), 'utf8').replace(/\r\n?/g, '\n');
}

function between(text, start, end) {
  const startIndex = text.indexOf(start);
  const endIndex = text.indexOf(end, startIndex);
  assert.notEqual(startIndex, -1, `missing start marker: ${start}`);
  assert.notEqual(endIndex, -1, `missing end marker: ${end}`);
  return text.slice(startIndex, endIndex);
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

run('App starts masked for redirected session targets and renders one shell-level mask', () => {
  const app = source('App.jsx');
  const startupState = between(
    app,
    'const startupOpenTargetRef',
    'const [guidedTourState',
  );

  assert.match(startupState, /pendingSessionNavigationIdsRef = useRef\(new Set\(\)\)/);
  assert.match(
    startupState,
    /sessionNavigationPending, setSessionNavigationPending\] = useState\(\s*\(\) => !!startupOpenTargetRef\.current/,
  );
  assert.match(app, /<SessionNavigationMask open=\{sessionNavigationPending\} \/>/);
});

run('shared session resume flow owns overlap-safe mask cleanup and redirect handoff', () => {
  const app = source('App.jsx');
  const navigation = between(
    app,
    'const beginSessionNavigation',
    'const openSettingsSection',
  );

  assert.match(navigation, /pendingSessionNavigationIdsRef\.current\.add\(navigationId\)/);
  assert.match(navigation, /pendingSessionNavigationIdsRef\.current\.delete\(navigationId\)/);
  assert.match(navigation, /pendingSessionNavigationIdsRef\.current\.size === 0/);
  assert.match(
    navigation,
    /const navigationId = beginSessionNavigation\(\);\s*let handedOffToPageLoad = false;\s*try \{/,
  );
  assert.match(
    navigation,
    /window\.location\.href = url;\s*handedOffToPageLoad = true;\s*return true;/,
  );
  assert.match(
    navigation,
    /finally \{\s*if \(!handedOffToPageLoad\) finishSessionNavigation\(navigationId\);\s*\}/,
  );
  assert.match(navigation, /toast\(\{ kind: 'err', text: '恢复失败:' \+ \(e\.message \|\| ''\) \}\)/);
});

run('session navigation mask blocks the viewport and exposes accessible progress', () => {
  const mask = source('components/SessionNavigationMask.jsx');

  assert.match(mask, /if \(!open\) return null/);
  assert.match(mask, /fixed inset-0 z-\[11000\]/);
  assert.match(mask, /data-session-navigation-mask="true"/);
  assert.match(mask, /role="status"/);
  assert.match(mask, /aria-live="polite"/);
  assert.match(mask, /aria-busy="true"/);
  assert.match(mask, /t\('sessionNavigation\.opening'\)/);
  assert.match(mask, /aria-label=\{label\}/);
  assert.match(mask, /className="ace-spinner text-\[28px\]"/);
  assert.match(mask, />\{label\}<\/span>/);
  assert.match(mask, /tabIndex=\{0\}[\s\S]*autoFocus/);
  assert.match(mask, /onKeyDown=\{stopInteraction\}/);
  assert.match(mask, /onPointerDown=\{stopInteraction\}/);
  assert.doesNotMatch(mask, /setTimeout|requestAnimationFrame/);
});
