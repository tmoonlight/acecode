import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { createServer } from 'vite';

const web = fileURLToPath(new URL('../', import.meta.url));
const { chromium } = await import(process.env.ACE_PLAYWRIGHT_MODULE
  ? pathToFileURL(path.resolve(process.env.ACE_PLAYWRIGHT_MODULE)).href : 'playwright');
const output = process.env.ACE_SPEECH_ARTIFACTS || await fs.mkdtemp(path.join(os.tmpdir(), 'ace-speech-ui-'));
await fs.mkdir(output, { recursive: true });
const fixture = String.raw`
import React,{useState,useRef} from 'react';
import {createRoot} from 'react-dom/client';
import '/src/styles/globals.css';
import {InputBar} from '/src/components/InputBar.jsx';
import {SettingsPage} from '/src/components/SettingsPage.jsx';
import '/src/i18n/index.js';
import {Toaster} from '/src/components/Toast.jsx';
import {api} from '/src/lib/api.js';
import {SPEECH_DEFAULTS,speechAvailabilityStore} from '/src/lib/speechRecognition.js';
import {composerContentFromText} from '/src/lib/composerContent.js';
let config={...SPEECH_DEFAULTS,configured:false,generation:0};
window.fixture={sent:[],uploads:[],cancelled:[],saves:[],tests:[],hold:false,nextText:'识别文字',failure:null,
 saved:()=>({...config}),failSave:false,holdSave:false,releaseSave(){this.pendingSave?.();this.pendingSave=null;},
 release(){this.pending?.();this.pending=null;},
 disable(){config={...config,enabled:false,generation:config.generation+1};speechAvailabilityStore(api).publish(config);}};
Object.assign(api,{
 getSpeechAvailability:async()=>({...config}),
 getSpeechRecognition:async()=>({...config}),
 setSpeechRecognition:async(next)=>{
  window.fixture.saves.push(next);
  if(window.fixture.holdSave)await new Promise(resolve=>window.fixture.pendingSave=resolve);
  if(window.fixture.failSave)throw {code:'PERSIST_FAILED'};
  config={...config,...next,generation:config.generation+1,configured:!!next.base_url&&(next.protocol!=='openai'||!!next.model)};return {...config};},
 testSpeechRecognition:async(next)=>{window.fixture.tests.push({draft:next,saved:{...config}});return {ok:true};},
 transcribeSpeech:async(id,audio,options)=>{
  const buffer=await audio.arrayBuffer();
  window.fixture.uploads.push({id,bytes:buffer.byteLength,rate:new DataView(buffer).getUint32(24,true)});
  if(window.fixture.hold)await new Promise(resolve=>window.fixture.pending=resolve);
  if(window.fixture.failure)throw {code:window.fixture.failure};
  return {status:window.fixture.nextText?'ok':'no_speech',text:window.fixture.nextText};
 },
 cancelSpeech:async(id)=>window.fixture.cancelled.push(id),
 listCommands:async()=>({commands:[]}),
 getImageGeneration:async()=>({enabled:false,source:'inline',base_url:'',api_key:'',models:{standard:'',high:'',ultra:''},default_quality:'standard',connections:[]}),
 getComputerUse:async()=>({enabled:false,available:true}),
});
function App(){
 const [value,setValue]=useState('已有草稿 '),[content,setContent]=useState(composerContentFromText('已有草稿 '));
 const [sid,setSid]=useState('session-a'),[width,setWidth]=useState(900),[variant,setVariant]=useState('default');
 const editor=useRef(null);
 const [settingsOpen,setSettingsOpen]=useState(true),[settingsNav,setSettingsNav]=useState('tools');
 window.fixture.settings=(open,nav='tools')=>{setSettingsNav(nav);setSettingsOpen(open);};
 window.fixture.state={value,content,sid};
 window.fixture.content=()=>editor.current?.getComposerContent();
 window.fixture.reset=(text)=>editor.current?.replaceText(text);
 window.fixture.tags=()=>editor.current?.setComposerContent({version:1,parts:[
  {type:'text',text:'草稿 '},{type:'path',path:'C:/work/sample.js',token:'@"C:/work/sample.js"'},
  {type:'text',text:' '},{type:'skill',name:'review',token:'/review',path:'C:/skills/review/SKILL.md'},
  {type:'text',text:' 结尾'}]});
 window.fixture.switch=()=>{setSid('session-b');setValue('另一会话草稿');setContent(composerContentFromText('另一会话草稿'));};
 window.fixture.layout=(next)=>{if(next.width)setWidth(next.width);if(next.variant)setVariant(next.variant);};
 const model={name:'ACEModel-gpt-6-astra',provider:'openai',model:'fixture'};
 return <main className="min-h-screen bg-bg text-fg p-4">
  {settingsOpen&&<SettingsPage initialNavKey={settingsNav} onClose={()=>setSettingsOpen(false)} office={{}}/>}
  <div data-fixture-composer style={{width,maxWidth:'100%'}}>
   <InputBar ref={editor} value={value} composerContent={content}
    onChange={(text,next)=>{setValue(text);if(next)setContent(next);}} onComposerContentChange={setContent}
    currentSessionId={sid} cwd="C:/fixture" mainComposer variant={variant}
    onSubmit={text=>window.fixture.sent.push(text)}
    sessionControls={{model:'ACEModel-gpt-6-astra',modelOptions:[model],selectedModelName:model.name,
      onModelChange(){},onRefreshModels(){},permissionMode:'default',reasoningOptions:{label:'极高',items:[]}}}/>
  </div><button type="button" onClick={()=>{setSettingsNav('shortcuts');setSettingsOpen(true);}}>打开快捷键设置</button><Toaster/>
 </main>;
}
createRoot(document.getElementById('root')).render(<App/>);
`;
const virtual = path.join(web, '__speech-fixture.jsx').replaceAll('\\', '/');
const server = await createServer({
  root: web, logLevel: 'error', cacheDir: path.join(output, '.vite'),
  server: { host: '127.0.0.1', port: 0, open: false, hmr: false },
  plugins: [{
    name: 'speech-fixture', enforce: 'pre',
    resolveId(id) { return id === '/__speech-fixture.jsx' ? virtual : undefined; },
    load(id) { return id.replaceAll('\\', '/') === virtual ? fixture : undefined; },
    configureServer(vite) {
      vite.middlewares.use(async (req, res, next) => {
        if (req.url !== '/speech-fixture') return next();
        res.setHeader('Content-Type', 'text/html');
        res.end(await vite.transformIndexHtml('/speech-fixture', '<!doctype html><html lang="zh-CN" data-theme="light"><meta name="viewport" content="width=device-width,initial-scale=1"><body><div id="root"></div><script type="module" src="/__speech-fixture.jsx"></script></body></html>'));
      });
    },
  }],
});
let browser, page;
const errors = [];
try {
  await server.listen();
  browser = await chromium.launch({
    headless: true, executablePath: process.env.ACE_CHROMIUM_EXECUTABLE || undefined,
    args: ['--use-fake-ui-for-media-stream', '--use-fake-device-for-media-stream'],
  });
  page = await browser.newPage({ viewport: { width: 1100, height: 850 }, reducedMotion: 'reduce' });
  await page.addInitScript(() => {
    const media=navigator.mediaDevices;
    if(!media)return;
    const capture=media.getUserMedia.bind(media);
    window.speechAudioRequests=[];
    media.getUserMedia=async(options)=>{
      window.speechAudioRequests.push(options);
      const stream=await capture(options);
      if(window.holdMicrophonePermission){
        window.voicePendingStream=stream;
        await new Promise(resolve=>window.releaseMicrophonePermission=resolve);
      }
      return stream;
    };
  });
  page.on('pageerror', error => {errors.push(error.message);console.error(error.stack);});
  page.on('console', message => { if (message.type() === 'error') console.error(message.text()); });
  await page.route('**/api/**', route => route.fulfill({ json: {} }));
  await page.goto(server.resolvedUrls.local[0] + 'speech-fixture', { timeout: 120000 });
  const editor = page.locator('[data-slate-editor]');
  await editor.waitFor();
  assert.equal(await page.locator('[data-composer-speech]').count(), 0);
  const settings = page.locator('[data-speech-settings]');
  await settings.waitFor();
  for (const theme of ['light', 'dark']) {
    await page.evaluate(theme => { document.documentElement.dataset.theme=theme; }, theme);
    for (const width of [1440, 390]) {
      await page.setViewportSize({ width, height: 1000 });
      const geometry = await page.evaluate(() => {
        const speech=document.querySelector('[data-speech-settings]');
        const image=speech.previousElementSibling;
        const metrics = (section) => {
          const row=section.firstElementChild, icon=row.firstElementChild;
          const title=icon.nextElementSibling, button=row.querySelector('button[aria-expanded]');
          const toggle=row.querySelector('[role="switch"]');
          const rect=(node)=>{const r=node.getBoundingClientRect();return {x:r.x,right:r.right,width:r.width,height:r.height};};
          return {row:rect(row),icon:rect(icon),title:rect(title),button:rect(button),toggle:rect(toggle),
            iconRadius:getComputedStyle(icon).borderRadius,iconBorder:getComputedStyle(icon).borderLeftWidth,
            divider:getComputedStyle(row).borderBottomWidth};
        };
        return {speech:metrics(speech),image:metrics(image)};
      });
      for (const part of ['row','icon','title','button','toggle'])
        assert.ok(Math.abs(geometry.speech[part].x-geometry.image[part].x)<1, `${theme}/${width}: ${part} aligns with adjacent tool: ${JSON.stringify(geometry)}`);
      assert.equal(geometry.speech.icon.height,geometry.image.icon.height);
      assert.equal(geometry.speech.iconRadius,geometry.image.iconRadius);
      assert.equal(geometry.speech.iconBorder,geometry.image.iconBorder);
      assert.equal(geometry.speech.divider,geometry.image.divider);
      await page.locator('.ace-settings-content').screenshot({path:path.join(output,`tools-${theme}-${width}.png`)});
    }
  }
  await page.setViewportSize({width:1100,height:850});
  await page.evaluate(() => {document.documentElement.dataset.theme='light';});
  await settings.getByRole('button', { name: '配置', exact: true }).click();
  assert.equal(await settings.getByRole('button', {name:'保存',exact:true}).count(),0);
  await page.getByLabel('语音服务地址', { exact: true }).fill('http://192.168.1.50:9876');
  await page.getByLabel('语音服务地址', { exact: true }).press('Tab');
  await page.waitForFunction(() => window.fixture.saved().base_url==='http://192.168.1.50:9876');
  assert.equal(await page.locator('[data-composer-speech]').count(), 0, 'configured but disabled');
  await page.getByRole('switch', { name: '启用语音识别' }).click();
  await page.locator('[data-composer-speech]').waitFor();
  await settings.screenshot({ path: path.join(output, 'settings-light.png') });
  await page.getByLabel('语音 API Key', {exact:true}).fill('fixture-key');
  await page.getByLabel('语音 API Key', {exact:true}).press('Enter');
  await page.waitForFunction(() => window.fixture.saved().api_key==='fixture-key');
  await page.getByLabel('语音 API Key', {exact:true}).fill('');
  await settings.getByRole('button', {name:'收起',exact:true}).click();
  await page.waitForFunction(() => window.fixture.saved().api_key==='');
  await settings.getByRole('button', {name:'配置',exact:true}).click();
  await page.getByLabel('语音服务地址', {exact:true}).fill('http://speech.internal:8888');
  await page.evaluate(() => window.fixture.settings(false));
  await page.waitForFunction(() => window.fixture.saved().base_url==='http://speech.internal:8888');
  await page.evaluate(() => window.fixture.settings(true));
  await settings.getByRole('button', {name:'配置',exact:true}).click();
  assert.equal(await page.getByLabel('语音服务地址', {exact:true}).inputValue(),'http://speech.internal:8888');
  await page.evaluate(() => {window.fixture.holdSave=true;});
  await page.getByLabel('语音服务地址', {exact:true}).fill('http://speech.internal:9999');
  await settings.getByRole('button', {name:'测试连接',exact:true}).click();
  await page.waitForFunction(() => !!window.fixture.pendingSave);
  assert.equal(await page.evaluate(() => window.fixture.tests.length),0,'connection test waits for autosave');
  await page.evaluate(() => {window.fixture.holdSave=false;window.fixture.releaseSave();});
  await page.waitForFunction(() => window.fixture.tests.length===1);
  assert.equal(await page.evaluate(() => window.fixture.tests[0].saved.base_url),'http://speech.internal:9999');
  await page.evaluate(() => {window.fixture.failSave=true;});
  await page.getByLabel('语音服务地址', {exact:true}).fill('http://speech.internal:7777');
  await settings.getByRole('button', {name:'测试连接',exact:true}).click();
  await settings.getByRole('button', {name:'重试',exact:true}).waitFor();
  assert.equal(await page.evaluate(() => window.fixture.tests.length),1,'failed autosave prevents connection test');
  assert.equal(await page.getByLabel('语音服务地址', {exact:true}).inputValue(),'http://speech.internal:7777');
  await page.evaluate(() => {window.fixture.failSave=false;});
  await settings.getByRole('button', {name:'重试',exact:true}).click();
  await page.waitForFunction(() => window.fixture.saved().base_url==='http://speech.internal:7777');
  await settings.getByRole('button', { name: '收起', exact: true }).click();
  await page.evaluate(() => window.fixture.settings(false));
  const start = async () => {
    const before=await page.locator('[data-speech-composer]').boundingBox();
    await editor.click();
    await page.keyboard.press('ControlOrMeta+End');
    await page.getByRole('button', { name: '听写', exact: true }).click();
    await page.locator('[data-speech-recording="recording"]').waitFor();
    await page.waitForFunction(() => [...document.querySelectorAll('[data-speech-wave] span[aria-hidden]')]
      .some(node => parseFloat(node.style.height) > 3.01));
    const after=await page.locator('[data-speech-composer]').boundingBox();
    for(const key of ['x','y','width','height'])assert.ok(Math.abs(before[key]-after[key])<1,`${key} stays stable when recording starts: ${JSON.stringify({before,after})}`);
  };
  const idle = () => page.locator('[data-composer-speech]').waitFor();
  for (const theme of ['light', 'dark']) {
    await page.evaluate(theme => { document.documentElement.dataset.theme = theme; }, theme);
    await page.locator('[data-fixture-composer]').screenshot({ path: path.join(output, theme + '-idle.png') });
    await start();
    await page.locator('[data-fixture-composer]').screenshot({ path: path.join(output, theme + '-recording.png') });
    await page.getByRole('button', { name: '取消听写' }).click();
    await idle();
  }
  assert.equal(await page.evaluate(() => window.fixture.uploads.length), 0);
  assert.equal(await page.evaluate(() => window.fixture.content().parts[0].text), '已有草稿 ');
  await start();
  await page.getByRole('button', { name: '停止录音并插入文字' }).click();
  await idle();
  assert.match(await editor.innerText(), /已有草稿.*识别文字/);
  assert.equal(await page.evaluate(() => window.fixture.sent.length), 0);
  const upload = await page.evaluate(() => window.fixture.uploads[0]);
  assert.equal(upload.rate, 16000); assert.ok(upload.bytes > 44);
  await page.evaluate(() => window.fixture.reset(''));
  await start();
  await page.getByRole('button', { name: '识别并发送' }).click();
  await page.waitForFunction(() => window.fixture.sent.length === 1);
  assert.equal(await page.evaluate(() => window.fixture.sent[0]), '识别文字');
  await page.evaluate(() => window.fixture.reset('保留草稿 '));
  await start();
  await page.keyboard.press('Enter');
  await page.waitForFunction(() => window.fixture.sent.length === 2);
  assert.equal(await page.evaluate(() => window.fixture.sent[1]), '保留草稿 识别文字');
  await page.evaluate(() => { window.fixture.reset('失败保留'); window.fixture.failure='SPEECH_CONNECTION_FAILED'; });
  await start();
  await page.getByRole('button', { name: '识别并发送' }).click();
  await idle();
  assert.equal(await editor.innerText(), '失败保留');
  assert.equal(await page.evaluate(() => window.fixture.sent.length), 2);
  await page.evaluate(() => { window.fixture.failure=null; window.fixture.nextText=''; });
  await start();
  await page.getByRole('button', { name: '识别并发送' }).click();
  await idle();
  assert.equal(await editor.innerText(), '失败保留');
  assert.equal(await page.evaluate(() => window.fixture.sent.length), 2);
  await page.evaluate(() => { window.fixture.nextText='识别文字'; });
  await page.evaluate(() => { window.fixture.reset('原稿');window.fixture.hold=true; });
  await start();
  await page.getByRole('button', { name: '停止录音并插入文字' }).click();
  await page.locator('[data-speech-recording="transcribing"]').waitFor();
  await page.waitForFunction(() => !!window.fixture.pending);
  await page.evaluate(() => window.fixture.switch());
  await idle();
  await page.evaluate(() => window.fixture.release());
  assert.equal(await editor.innerText(), '另一会话草稿');
  assert.ok(await page.evaluate(() => window.fixture.cancelled.length >= 1));
  await page.evaluate(() => { window.fixture.hold=false;window.fixture.tags(); });
  const before = await page.evaluate(() => window.fixture.content().parts.filter(p => p.type !== 'text'));
  await start();
  await page.getByRole('button', { name: '停止录音并插入文字' }).click();
  await idle();
  const after = await page.evaluate(() => window.fixture.content().parts.filter(p => p.type !== 'text'));
  assert.deepEqual(after, before, 'file and skill tags survive voice insertion');
  for (const variant of ['default', 'hero']) {
    await page.setViewportSize({ width: 390, height: 700 });
    await page.evaluate(variant => window.fixture.layout({ width: 350, variant }), variant);
    await start();
    const overflow = await page.locator('[data-speech-recording]').evaluate(node => node.scrollWidth > node.clientWidth + 1);
    assert.equal(overflow, false);
    await page.locator('[data-fixture-composer]').screenshot({ path: path.join(output, 'mobile-' + variant + '.png') });
    await page.getByRole('button', { name: '取消听写' }).click();
    await idle();
  }
  await page.setViewportSize({width:1100,height:850});
  await page.evaluate(()=>window.fixture.layout({width:900,variant:'default'}));
  const microphone=page.getByRole('button',{name:'听写',exact:true});
  assert.equal(await microphone.getAttribute('title'),'右键配置麦克风');
  await microphone.click({button:'right'});
  const microphoneMenu=page.getByRole('menu',{name:'麦克风配置'});
  await microphoneMenu.waitFor();
  await microphoneMenu.locator('[data-microphone-device]').first().waitFor();
  assert.equal(await microphoneMenu.getByText('Language',{exact:true}).count(),0);
  assert.equal(await microphoneMenu.getByText('识别语言',{exact:true}).count(),0);
  const device=await microphoneMenu.locator('[data-microphone-device]').evaluateAll(items=>(items.find(item=>item.dataset.microphoneDevice!=='default')||items[0]).dataset.microphoneDevice);
  await microphoneMenu.locator('[data-microphone-device]').evaluateAll((items,id)=>items.find(item=>item.dataset.microphoneDevice===id).click(),device);
  await microphone.click({button:'right'});
  await microphoneMenu.locator('[aria-checked="true"][data-microphone-device]').waitFor();
  assert.equal(await microphoneMenu.locator('[aria-checked="true"][data-microphone-device]').getAttribute('data-microphone-device'),device);
  await microphoneMenu.screenshot({path:path.join(output,'microphone-menu.png')});
  await page.keyboard.press('Escape');
  await page.keyboard.press('Control+d');
  await page.locator('[data-speech-recording="recording"]').waitFor();
  await page.waitForFunction(()=>window.speechAudioRequests.some(request=>request.audio?.deviceId));
  assert.equal(await page.evaluate(()=>window.speechAudioRequests.at(-1).audio.deviceId?.exact),device);
  await page.waitForFunction(()=>[...document.querySelectorAll('[data-speech-wave] span')].some(node=>parseFloat(node.style.height)>3.01));
  await page.keyboard.press('Control+d');
  await idle();
  await page.getByRole('button',{name:'打开快捷键设置',exact:true}).click();
  await page.getByRole('button',{name:'修改快捷键：开始／停止录音',exact:true}).click();
  await page.getByLabel('录入快捷键',{exact:true}).press('Control+Alt+u');
  assert.equal(await page.locator('[data-speech-recording]').count(),0,'shortcut recorder must not start audio');
  await page.getByRole('button',{name:'保存',exact:true}).click();
  await page.evaluate(()=>window.fixture.settings(false));
  await page.keyboard.press('Control+Alt+u');
  await page.locator('[data-speech-recording="recording"]').waitFor();
  await page.getByRole('button',{name:'取消听写'}).click();await idle();
  await microphone.click({button:'right'});
  await microphoneMenu.getByRole('switch',{name:'按住录音'}).click();
  await page.keyboard.press('Escape');
  await page.keyboard.down('Control');await page.keyboard.down('Alt');await page.keyboard.down('u');
  await page.locator('[data-speech-recording="recording"]').waitFor();
  await page.waitForFunction(()=>[...document.querySelectorAll('[data-speech-wave] span')].some(node=>parseFloat(node.style.height)>3.01));
  await page.keyboard.up('u');await page.keyboard.up('Alt');await page.keyboard.up('Control');await idle();
  const microphoneBounds=await microphone.boundingBox();
  await page.mouse.move(microphoneBounds.x+microphoneBounds.width/2,microphoneBounds.y+microphoneBounds.height/2);
  await page.mouse.down();await page.locator('[data-speech-recording="recording"]').waitFor();
  await page.waitForFunction(()=>[...document.querySelectorAll('[data-speech-wave] span')].some(node=>parseFloat(node.style.height)>3.01));
  await page.mouse.up();await idle();
  await page.keyboard.down('Control');await page.keyboard.down('Alt');await page.keyboard.down('u');
  await page.locator('[data-speech-recording="recording"]').waitFor();
  await page.waitForFunction(()=>[...document.querySelectorAll('[data-speech-wave] span')].some(node=>parseFloat(node.style.height)>3.01));
  await page.evaluate(()=>window.dispatchEvent(new Event('blur')));await idle();
  await page.keyboard.up('u');await page.keyboard.up('Alt');await page.keyboard.up('Control');
  const uploadCount=await page.evaluate(()=>window.fixture.uploads.length);
  await page.evaluate(()=>{window.holdMicrophonePermission=true;});
  await page.keyboard.down('Control');await page.keyboard.down('Alt');await page.keyboard.down('u');
  await page.waitForFunction(()=>!!window.releaseMicrophonePermission);
  await page.keyboard.up('u');await page.keyboard.up('Alt');await page.keyboard.up('Control');await idle();
  await page.evaluate(()=>{window.holdMicrophonePermission=false;window.releaseMicrophonePermission();});
  await page.waitForFunction(()=>window.voicePendingStream.getTracks().every(track=>track.readyState==='ended'));
  assert.equal(await page.evaluate(()=>window.fixture.uploads.length),uploadCount,'releasing before permission cannot upload');
  await microphone.click({button:'right'});
  await microphoneMenu.getByRole('switch',{name:'按住录音'}).click();
  await page.keyboard.press('Escape');
  await page.evaluate(() => { window.fixture.reset('设置变更保留'); window.fixture.hold=true; });
  await start();
  await page.getByRole('button', { name: '识别并发送' }).click();
  await page.waitForFunction(() => !!window.fixture.pending);
  await page.evaluate(() => window.fixture.disable());
  await page.locator('[data-composer-speech]').waitFor({ state: 'detached' });
  await page.evaluate(() => window.fixture.release());
  await page.locator('[data-speech-recording]').waitFor({ state: 'detached' });
  assert.equal(await editor.innerText(), '设置变更保留');
  assert.equal(await page.evaluate(() => window.fixture.sent.length), 2);
  assert.deepEqual(errors, []);
  console.log('[pass] autosave, device selection, editable shortcuts, hold/release/blur, late permission, stable layout, real WAV, stop/send/cancel, session isolation and tags');
  console.log('Artifacts: ' + output);
} catch (error) {
  console.error('Fixture state:', await page?.evaluate(() => ({
    sent: window.fixture?.sent, state: window.fixture?.state, content: window.fixture?.content(),
    uploads: window.fixture?.uploads, cancelled: window.fixture?.cancelled,
  })).catch(() => null));
  await page?.screenshot({ path: path.join(output, 'failure.png'), fullPage: true }).catch(() => {});
  console.error('Artifacts: ' + output, 'Page errors:', errors);
  throw error;
} finally {
  await browser?.close();
  await server.close();
}
