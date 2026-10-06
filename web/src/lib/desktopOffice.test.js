import assert from 'node:assert/strict';
import { recentOffices, officeActor, projectDesktopOffice, stableOfficeSeed, fitTail, speechTail } from './desktopOfficeState.js';
import { createDesktopOfficeController, OFFICE_LIVE_PUBLISH_MS } from './desktopOfficeController.js';
import { toolActivityVerb, toolIconName, toolIconSvg } from './toolIcons.js';
import { interfaceIconSvg } from './interfaceIcons.js';

function test(name, fn) {fn(); console.log(`[pass] desktop office: ${name}`);}
async function check(name, fn) {await fn(); console.log(`[pass] desktop office: ${name}`);}
const session = (id, overrides = {}) => ({id, active:true, last_user_message_at:'2026-10-06T10:00:00Z', ...overrides});
const snapshot = (id, overrides = {}) => ({selected:session(id),offices:[session(id)],agents:[session(id)],complete:true,...overrides});

test('recent five depend only on human send time across workspaces', () => {
  const rows = Array.from({length:7}, (_,i) => session(String(i),{workspace_hash:`w${i%2}`,last_user_message_at:`2026-10-06T10:00:0${i}Z`,updated_at:`2026-10-06T20:00:0${9-i}Z`}));
  rows.push(session('archived',{archived:true,last_user_message_at:'2099-01-01'}));
  rows.push(session('child',{parent_session_id:'6',last_user_message_at:'2099-01-01'}));
  rows.push(session('empty',{last_user_message_at:''}));
  assert.deepEqual(recentOffices(rows).map(s=>s.id),['6','5','4','3','2']);
});

test('main sleeps on completion and wakes; workers leave on completion', () => {
  assert.equal(officeActor(session('root',{last_turn_outcome:'completed'}),'root').state,'sleep');
  assert.equal(officeActor(session('root',{busy:true,last_turn_outcome:'completed'}),'root').state,'think');
  assert.equal(officeActor(session('child',{last_turn_outcome:'completed'}),'root').present,false);
  assert.equal(officeActor(session('child',{busy:true,last_turn_outcome:'completed'}),'root').present,true);
  assert.equal(officeActor(session('root'),'root').state,'idle');
});

test('waiting, retry, cancellation and errors are distinct', () => {
  for(const [phase,expected] of [['permission_waiting','permission'],['question_waiting','question'],['compacting','compact'],['model_retry','retry']]) {
    assert.equal(officeActor(session('root',{busy:true,activity:{phase}}),'root').state,expected);
  }
  assert.equal(officeActor(session('root',{last_turn_outcome:'aborted'}),'root').state,'stopped');
  assert.equal(officeActor(session('root',{last_turn_outcome:'error'}),'root').state,'error');
});

test('failed mesh workers with internal input stay visible for the current root turn', () => {
  const child=session('mesh',{last_user_message_at:'',updated_at:'2026-10-06T10:01:00Z',last_turn_outcome:'error'});
  assert.equal(projectDesktopOffice(snapshot('root',{agents:[session('root'),child]})).agents.length,2);
  const newer=session('root',{last_user_message_at:'2026-10-06T10:02:00Z'});
  assert.equal(projectDesktopOffice({...snapshot('root'),selected:newer,agents:[newer,child]}).agents.length,1);
});

test('paper thickness follows current context rather than lifetime totals', () => {
  const base=session('root',{context_window:1000,token_usage:{has_data:true,prompt_tokens:900},session_token_usage:{total_tokens:9000000}});
  const full=officeActor(base,'root');
  assert.equal(full.contextRatio,.9);
  const compacted=officeActor({...base,token_usage:{has_data:true,prompt_tokens:150}},'root');
  assert.equal(compacted.contextRatio,.15);
  assert.equal(officeActor(session('root'),'root').contextKnown,false);
});

test('stable identity and nested mesh children belong to one office with overflow', () => {
  const agents=[session('root'),...Array.from({length:9},(_,i)=>session(`child${i}`,{parent_session_id:'root',agent_path:`/root/a/child${i}`,busy:true}))];
  const view=projectDesktopOffice(snapshot('root',{agents}));
  assert.equal(view.agents.length,10);assert.equal(view.overflow,2);
  assert.equal(view.agents[0].name,'Maestro');
  assert.equal(stableOfficeSeed('root'),stableOfficeSeed('root'));
  assert.notEqual(stableOfficeSeed('root'),stableOfficeSeed('another'));
  assert.equal(view.agents.find(a=>a.id==='child0').seed,officeActor(agents[1],'root').seed);
});

test('tool SVG is exactly the shared chat icon source', () => {
  for(const tool of ['file_read','file_write','file_edit','apply_patch','grep','glob','web_search','bash','spawn_subagent','agent_send_message','AskUserQuestion','TodoWrite','task_complete','vision_analyze','skill_view','mcp__server__tool','unknown']) {
    const icon=toolIconName(tool);
    assert.equal(toolIconSvg(tool),interfaceIconSvg(icon,14));
    assert.match(toolIconSvg(tool),/^<svg/);
  }
  assert.equal(toolIconName('file_read'),'OpenFile');
  assert.equal(toolIconName('mcp__server__tool'),'MCP');
});

