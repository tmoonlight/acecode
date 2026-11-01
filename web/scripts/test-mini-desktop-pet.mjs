import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { interfaceIconSvg } from '../src/lib/interfaceIcons.js';

const root = fileURLToPath(new URL('../../', import.meta.url));
const { chromium } = await import(process.env.ACE_PLAYWRIGHT_MODULE
  ? pathToFileURL(path.resolve(process.env.ACE_PLAYWRIGHT_MODULE)).href : 'playwright');
const { PNG } = await import(process.env.ACE_PNG_MODULE
  ? pathToFileURL(path.resolve(process.env.ACE_PNG_MODULE)).href : 'pngjs');
const output = process.env.ACE_PET_CAPTURE_DIR || fs.mkdtempSync(path.join(os.tmpdir(), 'ace-mini-pet-'));
fs.mkdirSync(output, { recursive: true });
const html = fs.readFileSync(path.join(root, 'assets/desktop_pet/mini_pet.html'), 'utf8').replace(/<script src="([^"]+)"><\/script>/g,
  (_, source) => '<script>' + fs.readFileSync(path.join(root, 'assets/desktop_pet', source), 'utf8') + '</script>');
assert.ok(Buffer.byteLength(html) < 2 * 1024 * 1024);
assert.ok(!/<script src=|<img/.test(html), 'mini is self-contained and loads no character art');
const shim = fs.readFileSync(path.join(root, 'src/apps/desktop/desktop_pet_mac_support.hpp'), 'utf8')
  .match(/kBridgeShim = R"JS\(([\s\S]*?)\)JS";/)[1];
const card = (id, status = 'review') => ({ sessionId: id, workspaceHash: 'workspace', status, key: id,
  title: '调整侧边聊天按钮和输入框', body: '已运行命令' });
const snapshot = activities => ({ version: 1, connected: true, activities,
  composerAppearance:{size:28,iconSize:14,pixelRatio:1,background:'rgb(8, 115, 212)',color:'rgb(255, 255, 255)',iconSvg:interfaceIconSvg('Send',14)} });
