import assert from 'node:assert/strict';
import os from 'node:os';
import path from 'node:path';
import { mkdtemp } from 'node:fs/promises';
import { spawn } from 'node:child_process';
import { createServer as createNetServer } from 'node:net';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { createServer, transformWithEsbuild } from 'vite';

const web = fileURLToPath(new URL('../', import.meta.url));
const { chromium } = await import(process.env.ACE_PLAYWRIGHT_MODULE
  ? pathToFileURL(path.resolve(process.env.ACE_PLAYWRIGHT_MODULE)).href : 'playwright');
// ACE_FOCUS_NATIVE_HOST reuses tests/desktop/fixtures/window_focus_host.cpp.
// CDP drives the browser's IME implementation; this is not an OS candidate-picker test.
const entry = `
import React, {useEffect, useRef, useState} from 'react';
import {createRoot} from 'react-dom/client';
import {RichComposer} from '/src/components/RichComposer.jsx';
import {bindDesktopComposerAutoFocus} from '/src/lib/composerCaretRestore.js';
import '/src/styles/globals.css';
function App() {
  const composer = useRef(null);
  const [value, setValue] = useState('tail');
  window.fixture = {composer, setValue};
  useEffect(() => bindDesktopComposerAutoFocus({enabled:true,
    onFocus: canFocus => { window.focusCalls++; composer.current?.focus({canFocus}); }
  }), []);
  return <main style={{padding:40}}><button id="other">Other</button>
    <RichComposer ref={composer} value={value} onChange={setValue}
      className="ace-rich-composer-input" style={{minHeight:80}} aria-label="Chat"/>
  </main>;
}
window.focusCalls = 0;
window.activate = () => window.dispatchEvent(new Event('acecode:desktop-window-focus'));
createRoot(document.getElementById('root')).render(<App/>);
`;
const html = '<html><head><meta charset="UTF-8"/></head><body><div id="root"></div><script type="module" src="/@id/virtual:focus-selection"></script></body></html>';
const server = await createServer({root:web, logLevel:'error', server:{host:'127.0.0.1',port:0,hmr:false},
  plugins:[{name:'focus-selection-fixture',enforce:'pre',
    resolveId(id) { if (id === 'virtual:focus-selection') return '\0virtual:focus-selection'; },
    async load(id) {
      if (id === '\0virtual:focus-selection') return (await transformWithEsbuild(entry,'fixture.jsx',{loader:'jsx'})).code;
    },
    transform(code,id) {
      // Read-only probes for the actual editor; production exports stay unchanged.
      if (id.replaceAll('\\','/').endsWith('/components/RichComposer.jsx')) {
        return code.replace('const editableRef = useRef(null);',
          'const editableRef = useRef(null); window.composerProbe = {editor, renderPendingRef, compositionState: () => compositionStateRef.current};');
      }
    },
    configureServer(vite) { vite.middlewares.use('/focus-selection', async (_req,res) => {
      res.setHeader('Content-Type','text/html'); res.end(await vite.transformIndexHtml('/focus-selection',html));
    }); },
  }],
});
let browser;
let native;
try {
  await server.listen();
  const url = `http://127.0.0.1:${server.httpServer.address().port}/focus-selection`;
  let page;
  if (process.env.ACE_FOCUS_NATIVE_HOST) {
    const portServer=createNetServer(); await new Promise(resolve=>portServer.listen(0,'127.0.0.1',resolve));
    const port=portServer.address().port; await new Promise(resolve=>portServer.close(resolve));
    const profile=await mkdtemp(path.join(os.tmpdir(),'ace-caret-webview-'));
    native=spawn(process.env.ACE_FOCUS_NATIVE_HOST,[url],{windowsHide:true,stdio:'pipe',env:{...process.env,
      WEBVIEW2_USER_DATA_FOLDER:profile,WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS:'--remote-debugging-port='+port}});
    for(let attempt=0;attempt<100;attempt++) {
      try {browser=await chromium.connectOverCDP('http://127.0.0.1:'+port);break;} catch {await new Promise(resolve=>setTimeout(resolve,100));}
    }
    if(!browser) throw new Error('Native WebView debug endpoint unavailable');
    page=browser.contexts()[0].pages()[0];
    await page.waitForFunction(()=>typeof window.aceFocusTest==='function');
    await page.evaluate(()=>window.aceFocusTest('show'));
  } else {
    browser = await chromium.launch({headless:true,executablePath:process.env.ACE_CHROMIUM_EXECUTABLE || undefined});
    page = await browser.newPage();
  }
  const errors = [];
  page.on('pageerror', error => errors.push(String(error)));
  const editor = page.locator('[data-ace-rich-composer]');
  const cdp = await page.context().newCDPSession(page);
  const state = () => page.evaluate(() => {
    const el = document.querySelector('[data-ace-rich-composer]');
    const selection = getSelection();
    const offset = nodeOffset => {
      const range = document.createRange(); range.selectNodeContents(el);
      range.setEnd(selection[nodeOffset+'Node'],selection[nodeOffset+'Offset']); return range.toString().length;
    };
    return {text:fixture.composer.current.value, dom:el.textContent,
      model:composerProbe.editor.selection,
      anchor:offset('anchor'),focus:offset('focus'),
      pending:composerProbe.renderPendingRef.current,
      focusCalls:window.focusCalls, composition:composerProbe.compositionState()};
  });
  const reset = async () => {
    await page.goto(`http://127.0.0.1:${server.httpServer.address().port}/focus-selection`);
    await editor.click(); await page.keyboard.press('End'); await page.waitForTimeout(150);
    await page.evaluate(() => {window.focusCalls=0;});
  };
  const compose = text => cdp.send('Input.imeSetComposition',{text,selectionStart:text.length,selectionEnd:text.length});
  const commit = text => cdp.send('Input.insertText',{text});
  const outcomes=[];
  for (const variant of ['normal','focus-before-composition','focus-during-composition','drag-then-focus',
    'imperative-focus','selected-range', ...(native ? ['window-return'] : [])]) {
    await reset();
    if (variant==='window-return') {
      await page.evaluate(()=>window.aceFocusTest('away'));
      await page.waitForTimeout(80);
      await page.evaluate(()=>window.aceFocusTest('return'));
      await page.waitForTimeout(150);
    }
    if (variant==='drag-then-focus') await editor.dispatchEvent('dragstart',{dataTransfer:await page.evaluateHandle(()=>new DataTransfer())});
    await page.keyboard.press('Home');
    if (variant==='selected-range') {
      await page.keyboard.press('Shift+ArrowRight');
      await page.keyboard.press('Shift+ArrowRight');
    }
    if (variant==='focus-before-composition') {
      await page.evaluate(()=>window.activate()); await page.waitForTimeout(40);
    }
    await compose('中文');
    if (variant==='focus-during-composition'||variant==='drag-then-focus'||variant==='selected-range') await page.evaluate(()=>window.activate());
    if (variant==='imperative-focus') await page.evaluate(()=>fixture.composer.current.focus());
    await page.waitForTimeout(140);
    const during=await state();
    await commit('中文'); await page.waitForTimeout(150);
    await page.keyboard.type('abc'); await page.waitForTimeout(150);
    const after=await state();
    outcomes.push({variant,during,after});
    assert.equal(during.text,variant==='selected-range'?'il':'tail',variant+': activation must not commit the IME candidate');
    assert.equal(during.composition.active,true,variant+': composition remains owned by the IME');
    assert.equal(after.text,variant==='selected-range'?'中文abcil':'中文abctail',variant+': committed text must not duplicate or move');
    assert.equal(after.anchor,5,variant+': visible caret follows the typed text');
    assert.equal(after.focus,after.model.focus.offset,variant+': DOM caret matches Slate insertion point');
  }
  assert.deepEqual(errors,[]);
  console.log(JSON.stringify({result:'passed',native:!!native,scenarios:outcomes.map(result=>result.variant)}));
  if(native) await page.evaluate(()=>window.aceFocusTest('done',true,['caret diagnostic complete'])).catch(()=>{});
} finally { await browser?.close(); native?.kill(); await server.close(); }
