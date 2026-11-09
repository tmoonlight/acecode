// Production transcript reducer + recommendation hook + InputBar/Slate.
// Run from web with ACE_PLAYWRIGHT_MODULE and optional ACE_CHROMIUM_EXECUTABLE.
import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { createServer } from 'vite';

const web = fileURLToPath(new URL('../', import.meta.url));
const { chromium } = await import(process.env.ACE_PLAYWRIGHT_MODULE
  ? pathToFileURL(path.resolve(process.env.ACE_PLAYWRIGHT_MODULE)).href : 'playwright');
const output = process.env.ACE_FOLLOW_UP_ARTIFACTS || await fs.mkdtemp(path.join(os.tmpdir(), 'ace-follow-up-'));
await fs.mkdir(output, { recursive: true });
const fixture = String.raw`
import React,{useState,useMemo} from 'react';
import {createRoot} from 'react-dom/client';
import {InputBar} from '/src/components/InputBar.jsx';
import {ChatView} from '/src/components/ChatView.jsx';
import {connection} from '/src/lib/connection.js';
import {SlashCommandsProvider} from '/src/components/SlashCommandsContext.jsx';
import {useFollowUpPrompt} from '/src/lib/useFollowUpPrompt.js';
import {createTranscriptState,loadTranscriptHistory,reduceTranscriptEvent} from '/src/lib/sessionTranscript.js';
import {composerContentFromText} from '/src/lib/composerContent.js';
import {createPastedTextPart} from '/src/lib/pastedText.js';
import '/src/i18n/index.js';
import '/src/styles/globals.css';
const timing=(id,at=200,status='completed')=>({role:'system',metadata:{transcript_only:true,turn_timing:{user_message_uuid:id,started_at_ms:100,completed_at_ms:at,duration_ms:at-100,status}}});
const loaded=(id)=>loadTranscriptHistory(createTranscriptState(),{busy:false,messages:[{id,role:'user',content:'Implement the parser'},{role:'assistant',content:'The implementation is ready. Tests remain.'},timing(id)]}).state;
const requests=[];const sends=[];
const api={followUpPrompt:(sid,key)=>new Promise(resolve=>requests.push({sid,key,resolve})),getSpeechAvailability:async()=>({enabled:false})};
function Fixture(){
 const [sid,setSid]=useState('s1'),[states,setStates]=useState({s1:loaded('u1')}),[drafts,setDrafts]=useState({}),[extras,setExtras]=useState([]),[enabled,setEnabled]=useState(true);
 const sessionRef=useMemo(()=>({sessionId:sid,workspaceHash:'fixture'}),[sid]);
 const transcript=states[sid]||createTranscriptState({loadState:'loading'});
 const draft=drafts[sid]||{text:'',content:composerContentFromText('')};
 const suggestion=useFollowUpPrompt({sessionRef,transcript,api,enabled,draftReady:true,draft:{...draft,attachments:extras}});
 const change=(text,content)=>setDrafts(prev=>({...prev,[sid]:{text,content}}));
 const emit=(type,payload)=>setStates(prev=>({...prev,[sid]:reduceTranscriptEvent(prev[sid],{type,payload}).state}));
 window.fixture={requests,sends,sid,value:draft.text,state:transcript,suggestion,
  select:(id,load=true)=>{setSid(id);setExtras([]);if(load)setStates(prev=>prev[id]?prev:{...prev,[id]:loaded(id+'-u')});},
  load:()=>setStates(prev=>({...prev,[sid]:loaded(sid+'-u')})),
  emit,seed:(text)=>change(text,composerContentFromText(text)),setExtras,setEnabled,
  paste:()=>change('',{version:1,parts:[createPastedTextPart('Pasted context')]}),
  finish:(id,text)=>{const req=requests.findLast(r=>r.sid===id&&!r.finished);if(!req)throw Error('No pending request '+id);req.finished=true;req.resolve({completion_key:req.key,prompt:text});},
  start:(id)=>{emit('message',{role:'user',id,content:'Continue'});emit('busy_changed',{busy:true,turn_id:id});},
  complete:(id,at=300,status='completed')=>{emit('message',{role:'assistant',content:'Done.'});emit('message',timing(id,at,status));emit('busy_changed',{busy:false,turn_id:id,outcome:status});emit('done',{turn_id:id,outcome:status});},
 };
 return <main className="min-h-screen bg-bg text-fg" style={{padding:'32px 16px'}}>
 <div style={{maxWidth:860,margin:'0 auto'}}><div className="mb-8 text-[14px]">The implementation is ready. Tests remain.</div>
 <SlashCommandsProvider><InputBar mainComposer followUpPrompt={suggestion} value={draft.text} composerContent={draft.content}
  onChange={change} onSubmit={text=>sends.push(text)} busy={transcript.busy} currentSessionId={sid} cwd="C:/fixture"
  attachments={extras} pathReferenceApi={api}/></SlashCommandsProvider><button id="outside">Outside</button></div></main>;
}
function FullChat(){
 const [sid,setSid]=useState(null); const empty=useMemo(()=>[],[]);
 window.fullFixture={render:setSid,event:(type,payload)=>connection.dispatchEvent(new CustomEvent('message',{detail:{type,session_id:sid,payload}}))};
 return <div className="font-sans bg-bg text-fg" style={{height:'100vh',display:'flex'}}><ChatView sessionRef={sid?{sessionId:sid}:null}
  health={{}} permissionRequests={empty} recentExpertIds={empty}/></div>;
}
createRoot(document.getElementById('root')).render(<React.StrictMode>{location.search.includes('full')?<FullChat/>:<Fixture/>}</React.StrictMode>);
`;
const virtual = path.join(web, '__follow-up-prompts.jsx').replaceAll('\\', '/');
const html = `<html><body><div id="root"></div><script type="module">
import RefreshRuntime from '/@react-refresh'; RefreshRuntime.injectIntoGlobalHook(window);
window.$RefreshReg$=()=>{};window.$RefreshSig$=()=>type=>type;
window.__vite_plugin_react_preamble_installed__=true;</script>
<script type="module" src="/__follow-up-prompts.jsx"></script></body></html>`;
const server = await createServer({ root: web, logLevel: 'error',
  plugins: [{ name: 'follow-up-fixture', enforce: 'pre',
    resolveId(id) { if (id === '/__follow-up-prompts.jsx') return virtual; },
    load(id) { if (id.replaceAll('\\', '/') === virtual) return fixture; },
    configureServer(vite) { vite.middlewares.use((req, res, next) => {
      if (req.url?.split('?')[0] !== '/__follow-up-prompts') return next();
      res.setHeader('Content-Type', 'text/html'); res.end(html);
    }); },
  }], server: { host: '127.0.0.1', port: 0, open: false, hmr: false } });
