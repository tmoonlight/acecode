import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { createServer } from 'vite';

const web = fileURLToPath(new URL('../', import.meta.url));
const { chromium } = await import(process.env.ACE_PLAYWRIGHT_MODULE
  ? pathToFileURL(path.resolve(process.env.ACE_PLAYWRIGHT_MODULE)).href : 'playwright');
const output = await fs.mkdtemp(path.join(os.tmpdir(), 'ace-preview-presentation-'));
const fixture = String.raw`
import React,{useState,useCallback} from 'react';
import {createRoot} from 'react-dom/client';
import {PreviewDetailsPanel} from '/src/components/PreviewDetailsPanel.jsx';
import {DesktopContextMenu} from '/src/components/DesktopContextMenu.jsx';
import {usePreviewPresentation} from '/src/lib/usePreviewPresentation.js';
import {useAppShortcuts} from '/src/lib/useAppShortcuts.js';
import {mergeNextValue} from '/src/lib/usePreference.js';
import {sessionWorkbench} from '/src/lib/sessionWorkbench.js';
import {PREVIEW_TAB_TYPES} from '/src/lib/previewTabs.js';
import '/src/styles/globals.css';
window.__ACECODE_DESKTOP_SHELL__=true;
const code=Array.from({length:130},(_,i)=>'const line'+i+' = "detail text and long source line '.repeat(i===3?12:1)+'";').join('\n');
const markdown='# Preview heading\n\nEditable Markdown text.\n\n'+String.fromCharCode(96).repeat(3)+'js\nconst example = 42;\n'+String.fromCharCode(96).repeat(3)+'\n\n- List item\n';
const textFor=p=>p.endsWith('.md')?markdown:code;
let reads=0,saves=0;
const api={listFiles:async()=>[],readFile:async(_c,p)=>{reads++;if(p==='error.txt')throw Error('fixture error');if(p==='loading.txt')return new Promise(()=>{});return textFor(p);},
 readEditableFile:async(_c,p)=>{reads++;if(p==='error.txt')throw Error('fixture error');if(p==='loading.txt')return new Promise(()=>{});return {text:textFor(p),read_id:'v1'};},
 readFileBlob:async(_c,p)=>p.endsWith('.png')?new Blob(['<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100"><rect width="100" height="100" fill="skyblue"/></svg>'],{type:'image/svg+xml'}):new Blob(['fixture']),
 saveEditableFile:async()=>{saves++;return{};}};
const defaults={sidebarCollapsed:false,sidePanelCollapsed:false,sidePanelListCollapsed:false,sidePanelMaximized:false,fontSize:'medium'};
function Fixture(){
 const [base,setBase]=useState(defaults),[owner,setOwner]=useState('preview-test'),[visible,setVisible]=useState(true),[tab,setTab]=useState({key:'source',type:PREVIEW_TAB_TYPES.FILE,path:'source.js'}),[dialog,setDialog]=useState(false);
 const save=useCallback(updater=>setBase(prev=>mergeNextValue(prev,updater)),[]);
 const state=usePreviewPresentation(owner,base,save,visible);
 const {uiPrefs:p,setUiPrefs:set,active,toggle}=state;
 useAppShortcuts({previewPresentation:toggle});
 const change=(key)=>set(old=>({...old,[key]:!old[key]}));
 window.fixture={get prefs(){return p},get base(){return base},get active(){return active},get tab(){return tab},get reads(){return reads},get saves(){return saves},
  reset:value=>{setBase({...defaults,...value});setVisible(true);},owner:setOwner,visible:setVisible,dialog:setDialog,toggle,
  select:(p)=>setTab({key:p,type:PREVIEW_TAB_TYPES.FILE,path:p}),
  readonly:()=>setTab(t=>({...t,edit:{...t.edit,editing:false}})),
  diff:()=>setTab({key:'diff',type:PREVIEW_TAB_TYPES.SESSION_CHANGES,expandedFile:'source.js'}),
  zoom:value=>sessionWorkbench.set(owner,'previewTextZoom',value,1),
  prefs:set,
 };
 return <><header style={{height:38,display:'flex',gap:10}}>
  <button id="left-toggle" onClick={()=>change('sidebarCollapsed')}>会话列表</button>
  <button id="right-toggle" onClick={()=>change('sidePanelListCollapsed')}>文件列表</button>
  <button id="show" onClick={()=>setVisible(true)}>打开详情</button>
 </header><main style={{display:'flex',height:'calc(100vh - 38px)'}}>
 {!p.sidebarCollapsed&&<aside id="left" style={{width:140}}>Session list</aside>}
 <section id="chat" style={{display:p.sidePanelMaximized&&visible?'none':'block',width:180}}>Chat text</section>
 {visible&&<section id="detail" style={{display:'flex',flex:1,minWidth:0}}>
  <PreviewDetailsPanel owner={owner} api={api} cwd="C:/fixture" tabs={[tab]} activeTab={tab}
   changeGroups={[{file:'source.js',totalAdditions:1,totalDeletions:1,hunks:[{old_start:1,old_count:1,new_start:1,new_count:1,lines:[{kind:'removed',text:'old text'},{kind:'added',text:'new text'}]}]}]}
   maximized={p.sidePanelMaximized} presenting={active} onTogglePresentation={toggle}
   sidePanelListCollapsed={p.sidePanelListCollapsed} onToggleSidePanelList={()=>change('sidePanelListCollapsed')}
   onToggleMaximize={()=>change('sidePanelMaximized')} onHide={()=>setVisible(false)} onCloseTab={()=>setVisible(false)}
   onEditFileTab={(_key,patch)=>setTab(t=>({...t,edit:{...t.edit,...patch}}))} />
 </section>}
 {!p.sidePanelListCollapsed&&<aside id="right" style={{width:140}}>File list</aside>}
 </main>{dialog&&<div role="dialog">Blocking dialog</div>}<DesktopContextMenu/></>;
}
createRoot(document.getElementById('root')).render(<Fixture/>);
`;
const virtual = path.join(web, '__preview-presentation.jsx').replaceAll('\\', '/');
const html = `<html data-theme="light" data-color-theme="blue"><body style="margin:0"><div id="root"></div><script type="module">
import RefreshRuntime from '/@react-refresh';RefreshRuntime.injectIntoGlobalHook(window);
window.$RefreshReg$=()=>{};window.$RefreshSig$=()=>type=>type;window.__vite_plugin_react_preamble_installed__=true;
</script><script type="module" src="/__preview-presentation.jsx"></script></body></html>`;
const server = await createServer({ root:web,configFile:path.join(web,'vite.config.js'),logLevel:'error',
 plugins:[{name:'preview-presentation-fixture',enforce:'pre',
  resolveId(id){if(id==='/__preview-presentation.jsx')return virtual;},
  async load(id){
   if(id.replaceAll('\\','/')===virtual)return fixture;
   // Keep the real sandbox/channel/component; deterministic slide rendering
   // avoids making event-routing coverage depend on an external PPTX file.
   if(id.endsWith('/pptx2html.full.min.js?raw'))return 'export default '+JSON.stringify(
    await fs.readFile(id.slice(0,-4),'utf8')+'\nwindow.pptx2html=(buffer,root)=>{const slide=document.createElement("section");slide.tabIndex=0;slide.style.width="720px";slide.style.height="450px";slide.textContent="Slide fixture";root.append(slide);};');
  },
  configureServer(vite){vite.middlewares.use((req,res,next)=>{
   if(req.url?.split('?')[0]!=='/__preview-presentation')return next();
   res.setHeader('Content-Type','text/html');res.end(html);
  });},
 }],server:{host:'127.0.0.1',port:0,hmr:false},
});
let browser;
try {
 await server.listen();
 browser=await chromium.launch({headless:true,...(process.env.ACE_CHROMIUM_EXECUTABLE?{executablePath:process.env.ACE_CHROMIUM_EXECUTABLE}:{})});
 const page=await browser.newPage({viewport:{width:1360,height:850}});
 const errors=[];page.on('pageerror',error=>{errors.push(error.message);console.error('[pageerror]',error.message);});
 await page.goto(`http://127.0.0.1:${server.httpServer.address().port}/__preview-presentation`);
 const editor=page.locator('.ace-file-text-editor');await editor.waitFor();
 const settle=()=>page.evaluate(()=>new Promise(r=>requestAnimationFrame(()=>requestAnimationFrame(r))));
 const state=()=>page.evaluate(()=>({prefs:window.fixture.prefs,base:window.fixture.base,active:window.fixture.active}));
 const press=async()=>{await page.keyboard.press('Control+F11');await settle();};
 for(const sidebarCollapsed of [false,true])for(const sidePanelListCollapsed of [false,true])for(const sidePanelMaximized of [false,true]){
  await page.evaluate(p=>window.fixture.reset(p),{sidebarCollapsed,sidePanelListCollapsed,sidePanelMaximized});await settle();
  const before=await state();await press();
  assert.equal((await state()).active,true);assert.equal(await page.locator('#left').count(),0);assert.equal(await page.locator('#right').count(),0);assert.equal(await page.locator('#chat').isVisible(),false);
  assert.deepEqual((await state()).base,before.base);await press();assert.deepEqual(await state(),before);
 }
 console.log('[pass] eight initial panel combinations restore exactly without persisting presentation');
 await page.evaluate(()=>window.fixture.reset({}));await settle();
 await press();await page.locator('#left-toggle').click();await page.getByRole('button',{name:'展开列表面板',exact:true}).click();
 assert.equal(await page.locator('#left').count(),1);assert.equal(await page.locator('#right').count(),1);
 await page.getByRole('button',{name:'还原预览面板',exact:true}).click();assert.equal(await page.locator('#chat').isVisible(),true);
 await press();assert.equal((await state()).active,false);
 await press();await page.evaluate(()=>window.fixture.owner('different-owner'));await settle();assert.equal((await state()).active,false);assert.equal(await page.locator('#left').count(),1);
 await press();await page.getByRole('button',{name:'隐藏详情面板',exact:true}).click();await settle();assert.equal((await state()).active,false);
 await press();assert.equal((await state()).active,false);await page.locator('#show').click();await editor.waitFor();
 console.log('[pass] manual controls, owner change, hiding details and missing-detail shortcut boundaries');
 await page.evaluate(()=>window.fixture.dialog(true));await settle();await press();assert.equal((await state()).active,false);
 await page.evaluate(()=>window.fixture.dialog(false));await settle();await press();
 await page.evaluate(()=>window.dispatchEvent(new KeyboardEvent('keydown',{key:'F11',code:'F11',ctrlKey:true,repeat:true,bubbles:true,cancelable:true})));await settle();assert.equal((await state()).active,true);await press();
 await page.evaluate(()=>window.dispatchEvent(new KeyboardEvent('keydown',{key:'F11',ctrlKey:true,isComposing:true,bubbles:true})));await settle();assert.equal((await state()).active,false);
 console.log('[pass] modal, autorepeat and composition guards');
 await editor.fill('first line\nsecond edited line\nthird line');await editor.evaluate(e=>{e.focus();e.setSelectionRange(2,8);window.savedEditor=e;});
 const reads=await page.evaluate(()=>window.fixture.reads);
 const sizes=()=>page.evaluate(()=>Object.fromEntries(['#chat','#left','#right','.ace-preview-details-tab','.ace-file-text-editor','.ace-line-table'].map(s=>[s,getComputedStyle(document.querySelector(s)).fontSize])));
 const beforeSizes=await sizes();
 await editor.hover();await page.keyboard.down('Control');await page.mouse.wheel(0,-120);await page.keyboard.up('Control');await settle();
 const afterSizes=await sizes();for(const key of ['#chat','#left','#right','.ace-preview-details-tab'])assert.equal(afterSizes[key],beforeSizes[key]);
 assert.ok(parseFloat(afterSizes['.ace-file-text-editor'])>parseFloat(beforeSizes['.ace-file-text-editor']));
 assert.equal(afterSizes['.ace-file-text-editor'],afterSizes['.ace-line-table']);
 assert.equal(await editor.inputValue(),'first line\nsecond edited line\nthird line');
 assert.deepEqual(await editor.evaluate(e=>[e.selectionStart,e.selectionEnd]),[2,8]);
 assert.ok(await page.evaluate(()=>Math.abs(parseFloat(document.querySelector('.ace-file-text-editor').style.left)-document.querySelector('.ace-line-code').offsetLeft)<1));
 await press();await press();assert.equal(await page.evaluate(()=>window.savedEditor===document.querySelector('.ace-file-text-editor')),true);assert.equal(await page.evaluate(()=>window.fixture.reads),reads);assert.equal(await page.evaluate(()=>window.fixture.saves),0);
 console.log('[pass] real Ctrl+wheel changes only detail fonts, preserves draft/selection and keeps gutter/caret aligned');
 for(const value of [.5,2]){
  await page.evaluate(v=>window.fixture.zoom(v),value);await settle();
  const cancel=await editor.evaluate((e,value)=>{const w=new WheelEvent('wheel',{ctrlKey:true,deltaY:value===2?-100:100,bubbles:true,cancelable:true});e.dispatchEvent(w);return w.defaultPrevented;},value);
  assert.equal(cancel,true);await settle();assert.equal(await page.locator('.ace-preview-details-panel').evaluate(e=>Number(e.style.getPropertyValue('--ace-preview-text-zoom'))),value);
 }
 assert.equal(await editor.evaluate(e=>{const w=new WheelEvent('wheel',{deltaY:50,bubbles:true,cancelable:true});e.dispatchEvent(w);return w.defaultPrevented;}),false);
 assert.equal(await page.locator('#chat').evaluate(e=>{const w=new WheelEvent('wheel',{ctrlKey:true,deltaY:50,bubbles:true,cancelable:true});e.dispatchEvent(w);return w.defaultPrevented;}),false);
 assert.equal(await page.locator('.ace-preview-details-tabs').evaluate(e=>{const w=new WheelEvent('wheel',{ctrlKey:true,deltaY:-50,bubbles:true,cancelable:true});e.dispatchEvent(w);return w.defaultPrevented;}),true);
 console.log('[pass] both zoom bounds prevent global zoom; ordinary and outside-detail wheel remain independent');
 await page.evaluate(()=>window.fixture.zoom(1));await editor.evaluate(e=>{e.focus();e.setSelectionRange(0,5);});
 await editor.click({button:'right'});const menu=page.getByRole('menu');await menu.waitFor();
 let labels=await menu.getByRole('menuitem').allTextContents();assert.match(labels[0],/引用到聊天/);assert.match(labels[1],/全屏演示.*Ctrl\+F11/);
 await menu.getByRole('menuitem',{name:/全屏演示/}).click();await settle();assert.equal((await state()).active,true);
 await editor.click({button:'right'});await menu.getByRole('menuitem',{name:/退出全屏演示/}).click();await settle();assert.equal((await state()).active,false);
 console.log('[pass] editable selection menu order, displayed shortcut and enter/exit actions');
 await page.evaluate(()=>window.fixture.select('guide.md'));const markdown=page.locator('.ace-side-markdown-preview[contenteditable="true"]');await markdown.waitFor();
 await markdown.press('Control+End');await page.keyboard.type('Draft stays.');const mdBefore=await markdown.textContent();
 const mdSize=await markdown.evaluate(e=>parseFloat(getComputedStyle(e).fontSize));await markdown.hover();await page.keyboard.down('Control');await page.mouse.wheel(0,-120);await page.keyboard.up('Control');await settle();
 assert.ok(await markdown.evaluate((e,s)=>parseFloat(getComputedStyle(e).fontSize)>s,mdSize));assert.equal(await markdown.textContent(),mdBefore);
 await press();await page.screenshot({path:path.join(output,'presentation-markdown.png')});await press();assert.equal(await markdown.textContent(),mdBefore);
 await page.getByRole('button',{name:'查看 Markdown 原文',exact:true}).click();await editor.waitFor();assert.match(await editor.inputValue(),/Draft stays\./);
 await page.getByRole('button',{name:'渲染 Markdown',exact:true}).click();await markdown.waitFor();
 console.log('[pass] Markdown WYSIWYG/source switches, scale and presentation retain unsaved content');
 await page.evaluate(()=>window.fixture.readonly());await page.locator('.ace-side-markdown-preview:not([contenteditable])').waitFor();
 await page.locator('.ace-side-markdown-preview').click({button:'right',position:{x:20,y:50}});await menu.getByRole('menuitem',{name:/全屏演示/}).waitFor();await page.keyboard.press('Escape');
 await page.evaluate(()=>window.fixture.diff());const diff=page.locator('.d2h-diff-table').first();await diff.waitFor();
 const diffSize=await diff.evaluate(e=>parseFloat(getComputedStyle(e).fontSize));await diff.hover();await page.keyboard.down('Control');await page.mouse.wheel(0,-120);await page.keyboard.up('Control');await settle();
 assert.ok(await diff.evaluate((e,s)=>parseFloat(getComputedStyle(e).fontSize)>s,diffSize));
 console.log('[pass] read-only Markdown context menu and rendered diff font scaling');
 await page.evaluate(()=>window.fixture.select('slides.pptx'));
 const slide=page.frameLocator('.ace-side-presentation-frame').locator('section');await slide.waitFor();
 await slide.focus();await press();assert.equal((await state()).active,true);await slide.focus();await press();assert.equal((await state()).active,false);
 await page.frameLocator('.ace-side-presentation-frame').locator('body').click({button:'right',position:{x:150,y:30}});await menu.getByRole('menuitem',{name:/全屏演示/}).click();await settle();assert.equal((await state()).active,true);await press();
 await page.evaluate(()=>window.dispatchEvent(new MessageEvent('message',{source:document.querySelector('iframe').contentWindow,data:{source:'acecode:presentation-preview',channel:'forged',status:'presentation'}})));await settle();assert.equal((await state()).active,false);
 console.log('[pass] real sandbox frame forwards Ctrl+F11 and context menu; forged channel is ignored');
 for(const p of ['picture.png','slides.pptx','error.txt','loading.txt']){
  await page.evaluate(p=>window.fixture.select(p),p);await settle();
  await page.locator('.ace-preview-details-body').click({button:'right',position:{x:15,y:15}});await menu.getByRole('menuitem',{name:/全屏演示/}).waitFor();await page.keyboard.press('Escape');
 }
 await page.evaluate(()=>window.fixture.diff());await settle();await page.locator('.ace-preview-details-body').click({button:'right',position:{x:15,y:15}});await menu.getByRole('menuitem',{name:/全屏演示/}).waitFor();await page.keyboard.press('Escape');
 console.log('[pass] image, Office, error, loading and diff detail menus without a selection');
 await page.setViewportSize({width:430,height:680});await page.evaluate(()=>window.fixture.select('guide.md'));await markdown.waitFor();await press();
 await markdown.click({button:'right',position:{x:30,y:80}});await menu.waitFor();const bounds=await menu.boundingBox();assert.ok(bounds.x>=0&&bounds.x+bounds.width<=430);
 await page.screenshot({path:path.join(output,'presentation-narrow-menu.png')});await page.keyboard.press('Escape');
 assert.deepEqual(errors,[]);console.log('[pass] narrow presentation and menu stay within the viewport');
 console.log('Screenshots: '+output);
} finally {await browser?.close();await server.close();}
