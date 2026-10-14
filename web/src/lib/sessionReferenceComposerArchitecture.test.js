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

// 有查询词时必须交给服务端全局目录检索(searchSessionReferenceData),不能只拿
// 空查询的最近 50 条在本地过滤 —— 曾因此搜不到 36 天前、标题完全一致的会话。
assert.match(inputBar, /searchSessionReferenceData\(pathReferenceApi, query, \{/);
assert.match(inputBar, /signal: sessionAbort\?\.signal/);
assert.match(inputBar, /sessionAbort\?\.abort\(\);/);
assert.doesNotMatch(inputBar, /loadSessionReferenceData\(pathReferenceApi\)/);
// @ 词同时用于文件路径,每次按键都会触发检索;正文全量扫描要逐项目读 meta、开 SQLite,
// 全量一遍几十秒,不能挂在这里。全文搜索留在 Ctrl+K 搜索面板。
assert.doesNotMatch(inputBar, /searchSessionUserMessages/);
assert.match(inputBar, /currentSessionId,/);
assert.match(inputBar, /noWorkspaceLabel: t\('pathReference\.task'\)/);
assert.match(inputBar, /replaceQueryWithSessionReference\(value, pathMention\.token, item\)/);
assert.match(inputBar, /fileItems=\{activePathDropdown\.fileItems \|\| \[\]\}/);
assert.match(inputBar, /sessionItems=\{activePathDropdown\.sessionItems \|\| \[\]\}/);
assert.match(chatView, /currentSessionId=\{sid\}/);
assert.match(chatView, /extractSessionReferences\(String\(text \|\| ''\)\)/);
// 消息正文以会话引用转换后的编辑器文本为底,内联粘贴块拼在其后(粘贴材料不做引用转换)。
assert.match(chatView, /text: appendPastedTextToSubmission\(sessionReferences\.displayText, content\)/);
assert.match(chatView, /payload\.session_references = sessionReferences\.references/);
assert.match(chatView, /Array\.isArray\(payload\?\.session_references\)/);

assert.equal(zhCN.pathReference.files, '文件');
assert.equal(zhCN.pathReference.sessions, '会话');
assert.equal(zhCN.pathReference.task, '任务');
assert.equal(zhCN.pathReference.noReferences, '没有匹配的文件、文件夹或会话');
assert.equal(enUS.pathReference.files, 'Files');
assert.equal(enUS.pathReference.sessions, 'Sessions');
assert.equal(enUS.pathReference.task, 'Task');
assert.equal(enUS.pathReference.noReferences, 'No matching files, folders, or sessions');
assert.notEqual(zhCN.pathReference.sessions, '会话/Session');

console.log('ok - composer session references use localized grouped search and stable insertion');
