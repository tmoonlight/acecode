import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const source = (relativePath) => fs.readFileSync(path.join(srcRoot, relativePath), 'utf8');

function between(text, start, end) {
  const from = text.indexOf(start);
  const to = text.indexOf(end, from + start.length);
  assert.notEqual(from, -1, `missing start marker: ${start}`);
  assert.notEqual(to, -1, `missing end marker: ${end}`);
  return text.slice(from, to);
}

function test(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

test('standalone expert page owns management and explicit conversation targeting without a composer', () => {
  const page = source('components/ExpertComponentsPage.jsx');
  const editor = source('components/ExpertEditor.jsx');
  const catalog = source('components/ExpertCatalog.jsx');
  assert.match(page, /data-expert-components-page="true"/);
  assert.match(catalog, /api\.listExperts/);
  assert.match(editor, /api\.createExpert/);
  assert.match(editor, /api\.updateExpert/);
  assert.match(page, /api\.deleteExpert/);
  assert.match(page, /data-expert-target-conversation="true"/);
  assert.match(page, /api\.setSessionExpert/);
  assert.match(page, /\{ draftText: target\.prompt \}/);
  assert.doesNotMatch(page, /api\.setSessionDraft/);
  assert.doesNotMatch(page, /<InputBar|data-composer|模拟聊天|悬浮输入/);
  assert.doesNotMatch(page, /window\.confirm|window\.alert/);
});

test('catalog uses type tabs plus dynamic non-exclusive Tags and cards show expertise only', () => {
  const catalog = source('components/ExpertCatalog.jsx');
  const card = between(catalog, 'function ExpertCard', 'function DetailSection');
  assert.match(catalog, /EXPERT_PRIMARY_TABS/);
  assert.match(catalog, /collectExpertTags/);
  assert.match(catalog, /overflow-x-auto/);
  assert.match(catalog, /sortExperts/);
  assert.match(catalog, /搜索名称、作者、Tag 或擅长领域/);
  assert.match(card, /expert\.expertise\.slice\(0, 3\)/);
  assert.doesNotMatch(card, /quick_prompts/);
  assert.match(card, /<button[\s\S]*aria-label=\{detailLabel\}/);
  assert.doesNotMatch(card, /<article[\s\S]{0,160}role="button"/);
  assert.match(card, /event\.stopPropagation\(\)/);
  assert.match(card, /dispatching \? '派遣中…' : '派遣'/);
});

test('detail keeps opening prompts separate and uses accessible, focus-restoring Modal', () => {
  const catalog = source('components/ExpertCatalog.jsx');
  const modal = source('components/Modal.jsx');
  assert.match(catalog, /data-expert-detail="true"/);
  assert.match(catalog, /expert\.quick_prompts\.map/);
  assert.match(catalog, /不会自动发送/);
  assert.match(catalog, /派遣 \$\{expert\.display_name\}/);
  assert.match(modal, /aria-modal="true"/);
  assert.match(modal, /event\.key !== 'Tab'/);
  assert.match(modal, /data-ace-modal-dialog="true"/);
  assert.match(modal, /modalDialogs\[modalDialogs\.length - 1\] !== dialog/);
  assert.match(modal, /previouslyFocused\?\.focus/);
});

test('editor provides basic, advanced, and inline team-member workflows backed by runtime APIs', () => {
  const editor = source('components/ExpertEditor.jsx');
  const api = source('lib/api.js');
  assert.match(editor, /基础信息/);
  assert.match(editor, /高级功能/);
  assert.match(editor, /擅长领域/);
  assert.match(editor, /开场白/);
  assert.match(editor, /TagEditor/);
  assert.match(editor, /api\.listExpertCapabilities/);
  assert.match(editor, /title="Skill"/);
  assert.match(editor, /title="MCP"/);
  assert.match(editor, /ACECode 本地工具/);
  assert.match(editor, /capabilitySourceLabel/);
  assert.match(editor, /已在全局设置中禁用/);
  assert.match(editor, /<Toggle/);
  assert.match(editor, /ariaLabel=\{`\$\{checked \? '关闭' : '开启'\}本地工具 \$\{option\.label\}`\}/);
  assert.match(editor, /data-team-expert-picker="true"/);
  assert.match(editor, /aria-pressed=\{selected\}/);
  assert.match(editor, /data-team-member-unavailable/);
  assert.match(editor, /validateExpertFormFields\(form, experts\)/);
  assert.match(editor, /设为主理人/);
  assert.match(editor, />\s*移除\s*</);
  assert.match(editor, /放弃未保存的更改/);
  assert.match(api, /\/api\/experts\/capabilities/);
});

test('all real composers host the picker in place and opening prompts use atomic expert draft dispatch', () => {
  const chat = source('components/ChatView.jsx');
  const input = source('components/InputBar.jsx');
  const controls = source('components/ComposerSessionControls.jsx');
  const app = source('App.jsx');

  assert.equal((chat.match(/<ExpertPickerDialog/g) || []).length, 2);
  assert.match(chat, /setExpertPickerOpen\(true\)/);
  assert.match(chat, /selectComposerExpert\(expert, \{ draftText: String\(prompt \|\| ''\) \}\)/);
  assert.match(chat, /api\.setSessionExpert\(targetSessionId, expertId, requestOptions\)/);
  assert.match(chat, /normalizeExpertSwitchReceipt\(result, expertId\)/);
  assert.match(chat, /resolveCanonicalExpertSwitchPoll/);
  assert.match(chat, /api\.listWorkspaceSessions\(sessionWorkspaceHash\)/);
  assert.match(chat, /api\.listSessions\(\)/);
  assert.match(chat, /const restoredFallback = \{\s*\.\.\.acceptedFallback,\s*requestSequence,/);
  assert.doesNotMatch(chat, /if \(busy \|\| expertSwitching \|\| !pendingExpert\?\.confirmed \|\| !sid\) return;\s*const confirmedExpert/);
  assert.match(chat, /shouldApplyExpertSwitchResponse/);
  assert.match(chat, /latestExpertSwitchRequestRef/);
  assert.doesNotMatch(chat, /expertId === sessionExpertId\) \{/);
  assert.match(chat, /pendingExpert\?\.confirmed/);
  assert.match(chat, /onOpenExpertComponents=\{\(\) => setExpertPickerOpen\(true\)\}/);

  assert.match(input, /data-expert-components-submenu="true"/);
  assert.match(input, /createPortal\(/);
  assert.match(input, /placeExpertSubmenu/);
  assert.match(input, /className="fixed z-\[100\]/);
  assert.match(input, /recentExpertItems\.length > 0 &&/);
  assert.match(input, /compactExpertSummary\(expert\)/);
  assert.match(input, />更多专家</);
  assert.match(input, />文件或文件夹</);
  assert.doesNotMatch(input, /暂无最近使用的专家|没有最近的专家|onAddBrowserContext|>浏览器</);

  assert.match(controls, /data-composer-control="expert-pending"/);
  assert.match(controls, /下一轮/);
  assert.match(controls, /expertType === 'team' \? '专家团' : '专家'/);

  assert.match(app, /<ExpertComponentsPage/);
  assert.match(app, /recentExpertIds=\{recentExpertIds\}/);
  assert.doesNotMatch(app, /useExpertForNewTask|onUseExpert=/);
});

test('conversation picker does not expose management controls without callbacks', () => {
  const catalog = source('components/ExpertCatalog.jsx');
  const card = between(catalog, 'function ExpertCard', 'function DetailSection');
  const picker = between(catalog, 'export function ExpertPickerDialog', 'export function compactExpertSummary');
  assert.match(card, /expert\.managed_global && \(onEdit \|\| onDelete\)/);
  assert.match(card, /\{onEdit && \(/);
  assert.match(card, /\{onDelete && \(/);
  assert.doesNotMatch(picker, /onEdit=|onDelete=/);
});
