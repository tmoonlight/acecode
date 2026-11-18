import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';
import React from 'react';
import { transformWithEsbuild } from 'vite';
import { createPageAnnotationController, pageAnnotationPopupPosition, runPageAnnotationCommand } from './pageAnnotations.js';

const source = fs.readFileSync(new URL('../../../src/apps/desktop/page_annotation_runtime.hpp', import.meta.url), 'utf8');
const runtime = source.split('R"ACEANNOTATION(')[1].split(')ACEANNOTATION"')[0];
const textSource = fs.readFileSync(new URL('../../../src/apps/desktop/page_annotation_text_runtime.hpp', import.meta.url), 'utf8');
const textRuntime = textSource.split('R"ACETEXT(')[1].split(')ACETEXT"')[0];

function fixture() {
  const documentListeners = new Map();
  const windowListeners = new Map();
  const observers = [];
  class Observer {
    constructor(callback) { this.callback = callback; this.active = true; observers.push(this); }
    observe() {}
    disconnect() { this.active = false; }
  }
  let document;
  const element = (tag) => ({
    nodeType: 1, localName: tag, ownerDocument: document, isConnected: true,
    style: { cssText: '' }, children: [], listeners: new Map(), id: '', textContent: '',
    get childNodes() { return this.children; },
    offsetWidth: 80, offsetHeight: 24,
    bounds: { x: 10, y: 20, width: 100, height: 40 },
    append(...children) { children.forEach(child => { child.parent = this; }); this.children.push(...children); },
    remove() { this.parent.children = this.parent.children.filter(child => child !== this); },
    attachShadow() { this.shadow = element('shadow'); return this.shadow; },
    setAttribute() {},
    addEventListener(type, listener) { this.listeners.set(type, listener); },
    getBoundingClientRect() { return this.bounds; },
  });
  document = {
    createElement: element,
    addEventListener(type, listener) { documentListeners.set(type, listener); },
    removeEventListener(type) { documentListeners.delete(type); },
  };
  document.documentElement = element('html');
  const target = element('button');
  target.id = 'target';
  target.textContent = 'Original content';
  target.style.cssText = 'color: red';
  const child = element('strong');
  target.append(child);
  const context = vm.createContext({
    document, innerWidth: 800, innerHeight: 600, scrollX: 0, scrollY: 0,
    getComputedStyle: () => ({ display: 'block', visibility: 'visible' }),
    MutationObserver: Observer, ResizeObserver: Observer,
    addEventListener(type, listener) { windowListeners.set(type, listener); },
    removeEventListener(type) { windowListeners.delete(type); },
  });
  const run = (action, fields = {}) => {
    const current = action === 'save' ? run('read') : null;
    return JSON.parse(JSON.stringify(vm.runInContext(
      `${runtime}(${JSON.stringify({ action, document_id: 'doc-1',
        ...(current ? { target_ids: current.targets.map(item => item.id), region: current.region } : {}),
        ...fields })}, ${textRuntime})`, context,
    )));
  };
  const event = (node, trusted = true, fields = {}) => ({
    isTrusted: trusted, button: 0, composedPath: () => [node],
    preventDefault() { this.prevented = true; }, stopImmediatePropagation() {}, stopPropagation() {}, ...fields,
  });
  const click = (trusted = true) => {
    const input = event(target, trusted);
    documentListeners.get('click')(input);
    return input;
  };
  const control = (trusted = true) => {
    const button = document.documentElement.children[0].shadow.children[1];
    button.listeners.get('click')(event(button, trusted));
  };
  const choose = node => documentListeners.get('click')(event(node));
  const pointer = (type, clientX, clientY, fields = {}) => documentListeners.get(type)(
    event(target, true, { clientX, clientY, pointerId: 1, ...fields }),
  );
  return { document, target, child, context, run, click, choose, pointer, element, control, event, documentListeners, windowListeners, observers };
}

