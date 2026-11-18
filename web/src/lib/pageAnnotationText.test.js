import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';
import { DOMParser } from '@xmldom/xmldom';

const header = name => fs.readFileSync(new URL(`../../../src/apps/desktop/${name}.hpp`, import.meta.url), 'utf8');
const textRuntime = header('page_annotation_text_runtime').split('R"ACETEXT(')[1].split(')ACETEXT"')[0];
const runtime = header('page_annotation_runtime').split('R"ACEANNOTATION(')[1].split(')ACEANNOTATION"')[0];

function fixture(markup = '<button id="target">Hello <strong>world</strong>!<span hidden="">secret</span></button>') {
  const document = new DOMParser().parseFromString(`<html><body>${markup}</body></html>`, 'text/html');
  const listeners = new WeakMap();
  const contains = function contains(node) {
    for (let current = node; current; current = current.parentNode) if (current === this) return true;
    return false;
  };
  const add = function add(type, callback) {
    if (!listeners.has(this)) listeners.set(this, new Map());
    listeners.get(this).set(type, callback);
  };
  const remove = function remove(type) { listeners.get(this)?.delete(type); };
  const decorate = node => {
    node.remove = function removeNode() { this.parentNode?.removeChild(this); };
    if (node.nodeType === 3) Object.defineProperty(node, 'data', {
      get() { return this.nodeValue; }, set(value) { this.nodeValue = value; this.length = value.length; },
    });
    if (node.nodeType !== 1) return node;
    Object.defineProperty(node, 'isConnected', {get() { return document.documentElement.contains(this); }});
    node.style = {cssText:''};
    node.contains = contains;
    node.append = function append(...nodes) { nodes.forEach(child => this.appendChild(child)); };
    node.attachShadow = function attachShadow() { return (this.shadow = document.createElement('shadow')); };
    node.getBoundingClientRect = () => ({x:10, y:20, width:100, height:40});
    node.addEventListener = add;
    node.removeEventListener = remove;
    Array.from(node.childNodes).forEach(decorate);
    return node;
  };
  decorate(document.documentElement);
  const createElement = document.createElement.bind(document);
  const createTextNode = document.createTextNode.bind(document);
  document.createElement = tag => decorate(createElement(tag));
  document.createTextNode = value => decorate(createTextNode(value));
  document.addEventListener = add;
  document.removeEventListener = remove;
  const context = vm.createContext({
    document, innerWidth:800, innerHeight:600,
    getComputedStyle: element => ({display:element.hasAttribute('hidden') ? 'none' : 'block', visibility:'visible'}),
    addEventListener() {}, removeEventListener() {},
  });
  const target = document.getElementById('target');
  const text = vm.runInContext(`${textRuntime}()`, context);
  const run = (action, fields = {}) => JSON.parse(JSON.stringify(vm.runInContext(
    `${runtime}(${JSON.stringify({action, document_id:'doc', ...fields})}, ${textRuntime})`, context,
  )));
  const click = node => listeners.get(node === target ? document : node).get('click')({
    isTrusted:true, button:0, composedPath:() => [node], preventDefault() {}, stopImmediatePropagation() {}, stopPropagation() {},
  });
  const select = () => {
    click(target);
    const host = document.documentElement.lastChild;
    click(host.shadow.childNodes[1]);
    return run('read').target.id;
  };
  return {document, target, text, run, select, listeners};
}

{
  const {target, text} = fixture();
  const nodes = [...Array.from(target.childNodes), target.childNodes[1].firstChild];
  const original = target.toString();
  const child = target.childNodes[1];
  let clicks = 0;
  const listener = () => { ++clicks; };
  child.addEventListener('click', listener);
  assert.equal(text.describe(target, 'one').value, 'Hello world!');
  text.preview(target, 'one', 'Hi world!');
  assert.equal(child.textContent, 'world');
  assert.equal(target.lastChild.textContent, 'secret');
  assert.equal(text.differences()[0].old_value, 'Hello world!');
  text.setOriginal(true);
  assert.equal(target.toString(), original);
  text.setOriginal(false);
  assert.equal(target.firstChild.data, 'Hi ');
  text.cancel();
  assert.equal(target.toString(), original);
  assert.deepEqual([...Array.from(target.childNodes), child.firstChild], nodes);
  listener();
  assert.equal(clicks, 1);
  text.preview(target, 'one', 'Hello world!');
  assert.equal(text.differences().length, 0);
  console.log('[pass] reversible text preserves exact nested nodes, hidden content, and unchanged child text');
}