const browser = await chromium.launch({ channel: process.env.ACE_BROWSER_CHANNEL || 'msedge', headless: true });
const errors = [], external = [];
try {
  for (const host of ['windows', 'mac']) {
    const page = await browser.newPage({ viewport: { width: 544, height: 252 } });
    page.on('pageerror', error => errors.push(error.message));
    page.on('request', request => { if (/^https?:/.test(request.url())) external.push(request.url()); });
    await page.addInitScript({ content: `window.actions=[];
      window.post=value=>{actions.push(value);let action;try{action=JSON.parse(value);}catch{return;}
        if(action.type==='mini-popup')queueMicrotask(()=>hostMessage({type:'mini-popup-layout',expanded:action.expanded,
          request_id:action.request_id,anchor:action.expanded?{left:0,top:0,width:1,height:1}:undefined}));};
      ${host === 'mac' ? `window.webkit={messageHandlers:{acePet:{postMessage:post}}};
      ${shim}
      window.hostMessage=value=>window.__acePetDeliver(value);` : `window.chrome={webview:{postMessage:post,addEventListener:(_,listener)=>{window.hostMessage=value=>listener({data:value});}}};`}` });
    await page.goto('about:blank'); await page.setContent(html);
    const send = value => page.evaluate(data => hostMessage(data), value);
    const actions = () => page.evaluate(() => actions.map(value => { try { return JSON.parse(value); } catch { return value; } }));
    const overlay = async () => (await actions()).filter(value => value.type === 'overlay').at(-1);
    const toggle = page.locator('#notifications-toggle');
    assert.ok((await actions()).includes('ready'));
    assert.equal(await page.locator('#voice').isDisabled(),true,'recording waits until the background bridge is connected');
    await send(snapshot([card('done'), card('live', 'running')]));
    assert.equal(await page.locator('#count').innerText(), '2');
    assert.equal(await page.locator('#notifications').isVisible(), false);
    assert.equal((await overlay()).rect, null);
    await page.locator('#compose').click();
    assert.equal(await page.locator('#composer').isVisible(), true);
    await page.screenshot({path:path.join(output,`${host}-composer.png`)});
    await page.locator('#draft').fill('mini 独立任务');
    await page.locator('#draft').dispatchEvent('compositionstart');
    await page.locator('#draft').press('Enter');
    assert.equal((await actions()).filter(value => value.type === 'mini-submit').length, 0, 'IME Enter cannot submit');
    await page.locator('#draft').dispatchEvent('compositionend');
    await page.waitForTimeout(10);
    await page.locator('#send').click();
    const submitted = (await actions()).find(value => value.type === 'mini-submit');
    assert.equal(submitted.text, 'mini 独立任务');
    await page.locator('#composer').dispatchEvent('submit');
    assert.equal((await actions()).filter(value => value.type === 'mini-submit').length, 1, 'double submission is blocked');
    await send({...snapshot([card('done'),card('live','running')]),mini:{activeRequestId:submitted.request_id,sending:false,error:'offline'}});
    assert.equal(await page.locator('#draft').inputValue(),'mini 独立任务','failed submission preserves draft');
    await page.locator('#send').click();
    assert.equal((await actions()).filter(value => value.type === 'mini-submit').at(-1).request_id,submitted.request_id);
    await send({...snapshot([card('done'),card('live','running')]),mini:{activeRequestId:submitted.request_id,sending:false,
      lastSubmission:{requestId:submitted.request_id,sessionId:'mini-new'},pendingTasks:[card('mini-new','running')],speechPhase:'idle'}});
    assert.equal(await page.locator('#composer').isVisible(),false);
    assert.equal(await page.locator('#count').innerText(),'3','accepted task adds exactly one bubble');
    await page.locator('#voice').click();
    assert.ok((await actions()).some(value=>value.type==='mini-record-start'));
    await send({...snapshot([]),mini:{speechPhase:'recording'}});
    await page.screenshot({path:path.join(output,`${host}-recording.png`)});
    assert.equal(await page.locator('#record-dot').isVisible(),true);
    await page.locator('#voice').click();
    assert.ok((await actions()).some(value=>value.type==='mini-record-stop'));
    await send({...snapshot([card('voice-new','running')]),mini:{speechPhase:'idle',lastSubmission:{requestId:'voice-1',sessionId:'voice-new'}}});
    assert.equal(await page.locator('#count').innerText(),'1');
    assert.equal((await actions()).some(value=>['new-session','voice','activate','open'].includes(value.type)),false,'input never asks to open the main window');
    // A separate voice task must never consume a previously hidden text draft.
    await page.locator('#compose').click();await page.locator('#draft').fill('保留文字草稿');
    await page.locator('#files').setInputFiles({name:'draft.txt',mimeType:'text/plain',buffer:Buffer.from('draft')});
    await page.waitForFunction(()=>actions.some(raw=>raw.includes?.('mini-file-end')));
    const fileId=(await actions()).filter(value=>value.type==='mini-file-begin').at(-1).id;
    const files=[{id:fileId,name:'draft.txt',ready:true}];
    await send({...snapshot([]),mini:{speechPhase:'idle',files}});
    await page.keyboard.press('Escape');await page.locator('#voice').click();
    await send({...snapshot([]),mini:{speechPhase:'recording',files}});await page.locator('#voice').click();
    await send({...snapshot([]),mini:{speechPhase:'idle',files,lastSubmission:{requestId:'voice-separate',sessionId:'v2'}}});
    await page.locator('#compose').click();
    assert.equal(await page.locator('#draft').inputValue(),'保留文字草稿');
    assert.equal(await page.locator('.attachment').count(),1);
    await page.keyboard.press('Escape');await page.locator('#voice').click();
    await send({...snapshot([]),mini:{speechPhase:'idle',files,error:'offline',recovery:{requestId:'voice-retry',text:'待重试录音'}}});
    assert.equal(await page.locator('#draft').inputValue(),'保留文字草稿','voice failure cannot overwrite hidden text');
    await page.locator('#retry-voice').click();
    const retry=(await actions()).filter(value=>value.type==='mini-submit').at(-1);
    assert.deepEqual(retry.files,[]);assert.equal(retry.request_id,'voice-retry');assert.equal(retry.text,'待重试录音');
    await send({...snapshot([]),mini:{speechPhase:'idle',files,lastSubmission:{requestId:'voice-retry',sessionId:'v3'}}});
    await page.locator('#compose').click();
    assert.equal(await page.locator('#draft').inputValue(),'保留文字草稿');
    await page.getByRole('button',{name:'移除 draft.txt',exact:true}).click();
    await page.locator('#files').setInputFiles(Array.from({length:10},(_,i)=>({name:`${i}-${'很长的附件名称'.repeat(8)}.txt`,mimeType:'text/plain',buffer:Buffer.from('x')})));
    await page.waitForFunction(()=>document.querySelectorAll('.attachment').length===10);
    for(const [width,height] of [[172,126],[344,252]]){
      await page.setViewportSize({width,height});
      const last=page.locator('.attachment button').last();await last.scrollIntoViewIfNeeded();
      const lastBox=await last.boundingBox();assert.ok(lastBox.y>=0&&lastBox.y+lastBox.height<=height,'last attachment remains reachable');
      await last.click();
    }
    while(await page.locator('.attachment button').count())await page.locator('.attachment button').first().click();
    await page.locator('#draft').fill('');await page.keyboard.press('Escape');await page.setViewportSize({width:544,height:252});
    await send(snapshot([card('done'),card('live','running')]));
    await toggle.click();
    assert.equal(await toggle.getAttribute('aria-expanded'), 'true');
    assert.equal(await page.locator('#bell').isVisible(), false);
    assert.equal(await page.locator('#chevron').isVisible(), true);
    assert.equal(await page.locator('#cards > .card').count(), 2);
    await page.locator('#cards > .card').first().focus();
    await send(snapshot([{ ...card('done'), body: '最新结果' }, card('live', 'running')]));
    assert.equal(await page.evaluate(() => document.activeElement === document.querySelector('#cards > .card')), true, 'snapshots preserve keyboard focus');
    await page.keyboard.press('Enter');
    assert.ok((await actions()).some(value => value.type === 'open' && value.session_id === 'done' && value.workspace_hash === 'workspace'));
    assert.equal(await toggle.getAttribute('aria-expanded'), 'false');
    await toggle.click(); await page.keyboard.press('Escape');
    assert.equal((await overlay()).rect, null, 'collapse removes card hit regions');
    await send(snapshot([])); await toggle.click();
    assert.equal(await page.locator('#empty').innerText(), '暂无会话动态');
    await send({ version: 1, connected: false, activities: [card('stale')] });
    assert.equal(await page.locator('#cards > .card').count(), 0);
    assert.match(await page.locator('#empty').innerText(), /连接已中断/);
    await send(snapshot([card('done')]));
    await toggle.click();
    const beforeDrag = (await actions()).filter(value => value.type === 'mini-input' && value.active).length;
    const box = await page.locator('#compose').boundingBox();
    await page.mouse.move(box.x + box.width / 2, box.y + box.height / 2); await page.mouse.down();
    await page.mouse.move(box.x + box.width / 2 + 12, box.y + box.height / 2, { steps: 3 }); await page.mouse.up();
    assert.ok((await actions()).includes('drag'));
    assert.equal((await actions()).filter(value => value.type === 'mini-input' && value.active).length, beforeDrag, 'drag never composes');
    await page.locator('#compose').click();
    assert.equal((await actions()).filter(value => value.type === 'mini-input' && value.active).length, beforeDrag + 1);
    await page.keyboard.press('Escape');
    const dragCount = (await actions()).filter(value => value === 'drag').length;
    await page.evaluate(() => {
      for (const key of ['screenX','screenY']) Object.defineProperty(PointerEvent.prototype,key,{get:()=>0,configurable:true});
    });
    await page.mouse.move(box.x + box.width / 2, box.y + box.height / 2); await page.mouse.down();
    await page.mouse.move(box.x + box.width / 2 + 12, box.y + box.height / 2, {steps:3}); await page.mouse.up();
    assert.equal((await actions()).filter(value => value === 'drag').length, dragCount + 1, 'WebKit drag works without screen coordinates');
    await page.locator('#toolbar').click({ button: 'right' });
    assert.ok((await actions()).includes('menu'));
    await page.locator('#grip').focus(); await page.keyboard.press('ArrowUp');
    assert.ok((await actions()).includes('zoom 1 0.5 0.2'));
    for (const [width, height, theme] of [[544, 252, 'light'], [344, 252, 'light'], [172, 126, 'light'], [430, 315, 'dark']]) {
      await page.setViewportSize({ width, height }); await page.emulateMedia({ colorScheme: theme });
      await page.evaluate(value => {
        document.body.style.background = value === 'dark' ? '#242424' : '#fff';
        document.activeElement?.blur();
      }, theme);
      await send(snapshot([card('done')]));
      if (await toggle.getAttribute('aria-expanded') === 'true') await toggle.click();
      await page.mouse.move(0, height - 1); await page.evaluate(() => document.activeElement?.blur());
      await page.screenshot({ path: path.join(output, `${host}-${theme}-${width}-collapsed.png`), omitBackground: true });
      await toggle.click(); await page.mouse.move(0, height - 1);
      await page.evaluate(() => document.activeElement?.blur());
      assert.equal(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), true);
      const geometry = await page.locator('#cards > .card').first().boundingBox();
      assert.ok(geometry.x >= 0 && geometry.x + geometry.width <= width);
      assert.ok(geometry.y >= 0 && geometry.y + geometry.height <= height);
      assert.equal(await page.locator('#toolbar').evaluate(element => getComputedStyle(element).backgroundColor), 'rgb(255, 255, 255)');
      for (const rect of [(await overlay()).controls, (await overlay()).rect]) assert.ok(rect.every(value => value >= 0 && value <= 1));
      await page.screenshot({ path: path.join(output, `${host}-${theme}-${width}-expanded.png`), omitBackground: true });
      await page.evaluate(() => { document.body.style.background = 'transparent'; });
      const pixels = PNG.sync.read(await page.screenshot({omitBackground:true}));
      const region = await overlay();
      const rects = [region.controls,region.rect,region.grip,...region.bubbles].filter(Boolean);
      let clippedAlpha = 0;
      for(let y=0;y<pixels.height;y++)for(let x=0;x<pixels.width;x++){
        if(rects.some(([left,top,right,bottom])=>x>=Math.floor(left*width)&&x<Math.ceil(right*width)&&y>=Math.floor(top*height)&&y<Math.ceil(bottom*height)))continue;
        clippedAlpha=Math.max(clippedAlpha,pixels.data[(y*pixels.width+x)*4+3]);
      }
      assert.ok(clippedAlpha<=3,`${host}/${width}: native region clips shadow alpha ${clippedAlpha}`);
    }
    await send(snapshot(Array.from({ length: 12 }, (_, i) => ({ ...card(String(i)), title: '很长的会话名称'.repeat(16) }))));
    await page.locator('#cards > .card').last().scrollIntoViewIfNeeded();
    await page.locator('#cards > .card').last().click();
    assert.ok((await actions()).some(value => value.type === 'open' && value.session_id === '11'), 'all notifications remain reachable');
    await page.close();
  }
    assert.deepEqual(errors, []); assert.deepEqual(external, []);
  console.log(JSON.stringify({ ok: true, output, hosts: ['windows', 'mac bridge'], screenshots: 20 }));
} finally { await browser.close(); }
