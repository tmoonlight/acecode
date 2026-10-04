import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { mkdir, writeFile } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { createServer, transformWithEsbuild } from 'vite';

// Production InputBar/context menu and workspace modal; no daemon or account.
// ACE_PLAYWRIGHT_MODULE and ACE_CHROMIUM_EXECUTABLE select optional local tools.
// ACE_CONTEXT_TEST_OUTPUT saves results/screenshots; ACE_CONTEXT_TEST_FILTER filters names.
// --baseline loads the original menu/picker to reproduce the two reported bugs.
const web = fileURLToPath(new URL('../', import.meta.url));
const { chromium } = await import(process.env.ACE_PLAYWRIGHT_MODULE
  ? pathToFileURL(process.env.ACE_PLAYWRIGHT_MODULE).href : 'playwright');
const output = process.env.ACE_CONTEXT_TEST_OUTPUT;
const filter = process.env.ACE_CONTEXT_TEST_FILTER ? new RegExp(process.env.ACE_CONTEXT_TEST_FILTER) : null;
const baseline = process.argv.includes('--baseline');
const sources = new Map(baseline ? ['DesktopContextMenu.jsx', 'WorkspaceIconPicker.jsx'].map(name => [
  path.join(web, 'src/components', name).replaceAll('\\', '/'),
  execFileSync('git', ['show', `HEAD:web/src/components/${name}`], { cwd: web, encoding: 'utf8' }),
]) : []);
const entry = `
import React, {useRef, useState} from 'react';
import {createRoot} from 'react-dom/client';
import {InputBar} from '/src/components/InputBar.jsx';
import {DesktopContextMenu} from '/src/components/DesktopContextMenu.jsx';
import {EditWorkspaceModal} from '/src/components/EditWorkspaceModal.jsx';
import {SlashCommandsProvider} from '/src/components/SlashCommandsContext.jsx';
import {Toaster} from '/src/components/Toast.jsx';
import {appendComposerImageAttachments} from '/src/lib/composerImagePresentation.js';
import {composerContentText} from '/src/lib/composerContent.js';
import {applyLocalePreference} from '/src/i18n/index.js';
import '/src/styles/globals.css';
await applyLocalePreference('zh-CN');
window.uploads=[];window.saved=[];window.modalClosed=0;window.nativeReads=0;
if(new URLSearchParams(location.search).get('mode') === 'desktop'){
 window.__ACECODE_DESKTOP_SHELL__=true;
 window.aceDesktop_readClipboardContextItems=async()=>{
  window.nativeReads++;
  return window.nativeResult || {ok:true,filesystem_items:false,items:[]};
 };
}
function Fixture(){
 const ref=useRef();
 const [content,setContent]=useState({version:1,parts:[]});
 const [attachments,setAttachments]=useState([]);
 const [sid,setSid]=useState('first');
 const [disabled,setDisabled]=useState(false);
 const [modal,setModal]=useState(false);
 window.fixture={setSid,setDisabled,content:()=>ref.current.getComposerContent(),
  seed:text=>ref.current.replaceText(text),
  seedContent:(next,records=[])=>{setAttachments(records);ref.current.setComposerContent(next);}};
 const upload=files=>{
  const resources=files.map((file,index)=>({id:'image-'+(window.uploads.length+index),kind:'image',name:file.name,mime_type:file.type,url:URL.createObjectURL(file)}));
  for(const file of files) window.uploads.push({file,type:file.type,size:file.size,name:file.name});
  setAttachments(previous=>[...previous,...resources]);
  setContent(previous=>appendComposerImageAttachments(previous,resources));
 };
 return <main style={{padding:64}}>
  <button onClick={()=>setModal(true)}>Open workspace</button>
  <input aria-label="Outside input"/>
  <SlashCommandsProvider><InputBar ref={ref} value={composerContentText(content)} composerContent={content}
   attachments={attachments} currentSessionId={sid} cwd="C:/fixture" disabled={disabled}
   onChange={(_text,next)=>setContent(next)} onMediaFiles={upload}/></SlashCommandsProvider>
  {modal && <EditWorkspaceModal workspace={{hash:'fixture',name:'shz_test',cwd:'C:/fixture',icon:{id:'folder',color:'pink'}}}
   api={{updateWorkspace:async(_hash,payload)=>{window.saved.push(payload);return payload;}}}
   onClose={()=>{window.modalClosed++;setModal(false);}}/>}
  <DesktopContextMenu/><Toaster/>
 </main>;
}
createRoot(document.getElementById('root')).render(<Fixture/>);
`;
const server = await createServer({
  root: web, configFile: path.join(web, 'vite.config.js'), logLevel: 'error',
  plugins: [{ name: 'context-paste-fixture', enforce: 'pre',
    resolveId(id) { if (id === 'virtual:context-fixture') return '\0virtual:context-fixture'; },
    async load(id) {
      if (id === '\0virtual:context-fixture') return (await transformWithEsbuild(entry, 'fixture.jsx', { loader: 'jsx', jsx: 'automatic' })).code;
      return sources.get(id.replaceAll('\\', '/'));
    },
    configureServer(vite) {
      vite.middlewares.use('/context-fixture', async (_req, res) => {
        res.setHeader('Content-Type', 'text/html');
        res.end(await vite.transformIndexHtml('/context-fixture', '<!doctype html><html><head><meta charset="UTF-8"/></head><body><div id="root"></div><script type="module" src="/@id/virtual:context-fixture"></script></body></html>'));
      });
    },
  }], server: { host: '127.0.0.1', port: 0 },
});
let browser;
const results = [], errors = [];
try {
  await server.listen();
  browser = await chromium.launch({ headless: true, executablePath: process.env.ACE_CHROMIUM_EXECUTABLE || undefined });
  const page = await browser.newPage({ viewport: { width: 1100, height: 760 }, permissions: ['clipboard-read', 'clipboard-write'] });
  page.on('pageerror', error => { errors.push(error.message); console.error('[browser] '+error.message); });
  await page.route('**/api/**', route => route.fulfill({ contentType: 'application/json', body: '{"commands":[],"skills":[],"models":[]}' }));
  const editor = page.locator('[data-ace-rich-composer]');
  const settle = () => page.waitForTimeout(180);
  const state = () => page.evaluate(() => ({ content:window.fixture.content(),uploads:window.uploads.map(({file,...rest})=>rest),nativeReads:window.nativeReads }));
  const reset = async (mode='browser') => {
    await page.goto(`http://127.0.0.1:${server.httpServer.address().port}/context-fixture?mode=${mode}`);
    await editor.waitFor(); await settle();
  };
  const seed = async text => { await page.evaluate(text=>window.fixture.seed(text),text); await editor.click();await page.keyboard.press('Control+End');await settle(); };
  const contextPaste = async (target=editor) => {
    await target.click({button:'right'});await page.getByRole('menuitem',{name:'粘贴',exact:true}).click();await settle();
  };
  const writeImage = async () => page.evaluate(async()=>{
    const canvas=document.createElement('canvas');canvas.width=4;canvas.height=3;
    canvas.getContext('2d').fillRect(0,0,4,3);
    const png=await new Promise(resolve=>canvas.toBlob(resolve,'image/png'));
    await navigator.clipboard.write([new ClipboardItem({'image/png':png,'text/plain':new Blob(['alternate image text'],{type:'text/plain'})})]);
  });
  const deferRead = async () => page.evaluate(()=>{
    const read=navigator.clipboard.read.bind(navigator.clipboard);
    Object.defineProperty(navigator.clipboard,'read',{configurable:true,value:async()=>{
      const items=await read();await new Promise(resolve=>{window.finishRead=resolve;});return items;
    }});
  });
  async function run(name,test){
    if(filter && !filter.test(name))return;
    try{await test();results.push({name,ok:true});console.log('[pass] '+name);}
    catch(error){results.push({name,ok:false,error:error.message});console.error('[FAIL] '+name+': '+error.message);}
  }
  await run('real clipboard PNG produces the same thumbnail via Ctrl+V and context Paste',async()=>{
    const observed=[];
    for(const mode of ['browser','desktop'])for(const method of ['keyboard','context']){
      await reset(mode);await seed('before');await writeImage();
      if(method==='keyboard')await page.keyboard.press('Control+v');else await contextPaste();
      await page.locator('[data-composer-image-preview="true"] img').waitFor({timeout:5000});
      const current=await state();assert.equal(current.uploads.length,1);assert.equal(current.uploads[0].type,'image/png');
      assert.equal(composerText(current),'before');
      const pixels=await page.evaluate(async()=>{
        const file=window.uploads[0].file;const bitmap=await createImageBitmap(file);
        const canvas=document.createElement('canvas');canvas.width=bitmap.width;canvas.height=bitmap.height;
        canvas.getContext('2d').drawImage(bitmap,0,0);bitmap.close();return canvas.toDataURL();
      });
      observed.push(pixels);
      if(output && method==='context') {await mkdir(output,{recursive:true});await page.screenshot({path:path.join(output,mode+'-image-paste.png')});}
      await page.keyboard.type(' after');assert.equal(composerText(await state()),'before after');
    }
    assert(observed.every(value=>value===observed[0]));
  });
  await run('context Paste preserves text selection and undo',async()=>{
    await reset();await seed('replace me');await page.evaluate(()=>navigator.clipboard.writeText('new text'));
    await page.keyboard.press('Control+a');await settle();await contextPaste();
    assert.equal(composerText(await state()),'new text');await page.keyboard.press('Control+z');await settle();
    assert.equal(composerText(await state()),'replace me');
  });
  await run('context Paste replaces exactly one zero-text attachment',async()=>{
    await reset();await page.evaluate(()=>window.fixture.seedContent({version:1,parts:[
      {type:'attachment',key:'one',id:'one',name:'one.txt',kind:'file'},
      {type:'attachment',key:'two',id:'two',name:'two.txt',kind:'file'},
    ]},[{id:'one',name:'one.txt',kind:'file'},{id:'two',name:'two.txt',kind:'file'}]));
    const first=editor.locator('[data-composer-inline-tag="attachment"]').first();await first.click();await settle();
    await page.evaluate(()=>navigator.clipboard.writeText('replacement'));await contextPaste(first);
    const parts=(await state()).content.parts;
    assert.equal(parts.filter(part=>part.type==='attachment').length,1);assert.equal(parts.find(part=>part.type==='attachment').id,'two');
    assert.equal(composerText(await state()),'replacement');
  });
  await run('native file Paste remains available when browser clipboard reads fail',async()=>{
    await reset('desktop');await seed('before ');
    await page.evaluate(()=>{
      for(const key of ['read','readText'])Object.defineProperty(navigator.clipboard,key,{value:async()=>{throw Error('denied');}});
      window.nativeResult={ok:true,filesystem_items:true,items:[{kind:'file',path:'C:/fixture/notes.txt',name:'notes.txt',reference_only:true,size_bytes:1}]};
    });
    await contextPaste();await editor.locator('[data-composer-inline-tag="path"]').waitFor();
    const pasted=await state();
    assert.equal(pasted.nativeReads,1);
    assert.equal(pasted.content.parts.find(part=>part.type==='path')?.path,'C:/fixture/notes.txt');
    assert.equal(pasted.uploads.length,0);
  });
  for(const action of ['session','disabled','reset'])await run('delayed image Paste is discarded after '+action,async()=>{
    await reset();await seed('before');await writeImage();await deferRead();await contextPaste();
    await page.waitForFunction(()=>typeof window.finishRead==='function');
    await page.evaluate(action=>{
      if(action==='session')window.fixture.setSid('second');
      if(action==='disabled')window.fixture.setDisabled(true);
      if(action==='reset')window.fixture.seed('new draft');
    },action);await settle();await page.evaluate(()=>window.finishRead());await settle();
    assert.equal((await state()).uploads.length,0);
  });
  await run('delayed text Paste tracks intervening edits without moving focus',async()=>{
    await reset();await seed('before');await page.evaluate(()=>navigator.clipboard.writeText('PASTE'));await deferRead();await contextPaste();
    await page.waitForFunction(()=>typeof window.finishRead==='function');
    await page.keyboard.press('Control+Home');await settle();await page.keyboard.type('X');await settle();
    await page.getByRole('textbox',{name:'Outside input'}).click();
    await page.evaluate(()=>window.finishRead());await settle();assert.equal(composerText(await state()),'XbeforePASTE');
    assert(await page.getByRole('textbox',{name:'Outside input'}).evaluate(el=>document.activeElement===el));
  });
  await run('empty clipboard keeps the selection and denied reads report an error',async()=>{
    await reset();await seed('keep');await page.keyboard.press('Control+a');await settle();
    await page.evaluate(()=>Object.defineProperty(navigator.clipboard,'read',{configurable:true,value:async()=>[]}));
    await contextPaste();assert.equal(composerText(await state()),'keep');
    await page.evaluate(()=>{
      for(const key of ['read','readText'])Object.defineProperty(navigator.clipboard,key,{configurable:true,value:async()=>{throw Error('test clipboard denied');}});
    });
    await contextPaste();await page.getByRole('status').filter({hasText:'test clipboard denied'}).waitFor();
    assert.equal(composerText(await state()),'keep');
  });
  await run('ordinary input retains its text-only context Paste',async()=>{
    await reset();const input=page.getByRole('textbox',{name:'Outside input'});
    await input.fill('');await page.evaluate(()=>navigator.clipboard.writeText('ordinary'));
    await contextPaste(input);assert.equal(await input.inputValue(),'ordinary');
  });
  const picker=page.locator('.ace-workspace-icon-picker');
  const openModal=async()=>{await reset();await page.getByRole('button',{name:'Open workspace'}).click();};
  const openPicker=async()=>{await page.getByRole('button',{name:'选择图标'}).click();await picker.waitFor();};
  await run('icon picker closes on outside input or backdrop without losing the modal draft',async()=>{
    await openModal();const name=page.getByRole('textbox',{name:'项目名称'});await name.fill('unsaved');await openPicker();
    await name.click();await picker.waitFor({state:'hidden',timeout:3000});
    assert.equal(await name.inputValue(),'unsaved');assert(await name.evaluate(el=>document.activeElement===el));
    await openPicker();await page.mouse.click(10,10);await picker.waitFor({state:'hidden',timeout:3000});
    assert.equal(await name.inputValue(),'unsaved');assert.equal(await page.evaluate(()=>window.modalClosed),0);
  });
  await run('icon picker internal search and selection remain open, trigger and Escape close only once',async()=>{
    await openModal();await openPicker();
    await picker.getByRole('button').nth(2).click();assert(await picker.isVisible());
    await picker.getByRole('button').nth(10).click();assert(await picker.isVisible());
    await picker.getByRole('textbox').fill('folder');assert(await picker.isVisible());
    await page.getByRole('button',{name:'选择图标'}).click();assert.equal(await picker.count(),0);
    await openPicker();await page.keyboard.press('Escape');assert.equal(await picker.count(),0);
    assert.equal(await page.evaluate(()=>window.modalClosed),0);
    assert(await page.getByRole('button',{name:'选择图标'}).evaluate(el=>document.activeElement===el));
    await page.getByRole('button',{name:'保存',exact:true}).click();await settle();assert.equal(await page.evaluate(()=>window.saved.length),1);
  });
  await run('no browser runtime errors',async()=>assert.deepEqual(errors,[]));
} finally {
  if(output){await mkdir(output,{recursive:true});await writeFile(path.join(output,baseline?'baseline.json':'results.json'),JSON.stringify(results,null,2));}
  await browser?.close();await server.close();
}
console.log(JSON.stringify({passed:results.filter(result=>result.ok).length,failed:results.filter(result=>!result.ok).length}));
if(results.some(result=>!result.ok))process.exitCode=1;
function composerText(state){return state.content.parts.filter(part=>part.type==='text').map(part=>part.text).join('');}
