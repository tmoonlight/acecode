import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { runAnnotationIntegration } from './pageAnnotationIntegration.browser.testHelper.js';

const executable = process.env.ACECODE_TEST_BROWSER || 'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe';
assert.ok(fs.existsSync(executable), 'Set ACECODE_TEST_BROWSER to a Chromium-compatible browser executable');
const profile = fs.mkdtempSync(path.join(os.tmpdir(), 'acecode-annotation-styles-'));
const child = spawn(executable, ['--headless=new', '--disable-gpu', '--no-first-run', '--no-default-browser-check',
  '--disable-background-mode', '--disable-breakpad', '--remote-debugging-port=0', `--user-data-dir=${profile}`, 'about:blank'], { windowsHide: true, stdio: 'ignore' });
const exited = once(child, 'exit');
let socket;
const pending = new Map();
let sequence = 0;
const request = (method, params = {}) => new Promise((resolve, reject) => {
  const id = ++sequence;
  const timeout = setTimeout(() => { pending.delete(id); reject(new Error(`CDP timeout: ${method}`)); }, 15000);
  pending.set(id, { resolve, reject, timeout });
  socket.send(JSON.stringify({ id, method, params }));
});
const extract = (name, delimiter) => fs.readFileSync(new URL(`../../../src/apps/desktop/${name}`, import.meta.url), 'utf8')
  .split(`R"${delimiter}(`).slice(1).map(part => part.split(`)${delimiter}"`)[0]).join('');
