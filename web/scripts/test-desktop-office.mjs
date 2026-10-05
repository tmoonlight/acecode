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
  assert.equal(await page.evaluate(()=>AgentOffice.agents.length),0,'production starts without demo workers');
  const apply=async snapshot=>{await page.evaluate(value=>window.hostMessage({data:value}),snapshot);await page.waitForTimeout(90);};
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
  // Completion removes workers immediately while the main agent displays zzz.
  await apply(data({agents:[actor('root',{state:'sleep',label:'zzz',busy:false,contextRatio:.15})],completed:['代码审查','回归测试']}));
  assert.deepEqual(await page.evaluate(()=>AgentOffice.agents.map(a=>a.state)),['sleep']);
  await page.screenshot({path:path.join(output,'office-sleep-688.png')});
  await apply(data());
  assert.equal(await page.evaluate(()=>AgentOffice.agents.find(a=>a.id==='代码审查').look),look);
  assert.equal(await page.evaluate(()=>AgentOffice.layoutSignature),originalLayout);
  const toolImage=await page.locator('.office-tool:not([hidden])').first().getAttribute('src');
  assert.equal(decodeURIComponent(toolImage.split(',')[1]),toolIconSvg('file_read').replaceAll('currentColor','#27324f'));
  // Over-capacity members remain reachable, and the host receives their hit box.
  const many=[actor('root'),...Array.from({length:9},(_,i)=>actor('成员'+i))];
  await apply(data({agents:many,overflow:2}));
  assert.equal(await page.evaluate(()=>AgentOffice.agents.length),8);
  await page.locator('#officeOverflow').click();
  assert.equal(await page.locator('#officeMembers button').count(),10);
  assert.ok(await page.evaluate(()=>officeActions.some(raw=>{try{const m=JSON.parse(raw);return m.type==='overlay'&&m.rect?.[2]>m.rect?.[0];}catch{return false;}})));
  await apply(data({agents:many.map((a,i)=>i===9?{...a,state:'permission',label:'等待授权'}:a),overflow:2}));
  assert.match(await page.locator('#officeMembers button').last().textContent(),/等待授权/);
  await page.locator('#officeMembers button').last().click();
  assert.ok(await page.evaluate(()=>officeActions.some(raw=>{try{const m=JSON.parse(raw);return m.type==='open'&&m.session_id==='成员8';}catch{return false;}})));
  await page.locator('#officeOverflow').click();
  await apply(data({follow:false}));
  assert.equal(await page.locator('#officeMembers').isHidden(),true);
  assert.ok(await page.evaluate(()=>officeActions.some(raw=>{try{const m=JSON.parse(raw);return m.type==='overlay'&&m.rect===null;}catch{return false;}})));

  // The narrowest native scale keeps all five controls and the follow switch clickable.
  for(const [width,height] of [[172,126],[344,252],[860,630]]) {
    await page.setViewportSize({width,height});await page.waitForTimeout(90);
    const bounds=await page.locator('.office-controls button').evaluateAll(buttons=>buttons.map(b=>{const r=b.getBoundingClientRect();return {x:r.x,y:r.y,right:r.right,bottom:r.bottom};}));
    assert.equal(bounds.length,6);
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
  console.log(JSON.stringify({output,checks:22,errors,external},null,2));
} finally {await browser.close();}
