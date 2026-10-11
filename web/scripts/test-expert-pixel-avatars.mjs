import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { createServer } from 'vite';

const web = fileURLToPath(new URL('../', import.meta.url));
const root = path.dirname(web.replace(/[\\/]$/, ''));
const modulePath = process.env.ACE_PLAYWRIGHT_MODULE;
const { chromium } = await import(modulePath ? pathToFileURL(path.resolve(modulePath)).href : 'playwright');
const output = await fs.mkdtemp(path.join(os.tmpdir(), 'ace-pixel-avatars-'));
const shared = await fs.readFile(path.join(root, 'assets/desktop_pet/office_characters.js'), 'utf8');
const office = (await fs.readFile(path.join(root, 'assets/desktop_pet/agent_office_pet.html'), 'utf8'))
  .replace('<script src="./office_characters.js"></script>', `<script>${shared}</script>`);
const experts = [];
for (const id of await fs.readdir(path.join(root, 'assets/seed/experts'))) {
  const manifest = JSON.parse(await fs.readFile(path.join(root, 'assets/seed/experts', id, 'expert.json'), 'utf8'));
  experts.push({ id, display_name: manifest.displayName, type: manifest.expertType,
    references_existing_experts: !!manifest.teamInfo, lead_expert_id: manifest.teamInfo?.leadExpert });
}
const fixture = `
import React from 'react';
import {createRoot} from 'react-dom/client';
import {ExpertAvatar} from '/src/components/ExpertCatalog.jsx';
import {SubagentGroupBlock} from '/src/components/SubagentGroupBlock.jsx';
import {expertAvatarIdentity,stableLook,stableSeed} from '/src/lib/officeCharacters.js';
import '/src/styles/globals.css';
const experts=${JSON.stringify(experts)};
const member=experts.find(e=>e.id==='opc-value-designer');
const identity=expertAvatarIdentity(member);
window.avatarIdentity=identity;
window.expectedLook=stableLook(stableSeed(identity));
const task={id:'child',title:member.display_name,status:'running',expert:{avatar_identity:identity}};
createRoot(document.getElementById('root')).render(<div className="p-5 text-fg bg-bg">
  <div className="flex flex-wrap gap-4">{experts.map(e=><div key={e.id} className="flex flex-col items-center gap-1 text-xs">
    <ExpertAvatar expert={e}/><span>{e.display_name}</span></div>)}</div>
  <div className="mt-4"><SubagentGroupBlock agents={[{sessionId:'child',done:false}]}
    tasksById={new Map([['child',task]])} messageAutoCollapse={false}/></div>
  <div id="custom"><ExpertAvatar expert={{id:'custom',avatar_url:'/acecode-logo.png'}}/></div>
  <iframe title="office" srcDoc={${JSON.stringify(office)}} style={{width:688,maxWidth:'100%',height:504,border:0}}/>
</div>);
`;
const virtual = path.join(web, '__pixel-avatar-fixture.jsx').replaceAll('\\', '/');
const server = await createServer({ root: web,
  server: {host:'127.0.0.1',port:0,open:false,hmr:false},
  plugins:[{name:'pixel-avatar-fixture',enforce:'pre',
    resolveId(id){return id==='/__pixel-avatar-fixture.jsx'?virtual:undefined;},
    load(id){return id.replaceAll('\\','/')===virtual?fixture:undefined;},
    configureServer(vite){vite.middlewares.use(async(req,res,next)=>{
      if(req.url!=='/avatar-fixture')return next();
      res.setHeader('Content-Type','text/html');
      res.end(await vite.transformIndexHtml('/avatar-fixture', '<!doctype html><html data-theme="light"><meta name="viewport" content="width=device-width, initial-scale=1"><body><div id="root"></div><script type="module" src="/__pixel-avatar-fixture.jsx"></script></body></html>'));
    });}}],
});
let browser;
const errors=[];
try {
  await server.listen();
  browser=await chromium.launch({channel:'msedge',headless:true});
  const page=await browser.newPage({viewport:{width:1100,height:850}});
  page.on('pageerror',error=>errors.push(error.message));
  await page.goto(`http://127.0.0.1:${server.httpServer.address().port}/avatar-fixture`);
  await page.waitForFunction(()=>document.querySelectorAll('[data-pixel-avatar]').length===11);
  await page.waitForFunction(()=>[...document.querySelectorAll('[data-pixel-avatar]')].every(canvas=>
    canvas.getContext('2d').getImageData(0,0,14,14).data.some((value,index)=>index%4===3&&value>0)));
  const identity=await page.evaluate(()=>window.avatarIdentity);
  const pair=page.locator(`[data-pixel-avatar="${identity}"]`);
  assert.equal(await pair.count(),2);
  const pixels=await pair.evaluateAll(nodes=>nodes.map(c=>c.toDataURL()));
  assert.equal(pixels[0],pixels[1], 'catalog and live child use identical portrait pixels');
  assert.equal(await page.locator('#custom img').getAttribute('src'),'/acecode-logo.png');
  assert.equal(await page.locator('#custom canvas').count(),0);
  const frame=page.frames().find(f=>f!==page.mainFrame());
  await frame.waitForFunction(()=>!!window.AgentOffice);
  const seed=await page.evaluate(()=>globalThis.ACEOfficeCharacters.stableSeed(window.avatarIdentity));
  await frame.evaluate(({identity,seed})=>AgentOffice.applySnapshot({version:1,connected:true,complete:true,offices:[],selected:{sessionId:'root'},agents:[
    {id:'root',root:true,name:'主理人',seed,avatarIdentity:identity,state:'think',label:'思考中',transfers:[]},
    {id:'child',root:false,name:'言之',seed,avatarIdentity:identity,state:'work',label:'工作中',transfers:[]},
  ]}),{identity,seed});
  const looks=await frame.evaluate(()=>AgentOffice.agents.map(a=>a.look));
  assert.equal(looks.length,2);
  assert.equal(looks[0],looks[1], 'expert-bound leader and worker use the same shared appearance');
  const expected=await page.evaluate(()=>{const l=window.expectedLook;return [l.hair,l.skin,l.shirt,l.pants,l.acc.join('+'),l.accColor].join('|');});
  assert.equal(looks[0],expected);
  for(const [theme,width] of [['light',1100],['dark',520]]){
    await page.setViewportSize({width,height:850});
    await page.evaluate(value=>document.documentElement.dataset.theme=value,theme);
    await page.waitForTimeout(300); // Finish the application's theme color transition.
    assert.ok(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth));
    await page.screenshot({path:path.join(output,`pixel-avatars-${theme}.png`)});
  }
  assert.deepEqual(errors,[]);
  console.log(JSON.stringify({output,checks:9,errors}));
} finally {await browser?.close();await server.close();}
