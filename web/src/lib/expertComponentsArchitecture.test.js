import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const source = (relativePath) => fs.readFileSync(path.join(srcRoot, relativePath), 'utf8');

function test(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

test('expert components page uses local CRUD and shared immediate Modal', () => {
  const page = source('components/ExpertComponentsPage.jsx');
  assert.match(page, /data-expert-components-page="true"/);
  assert.match(page, /api\.listExperts/);
  assert.match(page, /api\.createExpert/);
  assert.match(page, /api\.updateExpert/);
  assert.match(page, /api\.deleteExpert/);
  assert.match(page, /<Modal onClose=/);
  assert.match(page, /data-expert-delete-dialog="true"/);
  assert.doesNotMatch(page, /window\.confirm|window\.alert/);
});

test('expert teams reuse the expert gallery and keep technical details out of the UI', () => {
  const page = source('components/ExpertComponentsPage.jsx');
  assert.match(page, />\s*新建专家\s*</);
  assert.match(page, />\s*组建专家团\s*</);
  assert.match(page, /data-team-expert-picker="true"/);
  assert.match(page, /和其他专家组团/);
  assert.match(page, /返回专家团/);
  assert.match(page, /设为主理人/);
  assert.doesNotMatch(page, /添加成员|updateMember|成员指令/);
  assert.doesNotMatch(page, /Skills|MCP|skill_roots|package_root|expert\.json|font-mono/);
});

test('all composers dispatch recent experts through the plus menu and switch active sessions in place', () => {
  const chat = source('components/ChatView.jsx');
  const input = source('components/InputBar.jsx');
  const controls = source('components/ComposerSessionControls.jsx');
  const api = source('lib/api.js');
  assert.match(chat, /expert_id: homeExpertId/);
  assert.match(chat, /api\.setSessionExpert\(sid, expertId\)/);
  assert.match(chat, /setSessionExpertId\(expertId\)/);
  assert.match(chat, /onSessionExpertChanged\?\.\(sid, confirmedExpert\)/);
  assert.match(chat, /setSessionExpertId\(previousId\)/);
  assert.doesNotMatch(chat, /onDispatchExpert/);
  assert.match(api, /setSessionExpert:\s+\(id, expertId\)/);
  assert.match(api, /`\/api\/sessions\/\$\{encodeURIComponent\(id\)\}\/expert`/);
  assert.equal((chat.match(/onOpenExpertComponents=\{onOpenExpertComponents\}/g) || []).length, 2);
  assert.match(input, /data-expert-components-submenu="true"/);
  assert.match(input, /expertOptions\) \? expertOptions\.slice\(0, 5\)/);
  assert.match(input, /grid-cols-\[16px_minmax\(88px,148px\)_minmax\(0,1fr\)\]/);
  const expertEntry = input.indexOf('>专家组件</span>');
  const separator = input.indexOf('className="my-1 border-t border-border"', expertEntry);
  const fileEntry = input.indexOf('>文件或文件夹</span>', separator);
  assert.ok(expertEntry >= 0 && separator > expertEntry && fileEntry > separator);
  assert.match(input, /<VsIcon name="extension" size=\{15\} className="shrink-0" \/>\s+<span>更多专家<\/span>/);
  assert.match(input, />更多专家</);
  assert.match(input, />文件或文件夹</);
  assert.doesNotMatch(input, /暂无最近使用的专家/);
  assert.doesNotMatch(input, /onAddBrowserContext|>浏览器</);
  assert.match(input, /expertId=\{selectedExpertId\}/);
  assert.match(input, /expertName=\{selectedExpertName\}/);
  assert.match(controls, /data-composer-control="expert"/);
  assert.match(controls, /role="status"/);
  assert.match(controls, /当前专家组件：\$\{expertName\}/);
  assert.doesNotMatch(controls, /expertLocked|onExpertChange|openMenu === 'expert'/);
});

test('app routes expert entry to page and use action back to a new task', () => {
  const app = source('App.jsx');
  const page = source('components/ExpertComponentsPage.jsx');
  assert.match(app, /expertComponents: true/);
  assert.match(app, /<ExpertComponentsPage/);
  assert.match(app, /recordRecentExpert\(current, id\)/);
  assert.match(app, /rememberRecentExpert\(expert\)/);
  assert.match(app, /expertId: expert\?\.id \|\| ''/);
  assert.match(app, /onSessionExpertChanged=\{replaceActiveSessionExpert\}/);
  assert.match(app, /replaceActiveRef\(\(current\) =>/);
  assert.doesNotMatch(app, /onDispatchExpert=/);
  assert.match(app, /!activeRef\?\.expertComponents/);
  assert.match(page, />派遣</);
  assert.doesNotMatch(page, />使用</);
});
