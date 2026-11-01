import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import net from 'node:net';
import { spawn } from 'node:child_process';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { createServer } from 'vite';

const web = fileURLToPath(new URL('../', import.meta.url));
const root = path.dirname(web);
const { chromium } = await import(process.env.ACE_PLAYWRIGHT_MODULE
  ? pathToFileURL(path.resolve(process.env.ACE_PLAYWRIGHT_MODULE)).href : 'playwright');
const output = fs.mkdtempSync(path.join(os.tmpdir(), 'ace-mini-input-'));
const native = process.env.ACE_MINI_NATIVE_SMOKE;
const fixture = `
import React from 'react';
import {createRoot} from 'react-dom/client';
import {useDesktopOffice} from '/src/lib/useDesktopOffice.js';
import {api} from '/src/lib/api.js';
import {connection} from '/src/lib/connection.js';
import {ComposerActionButton} from '/src/components/ComposerActionButton.jsx';
import '/src/styles/globals.css';
window.test={creates:[],sends:[],uploads:[],audio:[],tasks:[],focuses:0,fail:false,configured:true};
window.focus=()=>{test.focuses++;};
window.aceDesktop_createNewSession=()=>{throw new Error('mini must not invoke main navigation');};
const native=typeof window.aceMiniSmokeHide==='function';
let latest=null;
window.replayMini=()=>{if(latest)document.querySelector('iframe')?.contentWindow.hostMessage?.(latest);};
if(!native){
 window.aceDesktop_getOfficePreferences=async()=>({ok:true,available:true,enabled:true,petKind:'mini'});
 window.aceDesktop_getOfficeState=async()=>({follow:true});
 window.aceDesktop_updateOffice=async value=>{latest=JSON.parse(value);test.latest=latest;window.replayMini();return true;};
}else{
 const update=window.aceDesktop_updateOffice;
 window.aceDesktop_updateOffice=async value=>{test.latest=JSON.parse(value);return update(value);};
}
window.petAction=raw=>{
 if(raw==='ready'){window.replayMini();return;}
 if(typeof raw!=='string'||!raw.startsWith('{'))return;
 const action=JSON.parse(raw);
 if(action.type==='mini-input')return;
 window.dispatchEvent(new CustomEvent('ace-desktop-office-action',{detail:action}));
};
Object.assign(connection,{retainSession(){},releaseSession(){}});
const create=async(workspace,options)=>{const session={id:'mini-'+(test.creates.length+1),workspace_hash:workspace,title:options.title};test.creates.push({workspace,options,session});return session;};
Object.assign(api,{
 getDesktopOffice:async()=>({offices:[],agents:[],selected:null,activities:test.tasks,complete:true}),
 getSessionModel:async()=>({}),
 createSession:options=>create('',options),createWorkspaceSession:create,
 sendInput:async(id,payload)=>{test.sends.push({id,payload});if(test.fail)throw new Error('fixture offline');
  const session=test.creates.find(item=>item.session.id===id).session;
  test.tasks.push({...session,busy:true,active:true,updated_at:new Date().toISOString(),activity:{phase:'reasoning'}});return {queued:true};},
 uploadSessionAttachment:async(id,payload)=>{test.uploads.push({id,payload});return {id:'uploaded-'+test.uploads.length};},
 getSpeechAvailability:async()=>({enabled:test.configured,configured:test.configured,max_recording_seconds:60,timeout_ms:1000,generation:1}),
 transcribeSpeech:async(id,audio)=>{const buffer=await audio.arrayBuffer();test.audio.push({id,size:buffer.byteLength,rate:new DataView(buffer).getUint32(24,true)});return {status:'ok',text:'从 mini 录音启动任务'};},
 cancelSpeech:async()=>{},
});
function Fixture(){const office=useDesktopOffice({workspaceHash:'workspace-a'});window.office=office;return <>
  <div id="main-composer-probe" style={{position:'absolute',left:-10000}}><ComposerActionButton actionState={{mode:'send',canSubmit:true,submitLabel:'发送',submitTitle:'发送'}} onSubmit={()=>{}}/></div>
  {native?null:<iframe title="mini" src="/mini-page" style={{border:0,width:344,height:252}}/>}</>;}
createRoot(document.getElementById('root')).render(<Fixture/>);
`;
const virtual = path.join(web, '__mini-background-fixture.jsx').replaceAll('\\', '/');
const miniHtml = fs.readFileSync(path.join(root, 'assets/desktop_pet/mini_pet.html'), 'utf8')
  .replace(/<script src="([^"]+)"><\/script>/g,(_,source)=>'<script>'+fs.readFileSync(path.join(root,'assets/desktop_pet',source),'utf8')+'</script>')
  .replace('<head>', `<head><script>
window.chrome={webview:{postMessage:value=>parent.petAction(value),addEventListener:(_,fn)=>{window.hostMessage=data=>fn({data});}}};
</script>`);
const server = await createServer({root:web,logLevel:'error',cacheDir:path.join(output,'.vite'),
  server:{host:'127.0.0.1',port:0,open:false,hmr:false},plugins:[{
    name:'mini-background-fixture',enforce:'pre',
    resolveId:id=>id==='/__mini-background-fixture.jsx'?virtual:undefined,
    load:id=>id.replaceAll('\\','/')===virtual?fixture:undefined,
    configureServer(vite){vite.middlewares.use(async(req,res,next)=>{
      if(req.url==='/mini-page'){res.setHeader('Content-Type','text/html');res.end(miniHtml);return;}
      if(req.url!=='/mini-background')return next();
      res.setHeader('Content-Type','text/html');
      res.end(await vite.transformIndexHtml('/mini-background','<!doctype html><html><title>mini background fixture</title><body style="margin:0;background:#8bb1c7"><div id="root"></div><script type="module" src="/__mini-background-fixture.jsx"></script></body></html>'));
    });},
  }]});
