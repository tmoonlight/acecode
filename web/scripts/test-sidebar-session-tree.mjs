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
 {key:'session:n1',id:'n1',title:'普通会话一',worktree:{name:'fixture',branch:'fixture'},children:[]},
 {key:'loop:daily',id:'sidebar-group:loop:daily',group:true,loopId:'daily',title:'每日简报',children:[
   {key:'session:run-new',id:'run-new',loopId:'daily',title:'每日简报：最近执行',created:'2026-10-13T09:00:00Z',children:[]},
   ...Array.from({length:8},(_,i)=>({key:'session:old-'+i,id:'old-'+i,loopId:'daily',title:'每日简报：历史 '+i,created:'2026-10-'+String(12-i).padStart(2,'0')+'T09:00:00Z',children:[]}))]},
 {key:'session:n2',id:'n2',title:'普通会话二',children:[]},
 {key:'loop:check',id:'sidebar-group:loop:check',group:true,loopId:'check',title:'依赖检查',children:[
   {key:'session:check',id:'check',loopId:'check',title:'依赖检查：最近执行',children:[]},
   {key:'session:check-old',id:'check-old',loopId:'check',title:'依赖检查：历史',children:[]}]},
 {key:'loop:single',id:'sidebar-group:loop:single',group:true,loopId:'single',title:'定时检查',children:[{key:'session:single',id:'single',loopId:'single',title:'定时检查：首次执行',children:[]}]},
 ...Array.from({length:3},(_,i)=>({key:'session:n'+(i+3),id:'n'+(i+3),title:'普通会话 '+(i+3),children:[]}))
];
let roots=JSON.parse(localStorage.getItem('tree-fixture')||'null')||initial;
const persist=()=>localStorage.setItem('tree-fixture',JSON.stringify(roots));
const locate=key=>{
 for(const root of roots){if(root.key===key)return {node:root,parent:null,siblings:roots};
  const node=root.children.find(child=>child.key===key);if(node)return {node,parent:root,siblings:root.children};}
};
window.calls=[];window.moves=[];window.reads=[];window.retained=[];
const payload=(opts={})=>{
 const expanded=new Set(opts.expanded||[]);
 let limit=opts.limit||roots.length;
 const reveal=roots.findIndex(root=>root.id===opts.reveal||root.children.some(child=>child.id===opts.reveal));
 if(reveal>=0){limit=Math.max(limit,reveal+1);if(roots[reveal].id!==opts.reveal)expanded.add(roots[reveal].key);}
 const rows=[];
 function emit(node,parent,rank){const latest=node.group?node.children.reduce((a,b)=>(b.created||'')>(a.created||'')?b:a):node;
  rows.push({id:node.id,title:node.title,workspace_hash:'w',cwd:'/fixture',active:!node.group,created_at:latest.created||'2026-10-13T09:00:00Z',updated_at:latest.created||'2026-10-13T09:00:00Z',
  attention_state:node.state||'read',busy:node.state==='in_progress',
  ...(node.worktree?{worktree:node.worktree}:{}),
  ...(node.group?{sidebar_group:true,sidebar_group_name:node.title,sidebar_latest_session_id:latest.id,
   sidebar_members:node.children.map(child=>({id:child.id,state:child.state||'read',busy:child.state==='in_progress',workspace_hash:'w',cwd:'/fixture'}))}:{}),
  sidebar_key:node.key,sidebar_parent_key:parent,sidebar_child_count:node.children.length,sidebar_rank:rank,sidebar_expanded:expanded.has(node.key),
  ...(node.loopId?{loop_execution:{loop_id:node.loopId}}:{})});}
 roots.slice(0,limit).forEach((root,rank)=>{emit(root,'',rank);if(expanded.has(root.key))root.children.forEach((child,index)=>emit(child,root.key,index));});
 return {sidebar_tree:true,sessions:rows,total:roots.length,total_exact:true,has_more:limit<roots.length,visible_roots:Math.min(limit,roots.length)};
};
Object.assign(api,{
 listWorkspaces:async()=>[{hash:'w',cwd:'/fixture',name:'acecode',active:true}],
 getPinnedSessions:async()=>({session_ids:[]}),getNoWorkspacePinnedSessions:async()=>({session_ids:[]}),getPinnedSessionOrder:async()=>({items:[]}),
 getOpencodeImportPreview:async()=>({available:false,count:0}),
 moveSidebarSession:async(hash,move,opts)=>{
  const source=locate(move.source),target=locate(move.target);
  if(!validSidebarTreeDrop({key:source.node.key,parent:source.parent?.key,childCount:source.node.children.length,group:source.node.group},
    {key:target.node.key,parent:target.parent?.key,scheduled:Boolean(target.node.loopId)},move.placement,
    target.parent?{key:target.parent.key,scheduled:Boolean(target.parent.loopId)}:null))throw Error('invalid tree');
  source.siblings.splice(source.siblings.indexOf(source.node),1);
  let into=move.placement==='inside'?target.node:target.parent;
  if(move.placement==='inside'&&!into.group){
   const original=into;
   into={key:'group:'+original.id,id:'sidebar-group:group:'+original.id,group:true,loopId:original.loopId,
     title:{daily:'每日简报',check:'依赖检查',single:'定时检查'}[original.loopId],children:[original]};
   roots.splice(roots.indexOf(original),1,into);
  }
  const siblings=into?into.children:roots;
  const at=move.placement==='inside'?siblings.length:siblings.indexOf(target.node)+(move.placement==='after'?1:0);
  siblings.splice(at,0,source.node);persist();window.moves.push(move);
  const index=roots.indexOf(into||source.node);
  return payload({...opts,limit:Math.max(opts.limit||5,Math.ceil((index+1)/5)*5),expanded:[...(opts.expanded||[]),...(into?[into.key]:[])]});
 },
});
for(const name of ['reconfigure','retainSession','releaseSession','subscribeWorkspaceStatus','unsubscribeWorkspaceStatus'])connection[name]=()=>{};
connection.markSessionRead=payload=>window.reads.push(payload);
connection.retainSession=id=>window.retained.push(id);
// Exercise the production URL builder; mocking listSidebarSessions itself hides
// missing query parameters on old WebViews (notably URLSearchParams.size).
window.fetch=async(input)=>{
 const url=new URL(input,location.href);
 let body={};
 if(url.pathname.endsWith('/session-sidebar')){
  const hash=url.pathname.startsWith('/api/workspaces/')?'w':'';
  const opts={limit:Number(url.searchParams.get('limit'))||undefined,
    expanded:JSON.parse(url.searchParams.get('expanded')||'[]'),reveal:url.searchParams.get('reveal')||undefined};
  window.calls.push({hash,opts,url:url.pathname+url.search});
  body=hash?payload(opts):{sidebar_tree:true,sessions:[],total:0,has_more:false,visible_roots:0};
 }
 return new Response(JSON.stringify(body),{headers:{'Content-Type':'application/json'}});
};
function App(){
 const [active,setActive]=useState(null);
 window.fixture={roots:()=>roots,select:(id)=>setActive({sessionId:id,workspaceHash:'w',active:true}),
  attention(id,state){const found=locate('session:'+id);if(found)found.node.state=state;connection.dispatchEvent(new CustomEvent('message',{detail:{type:'session_status',payload:{session_id:id,workspace_hash:'w',cwd:'/fixture',state,timestamp_ms:Date.now()}}}));},
  addRun(){const root=roots.find(item=>item.key==='loop:daily');root.children.unshift({id:'run-next',key:'session:run-next',loopId:root.loopId,title:'每日简报：新执行',created:'2026-10-14T09:00:00Z',children:[]});persist();window.dispatchEvent(new CustomEvent(SESSION_LIST_CHANGED_EVENT,{detail:{workspaceHash:'w'}}));}};
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
  const row=id=>page.locator('.ace-sidebar-session-row[data-sidebar-tree-key="'+(/^(loop:|group:)/.test(id)?id:'session:'+id)+'"]');
  const select=id=>row(id).locator('.ace-sidebar-session-title-button').click();
  const assertParentIcons=async(id,expanded,attention='read')=>{
    assert.equal(await row(id).locator('[data-icon-name="Alarm"]').evaluate(el=>getComputedStyle(el).opacity),'1',
      'scheduled provenance must stay visible while idle, hovered, focused or selected');
    const arrow=row(id).locator('.ace-sidebar-session-tree-chevron');
    if(attention!=='unread'){
      assert.equal(await arrow.evaluate(el=>getComputedStyle(el).opacity),'1','read parents always show their disclosure');
      assert.equal(await arrow.evaluate(el=>el.classList.contains('is-expanded')),expanded);
    }else{
      assert.equal(await arrow.count(),0,'unread children take precedence over the group arrow');
      const status=row(id).locator('.ace-sidebar-session-tree-indicator [title="未读"]');
      assert.equal(await status.isVisible(),true);
      assert.equal(await status.evaluate(el=>getComputedStyle(el).opacity),'1');
    }
    assert.equal(await row(id).locator('.ace-sidebar-session-title-button').getAttribute('aria-expanded'),String(expanded));
    assert.equal(await row(id).locator('.ace-sidebar-session-tree-idle-lamp').count(),0,'parents must not add a gray placeholder dot');
    assert.equal(await row(id).locator('.ace-spinner').count(),0,'group headers never show a running indicator');
    assert.equal(await row(id).locator('[data-sidebar-row-control]').count(),0,'group headers do not expose real-session mutation controls');
  };
  const assertSessionActionIcons=async(id,marker,hovered)=>{
    const target=row(id);
    await target.locator('.ace-sidebar-session-source-icon').evaluate((el,expected)=>new Promise((resolve,reject)=>{
      const deadline=performance.now()+2000;
      const check=()=>getComputedStyle(el).opacity===expected?resolve()
        :performance.now()>deadline?reject(new Error('source icon did not reach opacity '+expected)):requestAnimationFrame(check);
      check();
    }),hovered?'0':'1');
    assert.equal(await target.locator('.ace-sidebar-session-archive-icon').evaluate(el=>getComputedStyle(el).opacity),hovered?'1':'0');
    const icons=await target.locator('[data-icon-name]').evaluateAll(elements=>elements
      .filter(el=>el.closest('.ace-sidebar-row-hover-action')&&el.getBoundingClientRect().width>0&&Number(getComputedStyle(el).opacity)>0.99)
      .map(el=>el.dataset.iconName));
    assert.deepEqual(icons,hovered?['Pin','Archive']:[marker],
      'real sessions share the marker/archive position; hover must not add a third icon');
    assert.equal(await target.locator('[data-icon-name="'+marker+'"]').count(),1,
      'the source marker must not be rendered again outside the shared action position');
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
  await row('loop:daily').waitFor();
  await page.mouse.move(950,750);
  await assertParentIcons('loop:daily',false);
  assert.equal(await page.locator('.ace-sidebar-session-row').count(),5);
  await select('loop:daily');await row('old-7').waitFor();
  await row('run-new').waitFor();
  assert.equal(await row('loop:daily').locator('.ace-sidebar-session-title-button').textContent(),'每日简报');
  assert.equal(await page.locator('[data-selected]').textContent(),'选择会话');
  assert.equal(await page.evaluate(()=>window.reads.length),0,'opening a group must not mark any execution as read');
  assert.equal(await row('loop:daily').getAttribute('data-desktop-session-id'),null);
  assert.ok(await page.evaluate(()=>window.calls.some(call=>call.opts.expanded?.includes('loop:daily'))),
    'expansion must reach the server through the production request URL');
  await page.mouse.move(950,750);
  await assertParentIcons('loop:daily',true);
  assert.equal(await page.locator('.ace-sidebar-session-row').count(),14);
  assert.equal(await page.locator('.ace-sidebar-session-tree-node').count(),5);
  await select('old-3');
  assert.equal(await page.locator('[data-selected]').textContent(),'old-3');
  assert.equal(await row('run-new').count(),1);
  await select('loop:daily');await row('old-7').waitFor({state:'hidden'});
  await assertParentIcons('loop:daily',false);
  await page.evaluate(()=>window.fixture.attention('old-6','unread'));
  await row('loop:daily').locator('[title="未读"]').waitFor();
  await assertParentIcons('loop:daily',false,'unread');
  await page.evaluate(()=>window.fixture.attention('old-6','in_progress'));
  await row('loop:daily').locator('.ace-sidebar-session-tree-chevron').waitFor();
  await assertParentIcons('loop:daily',false,'in_progress');
  await page.evaluate(()=>window.fixture.attention('old-6','read'));
  await select('loop:daily');await row('old-7').waitFor();
  await drag('loop:daily','n1','before');
  await page.waitForFunction(()=>window.moves.length===1);
  assert.equal((await page.evaluate(()=>window.fixture.roots()))[0].key,'loop:daily');
  await drag('old-1','n1','before');
  await page.waitForFunction(()=>window.moves.length===2);
  assert.equal(await row('old-1').getAttribute('data-sidebar-tree-parent'),'');
  await page.reload();await row('old-1').waitFor();
  assert.equal(await row('old-1').getAttribute('data-sidebar-tree-parent'),'');
  await drag('old-1','loop:daily');
  await page.waitForFunction(()=>window.moves.length===1);
  await row('old-1').waitFor();
  assert.equal(await row('old-1').getAttribute('data-sidebar-tree-parent'),'loop:daily');
  await drag('old-2','loop:check');
  await page.waitForFunction(()=>window.moves.length===2);
  assert.equal(await row('old-2').getAttribute('data-sidebar-tree-parent'),'loop:check');
  await drag('loop:daily','loop:check');
  await drag('loop:daily','old-2','before');
  await drag('old-3','old-2');
  assert.equal(await page.evaluate(()=>window.moves.length),2,'nested parents and third-level drops must be rejected');
  await drag('old-3','n2','inside',false);
  await drag('n2','n1','inside',false);
  assert.equal(await page.evaluate(()=>window.moves.length),2,'ordinary sessions cannot receive scheduled or ordinary leaves');
  assert.equal(await row('n1').getAttribute('data-sidebar-tree-children'),'0');
  assert.equal(await row('n2').getAttribute('data-sidebar-tree-children'),'0');
  await drag('n2','loop:single');
  await page.waitForFunction(()=>window.moves.length===3);
  assert.equal(await row('n2').getAttribute('data-sidebar-tree-parent'),'loop:single');
  await drag('old-2','n1','before');
  await page.waitForFunction(()=>window.moves.length===4);
  assert.equal(await row('old-2').getAttribute('data-sidebar-tree-scheduled'),'true');
  await drag('n2','old-2');
  await page.waitForFunction(()=>window.moves.length===5);
  assert.equal(await row('n2').getAttribute('data-sidebar-tree-parent'),'group:old-2');
  assert.equal(await row('old-2').getAttribute('data-sidebar-tree-parent'),'group:old-2');
  assert.equal(await row('group:old-2').locator('.ace-sidebar-session-title-button').textContent(),'每日简报');
  await page.evaluate(()=>window.fixture.addRun());await row('run-next').waitFor();
  assert.equal(await row('run-new').getAttribute('data-sidebar-tree-parent'),'loop:daily');
  assert.equal(await row('loop:daily').locator('.ace-sidebar-session-title-button').textContent(),'每日简报');
  await page.reload();await row('loop:daily').waitFor();
  await page.evaluate(()=>window.fixture.select('old-7'));await row('old-7').waitFor();
  for(const theme of ['light','dark']){
    await page.evaluate(mode=>document.documentElement.dataset.theme=mode,theme);
    for(const [id,marker] of [['run-next','Alarm'],['n1','Worktree']]){
      await page.evaluate(()=>document.activeElement?.blur());
      await page.mouse.move(950,750);
      await assertSessionActionIcons(id,marker,false);
      await row(id).hover();
      await assertSessionActionIcons(id,marker,true);
      await assertParentIcons('loop:daily',true);
      await page.screenshot({path:path.join(output,theme+'-'+marker.toLowerCase()+'-hover.png'),animations:'disabled'});
      await page.mouse.move(950,750);
      await row(id).locator('.ace-sidebar-session-title-button').focus();
      await page.keyboard.press('Tab');
      assert.equal(await row(id).getByRole('button',{name:'置顶',exact:true}).evaluate(el=>el===document.activeElement),true);
      await assertSessionActionIcons(id,marker,true);
      await page.keyboard.press('Tab');
      assert.equal(await row(id).getByRole('button',{name:marker==='Alarm'?'定时任务，归档':'工作树会话，归档',exact:true}).evaluate(el=>el===document.activeElement),true);
      await assertSessionActionIcons(id,marker,true);
      await page.evaluate(()=>document.activeElement?.blur());
      await assertSessionActionIcons(id,marker,false);
    }
    await page.mouse.move(950,750);
    await page.waitForFunction(()=>getComputedStyle(document.querySelector('[data-sidebar-tree-key="session:n1"]')).backgroundColor==='rgba(0, 0, 0, 0)',null,{timeout:2000});
    assert.equal(await row('old-0').evaluate(el=>getComputedStyle(el).backgroundColor),'rgba(0, 0, 0, 0)',
      'the group supplies its background; ordinary children remain transparent');
    assert.equal(await row('old-0').evaluate(el=>getComputedStyle(el).backgroundColor),
      await row('n1').evaluate(el=>getComputedStyle(el).backgroundColor));
    await assertParentIcons('loop:daily',true);
    const appearance=await page.evaluate(()=>{
      const header=document.querySelector('[data-sidebar-tree-key="loop:daily"]');
      const child=document.querySelector('[data-sidebar-tree-key="session:old-7"]');
      const children=child.closest('.ace-sidebar-session-tree-children');
      return {headerX:header.getBoundingClientRect().x,childX:child.getBoundingClientRect().x,
        padding:getComputedStyle(children).paddingLeft,border:getComputedStyle(children).borderLeftWidth,
        group:getComputedStyle(header.parentElement).backgroundColor,selected:getComputedStyle(child).backgroundColor};
    });
    assert.equal(appearance.headerX,appearance.childX);assert.equal(appearance.padding,'0px');assert.equal(appearance.border,'0px');
    assert.notEqual(appearance.group,'rgba(0, 0, 0, 0)');assert.notEqual(appearance.selected,appearance.group);
    await row('loop:daily').hover();
    await assertParentIcons('loop:daily',true);
    await page.screenshot({path:path.join(output,theme+'.png'),animations:'disabled'});
    await page.evaluate(()=>window.fixture.attention('check','unread'));
    await row('loop:check').locator('[title="未读"]').waitFor();
    await assertParentIcons('loop:check',false,'unread');
    await row('loop:check').hover();
    await assertParentIcons('loop:check',false,'unread');
    await row('loop:check').locator('.ace-sidebar-session-title-button').focus();
    await assertParentIcons('loop:check',false,'unread');
    await page.screenshot({path:path.join(output,theme+'-unread.png'),animations:'disabled'});
    await page.evaluate(()=>window.fixture.attention('check','in_progress'));
    await row('loop:check').locator('.ace-sidebar-session-tree-chevron').waitFor();
    await assertParentIcons('loop:check',false,'in_progress');
    await select('loop:check');
    await row('check').getByRole('status',{name:'进行中'}).waitFor();
    await assertParentIcons('loop:check',true,'in_progress');
    await select('loop:check');
    await page.evaluate(()=>window.fixture.attention('check','read'));
    await assertParentIcons('loop:check',false);
  }
  await page.mouse.move(950,750);
  await row('run-next').locator('.ace-sidebar-session-title-button').focus();
  await page.keyboard.press('Tab');
  await assertParentIcons('loop:daily',true);
  await page.keyboard.press('Tab');
  assert.equal(await row('run-next').getByRole('button',{name:'定时任务，归档',exact:true}).evaluate(el=>el===document.activeElement),true);
  await assertParentIcons('loop:daily',true);
  await page.setViewportSize({width:390,height:844});
  assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth),true);
  await assertParentIcons('loop:daily',true);
  await page.screenshot({path:path.join(output,'narrow.png'),animations:'disabled'});
  await page.evaluate(()=>window.fixture.select('missing-session'));
  await page.waitForFunction(()=>window.calls.some(call=>call.opts?.reveal==='missing-session'));
  await page.waitForTimeout(150);
  assert.ok(await page.evaluate(()=>window.calls.filter(call=>call.opts?.reveal==='missing-session').length)<4,
    'a stale navigation target must not trigger a request/render loop');
  assert.deepEqual(errors,[]);
  assert.equal(await page.evaluate(()=>window.retained.some(id=>id.startsWith('sidebar-group:'))),false);
  assert.equal(await page.evaluate(()=>window.reads.some(item=>item.sessionId?.startsWith('sidebar-group:'))),false);
  console.log(JSON.stringify({result:'PASS',browser:browser.version(),output,scenarios:28}));
} finally { await browser?.close(); await server.close(); }