let browser;
try {
  await server.listen();
  browser = await chromium.launch({ headless: true, executablePath: process.env.ACE_CHROMIUM_EXECUTABLE || undefined });
  const page = await browser.newPage({ viewport: { width: 1100, height: 720 }, reducedMotion: 'reduce' });
  const errors = []; page.on('pageerror', error => errors.push(String(error)));
  let full=false; const fullRequests=[]; const heldDrafts=new Map();
  await page.route('**/api/**', async route => {
    const pathname=new URL(route.request().url()).pathname;
    let body={enabled:false,items:[]};
    if(pathname.endsWith('/commands')) body={commands:[],skills:[],builtins:[{name:'help',description:'Help'}]};
    if(full){
      const sid=pathname.split('/')[3];
      if(pathname.endsWith('/messages')) body={busy:false,events:[],messages:[
        {id:sid+'-u',role:'user',content:'Implement the parser'},
        {id:sid+'-answer',role:'assistant',content:'The parser is implemented. Tests remain.'},
        {role:'system',metadata:{transcript_only:true,turn_timing:{user_message_uuid:sid+'-u',started_at_ms:100,completed_at_ms:200,duration_ms:100,status:'completed'}}},
      ]};
      else if(pathname.endsWith('/follow-up-prompt')) {fullRequests.push({sid,...route.request().postDataJSON()});body={...route.request().postDataJSON(),prompt:'Run the parser tests for '+sid+'.'};}
      else if(pathname.endsWith('/draft') && route.request().method()==='GET') {
        if(sid.startsWith('held-')) {heldDrafts.set(sid,route);return;}
        if(sid==='draft-error') {await route.fulfill({status:503,contentType:'application/json',body:'{"error":"UNAVAILABLE"}'});return;}
        body=sid==='saved-draft'?{text:'Saved unsent work'}:sid==='structured-draft'
          ?{text:'',composer_content:{version:1,parts:[{type:'pasted_text',key:'saved-paste',text:'Saved pasted context'}]}}
          :{text:''};
      }
      else if(pathname==='/api/models')body=[];
      else if(pathname.endsWith('/workspaces'))body={workspaces:[]};
      else if(pathname.endsWith('/sessions'))body={sessions:[]};
      else if(pathname.includes('/experts'))body={experts:[],items:[],capabilities:[]};
      else if(pathname.includes('/files'))body={files:[],entries:[]};
      else if(pathname.includes('/git'))body={is_repo:false,files:[],branches:[]};
    }
    await route.fulfill({contentType:'application/json',body:JSON.stringify(body)});
  });
  await page.goto(`http://127.0.0.1:${server.httpServer.address().port}/__follow-up-prompts`);
  const editor = page.locator('[data-ace-rich-composer]');
  const ghost = page.locator('[data-slate-placeholder]');
  const ghostText = async () => (await ghost.allTextContents()).join('');
  const waitRequest = (sid) => page.waitForFunction(sid=>window.fixture?.requests.some(r=>r.sid===sid&&!r.finished),sid);
  const settle = () => page.evaluate(() => new Promise(resolve=>setTimeout(resolve,80)));
  const resolve = async (sid, text = '请运行相关测试，并修复发现的问题。') => {
    await waitRequest(sid); await page.evaluate(([sid,text])=>window.fixture.finish(sid,text),[sid,text]); await settle();
  };
  const select = async (sid,load=true) => {await page.evaluate(([id,load])=>window.fixture.select(id,load),[sid,load]);await settle();};
  const value = () => page.evaluate(()=>window.fixture.value);
  await resolve('s1');
  assert.equal(await value(), '');
  assert.equal(await page.evaluate(()=>window.fixture.requests.length),1,'StrictMode and rerenders share one request');
  assert.match(await ghost.textContent(), /请运行相关测试/);
  await editor.click(); await page.keyboard.press('Tab'); await settle();
  assert.equal(await value(),'请运行相关测试，并修复发现的问题。');
  assert.equal(await page.evaluate(()=>window.fixture.sends.length),0);
  await page.keyboard.type(' Add detail.'); assert.match(await value(), /问题。 Add detail\.$/);
  await page.evaluate(()=>window.fixture.seed('')); await settle();
  assert.doesNotMatch(await ghost.textContent(),/请运行/);
  console.log('[pass] first loaded completion, StrictMode deduplication, Tab inserts editable text without sending');

  await page.evaluate(()=>window.fixture.start('u2')); await settle();
  assert.equal(await page.evaluate(()=>window.fixture.requests.length),1);
  await page.evaluate(()=>window.fixture.emit('reasoning',{text:'thinking'})); await settle();
  assert.equal(await page.evaluate(()=>window.fixture.requests.length),1);
  await page.evaluate(()=>window.fixture.complete('u2')); await resolve('s1','Review the final diff.');
  assert.equal(await page.evaluate(()=>window.fixture.requests.length),2);
  await editor.click(); await page.keyboard.press('Escape'); await settle();
  assert.doesNotMatch(await ghost.textContent(),/Review/);
  console.log('[pass] whole-turn completion triggers once; reasoning and trailing done do not; Escape dismisses');

  await select('loading',false); await settle();
  assert.equal(await page.evaluate(()=>window.fixture.requests.length),2);
  await page.evaluate(()=>window.fixture.load()); await resolve('loading');
  await select('s2'); await waitRequest('s2'); await select('s3');
  await resolve('s2','Suggestion for session two.');
  assert.doesNotMatch(await ghost.textContent(),/session two/);
  await resolve('s3','Suggestion for session three.');
  await select('s2'); assert.match(await ghost.textContent(),/session two/);
  assert.equal(await page.evaluate(()=>window.fixture.requests.filter(r=>r.sid==='s2').length),1);
  console.log('[pass] loading gate, late response isolation and cached revisit');

  await select('edited'); await waitRequest('edited'); await editor.click(); await page.keyboard.type('My draft');
  await resolve('edited'); assert.equal(await value(),'My draft');
  await page.evaluate(()=>window.fixture.seed('')); await settle(); assert.doesNotMatch(await ghostText(),/请运行/);
  await select('attachment'); await page.evaluate(()=>window.fixture.setExtras([{id:'file1',name:'notes.txt',kind:'file',size_bytes:4}]));
  await resolve('attachment'); assert.equal(await value(),''); assert.doesNotMatch(await ghostText(),/请运行/);
  await select('pasted'); await page.evaluate(()=>window.fixture.paste()); await resolve('pasted');
  assert.equal(await page.locator('[data-composer-pasted-text-strip]').count(),1);
  assert.doesNotMatch(await ghostText(),/请运行/);
  console.log('[pass] in-flight typing, attachments and pasted blocks preserve drafts and consume suggestions');

  await select('ime'); await resolve('ime'); await editor.click();
  await editor.dispatchEvent('compositionstart',{data:''});
  await editor.dispatchEvent('compositionupdate',{data:'测'});
  await editor.dispatchEvent('keydown',{key:'Tab',code:'Tab',isComposing:true,keyCode:229});
  assert.equal(await value(),''); await editor.dispatchEvent('compositionend',{data:''}); await settle();
  await select('shift'); await resolve('shift'); await editor.click(); await page.keyboard.press('Shift+Tab');
  assert.equal(await value(),'');
  await select('aborted'); await resolve('aborted');
  await page.evaluate(()=>window.fixture.start('aborted-next')); await settle();
  const beforeAbort=await page.evaluate(()=>window.fixture.requests.length);
  await page.evaluate(()=>window.fixture.complete('aborted-next',500,'aborted')); await settle();
  assert.equal(await page.evaluate(()=>window.fixture.requests.length),beforeAbort);
  console.log('[pass] IME and modified Tab keep priority; aborted turns do not generate');

  await select('live-draft'); await resolve('live-draft');
  await page.evaluate(()=>window.fixture.start('live-draft-next')); await editor.click(); await page.keyboard.type('Unsent follow-up');
  const beforeDraft=await page.evaluate(()=>window.fixture.requests.length);
  await page.evaluate(()=>window.fixture.complete('live-draft-next',600)); await settle();
  assert.equal(await page.evaluate(()=>window.fixture.requests.length),beforeDraft,'live text blocks generation');
  await page.evaluate(()=>window.fixture.seed('')); await settle();
  assert.equal(await page.evaluate(()=>window.fixture.requests.length),beforeDraft,'clearing is not a new trigger');
  for(const [id,seed] of [['live-file',()=>window.fixture.setExtras([{id:'file-gate',name:'draft.txt'}])],
    ['live-paste',()=>window.fixture.paste()]]) {
    await select(id); await resolve(id); await page.evaluate(id=>window.fixture.start(id+'-next'),id);
    await page.evaluate(seed); const before=await page.evaluate(()=>window.fixture.requests.length);
    await page.evaluate(id=>window.fixture.complete(id+'-next',700),id);await settle();
    assert.equal(await page.evaluate(()=>window.fixture.requests.length),before,'non-text draft blocks generation');
  }
  console.log('[pass] visible-turn completion with unsent text, attachments or pasted blocks sends no request');

  // One batched visual matrix, using the actual composer and production styles.
  const captures=[];
  for(const [theme,width,name,text] of [
    ['light',1100,'desktop','请运行相关测试，并修复发现的问题。'],
    ['dark',390,'mobile','Review the implementation, run the relevant tests, and fix any issues before preparing the final change for review.'],
  ]) {
    await page.setViewportSize({width,height:720});
    await page.evaluate(theme=>{document.documentElement.setAttribute('data-theme',theme);document.documentElement.classList.toggle('dark',theme==='dark');},theme);
    await select('visual-'+name); await resolve('visual-'+name,text); await editor.click();
    assert.equal(await value(),'');
    await ghost.waitFor({state:'visible',timeout:3000});
    assert.equal(await ghostText(),text,'the captured ghost must actually be visible');
    const preview=await ghost.boundingBox(),field=await editor.boundingBox();
    assert.ok(preview.y+preview.height<=field.y+field.height+1,'wrapped suggestion fits above the footer');
    assert.equal(await ghost.evaluate(node=>getComputedStyle(node).opacity),'1');
    const overflow=await page.evaluate(()=>document.documentElement.scrollWidth>innerWidth); assert.equal(overflow,false);
    const screenshot=path.join(output,name+'.png');await page.screenshot({path:screenshot});captures.push(screenshot);
  }
  assert.deepEqual(errors,[]);
  console.log('[pass] desktop/light and mobile/dark screenshots; no page errors or horizontal overflow');
  full=true;
  await page.setViewportSize({width:1100,height:720});
  await page.goto(`http://127.0.0.1:${server.httpServer.address().port}/__follow-up-prompts?full`);
  await editor.waitFor(); assert.equal(fullRequests.length,0,'home never generates a session suggestion');
  await page.evaluate(()=>window.fullFixture.render('full-a'));
  await page.waitForFunction(()=>document.querySelector('[data-slate-placeholder]')?.textContent.includes('full-a'));
  const originalPrompt = await ghost.textContent();
  await editor.click();
  for (const text of ['My own draft', ' ']) {
    await page.keyboard.type(text);
    assert.doesNotMatch(await ghostText(), /full-a/);
    await page.keyboard.press('Control+A');
    await page.keyboard.press('Backspace');
    await ghost.filter({hasText: originalPrompt}).waitFor({state:'visible'});
    assert.equal(await ghost.textContent(), originalPrompt);
  }
  assert.equal(fullRequests.filter(r=>r.sid==='full-a').length,1,'clearing reuses the original recommendation');
  await editor.click();await page.keyboard.press('Tab');await settle();
  assert.match(await editor.textContent(),/Run the parser tests for full-a/);
  await page.evaluate(()=>window.fullFixture.render('full-b'));
  await page.waitForFunction(()=>document.querySelector('[data-slate-placeholder]')?.textContent.includes('full-b'));
  assert.equal(fullRequests.filter(r=>r.sid==='full-a').length,1);
  assert.equal(fullRequests.filter(r=>r.sid==='full-b').length,1);
  const switchFull=async id=>{await page.evaluate(id=>window.fullFixture.render(id),id);await settle();};
  const requestCount=id=>fullRequests.filter(r=>r.sid===id).length;
  await switchFull('saved-draft');
  await page.waitForFunction(()=>document.querySelector('[data-ace-rich-composer]')?.textContent.includes('Saved unsent work'));
  assert.equal(requestCount('saved-draft'),0,'saved draft prevents the model request');
  await editor.click();await page.keyboard.press('Control+A');await page.keyboard.press('Backspace');await settle();
  assert.equal(requestCount('saved-draft'),0,'clearing a restored draft does not trigger generation');
  await switchFull('full-b');await switchFull('saved-draft');await settle();
  assert.equal(requestCount('saved-draft'),0,'revisiting the skipped completion still does not generate');
  await switchFull('structured-draft');
  await page.locator('[data-composer-pasted-text-strip]').waitFor();
  assert.equal(requestCount('structured-draft'),0,'persisted pasted blocks are draft content');
  const waitHeld=async id=>{const end=Date.now()+3000;while(!heldDrafts.has(id)&&Date.now()<end)await new Promise(r=>setTimeout(r,20));assert.ok(heldDrafts.has(id));};
  await switchFull('held-draft');await waitHeld('held-draft');await settle();
  assert.equal(requestCount('held-draft'),0,'loaded history must wait for the draft');
  await heldDrafts.get('held-draft').fulfill({contentType:'application/json',body:'{"text":"Late restored draft"}'});
  await page.waitForFunction(()=>document.querySelector('[data-ace-rich-composer]')?.textContent.includes('Late restored draft'));
  assert.equal(requestCount('held-draft'),0);
  await switchFull('held-empty');await waitHeld('held-empty');assert.equal(requestCount('held-empty'),0);
  await heldDrafts.get('held-empty').fulfill({contentType:'application/json',body:'{"text":""}'});
  await page.waitForFunction(()=>document.querySelector('[data-slate-placeholder]')?.textContent.includes('held-empty'));
  assert.equal(requestCount('held-empty'),1,'confirmed empty draft permits generation');
  await switchFull('draft-error');await settle();assert.equal(requestCount('draft-error'),0,'unknown persisted draft fails closed');
  assert.deepEqual(errors,[]);
  console.log('[pass] real ChatView home, loaded session, Tab acceptance and session switch use the endpoint once');
  console.log('[pass] real ChatView restored/structured drafts, slow restoration, empty restoration and load failure gate requests');
  console.log(JSON.stringify({artifacts:output,screenshots:captures}));
} finally { await browser?.close(); await server.close(); }
