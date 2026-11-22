import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const picker = fs.readFileSync(path.join(srcRoot, 'components/QuestionPicker.jsx'), 'utf8');
const chatView = fs.readFileSync(path.join(srcRoot, 'components/ChatView.jsx'), 'utf8');
const toolBlock = fs.readFileSync(path.join(srcRoot, 'components/ToolBlock.jsx'), 'utf8');
const rootClasses = picker.match(/clsx\('([^']*rounded-\[14px\][^']*)'/)?.[1] || '';

assert.ok(rootClasses, 'QuestionPicker root classes must be discoverable');
assert.match(rootClasses, /rounded-\[14px\]/);
assert.match(rootClasses, /ace-shadow-lg/);
assert.doesNotMatch(
  rootClasses,
  /(?:^|\s)w-full(?:\s|$)/,
  'QuestionPicker margins must fit inside the chat column instead of overflowing its width',
);

const homeComposer = chatView.slice(
  chatView.indexOf('<div data-tour-target="home-composer"'),
  chatView.indexOf('<div className="flex items-center gap-2 mr-auto ml-0">'),
);
assert.match(homeComposer, /questionForView\s*\?\s*\(/);
assert.match(homeComposer, /<QuestionPicker/);
assert.match(homeComposer, /:\s*\(\s*<InputBar/);
assert.doesNotMatch(homeComposer, /disabled=\{!!questionForView\}/);

const dockStart = chatView.indexOf('<div className="ace-composer-dock"');
const dockEnd = chatView.indexOf('<SessionContentLoading', dockStart);
const dock = chatView.slice(dockStart, dockEnd);
assert.match(dock, /questionForView\s*\?\s*\(\s*<QuestionPicker/);
// else 分支以片段开头、内含 InputBar：InputBar 之前允许挂待发批注卡片与编辑弹窗。
assert.match(dock, /:\s*\(\s*<>[\s\S]*?<InputBar/);
const dockElse = dock.slice(dock.indexOf(') : ('));
assert.ok(
  dockElse.indexOf('<PageAnnotationPendingCard') < dockElse.indexOf('<InputBar'),
  '待发批注卡片必须在输入框上方',
);
assert.match(dock, /<QuestionPicker[\s\S]*?className="mx-2\.5"/);

assert.match(picker, /selected\s*\?\s*'bg-accent-bg border border-transparent text-accent'/);
assert.match(picker, /bg-accent text-white hover:opacity-90/);
assert.doesNotMatch(picker, /selected\s*\?\s*'bg-fg text-bg border-fg'/);
assert.doesNotMatch(picker, /font-medium bg-fg text-bg/);
assert.doesNotMatch(picker, /<svg[^>]*aria-hidden="true">[\s\S]*?M20 15a3/);
assert.match(picker, /min-h-11 shrink-0 px-3\.5 py-2\.5/);
assert.match(picker, /group flex items-center gap-3 rounded-md px-3\.5 py-3/);
assert.match(picker, /border border-transparent hover:bg-accent-bg/);
assert.doesNotMatch(picker, /hover:border-accent/);
assert.doesNotMatch(picker, /bg-accent-bg border border-accent/);
assert.match(picker, /const focused = focusIndex === index/);
assert.match(picker, /const hovered = hoverIndex === index/);
assert.match(picker, /hovered\s*\?\s*'border border-transparent bg-accent-bg'/);

assert.match(toolBlock, /const isAskUserQuestionResult = askUserQuestionResult/);
assert.match(toolBlock, /translate\('用户已取消回答'\)/);
assert.doesNotMatch(toolBlock, /String\.fromCharCode/);

console.log('questionPickerLayout tests passed');
