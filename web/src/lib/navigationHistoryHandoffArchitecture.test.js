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

run('App restores one-shot navigation history before startup session recovery', () => {
  const app = source('App.jsx');
  assert.match(
    app,
    /const \[navHistory, setNavHistory\] = useState\(\(\) => \(\s*\(typeof window !== 'undefined' && navigationHistoryFromHash\(window\.location\.hash\)\)/,
  );
  assert.match(
    app,
    /const hash = stripNavigationHistoryHash\(window\.location\.hash\);[\s\S]*window\.history\.replaceState\(null, '', newUrl\);[\s\S]*resumeAndOpenSession\(target, \{ replace: true, allowDesktopActivate: false \}\)/,
  );
});

run('cross-workspace session handoff transfers the candidate history', () => {
  const app = source('App.jsx');
  const navigation = between(app, 'const resumeAndOpenSession', 'const openHistoryDestination');

  assert.match(
    navigation,
    /const redirectHistory = suppliedHistory \|\| \([\s\S]*pushNavigation\(navHistoryRef\.current, activeRefRef\.current, nextRef\)/,
  );
  assert.match(
    navigation,
    /desktopOpenSessionUrl\(\{[\s\S]*navigationHistory: redirectHistory,[\s\S]*\}\)/,
  );
});

run('session back and forward traversal commits history only through the shared opener', () => {
  const app = source('App.jsx');
  const traversal = between(app, 'const openHistoryDestination', 'const openSettingsSection');

  assert.match(
    traversal,
    /return resumeAndOpenSession\(result\.activeRef, \{\s*forceResume: true,\s*navigationHistory: result\.history,\s*replace: true,\s*\}\)/,
  );
  assert.match(traversal, /openHistoryDestination\(goBack\(navHistoryRef\.current, activeRefRef\.current\)\)/);
  assert.match(traversal, /openHistoryDestination\(goForward\(navHistoryRef\.current, activeRefRef\.current\)\)/);
  assert.doesNotMatch(traversal, /setNavHistory\(/);
});