{
  const page = fixture();
  assert.equal(page.run('read').error, 'stale_document');
  assert.equal(page.run('start').active, true);
  assert.equal(page.click(false).prevented, undefined);
  assert.equal(page.run('read').target, null);
  assert.equal(page.click().prevented, true);
  const selected = page.run('read');
  assert.equal(selected.target.id, 'target-1');
  assert.equal(selected.popup, false);
  page.control(false);
  assert.equal(page.run('read').popup, false);
  page.control();
  assert.equal(page.run('read').popup, true);
  page.click();
  assert.equal(page.run('read').popup, true);
  assert.equal(page.run('save', { target_id: 'wrong', number: 1 }).error, 'stale_target');
  const saved = page.run('save', { target_id: 'target-1', number: 1 });
  assert.equal(saved.target, null);
  const shadow = page.document.documentElement.children[0].shadow;
  assert.equal(shadow.children[3].textContent, '1');
  page.run('exit');
  assert.equal(shadow.children[0].style.display, 'none');
  assert.equal(shadow.children[1].style.display, 'none');
  assert.equal(shadow.children[3].style.display, 'block');
  page.target.bounds = { x: 150, y: 250, width: 100, height: 40 };
  page.observers[0].callback();
  assert.equal(shadow.children[3].style.left, '150px');
  assert.equal(shadow.children[3].style.top, '250px');
  assert.equal(page.target.style.cssText, 'color: red');
  assert.equal(page.target.textContent, 'Original content');
  assert.equal(page.target.children[0], page.child);
  page.run('start');
  page.click();
  page.control();
  page.run('cancel');
  assert.equal(page.run('read').popup, false);
  assert.equal(shadow.children.length, 4);
  page.click();
  assert.equal(page.run('read').target.id, 'target-1');
  page.documentListeners.get('keydown')(page.event(page.target, true, { key: 'Escape' }));
  assert.equal(page.run('read').active, false);
  assert.equal(shadow.children[3].style.display, 'block');
  const pageWorld = vm.createContext({ document: page.document });
  assert.equal(vm.runInContext('typeof __acecodePageAnnotationsV1', pageWorld), 'undefined');
  assert.equal(page.run('read', { document_id: 'doc-2' }).error, 'stale_document');
  assert.equal(page.run('send').error, 'unsupported_action');
  assert.equal(page.run('start', { document_id: 'doc-2' }).active, true);
  assert.equal(page.observers[0].active, false);
  assert.equal(page.observers[1].active, false);
  assert.equal(page.document.documentElement.children.length, 1);
  assert.equal(page.document.documentElement.children[0].shadow.children.length, 3);
  assert.equal(page.run('read').error, 'stale_document');
  console.log('[pass] annotation runtime selects, opens, cancels, saves immutable pins and exits without changing content');
}

{
  const page = fixture();
  page.run('start'); page.click(); page.control();
  const second = page.element('section');
  second.bounds = { x: 200, y: 300, width: 200, height: 100 };
  page.run('add_target');
  assert.equal(page.run('read').popup, true);
  assert.equal(page.run('read').selection_mode, 'element');
  page.choose(second);
  assert.equal(page.run('read').target.id, 'target-2');
  assert.equal(page.run('read').targets.length, 2);
  page.run('add_target'); page.choose(page.target);
  assert.equal(page.run('read').targets.length, 2);
  assert.equal(page.run('read').selection_mode, null);
  assert.equal(page.run('select_target', { target_id: 'target-2' }).target.tag, 'section');
  assert.equal(page.run('select_target', { target_id: 'absent' }).error, 'stale_target');
  page.run('add_region');
  page.context.scrollX = 40; page.context.scrollY = 500;
  page.pointer('pointerdown', 80, 90);
  page.pointer('pointermove', 20, 30);
  page.pointer('pointerup', 20, 30);
  const first = page.run('read');
  assert.deepEqual(first.region, { x: 60, y: 530, width: 60, height: 60 });
  assert.equal(first.target, null);
  assert.equal(first.targets.length, 2);
  const shadow = page.document.documentElement.children[0].shadow;
  assert.equal(shadow.children[2].style.top, '30px');
  page.context.scrollY = 520;
  page.windowListeners.get('scroll')();
  assert.equal(shadow.children[2].style.top, '10px');
  assert.deepEqual(page.run('read').region, first.region);
  page.run('add_region');
  page.pointer('pointerdown', 10, 15);
  page.context.scrollY = 550;
  page.pointer('pointerup', 90, 80);
  const replaced = page.run('read');
  assert.deepEqual(replaced.region, { x: 50, y: 535, width: 80, height: 95 });
  assert.equal(replaced.targets.length, 2);
  assert.equal(replaced.target, null);
  page.run('select_target', { target_id: 'target-1' });
  assert.equal(page.run('read').target.id, 'target-1');
  page.run('select_region');
  assert.equal(page.run('read').target, null);
  assert.equal(page.run('save', { target_id: null, target_ids: ['target-2'], number: 1 }).error, 'stale_target');
  page.run('save', { target_id: null, number: 1, region: {
    height: replaced.region.height, width: replaced.region.width, x: replaced.region.x, y: replaced.region.y,
  } });
  assert.equal(shadow.children.filter(node => node.textContent === '1').length, 1);
  assert.equal(page.run('read').targets.length, 0);
  assert.equal(page.run('read').region, null);
  page.run('exit');
  page.context.scrollY = 600;
  page.windowListeners.get('scroll')();
  assert.equal(shadow.children[3].style.top, '-65px');
  assert.equal(shadow.children[4].style.top, '-65px');
  page.run('start'); page.click(); page.control();
  page.run('add_target'); page.choose(second);
  page.run('add_region'); page.pointer('pointerdown', 10, 10); page.pointer('pointerup', 30, 30);
  page.run('cancel');
  assert.deepEqual(page.run('read').targets, []);
  assert.equal(page.run('read').region, null);
  assert.equal(shadow.children.length, 5);
  console.log('[pass] multiple exact targets deduplicate/switch, one page region replaces across scrolling, one pin saves and cancellation clears the whole draft');
}

