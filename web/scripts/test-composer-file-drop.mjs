import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { fileURLToPath, pathToFileURL } from 'node:url';
import path from 'node:path';
import { createServer } from 'vite';

// Exercise the production InputBar. Native focus is an explicit boundary:
// rejecting drag-enter activation must be followed by one retry on acceptance,
// never by another foreground request after asynchronous materialization.
const web = fileURLToPath(new URL('../', import.meta.url));
const modulePath = process.env.ACE_PLAYWRIGHT_MODULE;
const { chromium } = await import(modulePath ? pathToFileURL(path.resolve(modulePath)).href : 'playwright');
const port = Number(process.env.ACE_COMPOSER_DROP_TEST_PORT || 5195);
const baseline = process.argv.includes('--baseline');
const inputBarPath = path.join(web, 'src/components/InputBar.jsx').replaceAll('\\', '/');
const inputBarBaseline = baseline ? execFileSync('git', ['show', 'HEAD:web/src/components/InputBar.jsx'], { cwd: web, encoding: 'utf8' }) : null;
const fixture = `
import React, {useRef, useState} from 'react';
import {createRoot} from 'react-dom/client';
import {InputBar} from '/src/components/InputBar.jsx';
import {SlashCommandsProvider} from '/src/components/SlashCommandsContext.jsx';
import {composerContentText} from '/src/lib/composerContent.js';
import {installNativeFileDropRouter} from '/src/lib/macNativeFileDrag.js';
import '/src/styles/globals.css';
installNativeFileDropRouter();
function Fixture() {
 const ref = useRef();
 const [value, setValue] = useState('');
 const [content, setContent] = useState({version:1, parts:[]});
 const [disabled, setDisabled] = useState(false);
 const [sessionId, setSessionId] = useState('first');
 const [cwd, setCwd] = useState('C:/fixture');
 const [attachments, setAttachments] = useState([]);
 window.fixture = {setDisabled, setSessionId, setCwd, seed:text=>ref.current.replaceText(text),
  seedContent:(content,records)=>{setAttachments(records);ref.current.setComposerContent(content);},
  content:()=>ref.current.getComposerContent(), get value(){return composerContentText(ref.current?.getComposerContent());}};
 return <main style={{padding:80}}><input id="outside" aria-label="Other input"/>
  <div className="ace-console-term" data-tab-id="fixture-console" style={{height:40}}>Terminal drop target</div>
  <SlashCommandsProvider><InputBar ref={ref} value={value} composerContent={content} disabled={disabled} attachments={attachments}
   onChange={(text,next)=>{setValue(text);setContent(next);}} cwd={cwd} currentSessionId={sessionId}
   onMediaFiles={files=>window.uploads.push(files.map(file=>file.name))}/></SlashCommandsProvider>
 </main>;
}
createRoot(document.getElementById('root')).render(<Fixture/>);
`;
const html = `<html><body><div id="root"></div><script type="module">
const mode = new URLSearchParams(location.search).get('mode') || 'windows';
window.__ACECODE_DESKTOP_SHELL__ = mode !== 'browser';
window.__ACECODE_OS__ = mode === 'browser' || mode === 'windows-legacy' ? 'windows' : mode;
window.__ACECODE_NATIVE_FILE_DROP__ = ['windows','windows-legacy','macos'].includes(mode);
window.focusAttempts = 0; window.nativeActive = false; window.genericFocusAttempts = 0;
window.uploads = []; window.materializations = []; window.materializeDelay = false;
window.clipboardPaths = []; window.clipboardError = ''; window.clipboardReads = 0;
window.savedFiles = []; window.pendingMaterializations = [];
window.aceDesktop_activateFileDropWindow = async () => {
 window.focusAttempts++; if(window.focusAttempts > 1) window.nativeActive = true;
 return {ok:window.nativeActive};
};
window.aceDesktop_focusFileDropWindow = window.aceDesktop_activateFileDropWindow;
window.aceDesktop_focusWindow = async () => { window.genericFocusAttempts++; };
const item = path => ({kind:path.endsWith('/')?'folder':'file',path,name:path.split(/[\\\\/]/).filter(Boolean).pop(),reference_only:true,size_bytes:1});
if(mode !== 'browser') window.aceDesktop_materializeContextItems = async paths => {
 window.materializations.push({paths, activeAtStart:window.nativeActive});
 if(window.materializeDelay) await new Promise(resolve=>{window.finishMaterialization=resolve;window.pendingMaterializations.push(resolve);});
 return {ok:true,items:paths.map(item)};
};
if(mode !== 'browser') window.aceDesktop_readClipboardContextItems = async () => {
 window.clipboardReads++;
 if(window.clipboardError) return {ok:false,error:window.clipboardError};
 return window.clipboardPaths.length ? window.aceDesktop_materializeContextItems([...window.clipboardPaths]) : {ok:true,items:[]};
};
if(mode !== 'browser') window.aceDesktop_storeContextFiles = async files => {
 window.savedFiles.push(...files);
 return {ok:true,items:files.map(file=>item('C:/local-cache/' + file.name))};
};
window.chrome ||= {};
window.chrome.webview = mode === 'windows-legacy' ? {} : {postMessageWithAdditionalObjects:()=>{window.nativePosted=true;}};
window.consoleDrops = [];
window.__aceConsoleAcceptFileDrop = payload => window.consoleDrops.push(payload);
import RefreshRuntime from '/@react-refresh';
RefreshRuntime.injectIntoGlobalHook(window);
window.$RefreshReg$=()=>{};window.$RefreshSig$=()=>type=>type;
window.__vite_plugin_react_preamble_installed__=true;
</script><script type="module" src="/__composer-file-drop.jsx"></script></body></html>`;
const virtual = path.join(web, '__composer-file-drop.jsx').replaceAll('\\', '/');
const server = await createServer({
  root: web, configFile: path.join(web, 'vite.config.js'), logLevel: 'error',
  plugins: [{ name: 'composer-file-drop-fixture', enforce: 'pre',
    resolveId(id) { if (id === '/__composer-file-drop.jsx') return virtual; },
    load(id) {
      const normalized = id.replaceAll('\\', '/');
      if (normalized === virtual) return fixture;
      if (baseline && normalized === inputBarPath) return inputBarBaseline;
    },
    configureServer(vite) { vite.middlewares.use((req, res, next) => {
      if (req.url?.split('?')[0] !== '/__composer-file-drop') return next();
      res.setHeader('Content-Type', 'text/html'); res.end(html);
    }); },
  }], server: { host: '127.0.0.1', port, strictPort: true, hmr: false },
});
let browser;
let failed = 0;
try {
  await server.listen();
  browser = await chromium.launch({ headless: true, ...(process.env.ACE_CHROMIUM_EXECUTABLE ? { executablePath: process.env.ACE_CHROMIUM_EXECUTABLE } : {}) });
  console.log('Browser: ' + browser.version());
  const page = await browser.newPage();
  await page.route('**/api/**', route => route.fulfill({ contentType: 'application/json', body: '{"commands":[],"skills":[]}' }));
  const errors = [];
  page.on('pageerror', error => errors.push(error.message));
  const editor = page.locator('[data-ace-rich-composer]');
  const state = () => page.evaluate(() => ({ text:window.fixture.value, attempts:window.focusAttempts, active:window.nativeActive, general:window.genericFocusAttempts, materializations:window.materializations, uploads:window.uploads }));
  async function reset(mode = 'windows') {
    await page.goto('http://127.0.0.1:' + port + '/__composer-file-drop?mode=' + mode);
    await editor.waitFor(); await editor.click();
  }
  async function drag(types = ['dragenter', 'dragover', 'drop'], uri = '') {
    await editor.evaluate((element, {types, uri}) => {
      const dataTransfer = new DataTransfer();
      dataTransfer.items.add(new File(['sample'], 'notes.txt', {type:'text/plain'}));
      if (uri) dataTransfer.setData('text/uri-list', uri);
      const rect = element.getBoundingClientRect();
      for (const type of types) element.dispatchEvent(new DragEvent(type, {
        bubbles:true, cancelable:true, dataTransfer, clientX:rect.left+20, clientY:rect.top+10,
      }));
    }, {types, uri});
  }
  const accept = paths => page.evaluate(paths => window.__aceComposerAcceptFileDrop(paths), paths);
  const tags = () => editor.locator('[data-composer-inline-tag="path"]');
  async function paste({ paths = [], text = '', uri = '', withFile = true } = {}) {
    await page.evaluate(paths => { window.clipboardPaths = paths; }, paths);
    await editor.evaluate((element, {text, uri, withFile}) => {
      const clipboardData = new DataTransfer();
      if (withFile) clipboardData.items.add(new File(['sample'], 'notes.txt', {type:'text/plain'}));
      if (text) clipboardData.setData('text/plain', text);
      if (uri) clipboardData.setData('text/uri-list', uri);
      element.dispatchEvent(new ClipboardEvent('paste', {bubbles:true,cancelable:true,clipboardData}));
    }, {text, uri, withFile});
  }
  const content = () => page.evaluate(() => window.fixture.content());
  // Slate's native selectionchange listener is throttled by 100 ms.
  const settle = () => page.evaluate(() => new Promise(resolve => setTimeout(() => requestAnimationFrame(resolve), 130)));
  async function seed(text) {
    await page.evaluate(text => window.fixture.seed(text), text);
    await editor.click(); await page.keyboard.press('Control+End');
    await settle();
  }
  async function run(name, test) {
    if (process.env.ACE_COMPOSER_DROP_TEST_FILTER && !name.includes(process.env.ACE_COMPOSER_DROP_TEST_FILTER)) return;
    try { await test(); console.log('[pass] ' + name); }
    catch (error) { failed++; console.error('[FAIL] ' + name + ': ' + error.message); console.error(JSON.stringify(await state())); }
  }
  for (const [os, paths] of [
    ['windows', ['C:\\fixture\\notes.txt', 'C:/fixture/another.txt']],
    ['macos', ['/Users/fixture/notes.txt', '/Users/fixture/another.txt']],
  ]) {
    await run(os + ' native drop retries focus before resolving paths and permits immediate typing', async () => {
      await reset(os); await drag();
      assert.equal((await state()).attempts, 1, 'drag-over must not repeatedly activate');
      await accept(paths); await tags().nth(1).waitFor();
      const accepted = await state();
      assert.equal(accepted.attempts, 2); assert.equal(accepted.active, true);
      assert.equal(accepted.materializations[0].activeAtStart, true);
      const before = accepted.text;
      await page.keyboard.type('abc'); assert.equal((await state()).text, before + 'abc');
      await page.keyboard.press('ArrowLeft'); await page.keyboard.type('X');
      assert.equal((await state()).text, before + 'abXc');
      await accept(paths); assert.equal((await state()).attempts, 2, 'duplicate native callback must not activate again');
      assert.equal((await state()).general, 0, 'file drop must avoid notification foreground fallback');
    });
  }
  const nativeDropAt = (selector, paths) => page.locator(selector).evaluate((element, paths) => {
    const rect = element.getBoundingClientRect();
    return window.__aceRouteNativeFileDrop({paths, location:{
      xRatio:(rect.left + rect.width / 2) / window.innerWidth,
      yRatio:(rect.top + rect.height / 2) / window.innerHeight,
    }});
  }, paths);
  await run('legacy native hover reaches the composer and clears after leaving', async () => {
    await reset('windows-legacy');
    await editor.evaluate(element => {
      const rect = element.getBoundingClientRect();
      window.__aceRouteNativeFileDrag({xRatio:(rect.left+20)/innerWidth,yRatio:(rect.top+10)/innerHeight});
    });
    assert.equal((await state()).attempts, 1);
    await page.waitForFunction(()=>document.querySelector('.ace-composer-card').classList.contains('is-drag-active'));
    assert.equal(await page.locator('.ace-composer-card').evaluate(element=>element.classList.contains('is-drag-active')), true);
    await page.evaluate(()=>window.__aceRouteNativeFileDrag(null));
    await page.waitForFunction(()=>!document.querySelector('.ace-composer-card').classList.contains('is-drag-active'));
    assert.equal(await page.locator('.ace-composer-card').evaluate(element=>element.classList.contains('is-drag-active')), false);
    assert.equal((await state()).materializations.length, 0);
  });
  await run('WebView2 110 native OLE paths create tags without additional objects or DOM hover', async () => {
    await reset('windows-legacy'); await seed('before ');
    assert.equal(await page.evaluate(()=>typeof window.chrome.webview.postMessageWithAdditionalObjects), 'undefined');
    const paths = ['C:/fixture/中文 notes.txt', '//server/share/second.txt', 'C:/fixture/folder/'];
    assert.equal(await nativeDropAt('[data-ace-rich-composer]', paths), true);
    await tags().nth(2).waitFor();
    assert.deepEqual((await content()).parts.filter(part=>part.type==='path').map(part=>part.path), paths);
    assert.equal(await page.evaluate(()=>window.savedFiles.length), 0);
    assert.equal((await state()).uploads.length, 0);
    assert.equal((await state()).materializations.length, 1);
    assert.equal(await page.evaluate(()=>window.consoleDrops.length), 0);
    const before = (await state()).text;
    await page.keyboard.type('next'); assert.equal((await state()).text, before + 'next');
    await page.keyboard.press('ArrowLeft'); await page.keyboard.type('X');
    assert.equal((await state()).text, before + 'nexXt');
  });
  await run('WebView2 110 native drop routes terminal paths once and rejects other controls', async () => {
    await reset('windows-legacy');
    const paths = ['C:/fixture/notes.txt'];
    assert.equal(await nativeDropAt('.ace-console-term', paths), true);
    assert.deepEqual(await page.evaluate(()=>window.consoleDrops), [{paths,nativeLocation:true,tabId:'fixture-console'}]);
    assert.equal(await tags().count(), 0);
    assert.equal((await state()).materializations.length, 0);
    assert.equal(await nativeDropAt('#outside', paths), false);
    await page.evaluate(()=>window.fixture.setDisabled(true));
    await page.waitForFunction(()=>document.querySelector('[data-ace-rich-composer]').getAttribute('aria-disabled')==='true');
    assert.equal(await nativeDropAt('[data-ace-rich-composer]', paths), false);
    assert.equal((await state()).materializations.length, 0);
  });
  await run('asynchronous path resolution never reactivates a window the user left', async () => {
    await reset(); await drag();
    await page.evaluate(()=>{window.materializeDelay=true;});
    await accept(['C:/fixture/slow.txt']);
    assert.equal((await state()).attempts, 2);
    await page.evaluate(()=>{window.nativeActive=false;window.finishMaterialization();});
    await tags().waitFor();
    const result = await state();
    assert.equal(result.attempts, 2); assert.equal(result.active, false);
  });
  await run('unhovered and empty native callbacks do not take focus', async () => {
    await reset(); await accept(['C:/fixture/ignored.txt']);
    assert.equal((await state()).attempts, 0);
    await drag(['dragenter']); await accept([]);
    assert.equal((await state()).attempts, 1); assert.equal((await state()).materializations.length, 0);
  });
  await run('disabling the composer during a native drag rejects the release', async () => {
    await reset(); await drag(['dragenter']);
    await page.evaluate(()=>window.fixture.setDisabled(true));
    await page.waitForFunction(()=>document.querySelector('[data-ace-rich-composer]').getAttribute('aria-disabled')==='true');
    await accept(['C:/fixture/ignored.txt']);
    assert.equal((await state()).attempts, 1); assert.equal((await state()).materializations.length, 0);
  });
  await run('Linux URI drops share acceptance focus without changing the reference', async () => {
    await reset('linux'); await drag(undefined, 'file:///home/fixture/notes.txt');
    await tags().waitFor();
    const result = await state();
    assert.equal(result.attempts, 2); assert.equal(result.materializations[0].paths[0], '/home/fixture/notes.txt');
    await page.keyboard.type('next'); assert.equal((await state()).text, result.text + 'next');
  });
  await run('browser file drops retain uploads and immediate input', async () => {
    await reset('browser'); await drag();
    const result = await state();
    assert.equal(result.attempts, 2); assert.deepEqual(result.uploads, [['notes.txt']]);
    await page.keyboard.type('next'); assert.equal((await state()).text, 'next');
  });
  const samePaths = ['C:\\fixture\\中文 notes.txt', '//server/share/picture.png', 'C:/fixture/folder/'];
  const visuals = async () => tags().evaluateAll(nodes => nodes.map(node => {
    const body = node.querySelector('.ace-cmd-token');
    const style = getComputedStyle(body);
    return {name:body.textContent, radius:style.borderRadius, padding:style.padding, font:style.fontSize, icon:body.querySelector('svg')?.getBoundingClientRect().width};
  }));
  let dropContent, dropVisuals;
  for (const entry of ['drop', 'paste']) {
    await run(entry + ' replaces mixed selection in one undo and keeps the full ordered paths', async () => {
      await reset(); await seed('before @C:/old.txt tail'); await page.keyboard.press('Control+a'); await settle();
      if (entry === 'drop') { await drag(); await accept(samePaths); }
      else await paste({paths:samePaths, text:'this is the alternate text representation'});
      await tags().nth(2).waitFor();
      const result = await content();
      assert.deepEqual(result.parts.filter(part=>part.type==='path').map(part=>part.path), ['C:/fixture/中文 notes.txt', '//server/share/picture.png', 'C:/fixture/folder/']);
      assert.equal((await state()).uploads.length, 0);
      assert.ok(!(await state()).text.includes('alternate'));
      if (entry === 'drop') { dropContent=result; dropVisuals=await visuals(); }
      else { assert.deepEqual(result, dropContent); assert.deepEqual(await visuals(), dropVisuals); }
      if (process.env.ACE_COMPOSER_TRANSFER_SHOT_DIR) {
        await page.screenshot({path:path.join(process.env.ACE_COMPOSER_TRANSFER_SHOT_DIR, entry+'.png')});
      }
      await page.keyboard.press('Control+z');
      assert.equal((await state()).text, 'before @C:/old.txt tail');
      await page.keyboard.press('Control+y'); await settle(); assert.deepEqual(await content(), result);
      await page.keyboard.press('Control+End'); await settle();
      await page.keyboard.type('next'); assert.ok((await state()).text.endsWith('next'));
    });
  }
  await run('native paste falls back to plain text only when there are no filesystem items', async () => {
    await reset(); await seed('hello ');
    await paste({text:'C:\\ordinary text.txt',withFile:false});
    assert.equal((await state()).text, 'hello C:\\ordinary text.txt');
    assert.equal(await tags().count(), 0);
  });
  await run('paste failure preserves selection and never inserts text or uploads', async () => {
    await reset(); await seed('keep me'); await page.keyboard.press('Control+a');
    await page.evaluate(()=>{window.clipboardError='clipboard locked';});
    await paste({text:'must not be inserted'});
    assert.equal((await state()).text, 'keep me'); assert.equal((await state()).uploads.length, 0);
    assert.equal(await page.evaluate(()=>window.savedFiles.length), 0);
  });
  await run('desktop pathless paste saves locally and displays the same filename tag', async () => {
    await reset(); await paste(); await tags().waitFor();
    assert.equal((await content()).parts.find(part=>part.type==='path').path, 'C:/local-cache/notes.txt');
    assert.equal((await state()).uploads.length, 0);
    assert.equal(await page.evaluate(()=>window.savedFiles[0].name), 'notes.txt');
    await page.keyboard.type('abc'); assert.ok((await state()).text.endsWith('abc'));
  });
  await run('browser file paste excludes alternate text and shares drop upload behavior', async () => {
    await reset('browser'); await paste({text:'file representation'});
    assert.deepEqual((await state()).uploads, [['notes.txt']]); assert.equal((await state()).text, '');
    await page.keyboard.type('next'); assert.equal((await state()).text, 'next');
  });
  await run('pending paste follows edits without overwriting new text or moving the new caret', async () => {
    await reset(); await seed('left right');
    await page.keyboard.press('Home'); await settle();
    for(let i=0;i<5;i++) await page.keyboard.press('ArrowRight'); await settle();
    await page.evaluate(()=>{window.materializeDelay=true;});
    await paste({paths:['C:/fixture/slow.txt']});
    await page.keyboard.type('typed '); await settle();
    await page.keyboard.press('Control+Home'); await settle();
    await page.evaluate(()=>window.finishMaterialization()); await tags().waitFor();
    await settle();
    assert.equal((await state()).text, 'left typed @C:/fixture/slow.txt right');
    await settle();
    await page.keyboard.type('start '); assert.ok((await state()).text.startsWith('start left typed '));
  });
  await run('pending file IO never restores focus after another control is focused', async () => {
    await reset(); await page.evaluate(()=>{window.materializeDelay=true;});
    await paste({paths:['C:/fixture/slow.txt']}); await page.locator('#outside').click();
    await page.evaluate(()=>window.finishMaterialization()); await tags().waitFor();
    assert.equal(await page.evaluate(()=>document.activeElement.id), 'outside');
  });
  await run('pending results are discarded after a session switch or draft clear', async () => {
    for (const action of ['session', 'workspace', 'clear']) {
      await reset(); await seed('old'); await page.evaluate(()=>{window.materializeDelay=true;});
      await paste({paths:['C:/fixture/stale.txt']});
      await page.evaluate(action=>{if(action==='session')window.fixture.setSessionId('second');else if(action==='workspace')window.fixture.setCwd('C:/other');else window.fixture.seed('new draft');}, action);
      await settle();
      await page.evaluate(()=>window.finishMaterialization());
      assert.equal(await tags().count(), 0); assert.ok(!(await state()).text.includes('stale'));
    }
  });
  await run('file requests finishing out of order commit in gesture order', async () => {
    await reset(); await page.evaluate(()=>{window.materializeDelay=true;});
    await paste({paths:['C:/fixture/first.txt']});
    await paste({paths:['C:/fixture/second.txt']});
    await page.evaluate(()=>window.pendingMaterializations[1]());
    assert.equal(await tags().count(), 0);
    await page.evaluate(()=>window.pendingMaterializations[0]()); await tags().nth(1).waitFor();
    assert.deepEqual((await content()).parts.filter(part=>part.type==='path').map(part=>part.path), ['C:/fixture/first.txt','C:/fixture/second.txt']);
  });
  for (const entry of ['drop', 'paste']) {
    await run(entry + ' replaces a selection containing a zero-text attachment and undo restores it', async () => {
      await reset();
      await page.evaluate(()=>window.fixture.seedContent({version:1,parts:[
        {type:'text',text:'before '},
        {type:'attachment',key:'local-pdf',name:'old.pdf',kind:'file'},
        {type:'text',text:' after'},
      ]},[{local_id:'local-pdf',name:'old.pdf',kind:'file'}]));
      await settle(); await editor.click(); await page.keyboard.press('Control+a'); await settle();
      const before=await content();
      if(entry==='drop'){await drag();await accept(['C:/fixture/new.txt']);}
      else await paste({paths:['C:/fixture/new.txt']});
      await tags().waitFor();
      assert.deepEqual((await content()).parts.map(part=>part.type), ['path','text']);
      await page.keyboard.press('Control+z'); await settle(); assert.deepEqual(await content(),before);
    });
  }
  assert.deepEqual(errors, [], 'no browser exceptions');
} finally {
  await browser?.close(); await server.close();
}
if (failed) process.exitCode = 1;
