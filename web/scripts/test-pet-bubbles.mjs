import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const assets=fileURLToPath(new URL('../../assets/desktop_pet/',import.meta.url));
const {chromium}=await import(process.env.ACE_PLAYWRIGHT_MODULE?pathToFileURL(path.resolve(process.env.ACE_PLAYWRIGHT_MODULE)).href:'playwright');
const output=fs.mkdtempSync(path.join(os.tmpdir(),'ace-shared-pet-bubbles-'));
const read=name=>fs.readFileSync(path.join(assets,name),'utf8').replace(/<script src="([^"]+)"><\/script>/g,
  (_,source)=>'<script>'+fs.readFileSync(path.join(assets,source),'utf8')+'</script>');
const cards=[{sessionId:'running',workspaceHash:'w',key:'r',status:'running',title:'运行任务',body:'正在执行命令'},
  {sessionId:'done',workspaceHash:'w',key:'d',status:'review',title:'测试会话',body:'收到。'}];
const data={version:1,connected:true,offices:[],agents:[],activities:cards,transcriptFont:{size:'13px'}};
const browser=await chromium.launch({channel:process.env.ACE_BROWSER_CHANNEL||'msedge',headless:true});
const errors=[];
try{
  const pages={};
  for(const kind of ['classic','mini']){
    const page=await browser.newPage({viewport:{width:344,height:252}});pages[kind]=page;
    page.on('pageerror',e=>errors.push(e.message));
    await page.addInitScript(()=>{
      window.actions=[];
      window.chrome={webview:{postMessage:raw=>{actions.push(raw);let a;try{a=JSON.parse(raw);}catch{return;}
        if(a.type==='mini-popup')queueMicrotask(()=>hostMessage({data:{type:'mini-popup-layout',expanded:a.expanded,
          request_id:a.request_id,anchor:{left:0,top:0,width:1,height:1}}}));},addEventListener:(_,fn)=>{window.hostMessage=fn;}}};
    });
    await page.goto('about:blank');await page.setContent(read(`${kind}_pet.html`));
    await page.evaluate(data=>hostMessage({data}),data);
    if(kind==='mini')await page.locator('#notifications-toggle').click();else await page.locator('.card').first().click();
  }
  const metrics=page=>page.locator('.card[data-status="review"]').evaluate(el=>{
    const style=getComputedStyle(el),name=getComputedStyle(el.querySelector('.name')),body=getComputedStyle(el.querySelector('.body'));
    return {html:el.innerHTML,font:style.fontSize,line:style.lineHeight,padding:style.padding,radius:style.borderRadius,
      background:style.backgroundColor,shadow:style.boxShadow,nameWeight:name.fontWeight,bodyColor:body.color};
  });
  assert.deepEqual(await metrics(pages.mini),await metrics(pages.classic),'mini renders the existing pet bubble without its own presentation overrides');
  for(const [kind,page] of Object.entries(pages)){
    const stop=page.locator('.card[data-status="running"] .stop');await stop.focus();await page.keyboard.press('Enter');
    const actions=await page.evaluate(()=>actions.map(raw=>{try{return JSON.parse(raw);}catch{return {};}}));
    assert.equal(actions.filter(value=>value.type==='stop').length,1);
    assert.equal(actions.filter(value=>value.type==='open').length,0,'stop keyboard action cannot open its session');
    await page.locator('.card[data-status="review"]').hover();
    await page.locator('.card[data-status="review"] .dismiss').click();
    assert.ok(await page.evaluate(()=>actions.some(raw=>{try{return JSON.parse(raw).type==='read';}catch{return false;}})));
    await page.evaluate(()=>{document.body.style.background='#fff';document.activeElement?.blur();});
    await page.screenshot({path:path.join(output,`${kind}-shared-bubble.png`)});
  }
  const mini=pages.mini;
  const cases=await mini.evaluate(()=>{
    const place=PetActivityCards.placePopup,view={width:600,height:500},desired={width:344,height:200};
    return [place({left:250,right:334,top:30,bottom:58},desired,view),
      place({left:250,right:334,top:420,bottom:448},desired,view),
      place({left:560,right:644,top:230,bottom:258},{width:344,height:1000},view)];
  });
  assert.equal(cases[0].upward,false);assert.equal(cases[0].clamped,false);
  assert.equal(cases[1].upward,true);assert.equal(cases[1].clamped,false);
  assert.equal(cases[2].clamped,true);
  for(const box of cases){assert.ok(box.left>=8&&box.left+box.width<=592);assert.ok(box.top>=8&&box.top+box.height<=492);}
  await mini.setViewportSize({width:800,height:600});
  for(const [name,anchor,count] of [['down',{left:.25,top:.05,width:344/800,height:252/600},2],
    ['up',{left:.25,top:.55,width:344/800,height:252/600},4],
    ['clamped',{left:.7,top:.4,width:344/800,height:252/600},20]]){
    await mini.evaluate(({data,anchor,count})=>{
      hostMessage({data:{...data,activities:Array.from({length:count},(_,i)=>({...data.activities[1],sessionId:String(i),key:String(i)}))}});
      hostMessage({data:{type:'mini-popup-layout',expanded:true,anchor}});
    },{data,anchor,count});
    const panel=mini.locator('#notifications');
    assert.equal(await panel.evaluate(el=>getComputedStyle(el).scrollbarWidth),'none');
    if(name!=='clamped')assert.equal(await panel.getAttribute('data-direction'),name);
    else assert.equal(await panel.getAttribute('data-clamped'),'true');
    const box=await panel.boundingBox();assert.ok(box.x>=0&&box.y>=0&&box.x+box.width<=800&&box.y+box.height<=600);
    await mini.screenshot({path:path.join(output,`mini-${name}.png`)});
    await mini.locator('#cards > .card').last().focus();
    const last=await mini.locator('#cards > .card').last().boundingBox();assert.ok(last.y+last.height<=box.y+box.height+1,'keyboard reaches overflow cards');
  }
  assert.deepEqual(errors,[]);
  console.log(JSON.stringify({ok:true,output,coverage:'shared bubble parity, status actions, downward/upward/clamped placement, scrollbar-free overflow access'}));
}finally{await browser.close();}
