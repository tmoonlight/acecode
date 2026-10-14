import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { createServer } from 'vite';

const web = fileURLToPath(new URL('../', import.meta.url));
const { chromium } = await import(process.env.ACE_PLAYWRIGHT_MODULE
  ? pathToFileURL(path.resolve(process.env.ACE_PLAYWRIGHT_MODULE)).href : 'playwright');
const output = process.env.ACE_SIDEBAR_ARTIFACTS || await fs.mkdtemp(path.join(os.tmpdir(), 'ace-sidebar-tree-'));
await fs.mkdir(output, { recursive: true });
const fixture = String.raw`
import React,{useState} from 'react';
import {createRoot} from 'react-dom/client';
import {Sidebar} from '/src/components/Sidebar.jsx';
import {api} from '/src/lib/api.js';
import {connection} from '/src/lib/connection.js';
import {SESSION_LIST_CHANGED_EVENT} from '/src/lib/sessionListEvents.js';
import {validSidebarTreeDrop} from '/src/lib/sidebarSessionTree.js';
import '/src/styles/globals.css';
const initial = [
 {key:'session:n1',id:'n1',title:'普通会话一',children:[]},
 {key:'loop:daily',id:'run-new',loopId:'daily',title:'每日简报：最近执行',children:Array.from({length:8},(_,i)=>({key:'session:old-'+i,id:'old-'+i,loopId:'daily',title:'每日简报：历史 '+i,children:[]}))},
 {key:'session:n2',id:'n2',title:'普通会话二',children:[]},
 {key:'loop:check',id:'check',loopId:'check',title:'依赖检查：最近执行',children:[{key:'session:check-old',id:'check-old',loopId:'check',title:'依赖检查：历史',children:[]}]},
 {key:'loop:single',id:'single',loopId:'single',title:'定时检查：首次执行',children:[]},
 ...Array.from({length:3},(_,i)=>({key:'session:n'+(i+3),id:'n'+(i+3),title:'普通会话 '+(i+3),children:[]}))
];
let roots=JSON.parse(localStorage.getItem('tree-fixture')||'null')||initial;
const persist=()=>localStorage.setItem('tree-fixture',JSON.stringify(roots));
const locate=key=>{
 for(const root of roots){if(root.key===key)return {node:root,parent:null,siblings:roots};
  const node=root.children.find(child=>child.key===key);if(node)return {node,parent:root,siblings:root.children};}
};
window.calls=[];window.moves=[];
const payload=(opts={})=>{
 const expanded=new Set(opts.expanded||[]);
 let limit=opts.limit||roots.length;
 const reveal=roots.findIndex(root=>root.id===opts.reveal||root.children.some(child=>child.id===opts.reveal));
 if(reveal>=0){limit=Math.max(limit,reveal+1);if(roots[reveal].id!==opts.reveal)expanded.add(roots[reveal].key);}
 const rows=[];
 function emit(node,parent,rank){rows.push({id:node.id,title:node.title,workspace_hash:'w',active:true,created_at:'2026-10-13T09:00:00Z',updated_at:'2026-10-13T09:00:00Z',
  sidebar_key:node.key,sidebar_parent_key:parent,sidebar_child_count:node.children.length,sidebar_rank:rank,sidebar_expanded:expanded.has(node.key),
  ...(node.loopId?{loop_execution:{loop_id:node.loopId}}:{})});}
 roots.slice(0,limit).forEach((root,rank)=>{emit(root,'',rank);if(expanded.has(root.key))root.children.forEach((child,index)=>emit(child,root.key,index));});
 return {sidebar_tree:true,sessions:rows,total:roots.length,total_exact:true,has_more:limit<roots.length,visible_roots:Math.min(limit,roots.length)};
};
Object.assign(api,{
 listWorkspaces:async()=>[{hash:'w',cwd:'/fixture',name:'acecode',active:true}],
 listSidebarSessions:async(hash,opts)=>{window.calls.push({hash,opts});return hash?payload(opts):{sidebar_tree:true,sessions:[],total:0,has_more:false,visible_roots:0};},
 getPinnedSessions:async()=>({session_ids:[]}),getNoWorkspacePinnedSessions:async()=>({session_ids:[]}),getPinnedSessionOrder:async()=>({items:[]}),
 getOpencodeImportPreview:async()=>({available:false,count:0}),
 moveSidebarSession:async(hash,move,opts)=>{
  const source=locate(move.source),target=locate(move.target);
  if(!validSidebarTreeDrop({key:source.node.key,parent:source.parent?.key,childCount:source.node.children.length},
    {key:target.node.key,parent:target.parent?.key,scheduled:Boolean(target.node.loopId)},move.placement,
    target.parent?{key:target.parent.key,scheduled:Boolean(target.parent.loopId)}:null))throw Error('invalid tree');
  source.siblings.splice(source.siblings.indexOf(source.node),1);
  const into=move.placement==='inside'?target.node:target.parent;
  const siblings=into?into.children:roots;
  const at=move.placement==='inside'?siblings.length:siblings.indexOf(target.node)+(move.placement==='after'?1:0);
  siblings.splice(at,0,source.node);persist();window.moves.push(move);
  const index=roots.indexOf(into||source.node);
  return payload({...opts,limit:Math.max(opts.limit||5,Math.ceil((index+1)/5)*5),expanded:[...(opts.expanded||[]),...(into?[into.key]:[])]});
 },
});
for(const name of ['reconfigure','retainSession','releaseSession','subscribeWorkspaceStatus','unsubscribeWorkspaceStatus'])connection[name]=()=>{};
window.fetch=async()=>new Response('{}',{headers:{'Content-Type':'application/json'}});
function App(){
 const [active,setActive]=useState(null);
 window.fixture={roots:()=>roots,select:(id)=>setActive({sessionId:id,workspaceHash:'w',active:true}),
  attention(id,state){connection.dispatchEvent(new CustomEvent('message',{detail:{type:'session_status',payload:{session_id:id,workspace_hash:'w',state,timestamp_ms:Date.now()}}}));},
  addRun(){const root=roots.find(item=>item.key==='loop:daily');root.children.unshift({id:root.id,key:'session:'+root.id,loopId:root.loopId,title:root.title,children:[]});root.id='run-next';root.title='每日简报：新执行';persist();window.dispatchEvent(new CustomEvent(SESSION_LIST_CHANGED_EVENT,{detail:{workspaceHash:'w'}}));}};
 return <main className="flex h-screen bg-bg text-fg"><Sidebar width={340} activeRef={active} activeId={active?.sessionId} onSelect={setActive}/><section className="flex-1 p-8"><div data-selected>{active?.sessionId||'选择会话'}</div></section></main>;
}
createRoot(document.getElementById('root')).render(<App/>);
`;
const virtual = path.join(web, '__sidebar-tree-fixture.jsx').replaceAll('\\', '/');
const server = await createServer({ root: web, server: { host: '127.0.0.1', port: 0, open: false, hmr: false }, plugins: [{
  name: 'sidebar-tree-fixture', enforce: 'pre',
  resolveId(id) { return id === '/__sidebar-tree-fixture.jsx' ? virtual : undefined; },
  load(id) { return id.replaceAll('\\', '/') === virtual ? fixture : undefined; },
  configureServer(vite) { vite.middlewares.use(async(req,res,next)=>{
    if(req.url?.split('?')[0]!=='/sidebar-tree-fixture')return next();
    res.setHeader('Content-Type','text/html');
    res.end(await vite.transformIndexHtml('/sidebar-tree-fixture','<!doctype html><html lang="zh-CN" data-theme="light"><meta name="viewport" content="width=device-width,initial-scale=1"><body><div id="root"></div><script type="module" src="/__sidebar-tree-fixture.jsx"></script></body></html>'));
  }); },
}] });
let browser;
const errors=[];
try {
  await server.listen();
  browser=await chromium.launch({headless:true,...(process.env.ACE_CHROMIUM_EXECUTABLE?{executablePath:process.env.ACE_CHROMIUM_EXECUTABLE}:{})});
  const page=await browser.newPage({viewport:{width:1100,height:1000}});
  page.on('pageerror',error=>errors.push(error.message));
  const row=id=>page.locator('.ace-sidebar-session-row[data-desktop-session-id="'+id+'"]');
  const select=id=>row(id).locator('.ace-sidebar-session-title-button').click();
  const assertParentIcons=async(id,expanded,attention='read')=>{
    assert.equal(await row(id).locator('[data-icon-name="Alarm"]').evaluate(el=>getComputedStyle(el).opacity),'1',
      'scheduled provenance must stay visible while idle, hovered, focused or selected');
    const arrow=row(id).locator('.ace-sidebar-session-tree-chevron');
    if(attention==='read'){
      assert.equal(await arrow.evaluate(el=>getComputedStyle(el).opacity),'1','read parents always show their disclosure');
      assert.equal(await arrow.evaluate(el=>el.classList.contains('is-expanded')),expanded);
    }else{
      assert.equal(await arrow.count(),0,'unread and running indicators take precedence over the arrow');
      const status=row(id).locator('.ace-sidebar-session-tree-indicator [title="'+(attention==='unread'?'未读':'进行中')+'"]');
      assert.equal(await status.isVisible(),true);
      assert.equal(await status.evaluate(el=>getComputedStyle(el).opacity),'1');
    }
    assert.equal(await row(id).locator('.ace-sidebar-session-title-button').getAttribute('aria-expanded'),String(expanded));
    assert.equal(await row(id).locator('.ace-sidebar-session-tree-idle-lamp').count(),0,'parents must not add a gray placeholder dot');
  };
  const drag=async(source,target,placement='inside',allowed=true)=>{
    const a=await row(source).boundingBox(),b=await row(target).boundingBox();
    await page.mouse.move(a.x+70,a.y+a.height/2);await page.mouse.down();
    await page.mouse.move(a.x+74,a.y+a.height/2+9,{steps:3});
    await page.mouse.move(b.x+80,b.y+(placement==='before'?2:placement==='after'?b.height-2:b.height/2),{steps:10});
    if(!allowed)assert.equal(await page.locator('.is-drop-inside').count(),0,'invalid receivers must not show an inside-drop highlight');
    await page.mouse.up();
  };
  await page.goto(`http://127.0.0.1:${server.httpServer.address().port}/sidebar-tree-fixture`);
  await row('run-new').waitFor();
  await page.mouse.move(950,750);
  await assertParentIcons('run-new',false);
  assert.equal(await page.locator('.ace-sidebar-session-row').count(),5);
  await select('run-new');await row('old-7').waitFor();
  await page.mouse.move(950,750);
  await assertParentIcons('run-new',true);
  assert.equal(await page.locator('.ace-sidebar-session-row').count(),13);
  assert.equal(await page.locator('.ace-sidebar-session-tree-node').count(),5);
  await select('old-3');
  assert.equal(await page.locator('[data-selected]').textContent(),'old-3');
  assert.equal(await row('run-new').count(),1);
  await select('run-new');await row('old-7').waitFor({state:'hidden'});
  await assertParentIcons('run-new',false);
  await select('run-new');await row('old-7').waitFor();
  await drag('run-new','n1','before');
  await page.waitForFunction(()=>window.moves.length===1);
  assert.equal((await page.evaluate(()=>window.fixture.roots()))[0].key,'loop:daily');
  await drag('old-1','n1','before');
  await page.waitForFunction(()=>window.moves.length===2);
  assert.equal(await row('old-1').getAttribute('data-sidebar-tree-parent'),'');
  await page.reload();await row('old-1').waitFor();
  assert.equal(await row('old-1').getAttribute('data-sidebar-tree-parent'),'');
  await drag('old-1','run-new');
  await page.waitForFunction(()=>window.moves.length===1);
  await row('old-1').waitFor();
  assert.equal(await row('old-1').getAttribute('data-sidebar-tree-parent'),'loop:daily');
  await drag('old-2','check');
  await page.waitForFunction(()=>window.moves.length===2);
  assert.equal(await row('old-2').getAttribute('data-sidebar-tree-parent'),'loop:check');
  await drag('run-new','check');
  await drag('run-new','old-2','before');
  await drag('old-3','old-2');
  assert.equal(await page.evaluate(()=>window.moves.length),2,'nested parents and third-level drops must be rejected');
  await drag('old-3','n2','inside',false);
  await drag('n2','n1','inside',false);
  assert.equal(await page.evaluate(()=>window.moves.length),2,'ordinary sessions cannot receive scheduled or ordinary leaves');
  assert.equal(await row('n1').getAttribute('data-sidebar-tree-children'),'0');
  assert.equal(await row('n2').getAttribute('data-sidebar-tree-children'),'0');
  await drag('n2','single');
  await page.waitForFunction(()=>window.moves.length===3);
  assert.equal(await row('n2').getAttribute('data-sidebar-tree-parent'),'loop:single');
  await drag('old-2','n1','before');
  await page.waitForFunction(()=>window.moves.length===4);
  assert.equal(await row('old-2').getAttribute('data-sidebar-tree-scheduled'),'true');
  await drag('n2','old-2');
  await page.waitForFunction(()=>window.moves.length===5);
  assert.equal(await row('n2').getAttribute('data-sidebar-tree-parent'),'session:old-2');
  await page.evaluate(()=>window.fixture.addRun());await row('run-next').waitFor();
  assert.equal(await row('run-new').getAttribute('data-sidebar-tree-parent'),'loop:daily');
  await page.reload();await row('run-next').waitFor();
  await page.evaluate(()=>window.fixture.select('old-7'));await row('old-7').waitFor();
  for(const theme of ['light','dark']){
    await page.evaluate(mode=>document.documentElement.dataset.theme=mode,theme);
    await page.mouse.move(950,750);
    await assertParentIcons('run-next',true);
    await row('run-next').hover();
    await assertParentIcons('run-next',true);
    assert.equal(await row('run-next').getByRole('button',{name:'定时任务，归档',exact:true}).isVisible(),true);
    await page.screenshot({path:path.join(output,theme+'.png'),animations:'disabled'});
    await page.evaluate(()=>window.fixture.attention('check','unread'));
    await row('check').locator('[title="未读"]').waitFor();
    await assertParentIcons('check',false,'unread');
    await row('check').hover();
    await assertParentIcons('check',false,'unread');
    await row('check').locator('.ace-sidebar-session-title-button').focus();
    await assertParentIcons('check',false,'unread');
    await page.screenshot({path:path.join(output,theme+'-unread.png'),animations:'disabled'});
    await page.evaluate(()=>window.fixture.attention('check','in_progress'));
    await row('check').getByRole('status',{name:'进行中'}).waitFor();
    await assertParentIcons('check',false,'in_progress');
    await page.evaluate(()=>window.fixture.attention('check','read'));
    await row('check').locator('.ace-sidebar-session-tree-chevron').waitFor();
    await assertParentIcons('check',false);
  }
  await page.mouse.move(950,750);
  await row('run-next').locator('.ace-sidebar-session-title-button').focus();
  await page.keyboard.press('Tab');
  await assertParentIcons('run-next',true);
  await page.keyboard.press('Tab');
  assert.equal(await row('run-next').getByRole('button',{name:'定时任务，归档',exact:true}).evaluate(el=>el===document.activeElement),true);
  await assertParentIcons('run-next',true);
  await page.setViewportSize({width:390,height:844});
  assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth),true);
  await assertParentIcons('run-next',true);
  await page.screenshot({path:path.join(output,'narrow.png'),animations:'disabled'});
  await page.evaluate(()=>window.fixture.select('missing-session'));
  await page.waitForFunction(()=>window.calls.some(call=>call.opts?.reveal==='missing-session'));
  await page.waitForTimeout(150);
  assert.ok(await page.evaluate(()=>window.calls.filter(call=>call.opts?.reveal==='missing-session').length)<4,
    'a stale navigation target must not trigger a request/render loop');
  assert.deepEqual(errors,[]);
  console.log(JSON.stringify({result:'PASS',output,scenarios:18}));
} finally { await browser?.close(); await server.close(); }
