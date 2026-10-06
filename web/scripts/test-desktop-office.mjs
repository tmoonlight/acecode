import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { toolIconSvg } from '../src/lib/toolIcons.js';
const modulePath = process.env.ACE_PLAYWRIGHT_MODULE;
const { chromium } = await import(modulePath ? pathToFileURL(path.resolve(modulePath)).href : 'playwright');
const root = fileURLToPath(new URL('../../', import.meta.url));
const output = process.env.ACE_OFFICE_CAPTURE_DIR || fs.mkdtempSync(path.join(os.tmpdir(),'ace-office-browser-'));
fs.mkdirSync(output,{recursive:true});
const browser = await chromium.launch({channel:process.env.ACE_BROWSER_CHANNEL || 'msedge',headless:true});
const errors=[],external=[];
const actor=(id,extra={})=>({id,root:id==='root',name:id==='root'?'Maestro':id,state:'work',label:'工作中',busy:true,seed:id.length*173,contextKnown:true,contextRatio:.7,contextTokens:7000,contextLimit:10000,transfers:[],...extra});
const data=(extra={})=>({version:1,follow:true,connected:true,complete:true,seed:137,
  selected:{sessionId:'root',workspaceHash:'project',title:'修复登录流程'},
  offices:['修复登录流程','研究消息分发','整理设计文档','检查任务取消','验证桌面发布'].map((title,i)=>({sessionId:i?'root'+i:'root',workspaceHash:'project',title,workspaceName:'ACECode'})),
  agents:[actor('root',{tool:'file_read',iconSvg:toolIconSvg('file_read')}),actor('代码审查',{state:'think',label:'思考中'}),actor('回归测试',{state:'permission',label:'等待授权'})],overflow:0,...extra});
