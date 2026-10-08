import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { toolIconSvg } from '../src/lib/toolIcons.js';
const modulePath = process.env.ACE_PLAYWRIGHT_MODULE;
const playwright = await import(modulePath ? pathToFileURL(path.resolve(modulePath)).href : 'playwright');
const root = fileURLToPath(new URL('../../', import.meta.url));
const output = process.env.ACE_OFFICE_CAPTURE_DIR || fs.mkdtempSync(path.join(os.tmpdir(),'ace-office-browser-'));
fs.mkdirSync(output,{recursive:true});
const browserName = process.env.ACE_OFFICE_BROWSER || 'chromium';
const macHost = process.env.ACE_OFFICE_HOST === 'mac';
const browser = await playwright[browserName].launch({
  ...(browserName === 'chromium' ? {channel:process.env.ACE_BROWSER_CHANNEL || 'msedge'} : {}),headless:true});
const macChecks=[];
const errors=[],external=[];
const actor=(id,extra={})=>({id,root:id==='root',name:id==='root'?'Maestro':id,state:'work',label:'工作中',busy:true,seed:id.length*173,contextKnown:true,contextRatio:.7,contextTokens:7000,contextLimit:10000,transfers:[],...extra});
const data=(extra={})=>({version:1,follow:true,connected:true,complete:true,seed:137,
  selected:{sessionId:'root',workspaceHash:'project',title:'修复登录流程'},
  offices:['修复登录流程','研究消息分发','整理设计文档','检查任务取消','验证桌面发布'].map((title,i)=>({sessionId:i?'root'+i:'root',workspaceHash:'project',title,workspaceName:'ACECode'})),
  agents:[actor('root',{tool:'file_read',iconSvg:toolIconSvg('file_read')}),actor('代码审查',{state:'think',label:'思考中'}),actor('回归测试',{state:'permission',label:'等待授权'})],overflow:0,...extra});
