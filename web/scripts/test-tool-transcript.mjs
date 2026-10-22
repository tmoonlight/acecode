import assert from 'node:assert/strict';
import { mkdtemp } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { createServer, transformWithEsbuild } from 'vite';

const web = fileURLToPath(new URL('../', import.meta.url));
const { chromium } = await import(process.env.ACE_PLAYWRIGHT_MODULE
  ? pathToFileURL(path.resolve(process.env.ACE_PLAYWRIGHT_MODULE)).href : 'playwright');
const output = await mkdtemp(path.join(os.tmpdir(), 'ace-tool-transcript-'));
const entry = String.raw`
import React, {useState} from 'react';
import {createRoot} from 'react-dom/client';
import {ToolBlock} from '/src/components/ToolBlock.jsx';
import {applyLocalePreference} from '/src/i18n/index.js';
import {builtinTranscriptToolNames} from '/src/i18n/catalogs/toolTranscript.js';
import '/src/styles/globals.css';
const longCommand = 'Get-Content -LiteralPath "C:/project/a-very-long-directory-name/components/LeftSideBar/index.vue"\nWrite-Output complete';
const file = 'M:/AIGC/shz_test/web/src/components/LeftSideBar/index.vue';
const rows = [
 ['read', {tool:'file_read',args:{file_path:file},summary:{verb:'Read',object:file,metrics:[{label:'lines',value:200},{label:'size',value:'8.5KB'},{label:'enc',value:'utf-8'},{label:'eol',value:'lf'}]},output:'1: <template>\n2:   <div class="navigation-bar">\n3:   </div>\n4: </template>'}],
 ['grep', {tool:'grep',args:{pattern:'权限下发',path:'web/src',include_pattern:'*.vue',head_limit:50},output:'LeftSideBar/index.vue:86\n  // 权限下发'}],
 ['glob', {tool:'glob',args:{pattern:'**/*.vue',path:'web/src'},output:'LeftSideBar/index.vue\nHome.vue'}],
 ['bash', {tool:'bash',args:{command:'pnpm test'},summary:{verb:'Ran',object:'pnpm test',metrics:[{label:'time',value:'2.1s'},{label:'exit',value:0}]},output:'All tests passed\n'}],
 ['long', {tool:'bash',args:{command:longCommand},output:'complete'}],
 ['failed', {tool:'file_read',args:{file_path:file,max_bytes:15000},success:false,output:'[Error] Invalid parameter max_bytes'}],
 ['running', {tool:'grep',args:{pattern:'Permission',head_limit:80},isDone:false,totalLines:3,totalBytes:48,elapsed:1,tailLines:['permission.ts:3','menu.vue:14'],currentPartial:'checking...'}],
 ['metrics', {tool:'file_read',summary:{verb:'Read',metrics:[{label:'cache',value:'unchanged'}]}}],
 ['created', {tool:'file_write',args:{file_path:'/tmp/new.js',content:'const created = true;'},summary:{verb:'Created',object:'/tmp/new.js',metrics:[{label:'+',value:1}]},hunks:[{old_start:1,old_count:0,new_start:1,new_count:1,lines:[{kind:'added',text:'const created = true;',new_line_no:1}]}]}],
 ['edit', {tool:'file_edit',args:{file_path:'/tmp/edit.js',old_string:'old',new_string:'new'},summary:{verb:'Edited',object:'/tmp/edit.js',metrics:[{label:'+',value:1},{label:'-',value:1}]},hunks:[{old_start:1,old_count:1,new_start:1,new_count:1,lines:[{kind:'removed',text:'old',old_line_no:1},{kind:'added',text:'new',new_line_no:1}]}]}],
 ['question', {tool:'AskUserQuestion',askUserQuestionResult:{items:[{question:'使用哪种语言？',answer:'中文'}]}}],
];
function App() {
 const [inventory,setInventory] = useState(false);
 window.fixture = {locale:applyLocalePreference,inventory:setInventory,longCommand,file};
 const displayed = inventory ? builtinTranscriptToolNames.map(tool=>[tool,{tool}]) : rows;
 return <main style={{padding:20,minHeight:'100vh'}} className="bg-bg text-fg" data-inventory={String(inventory)}>
  {displayed.map(([id,entry])=><section data-case={id} key={id} style={{marginBottom:8}}>
   <ToolBlock entry={{isDone:true,success:true,...entry}} sessionRunning={false}/>
  </section>)}
 </main>;
}
Object.defineProperty(navigator,'clipboard',{configurable:true,value:{writeText:async text=>{window.copied=text;}}});
createRoot(document.getElementById('root')).render(<App/>);
`;
const html = '<!doctype html><html><head><meta charset="UTF-8"/></head><body><div id="root"></div><script type="module" src="/@id/virtual:tool-transcript"></script></body></html>';
const server = await createServer({
  root: web, configFile: path.join(web, 'vite.config.js'), logLevel: 'error',
  server: { host: '127.0.0.1', port: 0 },
  plugins: [{
    name: 'tool-transcript-fixture',
    resolveId(id) { if (id === 'virtual:tool-transcript') return '\0virtual:tool-transcript'; },
    async load(id) { if (id === '\0virtual:tool-transcript') return (await transformWithEsbuild(entry, 'fixture.jsx', { loader: 'jsx' })).code; },
    configureServer(vite) {
      vite.middlewares.use('/tool-transcript', async (_req,res) => {
        res.setHeader('Content-Type','text/html');
        res.end(await vite.transformIndexHtml('/tool-transcript',html));
      });
    },
  }],
});
const errors = [];
let browser;
try {
  await server.listen();
  browser = await chromium.launch({ headless:true, executablePath:process.env.ACE_CHROMIUM_EXECUTABLE || undefined });
  for (const [width,theme] of [[1280,'light'],[390,'dark']]) {
    const page = await browser.newPage({viewport:{width,height:1000}});
    page.on('pageerror',error=>errors.push(error.message));
    await page.goto(`http://127.0.0.1:${server.httpServer.address().port}/tool-transcript`);
    await page.waitForFunction(()=>!!window.fixture);
    await page.evaluate(theme=>document.documentElement.dataset.theme=theme,theme);
    const row = id=>page.locator(`[data-case="${id}"]`);
    const head = id=>row(id).locator('[data-unified-activity-line] > [data-activity-title-anchor]');
    for (const locale of ['zh-CN','en-US']) {
      await page.evaluate(locale=>window.fixture.locale(locale),locale);
      await page.waitForFunction(locale=>document.querySelector('[data-case="read"] .ace-activity-line-label').textContent.includes(locale==='zh-CN'?'读取文件':'Read File'),locale);
      assert.match(await head('read').innerText(),locale==='zh-CN'?/读取文件：\s*index.vue/:/Read File:\s*index.vue/);
      assert.doesNotMatch(await head('read').innerText(),/M:|lines=|utf-8|200/);
      assert.doesNotMatch(await head('grep').innerText(),/50|\.vue/);
      assert.doesNotMatch(await head('running').innerText(),/48|80|秒|1s/);
      await page.evaluate(()=>document.activeElement?.blur()); await page.mouse.move(0,0);
      await page.waitForFunction(()=>getComputedStyle(document.querySelector('[data-case="read"] .ace-activity-line-chevron')).opacity==='0');
      await head('read').hover();
      await page.waitForFunction(()=>getComputedStyle(document.querySelector('[data-case="read"] .ace-activity-line-chevron')).opacity==='1');
      for (const id of ['read','grep','glob','bash','long','failed','running','metrics','created','edit']) {
        if (await head(id).getAttribute('aria-expanded') !== 'true') await head(id).click();
      }
      assert.match(await row('read').innerText(),/M:\/AIGC/);
      assert.match(await row('read').innerText(),locale==='zh-CN'?/读取行数: 200/:/Lines Read: 200/);
      assert.match(await row('grep').innerText(),locale==='zh-CN'?/最多 50 条/:/Up to 50 results/);
      assert.equal((await row('bash').innerText()).match(/pnpm test/g)?.length,1);
      assert.equal(await row('bash').locator('.ace-shell-card-header').count(),0);
      assert.equal(await row('bash').locator('.ace-shell-card-command').count(),0);
      assert.equal(await row('long').locator('.ace-shell-card-command').count(),1);
      assert.equal(await row('long').locator('.ace-shell-card-command').innerText(),'$ '+await page.evaluate(()=>window.fixture.longCommand));
      await row('bash').locator('[data-code-copy-button]').click();
      assert.equal(await page.evaluate(()=>window.copied),'$ pnpm test\n\nAll tests passed\n');
      await row('long').locator('[data-code-copy-button]').click();
      assert.equal(await page.evaluate(()=>window.copied),await page.evaluate(()=>'$ '+window.fixture.longCommand+'\n\ncomplete'));
      assert.match(await row('failed').innerText(),/Invalid parameter max_bytes/);
      assert.match(await row('metrics').innerText(),locale==='zh-CN'?/未变化/:/Unchanged/);
      assert.equal(await row('created').locator('[data-created-file-source]').count(),1);
      assert.equal(await row('edit').locator('.d2h-wrapper').count(),1);
      assert.equal(await row('edit').locator('[data-tool-detail-metadata]').count(),0);
      assert.equal(await row('question').locator('.ace-qa-answer').innerText(),'中文');
      await page.evaluate(()=>document.activeElement?.blur()); await page.mouse.move(0,0);
      await page.waitForFunction(()=>getComputedStyle(document.querySelector('[data-case="read"] .ace-activity-line-chevron')).opacity==='0');
      await head('read').focus();
      await page.waitForFunction(()=>getComputedStyle(document.querySelector('[data-case="read"] .ace-activity-line-chevron')).opacity==='1');
      await page.keyboard.press('Enter'); assert.equal(await head('read').getAttribute('aria-expanded'),'false');
      await page.keyboard.press('Space'); assert.equal(await head('read').getAttribute('aria-expanded'),'true');
      await page.evaluate(()=>document.activeElement?.blur()); await page.mouse.move(0,0);
      await page.waitForFunction(()=>getComputedStyle(document.querySelector('[data-case="read"] .ace-activity-line-chevron')).opacity==='0');
      assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth>innerWidth+1),false);
      await page.screenshot({path:path.join(output,`${width}-${theme}-${locale}.png`),fullPage:true});
    }
    // Resizing must reveal the full command once when the actual row clips it.
    await page.setViewportSize({width:230,height:1000});
    await page.waitForFunction(()=>!!document.querySelector('[data-case="bash"] .ace-shell-card-command'));
    await page.setViewportSize({width:1280,height:1000});
    await page.waitForFunction(()=>!document.querySelector('[data-case="bash"] .ace-shell-card-command'));
    await page.evaluate(()=>window.fixture.inventory(true));
    await page.waitForFunction(()=>document.querySelector('main').dataset.inventory==='true');
    assert.ok(await page.locator('.ace-activity-line-label').count()>=81);
    assert.doesNotMatch(await page.locator('main').innerText(),/toolTranscript\./);
    await page.close();
  }
  assert.deepEqual(errors,[]);
  console.log(JSON.stringify({passed:4,output,checks:['native-inventory','locale-switch','basename','metadata','shell-no-duplicates','copy','history-unit-tests','hover-focus','narrow-layout','resize']}));
} finally {
  await browser?.close();
  await server.close();
}