let browser, child, childLog = '', page, pet;
const errors = [];
try {
  await server.listen();
  const url = `http://127.0.0.1:${server.httpServer.address().port}/mini-background`;
  if (native) {
    const probe = net.createServer(); await new Promise(resolve=>probe.listen(0,'127.0.0.1',resolve));
    const port = probe.address().port; await new Promise(resolve=>probe.close(resolve));
    const prefs = path.join(output,'local','ACECode','desktop-pet'); fs.mkdirSync(prefs,{recursive:true});
    fs.writeFileSync(path.join(prefs,'office.json'),JSON.stringify({enabled:true,pet_kind:'mini'}));
    fs.writeFileSync(path.join(prefs,'settings.json'),JSON.stringify({scale:2.3}));
    child=spawn(path.resolve(native),[url],{windowsHide:true,env:{...process.env,LOCALAPPDATA:path.join(output,'local'),
      WEBVIEW2_USER_DATA_FOLDER:path.join(output,'webview'),
      WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS:`--remote-debugging-port=${port} --use-fake-device-for-media-stream --autoplay-policy=user-gesture-required`}});
    child.stdout.on('data',value=>{childLog+=value;});child.stderr.on('data',value=>{childLog+=value;});
    const deadline=Date.now()+30000;
    while(!browser&&Date.now()<deadline){try{browser=await chromium.connectOverCDP(`http://127.0.0.1:${port}`,{timeout:1000});}catch{await new Promise(resolve=>setTimeout(resolve,250));}}
    assert.ok(browser,childLog);
    while(Date.now()<deadline){
      const pages=browser.contexts().flatMap(context=>context.pages());
      page=pages.find(value=>value.url().includes('/mini-background'));
      for(const candidate of pages)if(await candidate.title()==='mini')pet=candidate;
      if(page&&pet)break;
      await new Promise(resolve=>setTimeout(resolve,250));
    }
    assert.ok(page&&pet,`native main/pet not available: ${childLog}`);
    await page.evaluate(()=>window.aceMiniSmokeHide());
  } else {
    browser=await chromium.launch({channel:process.env.ACE_BROWSER_CHANNEL||'msedge',headless:true,
      args:['--use-fake-ui-for-media-stream','--use-fake-device-for-media-stream','--autoplay-policy=user-gesture-required']});
    page=await browser.newPage({viewport:{width:420,height:300},deviceScaleFactor:Number(process.env.ACE_DEVICE_SCALE)||1});
    await page.goto(url);
    pet=page.frameLocator('iframe');
  }
  page.on('pageerror',error=>errors.push(error.message));
  const hidden = async () => {
    assert.equal(await page.evaluate(()=>test.focuses),0);
    if(native){const result=await page.evaluate(()=>window.aceMiniSmokeState());const state=typeof result==='string'?JSON.parse(result):result;
      assert.equal(state.visible,false,'main HWND remains hidden');assert.equal(state.foreground,false,'main HWND never gains foreground');}
  };
  await page.waitForFunction(()=>window.office?.ready);
  await pet.locator('#compose').click();await hidden();
  await pet.locator('#draft').fill('mini 后台文字任务');
  await page.locator('#main-composer-probe').evaluate(el=>{el.style.left='24px';el.style.top='24px';});
  const mainButton = await page.locator('#main-composer-probe button').evaluate(button=>({
    diameter:button.getBoundingClientRect().height*devicePixelRatio,background:getComputedStyle(button).backgroundColor,
    color:getComputedStyle(button).color,path:button.querySelector('path').getAttribute('d')}));
  const miniButton = await pet.locator('#send').evaluate(button=>({
    diameter:button.getBoundingClientRect().height*devicePixelRatio,background:getComputedStyle(button).backgroundColor,
    color:getComputedStyle(button).color,path:button.querySelector('path').getAttribute('d')}));
  assert.ok(Math.abs(mainButton.diameter-miniButton.diameter)<1,`send control physical heights: ${JSON.stringify({mainButton,miniButton})}`);
  assert.equal(miniButton.background,mainButton.background);assert.equal(miniButton.color,mainButton.color);assert.equal(miniButton.path,mainButton.path);
  fs.writeFileSync(path.join(output,'send-comparison.json'),JSON.stringify({mainButton,miniButton},null,2));
  if(native){
    await page.screenshot({path:path.join(output,'native-main-send.png'),clip:{x:0,y:0,width:100,height:100}});
    await pet.screenshot({path:path.join(output,'native-mini-composer.png'),clip:{x:0,y:0,width:344,height:110},omitBackground:true});
  }
  await pet.locator('#send').click();
  await page.waitForFunction(()=>test.tasks.length===1);await hidden();
  await pet.locator('#composer-wrap').waitFor({state:'hidden'});
  assert.equal(await pet.locator('#count').innerText(),'1');
  assert.equal(await page.evaluate(()=>test.creates[0].workspace),'workspace-a');
  await pet.locator('#compose').click();
  const attachmentBytes=Buffer.from('mini attachment'.repeat(10000));
  await pet.locator('#files').setInputFiles({name:'mini.txt',mimeType:'text/plain',buffer:attachmentBytes});
  await pet.locator('#draft').fill('带附件的任务');
  await pet.locator('#send').click();
  await page.waitForFunction(()=>test.tasks.length===2);await hidden();
  assert.ok(Buffer.from(await page.evaluate(()=>test.uploads[0].payload.data_base64),'base64').equals(attachmentBytes),'native bridge preserves every attachment chunk');
  await pet.locator('#composer-wrap').waitFor({state:'hidden'});
  await pet.locator('#compose').click();await pet.locator('#draft').fill('暂存文字草稿');
  await pet.locator('#files').setInputFiles({name:'retained.txt',mimeType:'text/plain',buffer:Buffer.from('retained')});
  await pet.locator('.attachment[aria-busy="false"]').waitFor();await pet.locator('#draft').press('Escape');
  await page.evaluate(()=>{test.configured=false;});
  await pet.locator('#voice').click();await pet.locator('#error').waitFor();await hidden();
  assert.match(await pet.locator('#error').innerText(),/配置并启用语音识别/);
  await page.evaluate(()=>{test.configured=true;});
  await pet.locator('#voice').click();
  await page.waitForFunction(()=>test.latest?.mini?.speechPhase==='recording');
  await page.waitForTimeout(700);
  await pet.locator('#voice').click();
  await page.waitForFunction(()=>test.tasks.length===3,{},{timeout:20000});await hidden();
  assert.ok(await page.evaluate(()=>test.audio[0].size>44&&test.audio[0].rate===16000));
  assert.equal(await page.evaluate(()=>test.sends.at(-1).payload.text),'从 mini 录音启动任务');
  await pet.locator('#compose').click();
  assert.equal(await pet.locator('#draft').inputValue(),'暂存文字草稿');
  assert.equal(await pet.locator('.attachment').count(),1,'native voice task preserves text attachments');
  await pet.locator('#draft').press('Escape');await page.evaluate(()=>{test.fail=true;});
  await pet.locator('#voice').click();await page.waitForFunction(()=>test.latest?.mini?.speechPhase==='recording');
  await page.waitForTimeout(500);await pet.locator('#voice').click();
  await pet.locator('#retry-voice').waitFor();await hidden();
  assert.equal(await pet.locator('#draft').inputValue(),'暂存文字草稿','voice failure preserves the independent text draft');
  const voiceCreated=await page.evaluate(()=>test.creates.length);
  await page.evaluate(()=>{test.fail=false;});await pet.locator('#retry-voice').click();
  await page.waitForFunction(()=>test.tasks.length===4);await hidden();
  assert.equal(await page.evaluate(()=>test.creates.length),voiceCreated,'voice retry reuses its own session');
  assert.deepEqual(await page.evaluate(()=>test.sends.at(-1).payload.attachments),[],'voice retry cannot consume text attachments');
  await pet.locator('#compose').click();
  assert.equal(await pet.locator('#draft').inputValue(),'暂存文字草稿');
  assert.equal(await pet.locator('.attachment').count(),1);
  await pet.locator('#draft').fill('失败后重试');
  await page.evaluate(()=>{test.fail=true;});await pet.locator('#send').click();
  await pet.locator('#error').waitFor();assert.equal(await pet.locator('#draft').inputValue(),'失败后重试');
  const created=await page.evaluate(()=>test.creates.length);
  await page.evaluate(()=>{test.fail=false;});await pet.locator('#send').click();
  await page.waitForFunction(()=>test.tasks.length===5);await hidden();
  assert.equal(await page.evaluate(()=>test.creates.length),created,'retry reuses created session');
  if(native){
    const state=async()=>{const raw=await page.evaluate(()=>window.aceMiniSmokeState());return typeof raw==='string'?JSON.parse(raw):raw;};
    const screenAnchor=async()=>{
      const native=await state();
      const box=await pet.locator('#toolbar').evaluate(el=>{const r=el.getBoundingClientRect();return {x:r.x*devicePixelRatio,y:r.y*devicePixelRatio};});
      return {x:native.pet.x+box.x,y:native.pet.y+box.y};
    };
    for(const [position,direction] of [['top-left','down'],['bottom-right','up']]){
      await page.evaluate(value=>window.aceMiniSmokePosition(value),position);
      const before=await screenAnchor();
      await pet.locator('#notifications-toggle').click();
      await pet.locator('html[data-mini-popup="true"]').waitFor();
      await pet.locator(`#notifications[data-direction="${direction}"]`).waitFor();
      const current=await state(),after=await screenAnchor();
      assert.ok(Math.abs(before.x-after.x)<2&&Math.abs(before.y-after.y)<2,'expansion keeps the toolbar anchored');
      const popup=await pet.locator('#notifications').evaluate(el=>{const r=el.getBoundingClientRect();return {x:r.x*devicePixelRatio,y:r.y*devicePixelRatio,width:r.width*devicePixelRatio,height:r.height*devicePixelRatio,scrollbar:getComputedStyle(el).scrollbarWidth};});
      assert.equal(popup.scrollbar,'none');
      assert.ok(current.pet.x+popup.x>=current.work.x&&current.pet.y+popup.y>=current.work.y);
      assert.ok(current.pet.x+popup.x+popup.width<=current.work.x+current.work.width+1&&current.pet.y+popup.y+popup.height<=current.work.y+current.work.height+1);
      const hit=async point=>{const raw=await page.evaluate(value=>window.aceMiniSmokeHit(value),point);return typeof raw==='string'?JSON.parse(raw):raw;};
      assert.equal(await hit({x:Math.round(after.x-current.pet.x+10),y:Math.round(after.y-current.pet.y+10)}),true,'toolbar receives input');
      assert.equal(await hit({x:Math.round(popup.x+popup.width/2),y:Math.round(popup.y+popup.height/2)}),true,'bubbles receive input');
      assert.equal(await hit({x:Math.round(current.pet.width/2),y:Math.round(current.pet.height/2)}),false,'empty work area is outside the native hit region');
      await hidden();
      const clip=await pet.evaluate(()=>{
        const a=document.querySelector('#toolbar-wrap').getBoundingClientRect(),b=document.querySelector('#notifications').getBoundingClientRect();
        const x=Math.max(0,Math.min(a.left,b.left)-20),y=Math.max(0,Math.min(a.top,b.top)-20);
        return {x,y,width:Math.min(innerWidth,Math.max(a.right,b.right)+20)-x,height:Math.min(innerHeight,Math.max(a.bottom,b.bottom)+20)-y};
      });
      await pet.screenshot({path:path.join(output,`native-popup-${direction}.png`),clip,omitBackground:true});
      await pet.locator('#notifications-toggle').click();
      await pet.locator('html[data-mini-popup="false"]').waitFor();
      const closed=await screenAnchor();assert.ok(Math.abs(before.x-closed.x)<2&&Math.abs(before.y-closed.y)<2,'collapse restores the compact anchor');
    }
    const compact=await state();
    await pet.locator('#notifications-toggle').click();
    await pet.locator('html[data-mini-popup="true"]').waitFor();
    await pet.locator('#toolbar').evaluate(el=>{const r=el.getBoundingClientRect();el.dispatchEvent(new WheelEvent('wheel',{deltaY:-120,clientX:r.x+r.width/2,clientY:r.y+r.height/2,cancelable:true,bubbles:true}));});
    await pet.locator('html[data-mini-popup="false"]').waitFor();
    assert.ok((await state()).pet.width>compact.pet.width,'zoom restores compact geometry before resizing');
    await pet.evaluate(()=>chrome.webview.postMessage('size-reset'));
    await page.waitForFunction(async width=>{const raw=await window.aceMiniSmokeState();return (typeof raw==='string'?JSON.parse(raw):raw).pet.width===width;},compact.pet.width);
    await hidden();
  }
  assert.deepEqual(errors,[]);
  if(native)await page.evaluate(()=>window.aceMiniSmokeDone({ok:true,tasks:test.tasks.length,audio:test.audio}));
  console.log(JSON.stringify({ok:true,native:!!native,output,coverage:'shared send appearance, popup placement/hit regions/zoom, text, attachments, recording, auto-send, retry and hidden main window'}));
} finally {
  if(!native)await browser?.close();
  if(child&&child.exitCode===null){await new Promise(resolve=>setTimeout(resolve,300));if(child.exitCode===null)child.kill();}
  fs.writeFileSync(path.join(output,'native.log'),childLog);
  await server.close();
}
