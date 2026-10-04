import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { mkdir, writeFile } from 'node:fs/promises';
import { fileURLToPath, pathToFileURL } from 'node:url';
import path from 'node:path';
import { createServer } from 'vite';

/**
 * Real Chromium mouse/keyboard regression checks for RichComposer. Run from web:
 *   node scripts/test-composer-selection.mjs
 *
 * Install Playwright separately, or set ACE_PLAYWRIGHT_MODULE to its index.mjs.
 * Optional ACE_CHROMIUM_EXECUTABLE selects an already installed Chromium.
 * ACE_COMPOSER_TEST_OUTPUT writes the JSON results; --screenshots also captures
 * the fully selected fixture. ACE_COMPOSER_TEST_FILTER is a test-name RegExp.
 * --baseline loads the three production files from Git HEAD to reproduce bugs.
 * --headed opens a separate test browser; no existing desktop window is touched.
 *
 * DOM evaluation only observes geometry, selection, and fixture state. Every
 * selection/edit is driven by browser.mouse/browser.keyboard, including actual
 * browser clipboard events. This harness does not simulate a Windows IME.
 */
// Playwright is intentionally optional: install it separately or point this at
// an existing module. No daemon, account, or desktop application is required.
const web = fileURLToPath(new URL('../', import.meta.url));
const modulePath = process.env.ACE_PLAYWRIGHT_MODULE;
const { chromium } = await import(modulePath ? pathToFileURL(path.resolve(modulePath)).href : 'playwright');
const port = Number(process.env.ACE_COMPOSER_TEST_PORT || 5189);
const output = process.env.ACE_COMPOSER_TEST_OUTPUT;
const filter = process.env.ACE_COMPOSER_TEST_FILTER ? new RegExp(process.env.ACE_COMPOSER_TEST_FILTER) : null;
const baseline = process.argv.includes('--baseline');
const baselineSources = new Map(baseline ? [
 'src/components/RichComposer.jsx','src/lib/richComposerModel.js','src/styles/globals.css',
].map(relative => [path.join(web,relative).replaceAll('\\','/'),execFileSync('git',['show','HEAD:web/'+relative],{cwd:web,encoding:'utf8'})]) : []);
const fixture = `
import React, {useRef, useState} from 'react';
import {createRoot} from 'react-dom/client';
import {RichComposer} from '/src/components/RichComposer.jsx';
import {composerContentText} from '/src/lib/composerContent.js';
import {formatSessionReferenceToken} from '/src/lib/sessionReference.js';
import {i18n} from '/src/i18n/index.js';
import '/src/styles/globals.css';
const params = new URLSearchParams(location.search);
document.documentElement.dataset.theme = params.get('theme') || 'light';
await i18n.changeLanguage(params.get('locale') || 'zh-CN');
const commands = [{name:'init', token:'/init', kind:'builtin'}];
const attachments = [
 {id:'file-1',name:'notes.txt',kind:'file',path:'C:/fixture/notes.txt',uploading:params.get('upload')==='true'},
 {id:'image-1',name:'picture.png',kind:'image',url:'data:image/svg+xml,<svg xmlns="http://www.w3.org/2000/svg"/>'},
];
const text = (text) => ({type:'text',text});
const pathTag = {type:'path',path:'src/main.cpp',token:'@src/main.cpp'};
const skillTag = {type:'skill',name:'review',token:'$review',path:'C:/fixture/SKILL.md'};
const attachment = (id) => ({type:'attachment',key:id,id,name:id === 'file-1' ? 'notes.txt' : 'picture.png',kind:id === 'file-1' ? 'file' : 'image'});
const sessionToken = formatSessionReferenceToken({session_id:'fixture-session',title:'Earlier task',workspace_name:'Fixture'},{trailingSpace:false});
const fixtures = {
 all:[text('/init ALPHA '),pathTag,text(' BRAVO '),skillTag,text(' CHARLIE '+sessionToken+' DELTA '),attachment('file-1'),text(' ECHO '),attachment('image-1'),text(' FOXTROT')],
 path:[text('ALPHA '),pathTag,text(' BRAVO')],
 longPath:[text('ALPHA '),{...pathTag,path:'src/'+('long-directory/').repeat(12)+'main.cpp'},text(' BRAVO')],
 skill:[text('ALPHA '),skillTag,text(' BRAVO')],
 session:[text('ALPHA '+sessionToken+' BRAVO')],
 attachment:[text('ALPHA '),attachment('file-1'),text(' BRAVO')],
 image:[text('ALPHA '),attachment('image-1'),text(' BRAVO')],
  duplicate:[text('ALPHA '),attachment('file-1'),text(' BRAVO '),attachment('file-1'),text(' CHARLIE')],
  adjacent:[pathTag,attachment('file-1'),skillTag],
  edgeTags:[pathTag,text('\\n'),skillTag],
  rtl:[text('\\u05d0\\u05d1 '),pathTag,text(' \\u05d2\\u05d3')],
  command:[text('/init BRAVO')],
  multiline:[text('FIRST ALPHA '),pathTag,text(' BRAVO\\nSECOND CHARLIE '),skillTag,text(' DELTA\\nTHIRD ECHO '),attachment('file-1'),text(' FOXTROT')],
  typography:[text('/init AgTest \\u6d4b\\u8bd5\\n'),pathTag,text(' AgTest \\u6d4b\\u8bd5\\n'),skillTag,text(' AgTest \\u6d4b\\u8bd5\\n'+sessionToken+' AgTest \\u6d4b\\u8bd5\\n'),attachment('file-1'),text(' AgTest \\u6d4b\\u8bd5\\n'),attachment('image-1'),text(' AgTest \\u6d4b\\u8bd5')],
  empty:[],
};
window.previews = [];
window.__ACECODE_DESKTOP_SHELL__ = true;
function Fixture() {
 const ref = useRef();
 const variant = new URLSearchParams(location.search).get('fixture') || 'all';
 const [content,setContent] = useState({version:1,parts:fixtures[variant]});
 const [value,setValue] = useState(composerContentText(content));
 window.fixture = {get value(){return ref.current?.value;},get content(){return ref.current?.getComposerContent();}};
 return <main style={{padding:'80px 32px',maxWidth:1100,margin:'auto'}}>
   <h1 style={{marginBottom:24,fontSize:18}}>Composer selection regression</h1>
   <div style={{padding:20,border:'1px solid #999',borderRadius:12}}><RichComposer ref={ref}
    value={value} onChange={setValue} composerContent={content} onComposerContentChange={setContent}
    commands={commands} attachments={attachments} className="ace-rich-composer-input"
    style={{minHeight:180,fontSize:Number(params.get('font')||16),lineHeight:params.get('line')||'28px',outline:'none'}} aria-label="Selection fixture"
    onPreviewAttachment={(item)=>window.previews.push(item.attachmentKey)} /></div>
   <textarea aria-label="External clipboard target" style={{marginTop:30,width:'100%',height:80}} />
 </main>;
}
createRoot(document.getElementById('root')).render(<Fixture/>);
`;
const html = `<html><body><div id="root"></div><script type="module">
import RefreshRuntime from '/@react-refresh';
RefreshRuntime.injectIntoGlobalHook(window);
window.$RefreshReg$=()=>{};window.$RefreshSig$=()=>type=>type;
window.__vite_plugin_react_preamble_installed__=true;
</script><script type="module" src="/__composer-selection.jsx"></script></body></html>`;
const virtual = path.join(web, '__composer-selection.jsx').replaceAll('\\', '/');
const server = await createServer({
 root:web,configFile:path.join(web,'vite.config.js'),logLevel:'error',
 plugins:[{name:'composer-selection-fixture',enforce:'pre',
  resolveId(id){if(id === '/__composer-selection.jsx') return virtual;},
  load(id){const normalized=id.replaceAll('\\','/');if(normalized === virtual) return fixture;return baselineSources.get(normalized);},
  configureServer(vite){vite.middlewares.use((req,res,next)=>{
   if(req.url?.split('?')[0] !== '/__composer-selection') return next();
   res.setHeader('Content-Type','text/html');res.end(html);
  });}
 }],server:{host:'127.0.0.1',port,strictPort:true,hmr:false},
});
let browser;
const results=[];
try {
 await server.listen();
 browser = await chromium.launch({headless:!process.argv.includes('--headed'),...(process.env.ACE_CHROMIUM_EXECUTABLE ? {executablePath:process.env.ACE_CHROMIUM_EXECUTABLE}:{})});
 const context=await browser.newContext({viewport:{width:1200,height:760},permissions:['clipboard-read','clipboard-write']});
 const page=await context.newPage();
 const errors=[];
 page.on('pageerror',error=>errors.push(error.message));
 page.on('console',message=>{if(message.type()==='error') errors.push(message.text());});
 const editor=page.locator('[data-ace-rich-composer]');
 const tag=(type)=>editor.locator('[data-composer-inline-tag="'+type+'"]');
 // Slate throttles native selectionchange by 100 ms. Wait through that boundary
 // before inspecting its selected-void rendering or issuing another edit.
 const settle=()=>page.evaluate(()=>new Promise(resolve=>setTimeout(()=>requestAnimationFrame(resolve),130)));
 async function reset(name='all'){
  await page.goto('http://127.0.0.1:'+port+'/__composer-selection?fixture='+name);
  await editor.waitFor();await page.evaluate(()=>document.fonts.ready);await settle();
 }
 async function state(){await settle();return editor.evaluate(el=>({
  value:window.fixture.value,content:window.fixture.content,selected:[...el.querySelectorAll('[data-composer-selected="true"]')].map(el=>el.getAttribute('data-composer-inline-tag')),
  selection:window.getSelection()?.toString(),collapsed:window.getSelection()?.isCollapsed,previews:window.previews,
  focused:document.activeElement===el,
 }));}
 async function run(name,fn){
  if(filter && !filter.test(name) && name!=='no browser runtime or console errors') return;
  try{await fn();results.push({name,ok:true});console.log('[pass] '+name);}
  catch(error){results.push({name,ok:false,error:error.message,state:await state().catch(()=>null)});console.log('[FAIL] '+name+': '+error.message);}
 }
 async function pointAtText(text,offset){
  return editor.evaluate((el,{text,offset})=>{
   const span=[...el.querySelectorAll('[data-slate-string]')].find(el=>el.textContent.includes(text));
   if(!span) throw Error('Missing text '+text);
   const node=span.firstChild;const index=node.textContent.indexOf(text)+offset;
   const range=document.createRange();range.setStart(node,index);range.setEnd(node,index);
   const rect=range.getBoundingClientRect();return {x:rect.x,y:rect.y+rect.height/2};
  },{text,offset});
 }
 async function drag(from,to){await page.mouse.move(from.x,from.y);await page.mouse.down();await page.mouse.move(to.x,to.y,{steps:12});await page.mouse.up();await settle();}
 async function clickTag(type,index=0){await tag(type).nth(index).locator('.ace-cmd-token-name').click();await settle();}
 async function keyboardTag(type){
  await reset(type);
  if(type==='command'){await editor.click();await page.keyboard.press('Control+Home');}
   else {const point=await pointAtText('ALPHA',5);await page.mouse.click(point.x,point.y);await settle();await page.keyboard.press('ArrowRight');}
   await settle();
   for(let step=0;step<3;step++){
    await page.keyboard.press('Shift+ArrowRight');if((await state()).selected.includes(type))return;
   }
   assert.fail('Shift+ArrowRight did not select the tag');
  }
  for(const type of ['command','path','skill','session','attachment']){
   await run(type+' plain arrows skip the tag without selecting or replacing it',async()=>{
    for(const backwards of [false,true]){
     await reset(type);const initial=await state();const before=initial.content;
     await clickTag(type);await page.keyboard.press(backwards?'ArrowRight':'ArrowLeft');await settle();
     await page.keyboard.press(backwards?'ArrowLeft':'ArrowRight');
     const moved=await state();assert.equal(moved.collapsed,true);assert.deepEqual(moved.selected,[]);
     assert.deepEqual(moved.content,before,'movement cannot mutate the draft');
     await page.keyboard.type('X');const after=await state();
     const prefix=type==='command'?'':'ALPHA ';
     const offset=backwards?prefix.length:initial.value.length-' BRAVO'.length;
     assert.equal(after.value,initial.value.slice(0,offset)+'X'+initial.value.slice(offset),'typing lands exactly at the far boundary');
    }
   });
  await run(type+' single click selects whole tag',async()=>{
   await reset(type);await clickTag(type);const s=await state();assert(s.selected.includes(type),'tag must highlight');assert(s.focused,'editor must retain focus');
   await page.keyboard.press('Backspace');assert.equal(await tag(type).count(),0,'Backspace must remove selected tag');
   await page.keyboard.press('Control+z');assert.equal(await tag(type).count(),1,'Undo must restore tag');
  });
  await run(type+' single-click copy, cut, undo and redo',async()=>{
   await reset(type);const original=(await state()).content;await clickTag(type);await page.keyboard.press('Control+c');
   const copied=await page.evaluate(()=>navigator.clipboard.readText());
   const expected={command:'/init',path:'@src/main.cpp',skill:'$review',attachment:'[notes.txt]'}[type];
   assert(expected ? copied===expected : copied.startsWith('@session:'),'copy must serialize only clicked tag');
   await page.keyboard.press('Control+x');assert.equal(await tag(type).count(),0,'cut must remove clicked tag');
   await page.keyboard.press('Control+z');assert.deepEqual((await state()).content,original,'undo restores exact content');
   await page.keyboard.press('Control+Shift+z');assert.equal(await tag(type).count(),0,'redo removes tag again');
  });
  await run(type+' single-click typing replaces tag',async()=>{
   await reset(type);const original=(await state()).value;await clickTag(type);await page.keyboard.type('X');
   assert.equal(await tag(type).count(),0,'typing must replace clicked tag');
   assert.equal((await state()).value,type==='command'?'X BRAVO':'ALPHA X BRAVO');
   await page.keyboard.press('Control+z');assert.equal((await state()).value,original);assert.equal(await tag(type).count(),1);
  });
  await run(type+' single-click paste replaces tag',async()=>{
   await reset(type);const external=page.getByRole('textbox',{name:'External clipboard target'});
   await external.click();await page.keyboard.type('PASTE');await page.keyboard.press('Control+a');await page.keyboard.press('Control+c');
   await clickTag(type);await page.keyboard.press('Control+v');assert.equal(await tag(type).count(),0,'paste must replace clicked tag');
   assert.equal((await state()).value,type==='command'?'PASTE BRAVO':'ALPHA PASTE BRAVO');
  });
  await run(type+' Delete removes only the selected tag',async()=>{
   await reset(type);await clickTag(type);await page.keyboard.press('Delete');assert.equal(await tag(type).count(),0);
   assert.equal((await state()).value,type==='command'?' BRAVO':'ALPHA  BRAVO');
  });
   await run(type+' Shift-selected tag supports copy, cut and undo',async()=>{
   await keyboardTag(type);const original=(await state()).content;await page.keyboard.press('Control+c');
   const copied=await page.evaluate(()=>navigator.clipboard.readText());const expected={command:'/init',path:'@src/main.cpp',skill:'$review',attachment:'[notes.txt]'}[type];
   assert(expected ? copied===expected : copied.startsWith('@session:'),'copy must serialize keyboard-selected tag');
   await page.keyboard.press('Control+x');assert.equal(await tag(type).count(),0);await page.keyboard.press('Control+z');assert.deepEqual((await state()).content,original);
  });
   await run(type+' Shift-selected tag supports replacement typing',async()=>{
   await keyboardTag(type);await page.keyboard.type('X');assert.equal(await tag(type).count(),0);assert.equal((await state()).value,type==='command'?'X BRAVO':'ALPHA X BRAVO');
  });
   await run(type+' Shift-selected tag supports paste and line breaks',async()=>{
   await reset('empty');await page.getByRole('textbox',{name:'External clipboard target'}).click();await page.keyboard.type('PASTE');await page.keyboard.press('Control+a');await page.keyboard.press('Control+c');
   await keyboardTag(type);await page.keyboard.press('Control+v');assert.equal(await tag(type).count(),0,'paste replaces keyboard-selected tag');assert.equal((await state()).value,type==='command'?'PASTE BRAVO':'ALPHA PASTE BRAVO');
   await keyboardTag(type);await page.keyboard.press('Shift+Enter');assert.equal(await tag(type).count(),0,'line break replaces keyboard-selected tag');assert.equal((await state()).value,type==='command'?'\n BRAVO':'ALPHA \n BRAVO');
  });
  }
  await run('plain arrows cross adjacent tags one at a time in either direction',async()=>{
   for(const backwards of [false,true]){
    await reset('adjacent');const original=(await state()).content;
    await editor.click();await page.keyboard.press(backwards?'Control+End':'Control+Home');await settle();
    for(let step=1;step<=3;step++){
     await page.keyboard.press(backwards?'ArrowLeft':'ArrowRight');
     const s=await state();assert.equal(s.collapsed,true);assert.deepEqual(s.selected,[]);
     await page.keyboard.type('X');
     const parts=(await state()).content.parts;
     assert.equal(parts.findIndex(part=>part.type==='text' && part.text==='X'),backwards?3-step:step,JSON.stringify({backwards,step,parts}));
     assert.deepEqual(parts.filter(part=>part.type!=='text'),original.parts);
     await page.keyboard.press('Control+z');await settle();
    }
   }
  });
  await run('plain arrows retain line breaks and start/end tag caret stops',async()=>{
   await reset('edgeTags');await editor.click();await page.keyboard.press('Control+Home');await settle();
   for(const key of ['ArrowLeft','ArrowRight','ArrowRight','ArrowRight','ArrowRight']){
    await page.keyboard.press(key);const s=await state();assert(s.collapsed);assert.deepEqual(s.selected,[]);
   }
   await page.keyboard.type('X');assert.equal((await state()).value,'@src/main.cpp\n$reviewX');
   await page.keyboard.press('Control+z');await settle();
   for(let i=0;i<4;i++){await page.keyboard.press('ArrowLeft');assert.deepEqual((await state()).selected,[]);}
   await page.keyboard.type('X');assert.equal((await state()).value,'X@src/main.cpp\n$review');
  });
  await run('plain arrows follow RTL paragraph direction across tags',async()=>{
   for(const backwards of [false,true]){
    await reset('rtl');await clickTag('path');
    await page.keyboard.press(backwards?'ArrowLeft':'ArrowRight');await settle();
    await page.keyboard.press(backwards?'ArrowRight':'ArrowLeft');
    const s=await state();assert(s.collapsed);assert.deepEqual(s.selected,[]);
    await page.keyboard.type('X');assert.equal((await state()).value,backwards?'\u05d0\u05d1 X@src/main.cpp \u05d2\u05d3':'\u05d0\u05d1 @src/main.cppX \u05d2\u05d3');
   }
  });
  for(const type of ['path','skill','session','attachment']){
  await run(type+' drag from tag into following text',async()=>{
   await reset(type);const box=await tag(type).boundingBox();await drag({x:box.x+box.width/2,y:box.y+box.height/2},await pointAtText('BRAVO',3));
   const s=await state();assert.equal(s.collapsed,false,'drag must produce range');assert(s.selected.includes(type),'origin tag must join selection');assert(s.selection.includes('BRA'),'trailing text must join selection');
  });
  await run(type+' drag from tag into preceding text',async()=>{
   await reset(type);const box=await tag(type).boundingBox();await drag({x:box.x+box.width/2,y:box.y+box.height/2},await pointAtText('ALPHA',2));
   const s=await state();assert.equal(s.collapsed,false,'drag must produce range');assert(s.selected.includes(type),'origin tag must join selection');assert(s.selection.includes('PHA'),'preceding text must join selection');
   await page.keyboard.press('Backspace');assert.equal(await tag(type).count(),0);assert.equal((await state()).value,'AL BRAVO');
  });
  await run(type+' dragging from text onto a tag includes the tag',async()=>{
   for(const backwards of [false,true]){
    await reset(type);const box=await tag(type).boundingBox();await drag(await pointAtText(backwards?'BRAVO':'ALPHA',backwards?3:2),{x:box.x+box.width/2,y:box.y+box.height/2});
    const s=await state();assert(s.selected.includes(type));assert(s.selection.includes(backwards?'BRA':'PHA'));
    await page.keyboard.press('Control+c');const copied=await page.evaluate(()=>navigator.clipboard.readText());
    assert(copied.includes({path:'@src/main.cpp',skill:'$review',session:'@session:',attachment:'[notes.txt]'}[type]),'copy includes complete target tag');
    await page.keyboard.press('Backspace');assert.equal(await tag(type).count(),0);assert.equal((await state()).value,backwards?'ALPHA VO':'AL BRAVO');
   }
  });
  await run(type+' forward and backward cross-tag drag',async()=>{
   for(const backwards of [false,true]){
    await reset(type);const a=await pointAtText('ALPHA',2),b=await pointAtText('BRAVO',3);await drag(backwards?b:a,backwards?a:b);
    const s=await state();assert(s.selected.includes(type),'crossed tag must highlight');assert(s.selection.includes('PHA'),'leading text');assert(s.selection.includes('BRA'),'trailing text');
    await page.keyboard.press('Backspace');assert.equal(await tag(type).count(),0,'delete crossed tag');assert.equal((await state()).value,'ALVO','delete precise mixed range');
    await page.keyboard.press('Control+z');assert.equal(await tag(type).count(),1,'restore tag');
   }
  });
 }
 await run('image attachment click selects without preview',async()=>{await reset('image');await clickTag('attachment');const s=await state();assert(s.selected.includes('attachment'));assert.deepEqual(s.previews,[]);});
 await run('image attachment double-click preserves preview',async()=>{
  await reset('image');await tag('attachment').locator('.ace-cmd-token-name').dblclick();assert.deepEqual((await state()).previews,['image-1']);assert.equal(await tag('attachment').count(),1);
 });
 await run('image attachment drag selects text without preview',async()=>{
  await reset('image');const box=await tag('attachment').boundingBox();await drag({x:box.x+box.width/2,y:box.y+box.height/2},await pointAtText('BRAVO',3));
  const s=await state();assert(s.selected.includes('attachment'));assert(s.selection.includes('BRA'));assert.deepEqual(s.previews,[]);
 });
 await run('tag-to-tag drag selects both endpoint tags and intervening text',async()=>{
  await reset();const a=await tag('path').boundingBox(),b=await tag('skill').boundingBox();await drag({x:a.x+a.width/2,y:a.y+a.height/2},{x:b.x+b.width/2,y:b.y+b.height/2});
  const s=await state();assert.deepEqual(s.selected,['path','skill']);assert(s.selection.includes('BRAVO'));
  await page.keyboard.press('Delete');assert.equal(await tag('path').count(),0);assert.equal(await tag('skill').count(),0);assert.equal(await tag('attachment').count(),2);
 });
 await run('attachment remove control and undo restore one occurrence',async()=>{
  await reset();await editor.locator('[data-composer-inline-tag="attachment"]').first().getByRole('button',{name:'移除附件'}).click();
  assert.equal(await tag('attachment').count(),1);await editor.click();await page.keyboard.press('Control+z');assert.equal(await tag('attachment').count(),2);
 });
 await run('duplicate attachment selection affects one occurrence',async()=>{
  await reset('duplicate');const original=(await state()).content;await clickTag('attachment',1);await page.keyboard.press('Control+x');
  assert.equal(await tag('attachment').count(),1);const parts=(await state()).content.parts;assert.equal(parts.filter(part=>part.type==='attachment').length,1);
  assert.equal(parts.at(-1).text,' BRAVO  CHARLIE');await page.keyboard.press('Control+z');assert.deepEqual((await state()).content,original);
 });
 await run('Ctrl+A copy, cut, paste and undo mixed tags',async()=>{
  await reset();await editor.click();await page.keyboard.press('Control+a');const original=(await state()).content;
  assert.equal((await state()).selected.length,6);await page.keyboard.press('Control+c');
  const copied=await page.evaluate(()=>navigator.clipboard.readText());assert(copied.includes('@src/main.cpp'));assert(copied.includes('[notes.txt]'));
  await page.keyboard.press('Control+x');assert.equal((await state()).value,'');assert.equal(await tag('path').count(),0);
  await page.keyboard.press('Control+v');assert.deepEqual((await state()).content,original);
  await page.keyboard.press('Control+z');assert.equal((await state()).value,'');await page.keyboard.press('Control+z');assert.deepEqual((await state()).content,original);
 });
 await run('mixed reference clipboard pastes readable text outside composer',async()=>{
  await reset();await editor.click();await page.keyboard.press('Control+a');await settle();await page.keyboard.press('Control+c');
  const external=page.getByRole('textbox',{name:'External clipboard target'});await external.click();await page.keyboard.press('Control+v');
  const copied=await external.inputValue();assert(copied.includes('/init ALPHA @src/main.cpp BRAVO $review'));assert(copied.includes('[notes.txt]'));assert(copied.includes('[picture.png]'));
 });
 await run('Shift+Left and Shift+Right preserve mixed tags',async()=>{
  for(const direction of ['ArrowRight','ArrowLeft']){
   await reset('path');await editor.click();await page.keyboard.press(direction==='ArrowRight'?'Control+Home':'Control+End');
   await page.keyboard.down('Shift');for(let i=0;i<10;i++)await page.keyboard.press(direction);await page.keyboard.up('Shift');
   const s=await state();assert(s.selected.includes('path'));assert(s.selection.includes(direction==='ArrowRight'?'ALPHA':'BRAVO'));
   await page.keyboard.press('Backspace');assert.equal(await tag('path').count(),0);await page.keyboard.press('Control+z');assert.equal(await tag('path').count(),1);
  }
 });
 await run('Shift+Home and Shift+End select across tags',async()=>{
  for(const key of ['Home','End']){await reset('path');await editor.click();await page.keyboard.press(key==='Home'?'Control+End':'Control+Home');await page.keyboard.press('Shift+'+key);assert((await state()).selected.includes('path'));}
 });
 for(const type of ['path','skill','session','attachment']){
  await run(type+' Shift+arrow traverses tag as one atom',async()=>{
   for(const backwards of [false,true]){
    await reset(type);const point=await pointAtText(backwards?'BRAVO':'ALPHA',backwards?0:5);await page.mouse.click(point.x,point.y);await settle();
    await page.keyboard.press(backwards?'ArrowLeft':'ArrowRight');await settle();
    await page.keyboard.press(backwards?'Shift+ArrowLeft':'Shift+ArrowRight');assert.deepEqual((await state()).selected,[type]);
    await page.keyboard.press('Delete');assert.equal(await tag(type).count(),0);assert.equal((await state()).value,'ALPHA  BRAVO');
   }
  });
 }
 await run('selection direction reversal clears tag highlighting',async()=>{
  await reset('path');await editor.click();await page.keyboard.press('Control+Home');
  await page.keyboard.down('Shift');for(let i=0;i<9;i++)await page.keyboard.press('ArrowRight');await page.keyboard.up('Shift');assert((await state()).selected.includes('path'));
  await page.keyboard.down('Shift');for(let i=0;i<9;i++)await page.keyboard.press('ArrowLeft');await page.keyboard.up('Shift');const s=await state();assert.equal(s.collapsed,true);assert.deepEqual(s.selected,[]);
 });
 await run('Shift+click extends selection across text and tag',async()=>{
  await reset('path');const a=await pointAtText('ALPHA',2),b=await pointAtText('BRAVO',3);await page.mouse.click(a.x,a.y);await settle();
  await page.keyboard.down('Shift');await page.mouse.click(b.x,b.y);await page.keyboard.up('Shift');const s=await state();assert(s.selected.includes('path'));assert(s.selection.includes('PHA'));assert(s.selection.includes('BRA'));
 });
 await run('Shift+click on a tag extends the prior text selection',async()=>{
  await reset('path');const a=await pointAtText('ALPHA',2);await page.mouse.click(a.x,a.y);await settle();
  await page.keyboard.down('Shift');await clickTag('path');await page.keyboard.up('Shift');const s=await state();assert(s.selected.includes('path'));assert(s.selection.includes('PHA'));
  await page.keyboard.press('Delete');assert.equal((await state()).value,'AL BRAVO');
 });
 await run('Shift+Up and Shift+Down select multiline mixed tags',async()=>{
  for(const key of ['ArrowUp','ArrowDown']){await reset('multiline');await editor.click();await page.keyboard.press(key==='ArrowUp'?'Control+End':'Control+Home');await page.keyboard.press('Shift+'+key);await page.keyboard.press('Shift+'+key);assert((await state()).selected.length>=2);}
 });
 await run('typing a leading command remains undoable',async()=>{
  await reset('empty');await editor.click();await page.keyboard.type('/init');await page.keyboard.press('Space');await settle();assert.equal(await tag('command').count(),1);
  await page.keyboard.press('Control+z');assert.equal((await state()).value,'','undo typed command and delimiter');
 });
 await run('390px wrapped mixed selection deletes and restores all tags',async()=>{
  await page.setViewportSize({width:390,height:844});await reset();await editor.click();await page.keyboard.press('Control+Home');await page.keyboard.press('Control+Shift+End');
  const original=(await state()).content;assert.equal((await state()).selected.length,6);await page.keyboard.press('Delete');assert.equal((await state()).value,'');
  await page.keyboard.press('Control+z');assert.deepEqual((await state()).content,original);await page.setViewportSize({width:1200,height:760});
 });
 await run('390px mouse drag selects wrapped text and five references',async()=>{
  try {
   await page.setViewportSize({width:390,height:844});await reset();const original=(await state()).content;
   await drag(await pointAtText('ALPHA',2),await pointAtText('FOXTROT',4));assert.equal((await state()).selected.length,5);
   await page.keyboard.press('Backspace');assert.equal((await state()).value,'/init ALROT');assert.equal(await editor.locator('[data-composer-inline-tag]').count(),1);
   await page.keyboard.press('Control+z');assert.deepEqual((await state()).content,original);
  } finally {await page.setViewportSize({width:1200,height:760});}
 });
 await run('tags share the prose font, size, line height and baseline with only a heavier weight',async()=>{
  for(const sample of [
   {theme:'light',font:13,line:'20px'},
   {theme:'dark',font:14,line:'20px'},
   {theme:'light',font:20,line:'28px'},
   {theme:'dark',font:16,line:'28px'},
  ]){
   await reset('typography&'+new URLSearchParams(sample));
   const typography=await editor.evaluate(el=>[...el.querySelectorAll('[data-composer-inline-tag]')].map(tag=>{
    const label=tag.querySelector('.ace-cmd-token-name');
    const prose=tag.nextElementSibling.querySelector('[data-slate-string]');
    const labelCss=getComputedStyle(label),proseCss=getComputedStyle(prose);
    const rect=node=>{const range=document.createRange();range.selectNodeContents(node);return range.getClientRects()[0];};
    const a=rect(label),b=rect(prose);
    return {type:tag.dataset.composerInlineTag,
     label:[labelCss.fontFamily,labelCss.fontSize,labelCss.lineHeight],prose:[proseCss.fontFamily,proseCss.fontSize,proseCss.lineHeight],
     labelWeight:labelCss.fontWeight,proseWeight:proseCss.fontWeight,top:a.top-b.top,bottom:a.bottom-b.bottom,
     height:tag.getBoundingClientRect().height};
   }));
   assert.equal(typography.length,6);
   for(const item of typography){
    const message=JSON.stringify({sample,item});
    assert.deepEqual(item.label,item.prose,message);
    assert.equal(item.labelWeight,'600',message);assert.equal(item.proseWeight,'400',message);
    assert(Math.abs(item.top)<1 && Math.abs(item.bottom)<1,message);
    assert(Math.abs(item.height-parseFloat(sample.line))<1,message);
   }
  }
 });
 await run('unboxed accent tags use native selection colors without moving across themes and font sizes',async()=>{
  async function badgeStyles(){return editor.evaluate(el=>[...el.querySelectorAll('.ace-slate-inline-tag > .ace-cmd-token')].map(tag=>{
   const css=getComputedStyle(tag),rect=tag.getBoundingClientRect();
   return {background:css.backgroundColor,border:css.borderWidth,radius:css.borderRadius,
    padding:[css.paddingLeft,css.paddingRight],margin:[css.marginLeft,css.marginRight],rect:[rect.x,rect.y,rect.width,rect.height],
    color:css.color,opacity:css.opacity,
    colors:[...tag.querySelectorAll('.ace-cmd-token-name,.ace-cmd-token-glyph,.ace-file-type-glyph')].map(node=>getComputedStyle(node).color)};
  }));}
  const geometry=(styles)=>styles.map(({color,colors,opacity,...layout})=>layout);
  try {
   for(const sample of [
    {theme:'light',font:13,line:20,width:1200},
    {theme:'dark',font:14,line:20,width:1200,upload:true},
    {theme:'light',font:20,line:28,width:390},
    {theme:'dark',font:16,line:28,width:390},
   ]){
    await page.setViewportSize({width:sample.width,height:844});
    await page.goto('http://127.0.0.1:'+port+'/__composer-selection?fixture=all&'+new URLSearchParams({...sample,line:sample.line+'px'}),{waitUntil:'domcontentloaded',timeout:60000});
    await editor.waitFor();await page.evaluate(()=>document.fonts.ready);await settle();
    const point=await pointAtText('ALPHA',2);await page.mouse.click(point.x,point.y);await page.mouse.move(1,1);await settle();
    const before=await badgeStyles();
    const accent=await editor.evaluate(el=>getComputedStyle(el).getPropertyValue('--ace-accent').trim());
    const accentColor=await page.evaluate(color=>{
     const probe=document.createElement('span');probe.style.color=color;document.body.append(probe);
     const resolved=getComputedStyle(probe).color;probe.remove();return resolved;
    },accent);
    for(const style of before){
     assert.equal(style.background,'rgba(0, 0, 0, 0)');assert.equal(style.border,'0px');assert.equal(style.radius,'0px');
     assert.deepEqual(style.padding,['0px','0px']);assert.deepEqual(style.margin,['0px','0px']);
     assert.equal(style.color,accentColor);assert(style.colors.every(color=>color===accentColor),'tag text and icons use the theme accent');
    }
    await tag('command').hover();assert.deepEqual(await badgeStyles(),before,'hover must not restore a badge');await page.mouse.move(1,1);
    await page.keyboard.press('Control+a');assert.equal((await state()).selected.length,6);
    assert.deepEqual(geometry(await badgeStyles()),geometry(before),'selection must preserve transparent surfaces and geometry');
    const problems=await editor.evaluate(el=>{
     const failures=[],canvas=document.createElement('canvas').getContext('2d');
     const background=getComputedStyle(el,'::selection').backgroundColor;
     const foreground=getComputedStyle(el,'::selection').color;
     for(const tag of el.querySelectorAll('[data-composer-selected="true"]')){
      const badge=tag.querySelector('.ace-cmd-token'),css=getComputedStyle(badge),rect=badge.getBoundingClientRect();
      const fill=getComputedStyle(tag,'::before');
      if(fill.backgroundColor!==background || parseFloat(fill.height)<rect.height || fill.borderRadius!=='0px')failures.push('selection rectangle');
      if(css.color!==foreground || css.opacity!=='1')failures.push('selected tag foreground: '+JSON.stringify({actual:css.color,expected:foreground,opacity:css.opacity}));
      if(css.paddingLeft!==css.paddingRight || css.marginLeft!==css.marginRight)failures.push('asymmetric spacing');
      for(const node of tag.querySelectorAll('.ace-cmd-token-name,.ace-cmd-token-glyph,.ace-file-type-glyph')){
       if(getComputedStyle(node).color!==foreground || getComputedStyle(node,'::selection').color!==foreground)failures.push('selection foreground '+node.className+': '+JSON.stringify({actual:getComputedStyle(node).color,native:getComputedStyle(node,'::selection').color,expected:foreground}));
      }
      const glyph=tag.querySelector('.ace-cmd-token-glyph'),ink=glyph.querySelector('.ace-file-type-glyph');
      if(ink){
       const font=getComputedStyle(ink);canvas.font=font.font;const measure=canvas.measureText(ink.textContent);
       const scale=new DOMMatrixReadOnly(font.transform).a;
       if((measure.actualBoundingBoxAscent+measure.actualBoundingBoxDescent)*scale<parseFloat(css.fontSize))failures.push('file glyph smaller than font');
      }
     }
     if(el.scrollWidth>el.clientWidth+1)failures.push('horizontal overflow');
     return failures;
    });
    assert.deepEqual(problems,[],JSON.stringify(sample));
    await page.keyboard.press('ArrowRight');await settle();assert.deepEqual((await state()).selected,[]);
    assert.deepEqual(await badgeStyles(),before,'collapsing selection restores accent text and upload opacity');
    assert(await editor.evaluate(el=>[...el.querySelectorAll('[data-composer-inline-tag]')].every(tag=>getComputedStyle(tag,'::before').content==='none')));
    await clickTag('command');
    assert.equal(await tag('command').locator('.ace-cmd-token-name').evaluate(el=>getComputedStyle(el).color),await editor.evaluate(el=>getComputedStyle(el,'::selection').color),'single-click selection uses the native foreground');
    await page.keyboard.press('Control+a');assert.equal((await state()).selected.length,6);
    await page.getByRole('textbox',{name:'External clipboard target'}).click();assert.deepEqual((await state()).selected,[],'blur removes the tag selection layer');
    assert.deepEqual(await badgeStyles(),before,'blur restores unselected tag colors');
   }
  } finally {await page.setViewportSize({width:1200,height:760});}
 });
 await run('long path tag stays within the narrow composer and remains selectable',async()=>{
  try {
   await page.setViewportSize({width:390,height:844});await reset('longPath');
   const before=(await state()).content;const point=await pointAtText('ALPHA',2);await page.mouse.click(point.x,point.y);
   await page.keyboard.press('Control+a');assert.deepEqual((await state()).selected,['path']);
   assert(await editor.evaluate(el=>el.scrollWidth<=el.clientWidth+1),'long path must not overflow');
   await page.keyboard.press('Delete');assert.equal((await state()).value,'');await page.keyboard.press('Control+z');assert.deepEqual((await state()).content,before);
  } finally {await page.setViewportSize({width:1200,height:760});}
 });
 if(output){
  await mkdir(output,{recursive:true});
  if(process.argv.includes('--screenshots')){
   for(const sample of [
    {name:'desktop-light-zh',width:1200,height:760,theme:'light',locale:'zh-CN',reducedMotion:'no-preference'},
    {name:'desktop-dark-en',width:1200,height:760,theme:'dark',locale:'en',reducedMotion:'no-preference'},
    {name:'mobile-dark-zh-reduced',width:390,height:844,theme:'dark',locale:'zh-CN',reducedMotion:'reduce'},
    {name:'mobile-light-en-reduced',width:390,height:844,theme:'light',locale:'en',reducedMotion:'reduce'},
   ]){
    await page.setViewportSize({width:sample.width,height:sample.height});await page.emulateMedia({reducedMotion:sample.reducedMotion});
    await page.goto('http://127.0.0.1:'+port+'/__composer-selection?fixture=all&theme='+sample.theme+'&locale='+sample.locale);
    await editor.waitFor();await page.evaluate(()=>document.fonts.ready);await settle();
    await page.screenshot({path:path.join(output,sample.name+'-unselected.png')});
    const point=await pointAtText('ALPHA',2);await page.mouse.click(point.x,point.y);await page.keyboard.press('Control+a');await settle();
    assert.equal((await state()).selected.length,6,'screenshot must show six selected tags');assert.equal((await state()).focused,true);
    await page.screenshot({path:path.join(output,sample.name+'.png')});
   }
  }
 }
 await run('no browser runtime or console errors',async()=>assert.deepEqual(errors,[]));
 if(output)await writeFile(path.join(output,'results.json'),JSON.stringify(results,null,2)+'\n');
 console.log(JSON.stringify({passed:results.filter(x=>x.ok).length,failed:results.filter(x=>!x.ok).length}));
 if(results.some(x=>!x.ok)) process.exitCode=1;
} finally {await browser?.close();await server.close();}
