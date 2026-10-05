import assert from 'node:assert/strict';
import { recentOffices, officeActor, projectDesktopOffice, stableOfficeSeed } from './desktopOfficeState.js';
import { createDesktopOfficeController } from './desktopOfficeController.js';
import { toolIconName, toolIconSvg } from './toolIcons.js';
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

function fixture(fetchSnapshot) {
  const publications=[],retained=new Set(),released=[],timers=new Map();let timerId=0;
  const controller=createDesktopOfficeController({fetchSnapshot,publish:value=>publications.push(value),
    retainSession:id=>retained.add(id),releaseSession:id=>{retained.delete(id);released.push(id);},
    schedule:fn=>{timers.set(++timerId,fn);return timerId;},cancel:id=>timers.delete(id)});
  return {controller,publications,retained,released,timers};
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