{
  const page = fixture();
  page.run('start'); page.click(); page.control();
  page.run('add_region');
  page.pointer('pointerdown', 10, 10, { isTrusted: false });
  page.pointer('pointerup', 100, 100);
  assert.equal(page.run('read').region, null);
  page.pointer('pointerdown', 10, 10);
  page.pointer('pointerup', 20, 20, { pointerId: 2 });
  assert.equal(page.run('read').region, null);
  page.pointer('pointercancel', 20, 20);
  page.documentListeners.get('keydown')(page.event(page.target, true, { key: 'Escape' }));
  assert.equal(page.run('read').popup, true);
  assert.equal(page.run('read').selection_mode, null);
  assert.equal(page.run('read').targets.length, 1);
  page.run('add_region'); page.pointer('pointerdown', 10, 10); page.pointer('pointerup', 10, 10);
  assert.equal(page.run('read').region, null);
  page.run('cancel');
  console.log('[pass] synthetic/foreign/cancelled/zero-area drags cannot replace a region; Escape returns to the same draft');
}

{
  const page = fixture();
  page.run('start');
  page.click();
  page.control();
  page.target.isConnected = false;
  assert.equal(page.run('save', { target_id: 'target-1', number: 1 }).error, 'stale_target');
  assert.equal(page.run('read').popup, false);
  assert.equal(page.run('read').target, null);
  console.log('[pass] detached exact target cannot be saved or relocated');
}

{
  let release;
  const requests = [];
  const changes = [];
  const queuedRegion = { x: 20, y: 700, width: 80, height: 30 };
  const actor = createPageAnnotationController({
    pageId: 'page-1', onChange: next => changes.push(next),
    invoke: async (pageId, command) => {
      requests.push({ pageId, command });
      if (command.action === 'read') await new Promise(resolve => { release = resolve; });
      return { ok: true, page_id: pageId, annotations: { document_id: 'doc-1', revision: 2, active: true } };
    },
  });
  actor.observe({ document_id: 'doc-1', revision: 1, active: true, target: { id: 'target-1' },
    targets: [{ id: 'target-1' }, { id: 'target-2' }], region: queuedRegion });
  const read = actor.command('read');
  const save = actor.command('save', { description: 'Requested change' });
  await Promise.resolve();
  assert.equal(requests.length, 1);
  actor.observe({ document_id: 'doc-2', revision: 3, active: false });
  queuedRegion.x = 999;
  release();
  await read;
  await save;
  assert.equal(requests[1].command.target_id, 'target-1');
  assert.equal(requests[1].command.document_id, 'doc-1');
  assert.deepEqual(requests[1].command.target_ids, ['target-1', 'target-2']);
  assert.deepEqual(requests[1].command.region, { x: 20, y: 700, width: 80, height: 30 });
  assert.equal(actor.getSnapshot().document_id, 'doc-2');
  const count = changes.length;
  actor.dispose();
  assert.equal((await actor.command('start')).cancelled, true);
  actor.observe({ revision: 4 });
  assert.equal(changes.length, count);
  console.log('[pass] trusted controller serializes commands, binds save identity and rejects late snapshots after replacement/disposal');
}

{
  const calls = [];
  const result = await runPageAnnotationCommand('local-without-session', { action: 'start', document_id: '' }, {
    aceDesktop_agentBrowserAnnotationCommand: async request => { calls.push(request); return '{"ok":true}'; },
  });
  assert.equal(result.ok, true);
  assert.deepEqual(calls, [{ page_id: 'local-without-session', command: { action: 'start', document_id: '' } }]);
  assert.equal((await runPageAnnotationCommand('page-1', {}, {})).ok, false);
  const position = pageAnnotationPopupPosition({
    width: 400, height: 300, popupWidth: 200, popupHeight: 150,
    target: { bounds: { x: 700, y: 500, height: 50 } }, viewport: { width: 800, height: 600 },
  });
  assert.deepEqual(position, { left: 192, top: 92 });
  assert.deepEqual(pageAnnotationPopupPosition({ width: 120, height: 80, popupWidth: 104, popupHeight: 64 }), { left: 8, top: 8 });
  console.log('[pass] annotation bridge is session-independent and popup placement accounts for viewport zoom and edges');
}

