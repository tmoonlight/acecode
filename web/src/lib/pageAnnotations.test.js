import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';
import React from 'react';
import { transformWithEsbuild } from 'vite';
import {
  createPageAnnotationController,
  pageAnnotationPopupDragPosition,
  pageAnnotationPopupPosition,
  runPageAnnotationCommand,
} from './pageAnnotations.js';

const source = fs.readFileSync(new URL('../../../src/apps/desktop/page_annotation_runtime.hpp', import.meta.url), 'utf8');
const runtime = source.split('R"ACEANNOTATION(').slice(1)
  .map(part => part.split(')ACEANNOTATION"')[0]).join('');
const textSource = fs.readFileSync(new URL('../../../src/apps/desktop/page_annotation_text_runtime.hpp', import.meta.url), 'utf8');
const textRuntime = textSource.split('R"ACETEXT(')[1].split(')ACETEXT"')[0];
const styleSource = fs.readFileSync(new URL('../../../src/apps/desktop/page_annotation_style_runtime.hpp', import.meta.url), 'utf8');
const styleRuntime = styleSource.split('R"ACESTYLES(')[1].split(')ACESTYLES"')[0];

function fixture() {
  const documentListeners = new Map();
  const windowListeners = new Map();
  const observers = [];
  class Observer {
    constructor(callback) { this.callback = callback; this.active = true; this.records = []; observers.push(this); }
    observe() {}
    disconnect() { this.active = false; }
    takeRecords() { return this.records.splice(0); }
  }
  let document;
  const element = (tag) => ({
    nodeType: 1, localName: tag, ownerDocument: document, isConnected: true,
    style: { cssText: '', getPropertyValue: () => '', getPropertyPriority: () => '' },
    attributes: new Map(), children: [], listeners: new Map(), id: '', textContent: '',
    get childNodes() { return this.children; },
    offsetWidth: 80, offsetHeight: 24,
    bounds: { x: 10, y: 20, width: 100, height: 40 },
    append(...children) { children.forEach(child => { child.parent = this; }); this.children.push(...children); },
    replaceChildren(...children) { this.children = children; children.forEach(child => { child.parent = this; }); },
    remove() { this.parent.children = this.parent.children.filter(child => child !== this); },
    attachShadow() { this.shadow = element('shadow'); return this.shadow; },
    setAttribute(name, value) { this.attributes.set(name, value); },
    getAttribute(name) { return this.attributes.get(name) ?? null; },
    removeAttribute(name) { this.attributes.delete(name); },
    getRootNode() { return document; },
    addEventListener(type, listener) { this.listeners.set(type, listener); },
    getBoundingClientRect() { return this.bounds; },
  });
  document = {
    adoptedStyleSheets: [], querySelectorAll: () => [],
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
    crypto: { getRandomValues: values => values.fill(1) },
    CSS: { supports: () => true },
    CSSStyleSheet: class { replaceSync() {} },
    getComputedStyle: () => ({ display: 'block', visibility: 'visible',
      color: 'rgb(31, 35, 40)', fontSize: '14px', fontFamily: 'sans-serif',
      getPropertyValue: field => ({ width: '100px', height: '40px' })[field] || '' }),
    MutationObserver: Observer, ResizeObserver: Observer,
    addEventListener(type, listener) { windowListeners.set(type, listener); },
    removeEventListener(type) { windowListeners.delete(type); },
  });
  const run = (action, fields = {}) => {
    const current = action === 'save' ? run('read') : null;
    return JSON.parse(JSON.stringify(vm.runInContext(
      `${runtime}(${JSON.stringify({ action, document_id: 'doc-1',
        ...(current ? { target_ids: current.targets.map(item => item.id), region: current.region } : {}),
        ...fields })}, ${textRuntime}, ${styleRuntime})`, context,
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
  const choose = node => documentListeners.get('click')(event(node));
  const pointer = (type, clientX, clientY, fields = {}) => documentListeners.get(type)(
    event(target, true, { clientX, clientY, pointerId: 1, ...fields }),
  );
  return { document, target, child, context, run, click, choose, pointer, element, event, documentListeners, windowListeners, observers };
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
  assert.equal(selected.popup, true);
  page.click();
  assert.equal(page.run('read').popup, false);
  page.click();
  assert.equal(page.run('read').popup, true);
  assert.equal(page.run('save', { target_id: 'wrong', number: 1 }).error, 'stale_target');
  const saved = page.run('save', { target_id: 'target-1', number: 1 });
  assert.equal(saved.target, null);
  const shadow = page.document.documentElement.children[0].shadow;
  const pin = shadow.children.find(child => child.attributes?.get('data-ace-annotation-pin') === '1');
  assert.equal(pin.textContent, '1');
  assert.deepEqual(page.run('read').invalid_numbers, []);
  page.run('exit');
  assert.equal(shadow.children[0].style.display, 'none');
  assert.equal(shadow.children[1].style.display, 'none');
  assert.equal(pin.style.display, 'block');
  page.target.bounds = { x: 150, y: 250, width: 100, height: 40 };
  page.observers.forEach(observer => observer.callback());
  assert.equal(pin.style.left, '150px');
  assert.equal(pin.style.top, '250px');
  page.target.isConnected = false;
  // 目标掉线的那一刻就要被记为失效（read 就地 inspect），之后即使重新连上
  // 也不会复活——失效是粘性的。
  assert.deepEqual(page.run('read').invalid_numbers, [1]);
  page.target.isConnected = true;
  assert.deepEqual(page.run('read').invalid_numbers, [1]);
  assert.equal(pin.style.display, 'none');
  assert.equal(page.target.style.cssText, 'color: red');
  assert.equal(page.target.textContent, 'Original content');
  assert.equal(page.target.children[0], page.child);
  page.run('start');
  page.click();
  page.run('cancel');
  assert.equal(page.run('read').popup, false);
  assert.equal(shadow.children.length, 5);
  page.click();
  assert.equal(page.run('read').target.id, 'target-1');
  page.documentListeners.get('keydown')(page.event(page.target, true, { key: 'Escape' }));
  assert.equal(page.run('read').active, false);
  assert.equal(pin.style.display, 'none');
  const pageWorld = vm.createContext({ document: page.document });
  assert.equal(vm.runInContext('typeof __acecodePageAnnotationsV1', pageWorld), 'undefined');
  assert.equal(page.run('read', { document_id: 'doc-2' }).error, 'stale_document');
  assert.equal(page.run('send').error, 'unsupported_action');
  assert.equal(page.run('start', { document_id: 'doc-2' }).active, true);
  assert.equal(page.observers[0].active, false);
  assert.equal(page.observers[1].active, false);
  assert.equal(page.document.documentElement.children.length, 1);
  assert.equal(page.document.documentElement.children[0].shadow.children.length, 4);
  assert.equal(page.run('read').error, 'stale_document');
  console.log('[pass] annotation runtime selects, opens, cancels, saves immutable pins and exits without changing content');
}

{
  // A WebUI menu or dialog floating over the page suspends the outline and the
  // hover style card so they cannot punch through the app's own overlay.
  const page = fixture();
  page.run('start');
  assert.equal(page.run('read').modal_suspended, false);
  const shadow = page.document.documentElement.children[0].shadow;
  const [outline, hoverOutline, infoCard] = shadow.children;
  page.pointer('pointermove', 20, 30);
  assert.equal(hoverOutline.style.display, 'block', 'hovering paints the element outline');
  assert.equal(infoCard.style.display, 'block', 'hovering paints the style card');
  const suspended = page.run('set_modal_suspended', { enabled: true });
  assert.equal(suspended.modal_suspended, true);
  assert.equal(outline.style.display, 'none', 'suspension hides the selection outline');
  assert.equal(hoverOutline.style.display, 'none', 'suspension hides the hover outline');
  assert.equal(infoCard.style.display, 'none', 'suspension hides the hover style card');
  const resumed = page.run('set_modal_suspended', { enabled: false });
  assert.equal(resumed.modal_suspended, false);
  assert.equal(hoverOutline.style.display, 'none', 'resuming does not resurrect stale hover chrome');
  assert.equal(infoCard.style.display, 'none');
  page.pointer('pointermove', 20, 31);
  assert.equal(hoverOutline.style.display, 'block', 'the next hover repaints the outline after resuming');
  page.click();
  assert.equal(outline.style.display, 'block', 'selecting paints the selection outline');
  page.run('set_modal_suspended', { enabled: true });
  assert.equal(outline.style.display, 'none', 'suspension also hides a selected element outline');
  page.run('set_modal_suspended', { enabled: false });
  console.log('[pass] modal suspension hides selection/hover chrome and only repaints on the next hover');
}

{
  // The hover card belongs to the element under the pointer. Leaving the page
  // (onto the app chrome, another window, or off-screen) and losing window
  // focus both stop pointermove events, so both have to drop the card instead
  // of leaving the last element's bubble pinned over the content.
  const page = fixture();
  page.run('start');
  const shadow = page.document.documentElement.children[0].shadow;
  const [, hoverOutline, infoCard] = shadow.children;
  const pointerOut = page.documentListeners.get('pointerout');
  const blur = page.windowListeners.get('blur');
  assert.equal(typeof pointerOut, 'function', 'the runtime listens for the pointer leaving the document');
  assert.equal(typeof blur, 'function', 'the runtime listens for window focus loss');
  page.pointer('pointermove', 20, 30);
  assert.equal(infoCard.style.display, 'block');
  pointerOut(page.event(page.target, true, { relatedTarget: page.child }));
  assert.equal(infoCard.style.display, 'block', 'moving between page elements keeps the card');
  pointerOut(page.event(page.target, true, { relatedTarget: null }));
  assert.equal(hoverOutline.style.display, 'none', 'leaving the page hides the hover outline');
  assert.equal(infoCard.style.display, 'none', 'leaving the page hides the style card');
  page.pointer('pointermove', 20, 34);
  assert.equal(infoCard.style.display, 'block', 'hovering again repaints the card');
  blur();
  assert.equal(hoverOutline.style.display, 'none', 'window blur hides the hover outline');
  assert.equal(infoCard.style.display, 'none', 'window blur hides the style card');
  console.log('[pass] hover chrome dies on pointer-leave and window blur instead of persisting over the page');
}

{
  const page = fixture();
  page.run('start');
  page.click();
  page.run('save', { target_id: 'target-1', number: 1 });
  const shadow = page.document.documentElement.children[0].shadow;
  const pin = shadow.children.find(child => child.attributes?.get('data-ace-annotation-pin') === '1');
  assert.ok(pin);

  let snapshot = page.run('hide_markers', { enabled: true });
  assert.equal(snapshot.markers_hidden, true);
  assert.equal(pin.style.display, 'none');
  snapshot = page.run('hide_markers', { enabled: false });
  assert.equal(snapshot.markers_hidden, false);
  assert.equal(pin.style.display, 'block');

  page.run('original', { enabled: true });
  assert.equal(pin.style.display, 'none');
  page.run('original', { enabled: false });
  assert.equal(pin.style.display, 'block');

  snapshot = page.run('discard');
  assert.equal(snapshot.active, true);
  assert.equal(snapshot.popup, false);
  assert.equal(snapshot.targets.length, 0);
  assert.equal(snapshot.markers_hidden, false);
  assert.equal(shadow.children.includes(pin), false);
  console.log('[pass] hide markers, hold-to-compare original view, and discard-all keep separate toolbar semantics');
}

{
  for (const ancestorRemoval of [false, true]) {
    const page = fixture();
    const ancestor = page.element('section');
    page.target.parentNode = ancestor;
    page.run('start'); page.click();
    page.run('add_target');
    const second = page.element('aside');
    page.choose(second);
    page.run('add_region'); page.pointer('pointerdown', 1, 2); page.pointer('pointerup', 40, 50);
    page.run('save', { target_id: null, number: 1 });
    page.run('exit');
    const removed = ancestorRemoval ? ancestor : page.target;
    page.observers.forEach(observer => observer.records.push({ removedNodes: [removed] }));
    assert.equal(page.target.isConnected, true);
    assert.deepEqual(page.run('read').invalid_numbers, [1]);
    const similar = page.element('button'); similar.id = page.target.id;
    page.run('start'); page.choose(similar);
    assert.equal(page.run('read').target.id, 'target-3');
    assert.deepEqual(page.run('read').invalid_numbers, [1]);
  }
  const draft = fixture();
  draft.run('start'); draft.click();
  draft.observers.forEach(observer => observer.records.push({ removedNodes: [draft.target] }));
  assert.equal(draft.run('read').popup, false);
  assert.deepEqual(draft.run('read').targets, []);
  console.log('[pass] queued exact-node/ancestor removal is sticky across reinsertion, invalidates multi-target regions and never relocates similar nodes');
}

{
  const page = fixture();
  page.run('start'); page.click();
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
  assert.equal(shadow.children[3].style.top, '30px');
  page.context.scrollY = 520;
  page.windowListeners.get('scroll')();
  assert.equal(shadow.children[3].style.top, '10px');
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
  const savedPin = shadow.children.find(node => node.attributes?.get('data-ace-annotation-pin') === '1');
  const savedOutline = shadow.children.find(node => node.attributes?.get('data-ace-annotation-pin-outline') === '1');
  assert.equal(savedPin.style.top, '-65px');
  assert.equal(savedOutline.style.top, '-65px');
  page.run('start'); page.click();
  page.run('add_target'); page.choose(second);
  page.run('add_region'); page.pointer('pointerdown', 10, 10); page.pointer('pointerup', 30, 30);
  page.run('cancel');
  assert.deepEqual(page.run('read').targets, []);
  assert.equal(page.run('read').region, null);
  assert.equal(shadow.children.length, 6);
  console.log('[pass] multiple exact targets deduplicate/switch, one page region replaces across scrolling, one pin saves and cancellation clears the whole draft');
}

{
  const page = fixture();
  page.run('start'); page.click();
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
  page.target.isConnected = false;
  assert.equal(page.run('save', { target_id: 'target-1', number: 1 }).error, 'stale_target');
  assert.equal(page.run('read').popup, false);
  assert.equal(page.run('read').target, null);
  console.log('[pass] detached exact target cannot be saved or relocated');
}

{
  const page = fixture();
  assert.equal(page.run('start').active, true);
  // The toolbar must be able to start element picking before any popup exists.
  assert.equal(page.run('add_target').selection_mode, 'element');
  page.choose(page.target);
  const selected = page.run('read');
  assert.equal(selected.popup, true);
  assert.equal(selected.target.id, 'target-1');

  const regionPage = fixture();
  assert.equal(regionPage.run('start').active, true);
  assert.equal(regionPage.run('add_region').selection_mode, 'region');
  regionPage.pointer('pointerdown', 10, 20);
  regionPage.pointer('pointermove', 40, 60);
  regionPage.pointer('pointerup', 40, 60);
  const drawn = regionPage.run('read');
  assert.equal(drawn.popup, true);
  assert.deepEqual(drawn.region, { x: 10, y: 20, width: 30, height: 40 });
  assert.deepEqual(drawn.targets, []);
  regionPage.run('cancel');
  assert.equal(regionPage.run('read').popup, false);
  console.log('[pass] toolbar starts element and region drafts before a popup exists');
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
  assert.deepEqual(pageAnnotationPopupDragPosition({
    startLeft: 100, startTop: 50, deltaX: 300, deltaY: 250,
    width: 600, height: 400, popupWidth: 200, popupHeight: 150,
  }), { left: 392, top: 242 });
  assert.deepEqual(pageAnnotationPopupDragPosition({
    startLeft: 100, startTop: 50, deltaX: -300, deltaY: -250,
    width: 600, height: 400, popupWidth: 200, popupHeight: 150,
  }), { left: 8, top: 8 });
  console.log('[pass] annotation bridge is session-independent and popup placement accounts for viewport zoom and edges');
}

{
  const hooks = [];
  let cursor = 0;
  const calls = [];
  const popupSource = fs.readFileSync(new URL('../components/PageAnnotations.jsx', import.meta.url), 'utf8');
  const compiled = await transformWithEsbuild(
    popupSource.replace(/^import[\s\S]*?;\r?$/gm, '').replace(/^export /gm, ''),
    'PageAnnotations.jsx', { loader: 'jsx', jsxFactory: 'React.createElement' },
  );
  const popup = vm.runInNewContext(`${compiled.code}\nPageAnnotationPopup`, {
    React,
    PageAnnotationTextField: () => null,
    clearTimeout, setTimeout,
    useRef: value => ({ current: value ?? null }), useEffect: () => {}, useLayoutEffect: () => {},
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
  const adjust = nodes(tree).find(node => node.props?.['aria-label'] === '调整');
  assert.equal(adjust.props['aria-pressed'], undefined);
  assert.equal(adjust.props['aria-expanded'], false);
  const sendBar = fs.readFileSync(new URL('../components/PageAnnotationSendBar.jsx', import.meta.url), 'utf8');
  assert.match(sendBar, /run\('hide_markers'/);
  assert.match(sendBar, /run\('discard'\)/);
  assert.match(sendBar, /!active \|\| snapshot\.selection_mode \|\| snapshot\.popup/);
  assert.match(sendBar, /这一页的所有编辑都将被移除/);
  assert.match(sendBar, /其他页签的待发批注不受影响/);
  assert.match(sendBar, /<Modal[^>]*nativeOverlayMode="overlap"[^>]*nativeOverlayInteraction="disabled"/);
  assert.match(sendBar, /suspendDialogChrome[\s\S]*setAnnotationChromeSuspended\('dialog', enabled/);
  assert.match(sendBar, /closeDiscard[\s\S]*setConfirmDiscard\(false\)[\s\S]*suspendDialogChrome\(false\)/);
  assert.match(sendBar, /setConfirmDiscard\(true\)[\s\S]*suspendDialogChrome\(true\)/);
  assert.doesNotMatch(sendBar, />\s*放弃。\s*</);
  assert.match(sendBar, />\s*放弃\s*</);
  // Menus floated over the page must hide the in-page outline and hover card,
  // otherwise they show through a context menu that is narrower than the card.
  const annotationsSource = fs.readFileSync(new URL('../components/PageAnnotations.jsx', import.meta.url), 'utf8');
  assert.match(annotationsSource, /querySelectorAll\('\[role="menu"\]'\)/);
  assert.match(annotationsSource, /setAnnotationChromeSuspended\('menu', menuOpen\(\), apply\)/);
  assert.match(annotationsSource, /action === 'set_modal_suspended'/,
    'chrome suppression must not mark the annotation session busy');
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
  assert.equal(nodes(tree).filter(node => String(node.props?.className || '').includes('ace-annotation-chip')).length, 0);
  snapshot.region = { x: 0, y: 500, width: 40, height: 30 };
  snapshot.target = null;
  tree = render();
  assert.equal(nodes(tree).filter(node => node.type === 'textarea').length, 1);
  assert.equal(nodes(tree).filter(node => node.type === 'input').length, 0);
  await tree.props.onSubmit({ preventDefault() {} });
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
  assert.match(windows, /#include "agent_browser_native_commands_win\.hpp"/);
  assert.match(windows, /agent_browser_windows::evaluate_annotation\(page->webview,/);
  const commands = fs.readFileSync(new URL('../../../src/apps/desktop/agent_browser_native_commands_win.cpp', import.meta.url), 'utf8');
  assert.match(commands, /Page.createIsolatedWorld/);
  assert.match(commands, /"contextId", world\["executionContextId"\]/);
  assert.match(commands, /"grantUniveralAccess", false/);
  assert.doesNotMatch(source, /chrome\.webview|webkit\.messageHandlers|postMessage|fetch\(/);
  console.log('[pass] annotation popup uses local native occlusion and leaves source-selection/conversation path independent');
}