// 正在输出正文:显示「撰写回复」与最新的几个字,Markdown 标点不进气泡。
test('streaming reply types and shows only its latest words', () => {
  const actor=officeActor(session('root',{busy:true,activity:{phase:'responding',text:'第一段\n\n## 小标题\n**最后**几个字是这些'}}),'root');
  assert.equal(actor.state,'type');assert.equal(actor.label,'撰写回复');
  assert.ok(actor.detail.endsWith('最后几个字是这些'),actor.detail);
  assert.ok(!/[#*]/.test(actor.detail),actor.detail);
  assert.equal(actor.tool,'');
});

// 工具执行:动词 + 调用预览(长路径保留文件名一端);等成员回报单独一种状态。
test('tool work names the verb and keeps the end of long previews', () => {
  const read=officeActor(session('root',{busy:true,activity:{phase:'tool_running',tool:'file_read',detail:'src/apps/desktop/some/very/deep/folder/desktop_pet_mac.mm'}}),'root');
  assert.equal(read.state,'work');assert.equal(read.label,'读取');assert.equal(read.icon,'OpenFile');
  assert.ok(read.detail.startsWith('…')&&read.detail.endsWith('desktop_pet_mac.mm'),read.detail);
  assert.equal(officeActor(session('root',{busy:true,activity:{tool:'wait_subagent'}}),'root').state,'wait');
  assert.equal(officeActor(session('root',{busy:true,activity:{tool:'agent_wait'}}),'root').label,'等成员回报');
  const planning=officeActor(session('root',{busy:true,activity:{phase:'tool_planning',tool:'file_write'}}),'root');
  assert.equal(planning.state,'type');assert.equal(planning.label,'准备写入文件');assert.equal(planning.icon,'Save');
  assert.equal(toolActivityVerb('bash'),'运行命令');assert.equal(toolActivityVerb('mcp__x__y'),'调用 MCP');
});

// 「适合日常工作」模式的具体进度文案优先;引擎默认的笼统文案不进气泡。
test('concrete progress titles win over generic engine labels', () => {
  assert.equal(officeActor(session('root',{busy:true,activity:{phase:'model_waiting',label:'正在等待模型响应'}}),'root').label,'思考中');
  assert.equal(officeActor(session('root',{busy:true,activity:{phase:'tool_running',tool:'bash',label:'正在调用工具 bash'}}),'root').label,'运行命令');
  assert.equal(officeActor(session('root',{busy:true,activity:{phase:'tool_running',tool:'grep',label:'正在搜索代码并读取'}}),'root').label,'搜索代码并读取');
  assert.equal(officeActor(session('root',{busy:true,activity:{phase:'reasoning',label:'正在推理',text:'check the config'}}),'root').detail,'check the config');
});

// 气泡宽度按半角单位计:汉字算 2,超出时保留末尾并以省略号开头。
test('bubble text keeps the tail within its width budget', () => {
  assert.equal(fitTail('abcdef',4),'…cdef');
  assert.equal(fitTail('一二三四五',6),'…三四五');
  assert.equal(fitTail('  short  ',28),'short');
  assert.equal(speechTail('see [docs](http://x.test) `code`'),'see docs code');
});

// 子会话路径:/root/a/b 的上级是 /root/a,主会话没有上级。
test('mesh workers know their dispatching parent', () => {
  assert.equal(officeActor(session('c',{agent_path:'/root/a/b'}),'root').parentPath,'/root/a');
  assert.equal(officeActor(session('root'),'root').parentPath,'');
});

function fixture(fetchSnapshot) {
  const publications=[],retained=new Set(),released=[],timers=new Map(),delays=new Map();let timerId=0;
  const controller=createDesktopOfficeController({fetchSnapshot,publish:value=>publications.push(value),
    retainSession:id=>retained.add(id),releaseSession:id=>{retained.delete(id);released.push(id);},
    schedule:(fn,delay)=>{timers.set(++timerId,fn);delays.set(timerId,delay);return timerId;},cancel:id=>timers.delete(id)});
  // 只触发指定延迟的计时器(实时文字节流 vs 快照轮询),其余保持挂起。
  const fire=delay=>{for(const [id,fn] of [...timers]) if(delays.get(id)===delay){timers.delete(id);fn();}};
  return {controller,publications,retained,released,timers,fire};
}
const deferred=()=>{let resolve,reject;const promise=new Promise((a,b)=>{resolve=a;reject=b;});return {promise,resolve,reject};};

await check('manual choice turns follow off and keeps office fixed', async () => {
  const requests=[];const f=fixture(async ref=>{requests.push(ref.sessionId);return snapshot(ref.sessionId||'recent');});
  f.controller.setActive({sessionId:'a'});await f.controller.refresh();
  assert.equal(f.controller.state.follow,true);assert.equal(f.controller.state.selected.sessionId,'a');
  f.controller.select({session_id:'b'});await f.controller.refresh();
  f.controller.setActive({sessionId:'c'});await f.controller.refresh();
  assert.equal(f.controller.state.selected.sessionId,'b');assert.equal(f.controller.state.follow,false);
  f.controller.setFollow(true);await f.controller.refresh();
  assert.equal(f.controller.state.selected.sessionId,'c');f.controller.dispose();
  assert.equal(f.retained.size,0);assert.equal(f.timers.size,0);
});

await check('late responses cannot replace the new office or leak subscriptions', async () => {
  const a=deferred(),b=deferred();const f=fixture(ref=>ref.sessionId==='a'?a.promise:b.promise);
  f.controller.setActive({sessionId:'a'});const old=f.controller.refresh();
  f.controller.select({sessionId:'b'});const next=f.controller.refresh();
  b.resolve(snapshot('b'));await next;a.resolve(snapshot('a'));await old;
  assert.equal(f.controller.state.selected.sessionId,'b');assert.deepEqual([...f.retained],['b']);
  f.controller.dispose();assert.equal(f.retained.size,0);
});

await check('disposing with an in-flight response prevents further publication', async () => {
  const pending=deferred();const f=fixture(()=>pending.promise);
  const request=f.controller.refresh();f.controller.dispose();const count=f.publications.length;
  pending.resolve(snapshot('late'));await request;assert.equal(f.publications.length,count);
  assert.equal(f.retained.size,0);
});

await check('offline is explicit and reconnect recovers from a fresh snapshot', async () => {
  let fail=false;const f=fixture(async()=>{if(fail)throw new Error('offline');return snapshot('root');});
  await f.controller.refresh();fail=true;await f.controller.refresh();assert.equal(f.controller.state.connected,false);
  fail=false;const releases=f.released.length;f.controller.reconnected();assert.equal(f.released.length,releases);await f.controller.refresh();assert.equal(f.controller.state.connected,true);
  assert.equal(f.controller.state.selected.sessionId,'root');f.controller.dispose();
});

await check('completion releases a child while the selected office stays subscribed', async () => {
  let done=false;const f=fixture(async()=>snapshot('root',{agents:[session('root'),session('child',{busy:!done,last_turn_outcome:done?'completed':''})]}));
  await f.controller.refresh();assert.equal(f.retained.has('child'),true);
  done=true;await f.controller.refresh();assert.equal(f.retained.has('child'),false);
  assert.equal(f.controller.state.agents.some(a=>a.id==='child'),false);
  assert.equal(f.retained.has('root'),true);f.controller.dispose();
});

// 流式 token 走 WebSocket 本地叠加:不发额外快照请求,按节流间隔发布;
// 只认办公室里的成员;快照追上(seq 不再落后)后叠加层让位给服务端状态。
await check('streamed tokens update the speaker between snapshots without extra requests', async () => {
  let requests=0,serverActivity={seq:5,busy:true,phase:'model_waiting'};
  const f=fixture(async()=>{requests++;return snapshot('root',{agents:[session('root',{busy:true,activity:serverActivity})]});});
  await f.controller.refresh();requests=0;const published=f.publications.length;
  f.controller.onEvent({type:'token',session_id:'root',seq:6,payload:{text:'你好'}});
  f.controller.onEvent({type:'token',session_id:'root',seq:7,payload:{text:'世界'}});
  f.controller.onEvent({type:'token',session_id:'outsider',seq:8,payload:{text:'别人'}});
  assert.equal(f.publications.length,published,'节流期间不逐 token 发布');
  f.fire(OFFICE_LIVE_PUBLISH_MS);
  assert.equal(f.publications.length,published+1);
  let root=f.publications.at(-1).agents[0];
  assert.equal(root.state,'type');assert.equal(root.detail,'你好世界');assert.equal(requests,0);
  // 服务端快照已包含到 seq 7 的正文:以服务端为准,本地叠加被丢弃。
  serverActivity={seq:7,busy:true,phase:'responding',text:'你好世界'};
  await f.controller.refresh();
  root=f.controller.state.agents[0];assert.equal(root.detail,'你好世界');
  // 叠加层被清掉后,服务端进入工具阶段不会再被旧的正文覆盖。
  serverActivity={seq:9,busy:true,phase:'tool_running',tool:'bash',detail:'npm test'};
  await f.controller.refresh();
  root=f.controller.state.agents[0];assert.equal(root.state,'work');assert.equal(root.detail,'npm test');
  // 快照之后又来的 token 接在服务端尾巴后面(同一段正文)。
  serverActivity={seq:10,busy:true,phase:'responding',text:'前半'};
  await f.controller.refresh();
  f.controller.onEvent({type:'token',session_id:'root',seq:11,payload:{text:'后半'}});
  assert.equal(f.controller.state.agents[0].detail,'前半后半');
  f.controller.dispose();assert.equal(f.timers.size,0);
});
