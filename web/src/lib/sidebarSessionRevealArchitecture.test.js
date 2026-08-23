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

test('active-session reveal consumes each stable target only after its row renders', () => {
  const sidebar = source('components/Sidebar.jsx');
  const effectStart = sidebar.indexOf('const targetKey = sidebarRevealTargetKey(selectedRevealTarget);');
  const effectEnd = sidebar.indexOf(
    '\n  // 把已加载的跨 workspace sessions / pinned order / workspaceName 推到桌面 tray 菜单。',
    effectStart,
  );
  assert.ok(effectStart >= 0 && effectEnd > effectStart);

  const revealEffect = sidebar.slice(effectStart, effectEnd);
  assert.match(
    revealEffect,
    /if \(sessionRevealTargetRef\.current !== targetKey\) \{\s*sessionRevealTargetRef\.current = targetKey;\s*revealedSessionTargetRef\.current = '';\s*\}/,
  );
  assert.match(
    revealEffect,
    /if \(revealedSessionTargetRef\.current === targetKey\) return undefined;/,
  );
  assert.match(
    revealEffect,
    /const scrollRoot = sidebarScrollRef\.current;\s*if \(!scrollRoot\) return;\s*const rows = Array\.from\(scrollRoot\.querySelectorAll\(/,
  );
  assert.doesNotMatch(revealEffect, /document\.querySelectorAll\(/);

  const rowGuard = revealEffect.indexOf('if (!row) return;');
  const staleGuard = revealEffect.indexOf('if (sessionRevealTargetRef.current !== targetKey) return;');
  const consume = revealEffect.indexOf('revealedSessionTargetRef.current = targetKey;');
  const scroll = revealEffect.indexOf("row.scrollIntoView?.({ block: 'nearest' });");
  assert.ok(rowGuard >= 0 && staleGuard > rowGuard && consume > staleGuard && scroll > consume);
});

test('clearing the active target resets one-time reveal identity', () => {
  const sidebar = source('components/Sidebar.jsx');
  assert.match(
    sidebar,
    /if \(!selectedRevealTarget\.sessionId\) \{\s*sessionRevealTargetRef\.current = '';\s*revealedSessionTargetRef\.current = '';\s*return undefined;\s*\}/,
  );
});