{
  const {target, text} = fixture();
  text.preview(target, 'one', 'Saved world!');
  const first = JSON.stringify(text.save().differences);
  text.preview(target, 'one', 'Draft world!');
  assert.equal(text.differences()[0].old_value, 'Saved world!');
  text.cancel();
  assert.equal(target.firstChild.data, 'Saved ');
  text.preview(target, 'one', 'Saved world!');
  assert.equal(text.save().differences.length, 0);
  assert.equal(JSON.parse(first)[0].new_value, 'Saved world!');
  text.setOriginal(true);
  assert.equal(target.firstChild.data, 'Hello ');
  text.setOriginal(false);
  assert.equal(target.firstChild.data, 'Saved ');
  text.dispose();
  assert.equal(target.firstChild.data, 'Hello ');
  console.log('[pass] text saved/draft/original layers retain earlier saved previews and actual differences');
}

{
  const {target, text} = fixture();
  const original = target.toString();
  const child = target.childNodes[1];
  for (const value of ['', 'Prefix Hello world!', 'Hello world! suffix', 'Replacement', 'Hello world!', 'Hi \u{1f30d}!']) {
    assert.equal(text.preview(target, 'one', value).error, undefined);
    assert.equal(target.firstChild.data + child.textContent + target.childNodes[2].data, value);
    assert.equal(target.childNodes[1], child);
    text.setOriginal(true);
    assert.equal(target.toString(), original);
    text.setOriginal(false);
    text.cancel();
    assert.equal(target.toString(), original);
  }
  text.preview(target, 'one', 'Saved world!');
  text.save();
  text.setOriginal(true);
  text.preview(target, 'one', 'Hello world!');
  assert.equal(target.toString(), original);
  assert.equal(text.differences()[0].old_value, 'Saved world!');
  text.save();
  text.setOriginal(false);
  assert.equal(target.toString(), original);
  console.log('[pass] insertion/deletion/Unicode and edits during comparison retain exact originals and saved baselines');
}

{
  const {target, text} = fixture('<button id="target"><strong></strong></button>');
  const original = target.toString();
  const child = target.firstChild;
  text.preview(target, 'empty', 'New text');
  assert.equal(target.lastChild.data, 'New text');
  text.setOriginal(true);
  assert.equal(target.toString(), original);
  text.setOriginal(false);
  assert.equal(target.lastChild.data, 'New text');
  assert.equal(text.save().error, undefined);
  text.preview(target, 'empty', '');
  assert.equal(target.textContent, '');
  text.cancel();
  assert.equal(target.textContent, 'New text');
  text.dispose();
  assert.equal(target.toString(), original);
  assert.equal(target.firstChild, child);
  console.log('[pass] empty targets use a reversible owned text node without disturbing children');
}

{
  const {target, text, document} = fixture();
  text.preview(target, 'one', 'Saved world!');
  target.childNodes[1].append(document.createTextNode('external'));
  assert.equal(text.preview(target, 'one', 'Other').error, 'stale_text_nodes');
  assert.equal(text.save().error, 'stale_text_nodes');
  assert.equal(text.describe(target, 'one').editable, false);
  text.cancel();
  assert.equal(text.describe(target, 'one').editable, true);
  const large = fixture(`<div id="target">${'x'.repeat(4097)}</div>`);
  assert.equal(large.text.describe(large.target, 'large').editable, false);
  assert.equal(large.text.preview(large.target, 'large', 'Small').error, 'invalid_text');
  console.log('[pass] changed node topology and over-limit text fail explicitly without truncation');
}

{
  const {target, run, select, listeners} = fixture();
  const child = target.childNodes[1];
  let clicked = 0;
  child.addEventListener('click', () => { ++clicked; });
  const original = target.toString();
  run('start');
  const id = select();
  assert.equal(run('preview_text', {target_id:'wrong', text:'Bad'}).error, 'stale_target');
  assert.equal(run('preview_text', {target_id:id, text:'Saved world!'}).target.text_preview.value, 'Saved world!');
  const saved = run('save', {target_id:id, number:1});
  assert.deepEqual(saved.differences, [{target_id:id, property:'text', old_value:'Hello world!', new_value:'Saved world!'}]);
  select();
  run('preview_text', {target_id:id, text:'Draft world!'});
  run('original', {enabled:true});
  assert.equal(target.toString(), original);
  run('original', {enabled:false});
  run('cancel');
  assert.equal(target.firstChild.data, 'Saved ');
  const pin = target.ownerDocument.documentElement.lastChild.shadow.childNodes[2];
  assert.equal(pin.textContent, '1');
  select();
  run('preview_text', {target_id:id, text:'Unsaved world!'});
  run('exit');
  assert.equal(target.firstChild.data, 'Saved ');
  listeners.get(child).get('click')();
  assert.equal(clicked, 1);
  console.log('[pass] integrated runtime edits/saves/cancels text, retains pins/listeners and survives mode exit');
}
