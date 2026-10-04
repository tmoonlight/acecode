import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { createServer } from 'vite';

// Real preview panel, tabs, popup and CSS; deterministic filesystem responses.
// Optional ACE_PLAYWRIGHT_MODULE / ACE_CHROMIUM_EXECUTABLE match other UI tests.
const web = fileURLToPath(new URL('../', import.meta.url));
const modulePath = process.env.ACE_PLAYWRIGHT_MODULE;
const { chromium } = await import(modulePath ? pathToFileURL(path.resolve(modulePath)).href : 'playwright');
const output = process.env.ACE_BREADCRUMB_ARTIFACTS || await fs.mkdtemp(path.join(os.tmpdir(), 'ace-breadcrumbs-'));
await fs.mkdir(output, { recursive: true });
const fixture = String.raw`
import React, {useState} from 'react';
import {createRoot} from 'react-dom/client';
import {PreviewDetailsPanel} from '/src/components/PreviewDetailsPanel.jsx';
import {openFileTab,activePreviewTab,visiblePreviewTabs,activatePreviewTab,updateFileTabDraft,discardFileTabDraft} from '/src/lib/previewTabs.js';
import {createUnsavedFileGuard,runAfterFileApproval} from '/src/lib/unsavedFileGuard.js';
import '/src/styles/globals.css';
const context={scopeKey:'fixture',sessionId:'fixture'};
const entries=(names,dir='')=>names.map(([name,kind])=>({name,kind,path:dir?dir+'/'+name:name}));
const directories={
 '':entries([['web','dir'],['docs','dir'],['empty','dir'],['denied','dir'],['slow','dir'],['README.md','file']]),
 web:entries([['src','dir'],['index.html','file'],['style.css','file']],'web'),
 'web/src':entries([['app.js','file']],'web/src'),
 docs:entries([['guide.md','file']],'docs'), empty:[], denied:entries([['recovered.txt','file']],'denied'),
};
let deny=true, deferred=[];
const requests=[];
const api={
 listFiles:async(root,dir)=>{
  requests.push({root,dir});
  if(dir==='denied'&&deny){deny=false;throw Error('denied');}
  if(dir==='slow')return new Promise(resolve=>deferred.push(resolve));
  return directories[dir]||[];
 },
 readFile:async(_root,p)=>p.endsWith('.md')?'# Project guide\n\nWorkspace navigation.':'<!doctype html>\n<html>\n  <body>Workspace navigation</body>\n</html>',
 // Media loading is held to verify navigation independently of each renderer.
 readFileBlob:()=>new Promise(()=>{}),
};
function Fixture(){
 const [root,setRoot]=useState('C:/workspace/acecode');
 const [workspace,setWorkspace]=useState(true);
 const [panelWidth,setPanelWidth]=useState(720);
 const [tabState,setTabState]=useState(()=>openFileTab({}, {...context,cwd:root,path:'web/index.html'}));
 const [pending,setPending]=useState(null);
 const [guard]=useState(()=>createUnsavedFileGuard(setPending));
 const current=activePreviewTab(tabState,context);
 const open=(p)=>{
  const approval=guard.request({getTabs:()=>[current],save:async()=>{},discard:tabs=>setTabState(s=>tabs.reduce((next,t)=>discardFileTabDraft(next,{...context,tabKey:t.key}),s))});
  return runAfterFileApproval(approval,()=>setTabState(s=>openFileTab(s,{...context,cwd:root,path:p})));
 };
 window.fixture={requests,pending,
  get active(){return current?.path},get tabs(){return visiblePreviewTabs(tabState,context)},
  select(p,cwd=root){setTabState(s=>openFileTab(s,{...context,cwd,path:p}));},
  root(value){setRoot(value);setTabState(openFileTab({}, {...context,cwd:value,path:'web/index.html'}));},
  workspace:setWorkspace,width:setPanelWidth,
  dirty(){setTabState(s=>updateFileTabDraft(s,{...context,tabKey:current.key,patch:{text:'edited',baselineText:'original'}}));},
  choose(choice){return guard.choose(choice);},
  release(){deferred.splice(0).forEach(resolve=>resolve(entries([['stale.txt','file']],'slow')));},
 };
 return <main style={{display:'flex',height:'100vh',gap:16,padding:16}}>
  <aside className="ace-sidebar" style={{width:150,flexShrink:0,padding:12}}>工作区</aside>
  <section style={{width:panelWidth,minWidth:0,display:'flex',flexDirection:'column',border:'1px solid var(--ace-border)'}}>
   <PreviewDetailsPanel owner="breadcrumb-fixture" api={api} cwd={root} workspaceCwd={workspace?root:''}
    tabs={visiblePreviewTabs(tabState,context)} activeTab={current} sidePanelListCollapsed={true}
    onToggleSidePanelList={()=>{window.listToggles=(window.listToggles||0)+1;}}
    onActivateTab={key=>setTabState(s=>activatePreviewTab(s,{...context,tabKey:key}))}
    onEditFileTab={(key,patch)=>setTabState(s=>updateFileTabDraft(s,{...context,tabKey:key,patch}))}
    onOpenFilePreview={open} />
  </section>
  <section data-fixture-chrome style={{width:180,minWidth:0}}>
   <div className="ace-side-tabs"><button className="ace-side-tab" aria-selected="true">变更</button><button className="ace-side-tab">文件</button></div>
   <div className="ace-agent-browser-toolbar">浏览器工具栏</div>
  </section>
 </main>;
}
createRoot(document.getElementById('root')).render(<Fixture/>);
`;
const virtual = path.join(web, '__file-breadcrumbs.jsx').replaceAll('\\', '/');
const html = `<html data-theme="light" data-color-theme="blue"><body style="margin:0"><div id="root"></div><script type="module">
import RefreshRuntime from '/@react-refresh';RefreshRuntime.injectIntoGlobalHook(window);
window.$RefreshReg$=()=>{};window.$RefreshSig$=()=>type=>type;window.__vite_plugin_react_preamble_installed__=true;
</script><script type="module" src="/__file-breadcrumbs.jsx"></script></body></html>`;
const server = await createServer({ root: web, configFile: path.join(web, 'vite.config.js'), logLevel: 'error',
  plugins: [{name:'file-breadcrumb-fixture',enforce:'pre',
    resolveId(id){if(id==='/__file-breadcrumbs.jsx')return virtual;},
    load(id){if(id.replaceAll('\\','/')===virtual)return fixture;},
    configureServer(vite){vite.middlewares.use((req,res,next)=>{
      if(req.url?.split('?')[0]!=='/__file-breadcrumbs')return next();
      res.setHeader('Content-Type','text/html');res.end(html);
    });},
  }], server:{host:'127.0.0.1',port:0,hmr:false},
});
let browser;
try {
  await server.listen();
  browser = await chromium.launch({headless:true,...(process.env.ACE_CHROMIUM_EXECUTABLE ? {executablePath:process.env.ACE_CHROMIUM_EXECUTABLE} : {})});
  const page = await browser.newPage({viewport:{width:1200,height:760}});
  const errors=[];
  page.on('pageerror',error=>errors.push(error.message));
  await page.goto(`http://127.0.0.1:${server.httpServer.address().port}/__file-breadcrumbs`);
  const nav=page.getByRole('navigation',{name:'文件路径'});
  const menu=page.locator('.ace-breadcrumb-menu');
  const row=p=>menu.locator(`[data-path="${p}"]`);
  const settle=()=>page.evaluate(()=>new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(resolve))));
  async function active(expected){await page.waitForFunction(p=>window.fixture.active===p,expected);}
  async function openRoot(){await nav.getByRole('button',{name:'acecode',exact:true}).click();await row('web').waitFor();}
  async function theme(mode,color){await page.evaluate(([a,b])=>{
    document.documentElement.dataset.theme=a;document.documentElement.dataset.colorTheme=b;
  },[mode,color]);await page.waitForTimeout(300);}
  await nav.waitFor();
  for(const [mode,color] of [['light','blue'],['dark','blue'],['light','orange'],['light','ai-test']]){
    await theme(mode,color);
    const colors=await page.evaluate(()=>{
      const css=s=>getComputedStyle(document.querySelector(s)).backgroundColor;
      const probe=document.createElement('div');probe.style.background='var(--ace-surface-hi)';document.body.append(probe);
      const high=getComputedStyle(probe).backgroundColor;probe.remove();
      return {sidebar:css('.ace-sidebar'),high,bars:['.ace-preview-details-tabs','.ace-agent-browser-toolbar','.ace-side-tabs'].map(css),
        selected:getComputedStyle(document.querySelector('.ace-side-tab')).color,accent:getComputedStyle(document.documentElement).getPropertyValue('--ace-accent').trim()};
    });
    colors.bars.forEach(value=>assert.equal(value,mode==='light'&&color==='blue'?colors.sidebar:colors.high,`${mode}/${color}`));
  }
  console.log('[pass] default light chrome matches sidebar; dark, orange and custom palettes retain their backgrounds');
  await theme('light','blue');
  await nav.getByRole('button',{name:'web',exact:true}).click();
  await row('web').waitFor();
  assert.equal(await row('web').getAttribute('aria-selected'),'true');
  await row('docs').click();await row('docs/guide.md').click();await active('docs/guide.md');
  assert.equal(await page.getByRole('tab').count(),2);
  await openRoot();await row('web').click();await row('web/index.html').click();await active('web/index.html');
  assert.equal(await page.getByRole('tab').count(),2);
  assert.equal(await page.getByRole('button',{name:'展开列表面板'}).getAttribute('aria-expanded'),'false');
  assert.equal(await page.evaluate(()=>window.listToggles||0),0);
  console.log('[pass] directory segment browses siblings; nested file opens and existing tab is reused');
  await nav.getByRole('button',{name:'index.html',exact:true}).focus();await page.keyboard.press('ArrowDown');
  await row('web/index.html').waitFor();
  await page.waitForFunction(()=>document.activeElement?.dataset.path==='web/index.html');
  await page.keyboard.press('Home');await page.keyboard.press('ArrowRight');await row('web/src/app.js').waitFor();
  await page.waitForFunction(()=>document.activeElement?.dataset.path==='web/src/app.js');
  await page.keyboard.press('ArrowLeft');assert.equal(await row('web/src').evaluate(e=>e===document.activeElement),true);
  await page.keyboard.press('ArrowLeft');assert.equal(await row('web/src').getAttribute('aria-expanded'),'false');
  await page.keyboard.press('ArrowDown');await page.keyboard.press('ArrowDown');await page.keyboard.press('Enter');
  await active('web/style.css');
  await nav.getByRole('button',{name:'style.css',exact:true}).click();await row('web/style.css').waitFor();
  await page.keyboard.press('Escape');await menu.waitFor({state:'detached'});
  assert.equal(await nav.getByRole('button',{name:'style.css',exact:true}).evaluate(e=>e===document.activeElement),true);
  console.log('[pass] keyboard open, selected focus, tree navigation, Enter, Escape and focus restoration');
  await openRoot();await row('empty').click();await menu.getByText('空目录').waitFor();
  await row('denied').click();await menu.getByText('无法读取目录').waitFor();
  await menu.getByRole('button',{name:'重试'}).click();await row('denied/recovered.txt').waitFor();
  await row('slow').click();await menu.getByText('加载中...').waitFor();
  await page.evaluate(()=>window.fixture.root('C:/trees/feature'));await menu.waitFor({state:'detached'});
  await page.evaluate(()=>window.fixture.release());await settle();assert.equal(await page.getByText('stale.txt').count(),0);
  await nav.getByRole('button',{name:'feature',exact:true}).click();await row('web').waitFor();
  assert.equal(await page.evaluate(()=>window.fixture.requests.at(-1).root),'C:/trees/feature');
  await row('slow').click();await menu.getByText('加载中...').waitFor();
  await page.evaluate(()=>window.fixture.select('docs/guide.md'));await menu.waitFor({state:'detached'});
  await page.evaluate(()=>window.fixture.release());await settle();assert.equal(await page.getByText('stale.txt').count(),0);
  await nav.getByRole('button',{name:'guide.md',exact:true}).click();await row('docs/guide.md').waitFor();
  await page.keyboard.press('Escape');
  console.log('[pass] empty directory, retry, stale response isolation and actual worktree request root');
  await page.evaluate(()=>window.fixture.select('external.txt','C:/outside'));await nav.waitFor({state:'detached'});
  await page.evaluate(()=>window.fixture.select('web/index.html'));await nav.waitFor();
  await page.evaluate(()=>window.fixture.workspace(false));await nav.waitFor({state:'detached'});
  await page.evaluate(()=>window.fixture.workspace(true));await nav.waitFor();
  for(const name of ['file.cpp','file.md','file.png','file.pdf','file.docx','file.xlsx','file.pptx']){
    await page.evaluate(p=>window.fixture.select(p),name);await active(name);
    await nav.getByRole('button',{name,exact:true}).waitFor();
  }
  console.log('[pass] all seven file kinds expose shared navigation; outside/no-workspace files hide it');
  await page.evaluate(()=>window.fixture.root('C:/workspace/acecode'));await active('web/index.html');
  await page.evaluate(()=>window.fixture.dirty());await settle();
  await nav.getByRole('button',{name:'index.html',exact:true}).click();await row('web/style.css').click();
  await page.waitForFunction(()=>window.fixture.pending?.dirtyCount===1);assert.equal(await page.evaluate(()=>window.fixture.active),'web/index.html');
  await page.evaluate(()=>window.fixture.choose('cancel'));await settle();await active('web/index.html');
  await nav.getByRole('button',{name:'index.html',exact:true}).click();await row('web/style.css').click();
  await page.waitForFunction(()=>window.fixture.pending?.dirtyCount===1);
  await page.evaluate(()=>window.fixture.choose('discard'));await active('web/style.css');
  assert.equal(await page.evaluate(()=>window.fixture.tabs.find(t=>t.path==='web/index.html').edit.text), 'original');
  console.log('[pass] breadcrumb callback respects unsaved-file cancel and discard before switching');
  await page.waitForFunction(()=>document.querySelector('.ace-preview-details-body')?.textContent.includes('Workspace navigation'));
  await openRoot();await row('web').click();await row('web/src').click();await row('web/src/app.js').waitFor();
  await page.screenshot({path:path.join(output,'breadcrumbs-desktop.png')});
  await page.keyboard.press('Escape');
  await page.setViewportSize({width:430,height:650});
  await page.evaluate(()=>{document.querySelector('aside').style.display='none';document.querySelector('[data-fixture-chrome]').style.display='none';window.fixture.width(280);window.fixture.select('long-directory-name/another-long-directory/very-long-file-name.html');});
  const last=nav.getByRole('button',{name:'very-long-file-name.html',exact:true});await last.waitFor();
  await last.click();await menu.waitFor();await settle();
  const bounds=await menu.boundingBox();assert.ok(bounds.x>=0&&bounds.x+bounds.width<=430&&bounds.y+bounds.height<=650);
  assert.ok(await nav.evaluate(e=>e.scrollWidth>e.clientWidth&&e.scrollLeft>0));
  await page.screenshot({path:path.join(output,'breadcrumbs-narrow.png')});
  await page.locator('main').click({position:{x:395,y:610}});await menu.waitFor({state:'detached'});
  console.log('[pass] long path scroll, narrow popup containment and outside dismissal');
  assert.deepEqual(errors,[]);
  console.log(`Screenshots: ${output}`);
} finally { await browser?.close();await server.close(); }
