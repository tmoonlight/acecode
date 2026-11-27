import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { pathToFileURL } from 'node:url';

const binary = process.argv[2];
assert.ok(binary, 'Usage: node tests/desktop/page_annotation_text_smoke.mjs <Chromium executable>');
const source = fs.readFileSync(new URL('../../src/apps/desktop/page_annotation_text_runtime.hpp', import.meta.url), 'utf8');
const factory = source.split('R"ACETEXT(')[1].split(')ACETEXT"')[0];
const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'ace-annotation-text-'));
try {
  const fixture = path.join(directory, 'fixture.html');
  fs.writeFileSync(fixture, `<!doctype html><meta charset="utf-8">
<button id="target" style="color: rgb(255, 0, 0)">Hello <strong>world</strong>!<span hidden>secret</span></button>
<div id="empty"><em></em></div><pre id="result">NOT RUN</pre>
<script>
try {
  const check = (condition, message) => { if (!condition) throw new Error(message); };
  const text = ${factory}();
  const target = document.getElementById('target');
  const child = target.querySelector('strong');
  const original = target.innerHTML;
  const inline = target.getAttribute('style');
  const nodes = [...target.childNodes, child.firstChild];
  let clicks = 0;
  child.addEventListener('click', () => { ++clicks; });
  const styles = document.createElement('style');
  styles.textContent = '#target {color:rgb(0,0,255) !important;opacity:0.5}';
  document.head.append(styles);
  check(!text.preview(target, 'target', 'Hi world!').error, 'preview rejected');
  check(target.innerText === 'Hi world!', 'visible text');
  check(child.textContent === 'world', 'unchanged nested text');
  check(getComputedStyle(target).color === 'rgb(0, 0, 255)', 'style and text compose');
  check(text.save().differences[0].old_value === 'Hello world!', 'actual baseline');
  const saved = target.innerHTML;
  text.preview(target, 'target', 'Draft world!');
  styles.disabled = true;
  text.setOriginal(true);
  check(target.innerHTML === original && getComputedStyle(target).color === 'rgb(255, 0, 0)', 'exact original and styles');
  styles.disabled = false;
  text.setOriginal(false);
  check(target.innerText === 'Draft world!' && getComputedStyle(target).opacity === '0.5', 'draft comparison roundtrip');
  text.cancel();
  check(target.innerHTML === saved, 'cancel retained saved layer');
  text.preview(target, 'target', 'Hi world!');
  check(text.differences().length === 0, 'no pseudo-difference');
  text.preview(target, 'target', '');
  check(target.innerText === '', 'empty edit');
  text.cancel();
  check(target.innerHTML === saved, 'empty edit cancellation');
  const empty = document.getElementById('empty');
  const emptyOriginal = empty.innerHTML;
  const emptyChild = empty.firstChild;
  text.preview(empty, 'empty', 'Inserted');
  text.setOriginal(true);
  check(empty.innerHTML === emptyOriginal, 'owned node removed for original');
  text.setOriginal(false);
  check(empty.innerText === 'Inserted', 'owned node restored');
  text.cancel();
  check(empty.innerHTML === emptyOriginal && empty.firstChild === emptyChild, 'owned node cleanup');
  text.dispose();
  check(target.innerHTML === original && target.getAttribute('style') === inline, 'exact disposal');
  check([...target.childNodes, child.firstChild].every((node, index) => node === nodes[index]), 'all node identities');
  child.click();
  check(clicks === 1, 'nested event listener retained');
  document.getElementById('result').textContent = 'PASS: real DOM text identity, listeners, layers, actual diffs and independent CSS';
} catch (error) {
  document.getElementById('result').textContent = 'FAIL: ' + error.stack;
}
</script>`, 'utf8');
  const result = spawnSync(binary, ['--headless=new', '--disable-gpu', '--no-first-run', '--no-default-browser-check',
    `--user-data-dir=${path.join(directory, 'profile')}`, '--dump-dom', pathToFileURL(fixture).href],
  {encoding:'utf8', timeout:45000, windowsHide:true, maxBuffer:2 * 1024 * 1024});
  assert.ifError(result.error);
  assert.equal(result.status, 0, result.stderr);
  const outcome = result.stdout.match(/<pre id="result">([\s\S]*?)<\/pre>/)?.[1];
  assert.ok(outcome?.startsWith('PASS:'), outcome || result.stdout || result.stderr);
  console.log(outcome);
} finally {
  const relative = path.relative(fs.realpathSync(os.tmpdir()), fs.realpathSync(directory));
  assert.ok(relative && !relative.startsWith('..') && !path.isAbsolute(relative) && path.basename(directory).startsWith('ace-annotation-text-'));
  fs.rmSync(directory, {recursive:true, force:true, maxRetries:10, retryDelay:200});
}