{
  const hooks = [];
  let cursor = 0;
  const calls = [];
  const targetSource = fs.readFileSync(new URL('../components/PageAnnotationTargets.jsx', import.meta.url), 'utf8');
  const popupSource = fs.readFileSync(new URL('../components/PageAnnotations.jsx', import.meta.url), 'utf8');
  const compiled = await transformWithEsbuild(
    `${targetSource}\n${popupSource}`.replace(/^import .*;\r?$/gm, '').replace(/^export /gm, ''),
    'PageAnnotations.jsx', { loader: 'jsx', jsxFactory: 'React.createElement' },
  );
  const popup = vm.runInNewContext(`${compiled.code}\nPageAnnotationPopup`, {
    React,
    PageAnnotationTextField: () => null,
    useRef: () => ({ current: null }), useEffect: () => {}, useLayoutEffect: () => {},
    useState: initial => {
      const index = cursor++;
      if (!(index in hooks)) hooks[index] = initial;
      return [hooks[index], next => { hooks[index] = next; }];
    },
  });
  const snapshot = { document_id: 'doc-1', popup: true, target: { id: 'target-1', tag: 'button' },
    targets: [{ id: 'target-1', tag: 'button', locator: 'button#first' }], region: null };
  const render = () => {
    cursor = 0;
    return popup({ snapshot, command: (...args) => calls.push(args), working: false, viewportRef: { current: null } });
  };
  const nodes = node => {
    if (!node || typeof node !== 'object') return [];
    if (typeof node.type === 'function') return nodes(node.type(node.props));
    return [node, ...React.Children.toArray(node.props?.children).flatMap(nodes)];
  };
  const textarea = tree => nodes(tree).find(node => node.type === 'textarea');
  textarea(render()).props.onChange({ target: { value: 'Shared request for every target' } });
  let tree = render();
  nodes(tree).find(node => node.type === 'button' && node.props.children === '添加元素').props.onClick();
  assert.equal(calls.at(-1)[0], 'add_target');
  snapshot.selection_mode = 'element';
  tree = render();
  assert.equal(tree.props.style.display, 'none');
  assert.equal(tree.props['data-ace-native-overlay'], undefined);
  snapshot.targets.push({ id: 'target-2', tag: 'section', locator: 'section' });
  snapshot.target = snapshot.targets[1];
  snapshot.selection_mode = null;
  tree = render();
  assert.equal(textarea(tree).props.value, 'Shared request for every target');
  assert.equal(tree.props['data-ace-native-overlay'], 'overlap');
  nodes(tree).find(node => node.props['aria-pressed'] === false).props.onClick();
  assert.equal(calls.at(-1)[0], 'select_target');
  snapshot.region = { x: 0, y: 500, width: 40, height: 30 };
  snapshot.target = null;
  tree = render();
  assert.equal(nodes(tree).filter(node => node.type === 'textarea').length, 1);
  assert.equal(nodes(tree).filter(node => node.type === 'input').length, 0);
  tree.props.onSubmit({ preventDefault() {} });
  assert.equal(calls.at(-1)[0], 'save');
  assert.equal(calls.at(-1)[1].description, 'Shared request for every target');
  const panel = fs.readFileSync(new URL('../components/AgentBrowserPanel.jsx', import.meta.url), 'utf8');
  assert.match(panel, /key=\{`\$\{pageId\}:\$\{annotations.snapshot.document_id\}`\}/);
  console.log('[pass] trusted popup preserves shared description while hidden for selection and when switching element/region targets');
}

{
  const panel = fs.readFileSync(new URL('../components/AgentBrowserPanel.jsx', import.meta.url), 'utf8');
  const popup = fs.readFileSync(new URL('../components/PageAnnotations.jsx', import.meta.url), 'utf8');
  assert.match(panel, /createAgentBrowserElementContext\(selectedElement, event.detail\)/);
  assert.match(panel, /toggleAgentBrowserElementSelection\(pageId\)/);
  assert.match(popup, /data-ace-native-overlay=\{snapshot.selection_mode \? undefined : 'overlap'\}/);
  assert.doesNotMatch(popup, /onAddContext|submitMessage|fetch\(|aceDesktop_.*(?:Write|Send)/);
  const windows = fs.readFileSync(new URL('../../../src/apps/desktop/agent_browser_host.cpp', import.meta.url), 'utf8');
  assert.match(windows, /Page.createIsolatedWorld/);
  assert.match(windows, /"contextId", world\["executionContextId"\]/);
  assert.doesNotMatch(source, /chrome\.webview|webkit\.messageHandlers|postMessage|fetch\(/);
  console.log('[pass] annotation popup uses local native occlusion and leaves source-selection/conversation path independent');
}
