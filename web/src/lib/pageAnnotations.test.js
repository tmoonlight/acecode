import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';
import { createPageAnnotationController, pageAnnotationPopupPosition, runPageAnnotationCommand } from './pageAnnotations.js';

const source = fs.readFileSync(new URL('../../../src/apps/desktop/page_annotation_runtime.hpp', import.meta.url), 'utf8');
const runtime = source.split('R"ACEANNOTATION(')[1].split(')ACEANNOTATION"')[0];

function fixture() {
  const documentListeners = new Map();
  const windowListeners = new Map();
  let document;
  const element = (tag) => ({
    nodeType: 1, localName: tag, ownerDocument: document, isConnected: true,
    style: { cssText: '' }, children: [], listeners: new Map(), id: '', textContent: '',
    offsetWidth: 80, offsetHeight: 24,
    append(...children) { children.forEach(child => { child.parent = this; }); this.children.push(...children); },
    remove() { this.parent.children = this.parent.children.filter(child => child !== this); },
    attachShadow() { this.shadow = element('shadow'); return this.shadow; },
    setAttribute() {},
    addEventListener(type, listener) { this.listeners.set(type, listener); },
    getBoundingClientRect() { return { x: 10, y: 20, width: 100, height: 40 }; },
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
    document, innerWidth: 800, innerHeight: 600,
    addEventListener(type, listener) { windowListeners.set(type, listener); },
    removeEventListener(type) { windowListeners.delete(type); },
  });
  const run = (action, fields = {}) => JSON.parse(JSON.stringify(vm.runInContext(
    `${runtime}(${JSON.stringify({ action, document_id: 'doc-1', ...fields })})`, context,
  )));
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
  return { document, target, child, context, run, click, control, event, documentListeners, windowListeners };
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
  assert.equal(shadow.children[2].textContent, '1');
  page.run('exit');
  assert.equal(shadow.children[0].style.display, 'none');
  assert.equal(shadow.children[1].style.display, 'none');
  assert.equal(shadow.children[2].style.display, 'block');
  assert.equal(page.target.style.cssText, 'color: red');
  assert.equal(page.target.textContent, 'Original content');
  assert.equal(page.target.children[0], page.child);
  page.run('start');
  page.click();
  page.control();
  page.run('cancel');
  assert.equal(page.run('read').popup, false);
  assert.equal(shadow.children.length, 3);
  page.click();
  assert.equal(page.run('read').target.id, 'target-1');
  page.documentListeners.get('keydown')(page.event(page.target, true, { key: 'Escape' }));
  assert.equal(page.run('read').active, false);
  assert.equal(shadow.children[2].style.display, 'block');
  const pageWorld = vm.createContext({ document: page.document });
  assert.equal(vm.runInContext('typeof __acecodePageAnnotationsV1', pageWorld), 'undefined');
  assert.equal(page.run('read', { document_id: 'doc-2' }).error, 'stale_document');
  assert.equal(page.run('send').error, 'unsupported_action');
  assert.equal(page.run('start', { document_id: 'doc-2' }).active, true);
  assert.equal(page.document.documentElement.children.length, 1);
  assert.equal(page.document.documentElement.children[0].shadow.children.length, 2);
  assert.equal(page.run('read').error, 'stale_document');
  console.log('[pass] annotation runtime selects, opens, cancels, saves immutable pins and exits without changing content');
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
  const actor = createPageAnnotationController({
    pageId: 'page-1', onChange: next => changes.push(next),
    invoke: async (pageId, command) => {
      requests.push({ pageId, command });
      if (command.action === 'read') await new Promise(resolve => { release = resolve; });
      return { ok: true, page_id: pageId, annotations: { document_id: 'doc-1', revision: 2, active: true } };
    },
  });
  actor.observe({ document_id: 'doc-1', revision: 1, active: true, target: { id: 'target-1' } });
  const read = actor.command('read');
  const save = actor.command('save', { description: 'Requested change' });
  await Promise.resolve();
  assert.equal(requests.length, 1);
  actor.observe({ document_id: 'doc-2', revision: 3, active: false });
  release();
  await read;
  await save;
  assert.equal(requests[1].command.target_id, 'target-1');
  assert.equal(requests[1].command.document_id, 'doc-1');
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
  const panel = fs.readFileSync(new URL('../components/AgentBrowserPanel.jsx', import.meta.url), 'utf8');
  const popup = fs.readFileSync(new URL('../components/PageAnnotations.jsx', import.meta.url), 'utf8');
  assert.match(panel, /createAgentBrowserElementContext\(selectedElement, event.detail\)/);
  assert.match(panel, /toggleAgentBrowserElementSelection\(pageId\)/);
  assert.match(popup, /data-ace-native-overlay="overlap"/);
  assert.doesNotMatch(popup, /onAddContext|submitMessage|fetch\(|aceDesktop_.*(?:Write|Send)/);
  const windows = fs.readFileSync(new URL('../../../src/apps/desktop/agent_browser_host.cpp', import.meta.url), 'utf8');
  assert.match(windows, /Page.createIsolatedWorld/);
  assert.match(windows, /"contextId", world\["executionContextId"\]/);
  assert.doesNotMatch(source, /chrome\.webview|webkit\.messageHandlers|postMessage|fetch\(/);
  console.log('[pass] annotation popup uses local native occlusion and leaves source-selection/conversation path independent');
}