try {
  const page=await browser.newPage({viewport:{width:688,height:504},deviceScaleFactor:1});
  page.on('pageerror',e=>errors.push(e.message));page.on('request',r=>{if(/^https?:/.test(r.url()))external.push(r.url());});
  await page.addInitScript(()=>{window.officeActions=[];window.chrome={webview:{postMessage:value=>officeActions.push(value),addEventListener:(type,listener)=>{window.hostMessage=listener;}}};});
  await page.goto(pathToFileURL(path.join(root,'assets/desktop_pet/agent_office_pet.html')).href);
  await page.waitForFunction(()=>!!window.AgentOffice&&window.officeActions.includes('ready'));
  const controlsVisible=()=>page.locator('#officeControls').evaluate(node=>getComputedStyle(node).opacity==='1');
  const lastOverlay=()=>page.evaluate(()=>officeActions.map(raw=>{try{return JSON.parse(raw);}catch{return {};}}).filter(m=>m.type==='overlay').at(-1));
  assert.equal(await controlsVisible(),false,'menu starts hidden');
  assert.equal((await lastOverlay()).controls,null,'hidden menu has no native hit region');
  await page.screenshot({path:path.join(output,'office-controls-hidden.png')});
  await page.mouse.move(344,280);
  assert.equal(await controlsVisible(),true,'hover reveals the menu');
  assert.ok(Array.isArray((await lastOverlay()).controls),'visible menu supplies a native hit region');
  await page.mouse.move(-10,-10);
  await page.waitForTimeout(550);
  assert.equal(await controlsVisible(),true,'leaving allows time to cross the gap');
  await page.locator('#officePin').hover();
  await page.waitForTimeout(600);
  assert.equal(await controlsVisible(),true,'returning cancels the previous hide timer');
  await page.locator('#officePin').click();
  assert.ok(await page.evaluate(()=>officeActions.includes(JSON.stringify({type:'pin',pinned:false}))));
  await page.evaluate(()=>hostMessage({data:{type:'pet-window-state',pinned:false}}));
  assert.equal(await page.locator('#officePin').getAttribute('aria-pressed'),'false');
  await page.mouse.move(-10,-10);
  await page.waitForTimeout(1100);
  assert.equal(await controlsVisible(),false,'mouse click focus does not keep the menu open');
  assert.equal((await lastOverlay()).controls,null);
  await page.keyboard.press('Tab');
  assert.equal(await controlsVisible(),true,'Tab reveals controls without a pointer');
  await page.waitForTimeout(1100);
  assert.equal(await controlsVisible(),true,'keyboard focus keeps controls available');
  assert.equal(await page.locator('#officeClose').evaluate(node=>node===document.activeElement),true);
  await page.keyboard.press('Enter');
  assert.ok(await page.evaluate(()=>officeActions.includes(JSON.stringify({type:'close'}))));
  await page.mouse.move(344,280);
  await page.locator('#officePin').click();
  assert.ok(await page.evaluate(()=>officeActions.includes(JSON.stringify({type:'pin',pinned:true}))));
  await page.evaluate(()=>hostMessage({data:{type:'pet-window-state',pinned:true}}));
  assert.equal(await page.locator('#officePin').getAttribute('aria-pressed'),'true');
  assert.equal(await page.evaluate(()=>AgentOffice.agents.length),0,'production starts without demo workers');
  const apply=async snapshot=>{await page.evaluate(value=>window.hostMessage({data:value}),snapshot);await page.waitForTimeout(90);};
  // 以模拟时间推进(走路、信封、等待),不依赖真实帧率;predicate 在页面里求值。
  const advanceUntil=async(predicate,maxMs=20000)=>page.evaluate(({source,maxMs})=>{
    const test=new Function('return ('+source+')');
    for(let t=0;t<maxMs;t+=100){if(test())return true;AgentOffice.advance(100);}
    return test();
  },{source:predicate,maxMs});
  const present=()=>page.evaluate(()=>AgentOffice.agents.filter(a=>!a.leaving).map(a=>a.id));
  await apply(data());
  assert.equal(await page.locator('.office-tab').count(),5);
  assert.equal(await page.locator('#officeFollow').getAttribute('aria-checked'),'true');
  assert.equal(await page.evaluate(()=>AgentOffice.agents.length),3);
  const originalLayout=await page.evaluate(()=>AgentOffice.layoutSignature);
  const look=await page.evaluate(()=>AgentOffice.agents.find(a=>a.id==='代码审查').look);
  await page.locator('.office-tab').nth(1).click();
  assert.ok(await page.evaluate(()=>officeActions.some(raw=>{try{return JSON.parse(raw).type==='select'&&JSON.parse(raw).session_id==='root1';}catch{return false;}})));
  await page.locator('#officeFollow').click();
  assert.ok(await page.evaluate(()=>officeActions.some(raw=>{try{return JSON.parse(raw).type==='follow'&&JSON.parse(raw).follow===false;}catch{return false;}})));
  await page.locator('.office-tab').nth(2).focus();
  await page.keyboard.press('Enter');
  assert.ok(await page.evaluate(()=>officeActions.some(raw=>{try{const m=JSON.parse(raw);return m.type==='select'&&m.session_id==='root2';}catch{return false;}})));
  await page.screenshot({path:path.join(output,'office-working-688.png')});
  // Busy agents keep a persistent bubble: verb plus the call preview or the latest streamed words.
  await apply(data({agents:[actor('root',{tool:'file_read',iconSvg:toolIconSvg('file_read'),label:'读取',detail:'src/auth/login.cpp'}),
    actor('代码审查',{state:'type',label:'撰写回复',detail:'…并发登录会覆盖旧会话'}),actor('回归测试',{state:'permission',label:'等待授权',detail:'运行命令'})]}));
  const bubbles=await page.evaluate(()=>Object.fromEntries(AgentOffice.agents.map(a=>[a.id,a.bubble])));
  assert.match(bubbles.root,/读取.*src\/auth\/login\.cpp/);
  assert.match(bubbles['代码审查'],/撰写回复.*覆盖旧会话/);
  assert.match(bubbles['回归测试'],/等待授权/);
  assert.ok(await page.locator('.bub.type .bub-detail').count()>=1,'streaming bubble keeps the typing caret style');
  assert.ok(await page.evaluate(()=>officeActions.some(raw=>{try{const m=JSON.parse(raw);return m.type==='overlay'&&m.bubbles?.length>=3;}catch{return false;}})),'bubble regions reach the native window shape');
  await page.screenshot({path:path.join(output,'office-bubbles-688.png')});
  // Completed workers walk to their dispatcher, hand in a report envelope, then leave; the main agent sleeps.
  await apply(data({agents:[actor('root',{state:'sleep',label:'zzz',busy:false,contextRatio:.15})],completed:['代码审查','回归测试']}));
  assert.deepEqual(await present(),['root']);
  assert.ok(await page.evaluate(()=>AgentOffice.agents.filter(a=>a.leaving).every(a=>a.walking&&a.state==='done')));
  assert.ok(await advanceUntil("AgentOffice.envelopes.some(e=>e.label==='回报'&&e.to==='root')"),'report envelope flies to the dispatcher');
  assert.ok(await advanceUntil("AgentOffice.agents.some(a=>a.id==='root'&&/收到.*的结果/.test(a.bubble))"),'dispatcher acknowledges the report');
  assert.ok(await advanceUntil("AgentOffice.agents.some(a=>a.leaving&&a.bubble.includes('再见'))"),'leaving workers say goodbye');
  assert.ok(await advanceUntil("AgentOffice.door>0.6"),'the door opens for the exit');
  assert.ok(await advanceUntil("AgentOffice.agents.length===1"),'workers leave through the door');
  assert.ok(await advanceUntil("AgentOffice.door<0.1"),'the door closes again');
  assert.deepEqual(await page.evaluate(()=>AgentOffice.agents.map(a=>a.state)),['sleep']);
  await page.screenshot({path:path.join(output,'office-sleep-688.png')});
  // A new worker walks in, receives a task envelope at the dispatcher's desk, then sits down.
  await apply(data());
  assert.ok(await page.evaluate(()=>AgentOffice.agents.filter(a=>!a.root&&a.id!=='root').every(a=>a.walking)),'new workers enter on foot');
  assert.ok(await advanceUntil("AgentOffice.door>0.6",1000),'the door opens as workers step in');
  assert.ok(await advanceUntil("AgentOffice.envelopes.some(e=>e.label==='任务'&&e.from==='root')"),'task envelope leaves the dispatcher');
  assert.ok(await advanceUntil("AgentOffice.agents.every(a=>!a.walking)"),'workers reach their seats');
  assert.ok(await page.evaluate(()=>AgentOffice.agents.some(a=>a.bubble.includes('报到'))),'arrivals hop and report in at their desks');
  assert.equal(await page.evaluate(()=>AgentOffice.agents.find(a=>a.id==='代码审查').look),look);
  assert.equal(await page.evaluate(()=>AgentOffice.layoutSignature),originalLayout);
  const toolImage=await page.locator('.office-tool:not([hidden])').first().getAttribute('src');
  assert.equal(decodeURIComponent(toolImage.split(',')[1]),toolIconSvg('file_read').replaceAll('currentColor','#27324f'));
  // Over-capacity members remain reachable, and the host receives their hit box.
  const many=[actor('root'),...Array.from({length:9},(_,i)=>actor('成员'+i))];
  await apply(data({agents:many,overflow:2}));
  assert.equal((await present()).length,8);
  await page.locator('#officeOverflow').click();
  assert.equal(await page.locator('#officeMembers button').count(),10);
  assert.ok(await page.evaluate(()=>officeActions.some(raw=>{try{const m=JSON.parse(raw);return m.type==='overlay'&&m.rect?.[2]>m.rect?.[0];}catch{return false;}})));
  await apply(data({agents:many.map((a,i)=>i===9?{...a,state:'permission',label:'等待授权'}:a),overflow:2}));
  assert.match(await page.locator('#officeMembers button').last().textContent(),/等待授权/);
  await page.locator('#officeMembers button').last().click();
  assert.ok(await page.evaluate(()=>officeActions.some(raw=>{try{const m=JSON.parse(raw);return m.type==='open'&&m.session_id==='成员8';}catch{return false;}})));
  await page.locator('#officeOverflow').click();
  await apply(data({follow:false}));
  await advanceUntil("AgentOffice.agents.every(a=>!a.walking)");
  // Mesh messages between members fly as labelled envelopes.
  await apply(data({agents:[actor('root'),actor('代码审查',{path:'/root/review'}),actor('回归测试',{path:'/root/test',
    transfers:[{seq:77,sender:'/root/review',recipient:'/root/test',sender_session_id:'代码审查',type:'MESSAGE'}]})]}));
  assert.ok(await page.evaluate(()=>AgentOffice.envelopes.some(e=>e.from==='代码审查'&&e.to==='回归测试'&&e.label==='消息')),'mesh message envelope');
  // Reduced motion: no walking, but the task hand-off is still shown.
  await page.emulateMedia({reducedMotion:'reduce'});
  await apply(data({agents:[actor('root'),actor('代码审查',{path:'/root/review'}),actor('回归测试',{path:'/root/test'}),actor('文档')]}));
  assert.equal(await page.evaluate(()=>AgentOffice.agents.find(a=>a.id==='文档').walking),false);
  assert.ok(await page.evaluate(()=>AgentOffice.envelopes.some(e=>e.to==='文档'&&e.label==='任务')),'reduced motion keeps the hand-off');
  assert.match(await page.evaluate(()=>AgentOffice.agents.find(a=>a.id==='文档').bubble),/报到/,'reduced motion still reports in');
  await apply(data({agents:[actor('root'),actor('代码审查',{path:'/root/review'}),actor('回归测试',{path:'/root/test'})]}));
  assert.match(await page.evaluate(()=>AgentOffice.agents.find(a=>a.id==='文档')?.bubble||''),/再见/,'reduced motion says goodbye in place');
  assert.ok(await advanceUntil("!AgentOffice.agents.some(a=>a.id==='文档')",2000),'then leaves without walking');
  await page.emulateMedia({reducedMotion:'no-preference'});
  assert.equal(await page.locator('#officeMembers').isHidden(),true);
  assert.ok(await page.evaluate(()=>officeActions.some(raw=>{try{const m=JSON.parse(raw);return m.type==='overlay'&&m.rect===null;}catch{return false;}})));

  // The narrowest native scale keeps tabs, follow, pin and close clickable.
  for(const [width,height] of [[172,126],[344,252],[860,630]]) {
    await page.setViewportSize({width,height});await page.waitForTimeout(90);
    await page.mouse.move(width/2,height/2);
    const bounds=await page.locator('.office-controls button').evaluateAll(buttons=>buttons.map(b=>{const r=b.getBoundingClientRect();return {x:r.x,y:r.y,right:r.right,bottom:r.bottom};}));
    assert.equal(bounds.length,8);
    assert.ok(bounds.every(r=>r.x>=0&&r.y>=0&&r.right<=width&&r.bottom<=32*width/344+1),JSON.stringify({width,bounds}));
    await page.screenshot({path:path.join(output,`office-size-${width}.png`)});
  }
  await page.setViewportSize({width:688,height:504});
  await apply(data({connected:false}));
  assert.match(await page.locator('#officeNotice').textContent(),/连接已中断/);
  assert.ok(await page.evaluate(()=>{const notices=officeActions.map(raw=>{try{return JSON.parse(raw);}catch{return {};}}).filter(m=>m.type==='overlay');return notices.at(-1)?.rect?.[3]>notices.at(-1)?.rect?.[1];}),'native region includes disconnected notice');
  await apply(data({agents:[],offices:[],selected:null}));
  assert.match(await page.locator('#officeNotice').textContent(),/发送消息后/);
  assert.ok(await page.evaluate(()=>{const notices=officeActions.map(raw=>{try{return JSON.parse(raw);}catch{return {};}}).filter(m=>m.type==='overlay');return Array.isArray(notices.at(-1)?.rect);}), 'native region includes empty-state notice');
  assert.deepEqual(errors,[]);assert.deepEqual(external,[]);
  console.log(JSON.stringify({output,checks:64,errors,external},null,2));
} finally {await browser.close();}
