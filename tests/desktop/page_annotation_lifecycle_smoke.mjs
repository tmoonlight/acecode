import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { pathToFileURL } from 'node:url';

const binary = process.argv[2];
assert.ok(binary, 'Usage: node tests/desktop/page_annotation_lifecycle_smoke.mjs <Chromium executable>');
const factory = (file, delimiter) => fs.readFileSync(new URL(`../../src/apps/desktop/${file}`, import.meta.url), 'utf8')
  .split(`R"${delimiter}(`)[1].split(`)${delimiter}"`)[0];
const runtime = factory('page_annotation_runtime.hpp', 'ACEANNOTATION');
const text = factory('page_annotation_text_runtime.hpp', 'ACETEXT');
const styles = factory('page_annotation_style_runtime.hpp', 'ACESTYLES');
const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'ace-annotation-lifecycle-'));
try {
  const fixture = path.join(directory, 'fixture.html');
  fs.writeFileSync(fixture, `<!doctype html><meta charset="utf-8">
<div id="ancestor"><button id="first" style="color:rgb(255,0,0)">Original</button></div>
<button id="second">Second</button><div id="shadow-host"></div><div style="height:2000px"></div>
<pre id="result">NOT RUN</pre><script>
try {
  const check = (condition, message) => { if (!condition) throw new Error(message); };
  const listeners = new WeakMap();
  Element.prototype.setPointerCapture = () => {};
  const originalAdd = EventTarget.prototype.addEventListener;
  EventTarget.prototype.addEventListener = function(type, listener, options) {
    if (!listeners.has(this)) listeners.set(this, new Map());
    listeners.get(this).set(type, listener);
    return originalAdd.call(this, type, listener, options);
  };
  const roots = [];
  const originalAttach = Element.prototype.attachShadow;
  Element.prototype.attachShadow = function(options) {
    const root = originalAttach.call(this, options);
    roots.push(root);
    return root;
  };
  const run = command => (${runtime})({document_id:'document-1', ...command}, ${text}, ${styles});
  run({action:'start'});
  const annotationRoot = roots.find(root => root.querySelector('button')?.textContent === 'Annotate');
  const event = target => ({isTrusted:true, button:0, composedPath:() => [target],
    preventDefault() {}, stopPropagation() {}, stopImmediatePropagation() {}});
  const select = target => listeners.get(document).get('click')(event(target));
  const open = () => {
    const button = annotationRoot.querySelector('button');
    listeners.get(button).get('click')(event(button));
  };
  const save = number => {
    const snapshot = run({action:'read'});
    const result = run({action:'save', number, target_id:snapshot.target?.id || null,
      target_ids:snapshot.targets.map(target => target.id), region:snapshot.region});
    check(!result.error, 'save: ' + result.error);
  };
  const first = document.getElementById('first');
  const second = document.getElementById('second');
  select(first); open();
  const firstId = run({action:'read'}).target.id;
  check(!run({action:'preview_text',target_id:firstId,text:'Edited'}).error, 'text preview');
  check(!run({action:'preview_style',target_id:firstId,styles:{color:'rgb(0,0,255)'}}).error, 'style preview');
  run({action:'add_target'}); select(second);
  const ids = run({action:'read'}).targets.map(target => target.id);
  run({action:'add_region'});
  for (const [type,clientX,clientY] of [['pointerdown',10,20],['pointerup',60,80]]) {
    listeners.get(document).get(type)({...event(second),pointerId:1,clientX,clientY});
  }
  save(1);
  run({action:'original',enabled:true});
  run({action:'original',enabled:false});
  run({action:'exit'});
  const ancestor = document.getElementById('ancestor');
  ancestor.remove(); document.body.prepend(ancestor);
  const validation = run({action:'validate_batch',target_ids:ids,record_numbers:[1]});
  check(!validation.valid && validation.invalid_numbers.includes(1), 'queued ancestor removal and reinsertion');
  check(first.textContent === 'Original', 'invalid text restored');
  check(getComputedStyle(first).color === 'rgb(255, 0, 0)', 'invalid style restored');
  check(annotationRoot.querySelector('span').style.display === 'none', 'whole multi-target pin hidden');
  const replacement = first.cloneNode(true);
  first.replaceWith(replacement);
  run({action:'start'}); select(replacement); open();
  check(run({action:'read'}).target.id !== firstId, 'similar element not relocated');
  run({action:'cancel'});
  const shadowHost = document.getElementById('shadow-host');
  const shadow = shadowHost.attachShadow({mode:'open'});
  shadow.innerHTML = '<div><button>Shadow</button></div>';
  const shadowTarget = shadow.querySelector('button');
  select(shadowTarget); open(); save(2);
  const parent = shadowTarget.parentNode;
  parent.remove(); shadow.append(parent);
  check(run({action:'read'}).invalid_numbers.includes(2), 'shadow ancestor removal and reinsertion');
  run({action:'start'}); select(second); open();
  run({action:'add_region'});
  for (const [type,clientX,clientY] of [['pointerdown',30,40],['pointerup',80,90]]) {
    listeners.get(document).get(type)({...event(second),pointerId:2,clientX,clientY});
  }
  save(3);
  scrollTo(0,300);
  run({action:'exit'});
  check(!run({action:'read'}).invalid_numbers.includes(3), 'scroll and mode exit keep valid region');
  const replacementRoot = document.documentElement.cloneNode(true);
  document.replaceChild(replacementRoot, document.documentElement);
  check(run({action:'read'}).error === 'replaced_document', 'root replacement rejected');
  document.getElementById('result').textContent = 'PASS: exact DOM/shadow identity, queued removal, multi-target previews, regions and root replacement';
} catch (error) {
  document.getElementById('result').textContent = 'FAIL: ' + error.stack;
}
</script>`, 'utf8');
  const result = spawnSync(binary, ['--headless=new', '--disable-gpu', '--no-first-run', '--no-default-browser-check',
    `--user-data-dir=${path.join(directory, 'profile')}`, '--dump-dom', pathToFileURL(fixture).href],
  { encoding:'utf8', timeout:45000, windowsHide:true, maxBuffer:2 * 1024 * 1024 });
  assert.ifError(result.error);
  assert.equal(result.status, 0, result.stderr);
  const outcome = result.stdout.match(/<pre id="result">([\s\S]*?)<\/pre>/)?.[1];
  assert.ok(outcome?.startsWith('PASS:'), outcome || result.stdout || result.stderr);
  console.log(outcome);
} finally {
  const relative = path.relative(fs.realpathSync(os.tmpdir()), fs.realpathSync(directory));
  assert.ok(relative && !relative.startsWith('..') && !path.isAbsolute(relative) && path.basename(directory).startsWith('ace-annotation-lifecycle-'));
  fs.rmSync(directory, { recursive:true, force:true, maxRetries:10, retryDelay:200 });
}
