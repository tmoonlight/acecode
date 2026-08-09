import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { enUS } from '../i18n/catalogs/en-US.js';
import { zhCN } from '../i18n/catalogs/zh-CN.js';

function source(path) {
  return readFileSync(fileURLToPath(new URL(path, import.meta.url)), 'utf8');
}

const inputBar = source('../components/InputBar.jsx');
const chatView = source('../components/ChatView.jsx');

assert.match(inputBar, /loadSessionReferenceData\(pathReferenceApi\)/);
assert.match(inputBar, /searchSessionUserMessages\(query, 100\)/);
assert.match(inputBar, /currentSessionId,/);
assert.match(inputBar, /noWorkspaceLabel: t\('pathReference\.task'\)/);
assert.match(inputBar, /replaceQueryWithSessionReference\(value, pathMention\.token, item\)/);
assert.match(inputBar, /fileItems=\{activePathDropdown\.fileItems \|\| \[\]\}/);
assert.match(inputBar, /sessionItems=\{activePathDropdown\.sessionItems \|\| \[\]\}/);
assert.match(chatView, /currentSessionId=\{sid\}/);
assert.match(chatView, /extractSessionReferences\(String\(text \|\| ''\)\)/);
assert.match(chatView, /text: sessionReferences\.displayText/);
assert.match(chatView, /payload\.session_references = sessionReferences\.references/);
assert.match(chatView, /Array\.isArray\(payload\?\.session_references\)/);

assert.equal(zhCN.pathReference.files, '文件');
assert.equal(zhCN.pathReference.sessions, '会话');
assert.equal(zhCN.pathReference.task, '任务');
assert.equal(enUS.pathReference.files, 'Files');
assert.equal(enUS.pathReference.sessions, 'Sessions');
assert.equal(enUS.pathReference.task, 'Task');
assert.notEqual(zhCN.pathReference.sessions, '会话/Session');

console.log('ok - composer session references use localized grouped search and stable insertion');
