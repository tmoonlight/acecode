import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

const source = readFileSync(
  fileURLToPath(new URL('../components/PathReferenceDropdown.jsx', import.meta.url)),
  'utf8',
);

assert.match(source, /const VISIBLE_ROWS = 9;/);
assert.match(source, /const ROW_HEIGHT = 36;/);
assert.match(
  source,
  /w-\[400px\] max-w-full flex flex-col bg-surface border border-border rounded-lg ace-shadow-lg overflow-hidden font-sans/,
);
assert.match(source, /'flex items-center gap-2 px-2 text-\[13px\]'/);
assert.match(source, /className="min-h-0 flex-1 overflow-y-auto"/);
assert.match(source, /style=\{\{ maxHeight: MAX_LIST_HEIGHT \}\}/);
assert.match(source, /computeAnchoredDropdownLayout\(\{/);
assert.match(source, /data-placement=\{layout\.placement\}/);
assert.match(source, /top: opensBelow \?/);
assert.match(source, /bottom: opensBelow \?/);
assert.match(source, /maxHeight: layout\.maxHeight/);
assert.match(source, /const showFileGroup = filesLoading \|\| !!filesError \|\| files\.length > 0;/);
assert.match(source, /const showSessionGroup = sessionLoading \|\| !!sessionError \|\| sessions\.length > 0;/);
assert.match(source, /const showEmptyReferenceState = !showFileGroup && !showSessionGroup;/);
assert.match(source, /\{showFileGroup && \(/);
assert.match(source, /\{showSessionGroup && \(/);
assert.match(source, /\{showEmptyReferenceState && \(/);
assert.match(source, /border-y border-border bg-surface-alt px-3 py-1\.5 text-\[12px\] font-semibold text-fg-2/);
assert.match(source, /t\('pathReference\.noReferences'\)/);
assert.doesNotMatch(source, /pathReference\.noFiles|pathReference\.noSessions/);

const fileGroup = source.indexOf("t('pathReference.files')");
const sessionGroup = source.indexOf("t('pathReference.sessions')");
assert.ok(fileGroup >= 0 && sessionGroup > fileGroup);
assert.equal((source.match(/overflow-y-auto/g) || []).length, 1);
assert.doesNotMatch(source, /grid-cols-2|flex-row[^\n]*pathReference\.sessions/);
assert.doesNotMatch(source, /updated_at|updatedAt|更新时间/);
assert.doesNotMatch(source, /bottom-full/);

console.log('ok - path reference dropdown keeps one styled scroll surface with grouped rows');