const styles = extract('page_annotation_style_runtime.hpp', 'ACESTYLES');
const runtime = extract('page_annotation_runtime.hpp', 'ACEANNOTATION');
const text = extract('page_annotation_text_runtime.hpp', 'ACETEXT');
const globalStyles = fs.readFileSync(new URL('../styles/globals.css', import.meta.url), 'utf8');
const popupStyles = globalStyles.slice(
  globalStyles.indexOf('.ace-annotation-card {'),
  globalStyles.indexOf('.ace-agent-browser-frame {'),
);
assert.ok(popupStyles.includes('.ace-annotation-card.is-adjusting'), 'annotation popup CSS must be present');
let contextId;
const evaluate = async expression => {
  const response = await request('Runtime.evaluate', { expression, contextId, returnByValue: true, awaitPromise: true });
  assert.equal(response.exceptionDetails, undefined, JSON.stringify(response.exceptionDetails));
  return response.result.value;
};
try {
  const portFile = path.join(profile, 'DevToolsActivePort');
  const deadline = Date.now() + 20000;
  let port;
  while (!port && Date.now() < deadline) {
    try {
      const candidate = fs.readFileSync(portFile, 'utf8').split('\n')[0].trim();
      if (/^\d+$/.test(candidate)) port = candidate;
    } catch (error) {
      if (!['ENOENT', 'EBUSY'].includes(error.code)) throw error;
    }
    if (!port) await new Promise(resolve => setTimeout(resolve, 100));
  }
  assert.ok(port, 'Headless browser did not expose its debugging endpoint');
  const pages = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
  socket = new WebSocket(pages.find(page => page.type === 'page').webSocketDebuggerUrl);
  socket.addEventListener('message', event => {
    const message = JSON.parse(event.data);
    const waiter = pending.get(message.id);
    if (!waiter) return;
    clearTimeout(waiter.timeout);
    pending.delete(message.id);
    if (message.error) waiter.reject(new Error(JSON.stringify(message.error)));
    else waiter.resolve(message.result);
  });
  await once(socket, 'open');
  await request('Page.enable');
  const tree = await request('Page.getFrameTree');
  contextId = (await request('Page.createIsolatedWorld', {
    frameId: tree.frameTree.frame.id, worldName: 'acecode-annotation-style-tests', grantUniveralAccess: false,
  })).executionContextId;
  await request('Emulation.setDeviceMetricsOverride', {
    width: 600, height: 500, deviceScaleFactor: 1, mobile: false,
  });
  await evaluate(`(() => {
    const style = document.createElement('style');
    style.id = 'annotation-popup-layout';
    style.textContent = ${JSON.stringify(popupStyles)};
    document.head.append(style);
    document.body.innerHTML = \`
      <form class="ace-annotation-card is-adjusting" style="--ace-annotation-adjust-width:380px;--ace-annotation-adjust-height:460px">
        <div class="ace-annotation-design">
          <div class="ace-annotation-row">
            <span>宽度</span>
            <span class="ace-annotation-value-cell">
              <span class="ace-annotation-value-box"><input value="150.475"><span class="ace-annotation-value-unit">px</span></span>
            </span>
          </div>
          <div class="ace-annotation-row">
            <span>间隙</span>
            <span class="ace-annotation-value-pair">
              <span class="ace-annotation-value-cell"><span class="ace-annotation-value-box"><input><span class="ace-annotation-value-unit">px</span></span></span>
              <span class="ace-annotation-value-cell"><span class="ace-annotation-value-box"><input><span class="ace-annotation-value-unit">px</span></span></span>
            </span>
          </div>
          <div class="ace-annotation-spacing">
            <div class="ace-annotation-row">
              <span>内边距</span>
              <span class="ace-annotation-spacing-values">
                ${[1, 2, 3, 4].map(value => `<span class="ace-annotation-value-cell is-compact"><span class="ace-annotation-value-box is-compact"><input value="${value}"><span class="ace-annotation-value-unit is-compact">px</span></span></span>`).join('')}
              </span>
            </div>
          </div>
        </div>
      </form>\`;
  })()`);
  const popupLayout = await evaluate(`(() => {
    const rect = selector => {
      const value = document.querySelector(selector).getBoundingClientRect();
      return { width: value.width, top: value.top };
    };
    return {
      card: rect('.ace-annotation-card'),
      value: rect('.ace-annotation-row .ace-annotation-value-cell'),
      pair: [...document.querySelectorAll('.ace-annotation-value-pair > .ace-annotation-value-cell')].map(node => node.getBoundingClientRect().width),
      compact: [...document.querySelectorAll('.ace-annotation-spacing-values > .ace-annotation-value-cell')].map(node => ({ width: node.getBoundingClientRect().width, top: node.getBoundingClientRect().top })),
    };
  })()`);
  assert.equal(Math.round(popupLayout.card.width), 380);
  assert.ok(popupLayout.value.width >= 175, JSON.stringify(popupLayout));
  assert.ok(popupLayout.pair.every(width => width >= 45), JSON.stringify(popupLayout));
  assert.ok(popupLayout.compact.every(cell => cell.width >= 45), JSON.stringify(popupLayout));
  await request('Emulation.setDeviceMetricsOverride', {
    width: 300, height: 500, deviceScaleFactor: 1, mobile: false,
  });
  const narrowPopupLayout = await evaluate(`(() => ({
    cardWidth: document.querySelector('.ace-annotation-card').getBoundingClientRect().width,
    compact: [...document.querySelectorAll('.ace-annotation-spacing-values > .ace-annotation-value-cell')].map(node => ({ width: node.getBoundingClientRect().width, top: node.getBoundingClientRect().top })),
  }))()`);
  assert.ok(narrowPopupLayout.cardWidth <= 284, JSON.stringify(narrowPopupLayout));
  assert.ok(narrowPopupLayout.compact.every(cell => cell.width >= 45), JSON.stringify(narrowPopupLayout));
  assert.ok(narrowPopupLayout.compact[3].top > narrowPopupLayout.compact[0].top, JSON.stringify(narrowPopupLayout));
  console.log('[pass] browser layout keeps the expanded popup value column and 45px spacing inputs readable, wrapping at narrow widths');
  await evaluate(`(() => {
    const design = document.querySelector('.ace-annotation-design');
    const row = document.createElement('div');
    row.className = 'ace-annotation-row';
    row.innerHTML = '<button class="ace-annotation-expand" type="button"><span>内边距</span><svg width="12" height="12" viewBox="0 0 16 16"><path d="M6 4.2 9.8 8 6 11.8"/></svg></button>';
    design.append(row);
    const pair = document.createElement('div');
    pair.className = 'ace-annotation-pair';
    pair.innerHTML = '<span class="ace-annotation-pair-lock"><svg class="ace-annotation-pair-bracket" viewBox="0 0 26 62" width="26" height="62"><path d="M4 3H23V21M23 41V59H4"/></svg><button class="ace-annotation-pair-lock-button" type="button"><svg width="12" height="12" viewBox="0 0 24 24"></svg></button></span>';
    design.append(pair);
  })()`);
  const controlMetrics = await evaluate(`(() => {
    const center = node => {
      const rect = node.getBoundingClientRect();
      return rect.top + rect.height / 2;
    };
    const expand = document.querySelector('.ace-annotation-expand');
    const lock = document.querySelector('.ace-annotation-pair-lock-button');
    return {
      expandDisplay: getComputedStyle(expand).display,
      expandPadding: [
        getComputedStyle(expand).paddingLeft,
        getComputedStyle(expand).paddingRight,
      ].join('/'),
      labelCenter: center(expand.querySelector('span')),
      chevronCenter: center(expand.querySelector('svg')),
      lockWidth: lock.getBoundingClientRect().width,
      lockDisplay: getComputedStyle(lock).display,
      bracketPath: document.querySelector('.ace-annotation-pair-bracket path').getAttribute('d'),
    };
  })()`);
  assert.ok(['flex', 'inline-flex'].includes(controlMetrics.expandDisplay), JSON.stringify(controlMetrics));
  assert.equal(controlMetrics.expandPadding, '2px/2px', JSON.stringify(controlMetrics));
  assert.ok(Math.abs(controlMetrics.labelCenter - controlMetrics.chevronCenter) <= 1, JSON.stringify(controlMetrics));
  assert.equal(controlMetrics.lockWidth, 16, JSON.stringify(controlMetrics));
  assert.ok(['flex', 'inline-flex'].includes(controlMetrics.lockDisplay), JSON.stringify(controlMetrics));
  assert.equal(controlMetrics.bracketPath, 'M4 3H23V21M23 41V59H4');
  console.log('[pass] spacing expander keeps icon and label on one line, lock badge matches Codex bracket geometry');

  await evaluate(`(() => {
    const design = document.querySelector('.ace-annotation-design');
    document.querySelectorAll('.ace-annotation-dimensions, .ace-annotation-pair').forEach(node => node.remove());
    const dimensions = document.createElement('div');
    dimensions.className = 'ace-annotation-dimensions';
    dimensions.innerHTML = '<div class="ace-annotation-row"><span>宽度</span><span class="ace-annotation-value-cell"><span class="ace-annotation-value-box"><input value="150"></span></span></div><div class="ace-annotation-row"><span>高度</span><span class="ace-annotation-value-cell"><span class="ace-annotation-value-box"><input value="60"></span></span></div><span class="ace-annotation-pair-lock"><svg class="ace-annotation-pair-bracket" viewBox="0 0 26 62" width="26" height="62"><path d="M4 3H23V21M23 41V59H4"/></svg><button class="ace-annotation-pair-lock-button" type="button"><svg width="12" height="12" viewBox="0 0 24 24"></svg></button></span>';
    design.append(dimensions);
  })()`);
  const dimensionLock = await evaluate(`(() => {
    const box = document.querySelector('.ace-annotation-dimensions');
    const bracket = box.querySelector('.ace-annotation-pair-bracket').getBoundingClientRect();
    const lock = box.querySelector('.ace-annotation-pair-lock-button').getBoundingClientRect();
    const rows = [...box.querySelectorAll('.ace-annotation-row')].map(row => row.getBoundingClientRect());
    return {
      bracketHeight: Math.round(bracket.height),
      bracketWidth: Math.round(bracket.width),
      rowsSpan: Math.round(rows[1].bottom - rows[0].top),
      badgeCenter: lock.top + lock.height / 2,
      bracketCenter: bracket.top + bracket.height / 2,
      badgeSize: [Math.round(lock.width), Math.round(lock.height)].join('x'),
    };
  })()`);
  assert.equal(dimensionLock.badgeSize, '16x16', JSON.stringify(dimensionLock));
  assert.equal(dimensionLock.bracketWidth, 26, JSON.stringify(dimensionLock));
  // The bracket is a fixed 62px so it matches the padding/margin axis pairs; the
  // two 30px rows span 66px, leaving a symmetric 2px inset on each side.
  assert.ok(dimensionLock.rowsSpan - dimensionLock.bracketHeight <= 6, JSON.stringify(dimensionLock));
  assert.ok(Math.abs(dimensionLock.badgeCenter - dimensionLock.bracketCenter) <= 1, JSON.stringify(dimensionLock));
  console.log('[pass] width/height lock reuses the padding axis bracket: 62px span with a centred 16px badge');

  await evaluate(`(() => {
    const editor = document.createElement('div');
    editor.className = 'ace-annotation-editor';
    editor.innerHTML = '<fieldset><div class="ace-annotation-design"><div class="ace-annotation-row"><span>宽度</span><span class="ace-annotation-value-cell"><span class="ace-annotation-value-box"><input value="150"><span class="ace-annotation-value-unit">px</span></span></span></div></div><label class="ace-annotation-text-field"><span>可见文本</span><textarea class="ace-annotation-text-input">hello</textarea></label></fieldset>';
    document.querySelector('.ace-annotation-card').append(editor);
  })()`);
  const editorOverflow = await evaluate(`(() => {
    const editor = document.querySelector('.ace-annotation-card.is-adjusting .ace-annotation-editor');
    const style = getComputedStyle(editor);
    return { x: style.overflowX, y: style.overflowY };
  })()`);
  assert.equal(editorOverflow.x, 'hidden', JSON.stringify(editorOverflow));
  assert.equal(editorOverflow.y, 'auto', JSON.stringify(editorOverflow));
  console.log('[pass] adjusting editor clips horizontal overflow without a left/right scrollbar');

  // The target tag belongs on the drag-handle row, and the scrolling editor has
  // to leave the visible-text box clear of the action bar divider.
  await evaluate(`(() => {
    const card = document.querySelector('.ace-annotation-card');
    const handle = document.createElement('div');
    handle.className = 'ace-annotation-design-handle';
    handle.innerHTML = '<div class="ace-annotation-target-tag">div</div><button class="ace-annotation-design-grip" type="button"><svg width="16" height="16" viewBox="0 0 16 16"></svg></button>';
    card.prepend(handle);
  })()`);
  const handleLayout = await evaluate(`(() => {
    const handle = document.querySelector('.ace-annotation-design-handle');
    const tag = handle.querySelector('.ace-annotation-target-tag');
    const grip = handle.querySelector('.ace-annotation-design-grip');
    const editor = document.querySelector('.ace-annotation-card.is-adjusting .ace-annotation-editor');
    const textarea = editor.querySelector('.ace-annotation-text-input');
    const frame = handle.getBoundingClientRect();
    const tagBox = tag.getBoundingClientRect();
    const gripBox = grip.getBoundingClientRect();
    return {
      rowCenters: [Math.round(tagBox.top + tagBox.height / 2), Math.round(gripBox.top + gripBox.height / 2)],
      tagInset: Math.round(tagBox.left - frame.left),
      gripInset: Math.round(frame.right - gripBox.right),
      belowTextarea: Math.round(editor.getBoundingClientRect().bottom - textarea.getBoundingClientRect().bottom),
      scrolls: editor.scrollHeight > editor.clientHeight,
    };
  })()`);
  assert.ok(Math.abs(handleLayout.rowCenters[0] - handleLayout.rowCenters[1]) <= 1,
    `the target tag and drag grip must share one visual row: ${JSON.stringify(handleLayout)}`);
  assert.equal(handleLayout.tagInset, 16, JSON.stringify(handleLayout));
  assert.equal(handleLayout.gripInset, 16, JSON.stringify(handleLayout));
  assert.equal(handleLayout.scrolls, false, 'fixture must fit the editor so the padding is measurable');
  assert.equal(handleLayout.belowTextarea, 2,
    `the visible-text box keeps 2px to the action bar divider: ${JSON.stringify(handleLayout)}`);
  console.log('[pass] target tag rides the drag-handle row; visible-text box keeps 2px above the action divider');

  await evaluate(`document.getElementById('annotation-popup-layout').remove(); document.body.replaceChildren()`);
  await request('Emulation.setDeviceMetricsOverride', {
    width: 800, height: 600, deviceScaleFactor: 1, mobile: false,
  });
  await evaluate(`document.body.innerHTML = '<div id="target" style="position:absolute;left:10px;top:20px;width:100px;height:40px;box-sizing:content-box;color:rgb(255,0,0);background-color:white;font:400 16px serif;border:0 none black;padding:0;margin:0;letter-spacing:normal;word-spacing:normal">Original <strong>child</strong></div>'; globalThis.target = document.getElementById('target'); globalThis.originalInline = target.getAttribute('style'); globalThis.originalChild = target.firstElementChild; globalThis.previews = (${styles})(document); previews.select(target,'target-1');`);
  const value = property => evaluate(`getComputedStyle(target).getPropertyValue(${JSON.stringify(property)})`);
  const preview = fields => evaluate(`previews.preview(target,'target-1',${JSON.stringify(fields)}); previews.differences()`);
  const cases = [
    ['color', '#00ff00', 'rgb(0, 255, 0)'], ['background-color', '#0000ff', 'rgb(0, 0, 255)'],
    ['opacity', '0.5', '0.5'], ['font-family', 'monospace', 'monospace'], ['font-size', '24px', '24px'],
    ['font-weight', '700', '700'], ['border-radius', '8px', '8px'], ['border-color', '#00ff00', 'rgb(0, 255, 0)'],
    ['border-width', '3px', '3px'], ['width', '150px', '150px'], ['height', '70px', '70px'],
    ['letter-spacing', '2px', '2px'], ['word-spacing', '4px', '4px'], ['gap', '6px', '6px'],
    ['padding', '5px', '5px'], ['margin', '7px', '7px'],
    ['padding-top', '1px', '1px'], ['padding-right', '2px', '2px'], ['padding-bottom', '3px', '3px'], ['padding-left', '4px', '4px'],
    ['margin-top', '1px', '1px'], ['margin-right', '2px', '2px'], ['margin-bottom', '3px', '3px'], ['margin-left', '4px', '4px'],
  ];
  for (const [property, requested, effective] of cases) {
    const differences = await preview({ [property]: requested });
    assert.equal(await value(property), effective, property);
    assert.ok(differences.length > 0, `${property} must produce a real difference`);
    assert.ok(differences.every(change => change.target_id === 'target-1' && change.old_value !== change.new_value));
    assert.equal(await evaluate('target.getAttribute("style") === originalInline && target.firstElementChild === originalChild'), true);
    await evaluate("previews.cancel(); previews.select(target,'target-1')");
  }
  console.log('[pass] real Chromium CSS engine previews every style field without altering original inline styles or child identity');

  assert.deepEqual(await preview({ color: '#ff0000', width: '100px', opacity: '1' }), []);
  await preview({ padding: '1px 2px 3px 4px', margin: '5px 6px' });
  assert.equal(await value('padding-left'), '4px');
  assert.equal(await value('margin-right'), '6px');
  await evaluate("previews.cancel(); previews.select(target,'target-1')");
  for (const invalid of [{ color: 'red; width:999px' }, { 'background-color': 'url(https://example.invalid)' },
    { position: 'fixed' }, { 'font-family': 'var(--page-data)' }, { width: 'invalid-length' }]) {
    assert.equal(await evaluate(`(() => { try { previews.preview(target,'target-1',${JSON.stringify(invalid)}); return false; } catch { return true; } })()`), true);
  }
  await preview({ color: '#0000ff' });
  const firstDifferences = await evaluate('previews.save()');
  assert.deepEqual(firstDifferences, [{ target_id: 'target-1', property: 'color', old_value: 'rgb(255, 0, 0)', new_value: 'rgb(0, 0, 255)' }]);
  await evaluate("previews.select(target,'target-1')");
  const secondDifferences = await preview({ color: '#00ff00' });
  assert.equal(secondDifferences[0].old_value, 'rgb(0, 0, 255)');
  await evaluate('previews.setOriginal(true)');
  assert.equal(await value('color'), 'rgb(255, 0, 0)');
  assert.deepEqual(await evaluate('previews.differences()'), secondDifferences);
  await evaluate('previews.setOriginal(false)');
  assert.equal(await value('color'), 'rgb(0, 255, 0)');
  await evaluate('previews.cancel()');
  assert.equal(await value('color'), 'rgb(0, 0, 255)');
  console.log('[pass] canonical no-ops, injection rejection, same-target layered cancel, and original/draft round trips');

  await evaluate("previews.select(target,'target-1')");
  assert.equal(await evaluate("previews.describe(target,'target-1').ratio_locked"), false);
  await preview({ width: '200px' });
  assert.equal(await value('height'), '40px');
  await evaluate("previews.preview(target,'target-1',{},true)");
  assert.equal(await evaluate("previews.describe(target,'target-1').ratio"), 5);
  await preview({ height: '60px' });
  assert.equal(await value('width'), '300px');
  await preview({ width: '400px' });
  assert.equal(await value('height'), '80px');
  await evaluate("previews.preview(target,'target-1',{},false)");
  await preview({ width: '200px' });
  assert.equal(await value('height'), '80px');
  await evaluate('previews.cancel()');
  assert.equal(await value('width'), '100px');
  assert.equal(await value('height'), '40px');
  await evaluate("globalThis.inlineTarget = document.createElement('span'); inlineTarget.style.cssText='width:100px;height:40px'; inlineTarget.textContent='inline'; document.body.append(inlineTarget); previews.select(inlineTarget,'inline'); previews.preview(inlineTarget,'inline',{},true)");
  assert.equal(await evaluate("(() => { try { previews.preview(inlineTarget,'inline',{width:'auto'}); return false; } catch (error) { return error.message; } })()"), 'invalid_current_ratio');
  assert.equal(await evaluate("previews.describe(inlineTarget,'inline').values.width"), '100px');
  assert.deepEqual(await evaluate("previews.describe(inlineTarget,'inline').styles"), {});
  await evaluate('previews.cancel(); inlineTarget.remove()');
  console.log('[pass] ratio is default-off, captures current edited ratio, links either dimension, and unlocks/cancels');

  await evaluate("target.style.setProperty('color','red','important'); previews.select(target,'target-1')");
  assert.deepEqual(await preview({ color: 'green' }), []);
  assert.equal(await value('color'), 'rgb(255, 0, 0)');
  assert.equal(await evaluate("target.style.getPropertyPriority('color')"), 'important');
  assert.deepEqual(await evaluate("previews.describe(target,'target-1').errors"), []);
  await evaluate('previews.cancel(); target.setAttribute("style",originalInline)');
  await evaluate("globalThis.clone = target.cloneNode(true); document.body.append(clone)");
  await evaluate('Promise.resolve()');
  assert.equal(await evaluate('getComputedStyle(clone).color'), 'rgb(255, 0, 0)');
  assert.equal(await value('color'), 'rgb(0, 0, 255)');
  await evaluate('target.remove(); Promise.resolve()');
  assert.equal(await evaluate('getComputedStyle(clone).color'), 'rgb(255, 0, 0)');
  await evaluate('document.body.append(target); Promise.resolve()');
  assert.equal(await value('color'), 'rgb(0, 0, 255)');
  await evaluate('previews.dispose(); clone.remove()');
  assert.equal(await value('color'), 'rgb(255, 0, 0)');
  assert.equal(await evaluate('document.adoptedStyleSheets.length'), 0);
  console.log('[pass] inline-important stays red without a recorded difference or error; clones never inherit previews; disposal restores original stylesheet membership');

  await evaluate("target.style.transition='color 10s'; globalThis.motionPreviews = (" + styles + ")(document); motionPreviews.select(target,'motion'); motionPreviews.preview(target,'motion',{color:'blue'});");
  assert.equal(await value('color'), 'rgb(0, 0, 255)');
  await evaluate('motionPreviews.setOriginal(true)');
  assert.equal(await value('color'), 'rgb(255, 0, 0)');
  assert.equal(await value('transition-duration'), '10s');
  await evaluate('motionPreviews.setOriginal(false)');
  assert.equal(await value('color'), 'rgb(0, 0, 255)');
  await evaluate('motionPreviews.cancel()');
  assert.equal(await value('color'), 'rgb(255, 0, 0)');
  assert.equal(await value('transition-duration'), '10s');
  await evaluate('motionPreviews.dispose(); target.setAttribute("style",originalInline)');
  console.log('[pass] original comparison and cancel settle instantly on transitioning pages and restore author transition declarations');

  await evaluate("globalThis.shadowHost = document.createElement('section'); document.body.append(shadowHost); globalThis.shadowRoot = shadowHost.attachShadow({mode:'open'}); globalThis.shadowTarget = document.createElement('span'); shadowTarget.textContent='shadow'; shadowRoot.append(shadowTarget); globalThis.shadowPreviews = (" + styles + ")(document); shadowPreviews.select(shadowTarget,'shadow'); shadowPreviews.preview(shadowTarget,'shadow',{color:'blue'}); ");
  assert.equal(await evaluate('getComputedStyle(shadowTarget).color'), 'rgb(0, 0, 255)');
  await evaluate('document.body.append(shadowTarget); Promise.resolve()');
  assert.equal(await evaluate('getComputedStyle(shadowTarget).color'), 'rgb(0, 0, 255)');
  await evaluate('shadowRoot.append(shadowTarget); Promise.resolve()');
  assert.equal(await evaluate('getComputedStyle(shadowTarget).color'), 'rgb(0, 0, 255)');
  await evaluate('shadowPreviews.dispose()');
  assert.equal(await evaluate('shadowRoot.adoptedStyleSheets.length'), 0);
  assert.equal(await evaluate('document.adoptedStyleSheets.length'), 0);
  console.log('[pass] stylesheet ownership and cleanup also work for shadow-root targets');

  const run = async (action, fields = {}) => {
    const current = action === 'save' ? await run('read') : null;
    return evaluate(`(${runtime})(${JSON.stringify({ action, document_id: 'doc-browser',
      ...(current ? {target_ids:current.targets.map(target => target.id), region:current.region} : {}),
      ...fields })},${text},${styles})`);
  };
  const click = async (x, y) => {
    for (const type of ['mousePressed', 'mouseReleased']) await request('Input.dispatchMouseEvent', { type, x, y, button: 'left', clickCount: 1 });
  };
  await run('start');
  await click(20, 30);
  assert.equal((await run('read')).popup, true);
  const first = await run('preview_style', { target_id: 'target-1', styles: { color: 'blue' } });
  assert.equal(first.target.style_preview.ratio_locked, false);
  assert.equal(first.differences.length, 1);
  const saved = await run('save', { target_id: 'target-1', number: 1 });
  assert.equal(saved.differences[0].property, 'color');
  assert.equal(saved.target, null);
  await run('exit');
  assert.equal(await value('color'), 'rgb(0, 0, 255)');
  await run('original', { enabled: true });
  assert.equal(await value('color'), 'rgb(255, 0, 0)');
  await run('original', { enabled: false });
  assert.equal(await value('color'), 'rgb(0, 0, 255)');
  await run('start');
  await click(20, 30);
  await run('preview_style', { target_id: 'target-1', styles: { color: 'green' } });
  await run('cancel');
  assert.equal(await value('color'), 'rgb(0, 0, 255)');
  await run('start', { document_id: 'replacement-document' });
  assert.equal(await value('color'), 'rgb(255, 0, 0)');
  assert.equal(await evaluate('target.getAttribute("style") === originalInline && target.firstElementChild === originalChild'), true);
  console.log('[pass] isolated runtime/trusted browser input integrates preview/save/differences, mode exit, original compare, cancel, and document rebinding');

  // A menu or dialog floating over the page suspends the in-page outline and
  // hover card so they cannot punch through the app's own overlay.
  await run('start');
  assert.equal((await run('read')).modal_suspended, false, 'a fresh session starts unsuspended');
  const suspended = await run('set_modal_suspended', { enabled: true });
  assert.equal(suspended.modal_suspended, true, 'the runtime reports the suspended state back to native');
  assert.equal((await run('read')).modal_suspended, true, 'the suspended state survives a read');
  const resumed = await run('set_modal_suspended', { enabled: false });
  assert.equal(resumed.modal_suspended, false);
  await run('exit');
  console.log('[pass] set_modal_suspended suspends and resumes the in-page chrome through the real runtime');
  await runAnnotationIntegration({ evaluate, request });
} finally {
  for (const waiter of pending.values()) clearTimeout(waiter.timeout);
  pending.clear();
  if (socket?.readyState === WebSocket.OPEN) {
    socket.send(JSON.stringify({ id: ++sequence, method: 'Browser.close' }));
    socket.close();
  }
  if (child.exitCode === null) {
    await Promise.race([exited, new Promise(resolve => setTimeout(resolve, 3000))]);
    if (child.exitCode === null) child.kill();
  }
  assert.equal(path.dirname(path.resolve(profile)), path.resolve(os.tmpdir()));
  try { await fs.promises.rm(profile, { recursive: true, force: true, maxRetries: 10, retryDelay: 200 }); }
  catch (error) {
    if (!['EPERM', 'EBUSY'].includes(error.code)) throw error;
    console.warn(`Temporary browser profile remains locked: ${profile}`);
  }
}