try {
  const page=await browser.newPage({viewport:{width:688,height:504},deviceScaleFactor:1});
  page.on('pageerror',e=>errors.push(e.message));page.on('request',r=>{if(/^https?:/.test(r.url()))external.push(r.url());});
  if (macHost) {
    const source=fs.readFileSync(path.join(root,'src/apps/desktop/desktop_pet_mac.mm'),'utf8');
    const shim=source.match(/kBridgeShim = R"JS\(([\s\S]*?)\)JS";/)?.[1];
    assert.ok(shim,'load the production macOS bridge');
    await page.addInitScript({content:`
      window.officeActions=[];
      window.webkit={messageHandlers:{acePet:{postMessage:value=>officeActions.push(value)}}};
      ${shim}
      window.hostMessage=event=>window.__acePetDeliver(event.data);
      document.addEventListener('pointerdown',event=>window.lastPointerId=event.pointerId,true);
    `});
  } else {
    await page.addInitScript(()=>{window.officeActions=[];window.chrome={webview:{postMessage:value=>officeActions.push(value),addEventListener:(type,listener)=>{window.hostMessage=listener;}}};});
  }
  await page.goto(pathToFileURL(path.join(root,'assets/desktop_pet/agent_office_pet.html')).href);
  await page.waitForFunction(()=>!!window.AgentOffice&&window.officeActions.includes('ready'));
  const setReducedMotion=async enabled=>{
    // WebKit updates existing MediaQueryList objects asynchronously after
    // emulateMedia; wait for the change before creating animation state.
    await page.evaluate(value=>{
      const query=matchMedia('(prefers-reduced-motion: reduce)');
      window.officeMotionReady=query.matches===value;
      if(!window.officeMotionReady)query.addEventListener('change',()=>window.officeMotionReady=true,{once:true});
    },enabled);
    await page.emulateMedia({reducedMotion:enabled?'reduce':'no-preference'});
    await page.waitForFunction(()=>window.officeMotionReady);
  };
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
  if (macHost) {
    const canvas=page.locator('#cv');
    const capture=()=>canvas.evaluate(node=>node.hasPointerCapture(window.lastPointerId));
    const count=message=>page.evaluate(value=>officeActions.filter(action=>action===value).length,message);
    await canvas.evaluate(node=>{window.canvasClicks=0;node.addEventListener('click',()=>window.canvasClicks++);});
    await page.mouse.move(344,280);
    let drags=await count('drag');
    await page.mouse.down();
    assert.equal(await capture(),true,'capture immediately on canvas press');
    await page.mouse.move(-20,280);
    assert.equal(await count('drag'),drags+1,'crossing outside the canvas still starts drag');
    await page.mouse.up();
    assert.equal(await capture(),false,'outside release ends capture');
    assert.equal(await count('gesture-cancel'),0,'ordinary release is not cancellation');
    macChecks.push('outside-canvas drag and release');

    await page.mouse.move(344,280);
    await page.mouse.down();
    await page.mouse.move(345,280);
    await canvas.evaluate(node=>node.releasePointerCapture(window.lastPointerId));
    await page.mouse.move(346,280);
    assert.equal(await count('gesture-cancel'),1,'lost capture cancels native tracking');
    await page.mouse.up();
    await page.mouse.down();
    await canvas.evaluate(node=>node.dispatchEvent(new PointerEvent('pointercancel',{pointerId:window.lastPointerId,bubbles:true})));
    assert.equal(await count('gesture-cancel'),2,'pointer cancellation ends native tracking');
    await page.mouse.up();
    drags=await count('drag');
    await page.mouse.down();
    await page.mouse.move(360,280);
    await page.mouse.up();
    assert.equal(await count('drag'),drags+1,'drag works after cancellation');
    assert.equal(await page.evaluate(()=>window.canvasClicks),0,'drag and cancellation do not select characters');
    macChecks.push('lost capture, cancellation and next drag');

    await page.mouse.move(344,280);await page.mouse.down();
    await page.mouse.move(346,280);await page.mouse.up();
    assert.equal(await page.evaluate(()=>window.canvasClicks),1,'movement below the threshold preserves clicks');
    assert.equal(await count('drag'),drags+1,'movement below the threshold does not start a drag');
    macChecks.push('ordinary canvas click');

    await page.mouse.click(344,280,{button:'right'});
    assert.equal(await capture(),false,'right click is not captured');
    assert.ok(await count('menu'),'context menu keeps working');
    await page.locator('#grip').dblclick();
    assert.ok(await count('resize'),'handle still starts resize');
    assert.ok(await count('size-reset'),'handle double-click still resets size');
    macChecks.push('right click and handle double-click');
  }
  const apply=async snapshot=>{await page.evaluate(value=>window.hostMessage({data:value}),snapshot);await page.waitForTimeout(90);};
  // 以模拟时间推进(走路、信封、等待),不依赖真实帧率;predicate 在页面里求值。
  const advanceUntil=async(predicate,maxMs=20000)=>page.evaluate(({source,maxMs})=>{
    const test=new Function('return ('+source+')');
    for(let t=0;t<maxMs;t+=100){if(test())return true;AgentOffice.advance(100);}
    return test();
  },{source:predicate,maxMs});
  const present=()=>page.evaluate(()=>AgentOffice.agents.filter(a=>!a.leaving).map(a=>a.id));
  await apply(data({complete:false}));
  assert.equal(await page.locator('#officeNotice').textContent(),'修复登录流程','selected title replaces the recent-session loading notice');
  await apply(data({complete:false,selected:{sessionId:'root',workspaceHash:'project',title:'当前会话已改名'}}));
  assert.equal(await page.locator('#officeNotice').textContent(),'当前会话已改名','title-only updates refresh even when recent tabs do not change');
  const longTitle='当前会话的完整标题需要保持单行显示而不遮挡办公室场景。'.repeat(8);
  await apply(data({selected:{sessionId:'outside-recent-five',workspaceHash:'project',title:longTitle}}));
  assert.equal(await page.locator('#officeNotice').textContent(),longTitle,'switching to a session outside the recent five updates the title');
  assert.equal(await page.locator('#officeNotice').getAttribute('title'),longTitle,'hover exposes the complete title');
  assert.ok(await page.locator('#officeNotice').evaluate(node=>node.scrollWidth>node.clientWidth&&getComputedStyle(node).whiteSpace==='nowrap'),'long titles stay on one line');
  await apply(data());
  assert.equal(await page.locator('#officeNotice').textContent(),'修复登录流程','title remains visible after loading completes and selection returns');
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
  await setReducedMotion(true);
  await apply(data({agents:[actor('root'),actor('代码审查',{path:'/root/review'}),actor('回归测试',{path:'/root/test'}),actor('文档')]}));
  assert.equal(await page.evaluate(()=>AgentOffice.agents.find(a=>a.id==='文档').walking),false);
  assert.ok(await page.evaluate(()=>AgentOffice.envelopes.some(e=>e.to==='文档'&&e.label==='任务')),'reduced motion keeps the hand-off');
  assert.match(await page.evaluate(()=>AgentOffice.agents.find(a=>a.id==='文档').bubble),/报到/,'reduced motion still reports in');
  await apply(data({agents:[actor('root'),actor('代码审查',{path:'/root/review'}),actor('回归测试',{path:'/root/test'})]}));
  assert.match(await page.evaluate(()=>AgentOffice.agents.find(a=>a.id==='文档')?.bubble||''),/再见/,'reduced motion says goodbye in place');
  assert.ok(await advanceUntil("!AgentOffice.agents.some(a=>a.id==='文档')",2000),'then leaves without walking');
  await setReducedMotion(false);
  assert.equal(await page.locator('#officeMembers').isHidden(),true);
  assert.ok((await lastOverlay()).rect?.[3]<.25,'closing members leaves only the compact title hit region');

  // The narrowest native scale keeps tabs, follow, pin and close clickable.
  for(const [width,height] of [[172,126],[344,252],[860,630]]) {
    await page.setViewportSize({width,height});await page.waitForTimeout(90);
    await page.mouse.move(width/2,height/2);
    const bounds=await page.locator('.office-controls button').evaluateAll(buttons=>buttons.map(b=>{const r=b.getBoundingClientRect();return {x:r.x,y:r.y,right:r.right,bottom:r.bottom};}));
    assert.equal(bounds.length,8);
    assert.ok(bounds.every(r=>r.x>=0&&r.y>=0&&r.right<=width&&r.bottom<=32*width/344+1),JSON.stringify({width,bounds}));
    await page.screenshot({path:path.join(output,`office-size-${width}.png`)});
  }
  if (macHost) {
    for (const [width,height] of [[172,126],[430,315],[517,379],[860,630]]) {
      await page.setViewportSize({width,height});
      const grip=await page.locator('#grip').boundingBox();
      assert.equal(grip.width,16);assert.equal(grip.height,16);
      assert.ok(Math.abs(grip.x-11*width/344)<.05);
      assert.ok(Math.abs(grip.y-112*height/252)<.05);
    }
    macChecks.push('native handle geometry agrees with CSS at four scales');
  }
  await page.setViewportSize({width:688,height:504});
  await apply(data({connected:false}));
  assert.match(await page.locator('#officeNotice').textContent(),/连接已中断/);
  assert.ok(await page.evaluate(()=>{const notices=officeActions.map(raw=>{try{return JSON.parse(raw);}catch{return {};}}).filter(m=>m.type==='overlay');return notices.at(-1)?.rect?.[3]>notices.at(-1)?.rect?.[1];}),'native region includes disconnected notice');
  await apply(data({agents:[],offices:[],selected:null}));
  assert.match(await page.locator('#officeNotice').textContent(),/发送消息后/);
  assert.ok(await page.evaluate(()=>{const notices=officeActions.map(raw=>{try{return JSON.parse(raw);}catch{return {};}}).filter(m=>m.type==='overlay');return Array.isArray(notices.at(-1)?.rect);}), 'native region includes empty-state notice');
  assert.deepEqual(errors,[]);assert.deepEqual(external,[]);
  console.log(JSON.stringify({output,browserName,macHost,checks:70,macChecks,errors,external},null,2));
} finally {await browser.close();}
