// 「粘贴的文本」块纯逻辑测试(第 2 条反馈 f300:24,597,780 字符的粘贴整段进了 Slate,
// 输入框与切进会话都卡死)。耗时断言一律放宽到 300ms,只是粗防护;主断言是「只扫到上限 /
// 输出长度有界」。
import assert from 'node:assert/strict';
import { composerContentSubmissionText, composerContentText, normalizeComposerContent } from './composerContent.js';
import {
  GOAL_OBJECTIVE_MAX_BYTES, NO_WORKSPACE_DRAFT_SCOPE, PASTED_FILE_CHUNK_MAX_BYTES, PASTED_FILE_MIN_BYTES,
  PASTED_INLINE_TOTAL_MAX_BYTES, PASTED_TEXT_UPLOAD_TIMEOUT_MS, SIDE_QUESTION_MAX_BYTES,
  appendPastedTextPart, appendPastedTextToSubmission, composerContentDisplayText, createPastedTextPart,
  editorAttachmentResources, homeDraftAttachmentScope, inlinePastedBytes, inputHistoryTextForPayload,
  isPasteResource, legacyFoldUploadPending, legacyTextNeedsFold, normalizePastedText, pasteBlockTextSource, pasteBlocksOf,
  pasteResourceKeys, pasteTooLongForCommand, pastedTextFileMeta, pastedTextFileName, pastedTextStats,
  pastedTextTitle, pastedTextUploadBody, payloadWithImportedPastes, planPastedTextInsertion,
  reconcileHomeDraftUpload, registerPastedTextFile, removePastedTextPart, replacePastedTextPart,
  sessionTitleSeedForPayload, shouldFoldPastedText, splitUtf8Ranges, utf8ByteLengthBounded,
  utf8RangeStats, withPasteBlocksFrom, withoutPasteBlocks, workspaceDraftPasteRefs,
} from './pastedText.js';
import { inputRouteForText } from './builtinCommandRouting.js';

function run(name, fn) { fn(); console.log(`[pass] ${name}`); }

const lines = (count, text = 'x') => Array.from({ length: count }, () => text).join('\n');
const doc = (...parts) => ({ version: 1, parts });
const inline = (text, key = `k-${text.length}`) => ({ type: 'pasted_text', key, text });
const fileBlock = (extra = {}) => ({
  type: 'attachment', key: 'pf-1', id: 'att-1', name: 'pasted-text-20260924-101500.txt', kind: 'file',
  mime_type: 'text/plain', paste: { title: '日志', chars: 3, lines: 1 }, ...extra,
});

// 触发场景:判断一次粘贴是否要折叠成块。
// 期望:20 行 / 2000 字符任一达到即折叠;19 行、1999 字符不折叠;末尾换行不算一行。
run('fold threshold boundaries', () => {
  assert.equal(shouldFoldPastedText(lines(19)), false);
  assert.equal(shouldFoldPastedText(lines(20)), true);
  assert.equal(shouldFoldPastedText(`${lines(19)}\n`), false, 'trailing newline is not an extra line');
  assert.equal(shouldFoldPastedText(`${lines(20)}\n`), true);
  assert.equal(shouldFoldPastedText('a'.repeat(1999)), false);
  assert.equal(shouldFoldPastedText('a'.repeat(2000)), true);
  assert.equal(shouldFoldPastedText(''), false);
  assert.equal(normalizePastedText('a\r\nb\rc\0d'), 'a\nb\ncd');
});

// 触发场景:按 UTF-8 字节数分内联 / 文件。
// 期望:ASCII 131071 字节内联、131072 字节落文件;中文(3 字节/字)43690 字内联、43691 字落文件;
// 已有 200 KiB 内联块时再粘 60 KiB → 合计超 256 KiB,新块落文件(已有块不回溯改形态)。
run('inline versus file classification', () => {
  const empty = doc();
  assert.equal(planPastedTextInsertion(empty, 'short').kind, 'plain');
  assert.equal(planPastedTextInsertion(empty, 'a'.repeat(PASTED_FILE_MIN_BYTES - 1)).kind, 'inline');
  assert.equal(planPastedTextInsertion(empty, 'a'.repeat(PASTED_FILE_MIN_BYTES)).kind, 'file');
  assert.equal(planPastedTextInsertion(empty, '字'.repeat(43690)).kind, 'inline');
  assert.equal(planPastedTextInsertion(empty, '字'.repeat(43691)).kind, 'file');
  const existing = doc(inline('a'.repeat(200 * 1024), 'big'));
  assert.equal(inlinePastedBytes(existing), 200 * 1024);
  const second = planPastedTextInsertion(existing, 'b'.repeat(60 * 1024));
  assert.equal(second.kind, 'file');
  assert.equal(planPastedTextInsertion(existing, 'b'.repeat(56 * 1024)).kind, 'inline', 'exactly 256 KiB total stays inline');
  const plan = planPastedTextInsertion(empty, `标题行\n${'a'.repeat(PASTED_FILE_MIN_BYTES)}`);
  assert.equal(plan.byteLength, plan.bytes.length);
  assert.equal(plan.chunks.length, 1);
  assert.equal(plan.chunks[0].title, '标题行');
  assert.deepEqual(plan.chunks[0].paste, { title: '标题行', chars: PASTED_FILE_MIN_BYTES + 4, lines: 2 });
  assert.equal(plan.chunks[0].part, undefined, 'single chunk has no part/parts');
  assert.equal(plan.notice, false);
});

// 触发场景:f300 那条 2400 万字符的粘贴做字节计数。
// 期望:只扫到上限就返回(超长时不逐字扫描),结果 > 上限;短文本精确计数(含代理对与孤立代理)。
run('bounded UTF-8 length stops at the limit', () => {
  const huge = 'x'.repeat(24_000_000);
  const started = Date.now();
  assert.ok(utf8ByteLengthBounded(huge, PASTED_INLINE_TOTAL_MAX_BYTES) > PASTED_INLINE_TOTAL_MAX_BYTES);
  const nearlyHuge = '字'.repeat(PASTED_INLINE_TOTAL_MAX_BYTES);
  assert.ok(utf8ByteLengthBounded(nearlyHuge, PASTED_INLINE_TOTAL_MAX_BYTES) > PASTED_INLINE_TOTAL_MAX_BYTES);
  assert.ok(Date.now() - started < 300, 'coarse guard only');
  assert.equal(utf8ByteLengthBounded('aé字😀'), 1 + 2 + 3 + 4);
  assert.equal(utf8ByteLengthBounded('\uD800x'), 3 + 1, 'lone surrogate encodes as U+FFFD');
  assert.equal(utf8ByteLengthBounded('😀'), new TextEncoder().encode('😀').length);
});

// 触发场景:> 24 MiB 的粘贴按字节切段(用小样例代替真实阈值)。
// 期望:换行优先(切点落在窗口内最后一个换行之后);窗口内无换行时按上限切但不切开多字节字符;
// 区间首尾相接、覆盖全部字节;每段不超过上限。
run('splitUtf8Ranges prefers newlines and keeps characters whole', () => {
  const encoder = new TextEncoder();
  const bytes = encoder.encode('aaaa\nbbbbbbbb\ncc');
  assert.deepEqual(splitUtf8Ranges(bytes, 10, 8), [{ start: 0, end: 5 }, { start: 5, end: 14 }, { start: 14, end: 16 }]);
  const cjk = encoder.encode('字'.repeat(10)); // 30 字节
  const ranges = splitUtf8Ranges(cjk, 8, 4);
  assert.deepEqual(ranges.map(({ start, end }) => end - start), [6, 6, 6, 6, 6]);
  const decoder = new TextDecoder('utf-8', { fatal: true });
  assert.equal(ranges.map(({ start, end }) => decoder.decode(cjk.subarray(start, end))).join(''), '字'.repeat(10));
  const noNewline = encoder.encode('a'.repeat(25));
  const plain = splitUtf8Ranges(noNewline, 10, 4);
  assert.deepEqual(plain, [{ start: 0, end: 10 }, { start: 10, end: 20 }, { start: 20, end: 25 }]);
  for (const list of [ranges, plain]) {
    list.forEach((range, index) => { if (index) assert.equal(range.start, list[index - 1].end); });
  }
  assert.deepEqual(splitUtf8Ranges(encoder.encode(''), 10), []);
  assert.deepEqual(utf8RangeStats(encoder.encode('字a\nb\n'), 0, 7), { chars: 5, lines: 2 });
});

// 触发场景:超过单附件上限的粘贴(真实阈值 24 MiB,约 25 MB ASCII)。
// 期望:切成两段,各段带 part/parts,标题加「(k/n)」,段长不超过上限且合起来等于原文。
run('file plan splits oversize pastes into numbered chunks', () => {
  const text = `${'a'.repeat(1023)}\n`.repeat(PASTED_FILE_CHUNK_MAX_BYTES / 1024 + 10);
  const plan = planPastedTextInsertion(doc(), text);
  assert.equal(plan.kind, 'file');
  assert.equal(plan.chunks.length, 2);
  assert.deepEqual(plan.chunks.map((chunk) => [chunk.part, chunk.parts]), [[1, 2], [2, 2]]);
  assert.ok(plan.chunks[0].title.endsWith('（1/2）'));
  assert.equal(plan.chunks[1].paste.part, 2);
  assert.ok(plan.chunks.every((chunk) => chunk.end - chunk.start <= PASTED_FILE_CHUNK_MAX_BYTES));
  assert.equal(plan.chunks[0].end, plan.chunks[1].start);
  assert.equal(plan.chunks[1].end, plan.byteLength);
  assert.equal(plan.bytes[plan.chunks[0].end - 1], 0x0A, 'first chunk ends right after a newline');
  assert.equal(plan.chunks[0].paste.lines + plan.chunks[1].paste.lines, pastedTextStats(text).lines);
});

// 触发场景:两次内容相同的粘贴落在同一秒。
// 期望:文件名仍不同(否则 composerFileIdentity 按 名字+大小+类型+时间 把第二次当重复去掉);
// 多段各自带段号;全是 ASCII。
run('pasted file names are ASCII and unique', () => {
  const now = new Date(2026, 8, 24, 10, 15, 0);
  const first = pastedTextFileName(now, 0);
  const second = pastedTextFileName(now, 0);
  assert.equal(first, 'pasted_text_20260924_101500.txt');
  assert.notEqual(first, second);
  const chunk1 = pastedTextFileName(now, 1);
  const chunk2 = pastedTextFileName(now, 2);
  assert.ok(chunk1.endsWith('_1.txt') && chunk2.endsWith('_2.txt'));
  assert.equal(chunk1.slice(0, -6), chunk2.slice(0, -6));
  assert.ok(new Set([first, second, chunk1, chunk2]).size === 4);
  for (const name of [first, second, chunk1, chunk2]) assert.match(name, /^[\x20-\x7e]+$/);
});

// 触发场景:上传完成后服务端记录整体替换了本地资源,资源上的 paste 标记随之丢失。
// 期望:isPasteResource 的每个条件单独成立都能认出粘贴资源(资源 paste / 服务端 metadata.origin /
// File 上登记的描述 / 部件 key)。回归:上传回填后资源丢了 paste 标记,被 RichComposer 当普通附件
// 插进编辑器,输入框里多出一个附件标签。
run('paste resources are recognized after the upload replaced the resource', () => {
  assert.equal(isPasteResource({ id: 'a', paste: { title: 't' } }), true);
  assert.equal(isPasteResource({ id: 'a', metadata: { origin: 'pasted_text' } }), true);
  const file = { name: 'pasted.txt' };
  registerPastedTextFile(file, { title: 't', chars: 1, lines: 1 });
  assert.equal(isPasteResource({ local_id: 'x', file }), true);
  assert.equal(isPasteResource({ local_id: 'pf-1', id: 'att-1' }, pasteResourceKeys(doc(fileBlock()))), true);
  assert.equal(isPasteResource({ id: 'att-1' }, new Set(['att-1'])), true);
  assert.equal(isPasteResource({ id: 'ordinary', name: 'notes.pdf' }, pasteResourceKeys(doc(fileBlock()))), false);
  assert.equal(isPasteResource(null), false);
});

// 触发场景:InputBar 把附件交给 RichComposer。
// 期望:图片与粘贴资源都剥掉,只剩普通附件;即使部件已被删除(key 集合里没有了),带服务端
// metadata.origin 的粘贴资源仍被排除。
run('editor attachment resources exclude images and paste resources', () => {
  const attachments = [
    { local_id: 'img', kind: 'image', mime_type: 'image/png', name: 'a.png' },
    { local_id: 'pf-1', id: 'att-1', name: 'p.txt', mime_type: 'text/plain' },
    { local_id: 'gone', id: 'att-2', name: 'q.txt', metadata: { origin: 'pasted_text' } },
    { local_id: 'doc', id: 'd', name: 'notes.pdf', kind: 'file' },
  ];
  const result = editorAttachmentResources(attachments, doc(fileBlock()));
  assert.deepEqual(result.map((item) => item.local_id), ['doc']);
  assert.deepEqual(editorAttachmentResources(attachments, null).map((item) => item.local_id), ['pf-1', 'doc']);
});

// 触发场景:草稿恢复时重新保留同一个 File(保留项对象是新的)。
// 期望:WeakMap 登记的描述跟着 File 走,仍可取回;非法计数被清掉;非对象安全返回 null。
run('paste descriptors registered on a File survive re-reservation', () => {
  const file = { name: 'pasted-text.txt' };
  registerPastedTextFile(file, { title: 'T', chars: 10, lines: -3, part: 1, parts: 2, extra: 1 });
  const reReserved = { file, identity: 'new-identity', localId: 'another' };
  assert.deepEqual(pastedTextFileMeta(reReserved.file), { title: 'T', chars: 10, part: 1, parts: 2 });
  assert.equal(pastedTextFileMeta({ name: 'other' }), null);
  assert.equal(pastedTextFileMeta(null), null);
  assert.deepEqual(pastedTextUploadBody({ title: 'T', chars: 10, lines: 2, part: 1, parts: 2 }),
    { origin: 'pasted_text', paste: { chars: 10, lines: 2, part: 1, parts: 2 } });
  assert.deepEqual(pastedTextUploadBody(null), { origin: 'pasted_text', paste: {} });
});

// 触发场景:首页粘贴的文件块要存到哪个 scope。
// 期望:无 workspace('')→ __no_workspace__;真实 hash 原样;AI 主题等临时首页 → ''(不上传)。
run('home draft attachment scope', () => {
  assert.equal(homeDraftAttachmentScope(''), NO_WORKSPACE_DRAFT_SCOPE);
  assert.equal(homeDraftAttachmentScope('abc123'), 'abc123');
  assert.equal(homeDraftAttachmentScope('__ai_theme__:abc'), '');
});

// 触发场景:发送前把首页草稿附件导入新会话。
// 期望:引用按 id 去重;导入后 payload.attachments 与 composer 部件都换成新 id,store / store_scope 清掉。
run('workspace draft refs are imported by id and lose store fields', () => {
  const payload = {
    text: '看下',
    attachments: [{ id: 'd1', store: 'workspace_draft', store_scope: 'abc' }, { id: 'plain' }],
    composer_content: doc({ type: 'text', text: '看下' }, fileBlock({ id: 'd1', store: 'workspace_draft', store_scope: 'abc' })),
  };
  assert.deepEqual(workspaceDraftPasteRefs(payload), [{ id: 'd1', workspace: 'abc' }]);
  const next = payloadWithImportedPastes(payload, [{ id: 'd1', attachment: { id: 's1' } }]);
  assert.deepEqual(next.attachments, [{ id: 's1' }, { id: 'plain' }]);
  const part = next.composer_content.parts[1];
  assert.equal(part.id, 's1');
  assert.equal('store' in part, false);
  assert.equal('store_scope' in part, false);
  assert.deepEqual(workspaceDraftPasteRefs(next), []);
  assert.equal(payloadWithImportedPastes(payload, []), payload);
});

// 触发场景:离开首页后才完成的草稿附件上传,回填到 store 里的最新草稿。
// 期望:草稿仍有该块时回填 id + store + store_scope 并替换本地资源;块已删除时返回 null。
// 回归:已发送(草稿已清空)的首页草稿被迟到的上传结果复活。
run('late home draft upload reconciles only while the block still exists', () => {
  const draft = {
    text: '',
    composer_content: doc(fileBlock({ id: '' })),
    attachments: [{ local_id: 'pf-1', name: 'p.txt', pending_upload: true, file: {} }],
  };
  const uploaded = { local_id: 'pf-1', id: 'd9', session_id: '.workspace-draft', name: 'p.txt' };
  const next = reconcileHomeDraftUpload(draft, uploaded, 'abc');
  assert.equal(next.composer_content.parts[0].id, 'd9');
  assert.equal(next.composer_content.parts[0].store, 'workspace_draft');
  assert.equal(next.composer_content.parts[0].store_scope, 'abc');
  assert.equal(next.attachments.length, 1);
  assert.equal(next.attachments[0].id, 'd9');
  assert.equal(next.attachments[0].pending_upload, undefined);
  assert.equal(reconcileHomeDraftUpload({ text: '' }, uploaded, 'abc'), null);
  assert.equal(reconcileHomeDraftUpload({ text: 'x', composer_content: doc({ type: 'text', text: 'x' }) }, uploaded, 'abc'), null);
});

// 触发场景:/goal、/btw 后面带粘贴块(D9 ④)。
// 期望:/goal + 1000 字节英文内联 → null(照常并入参数);/goal + 2000 个汉字(6000 字节)→ 超 4000 上限;
// /goal + 任一文件块 → 超限;/btw 同理(16000);/compact + 文件块 → null(按普通消息处理,不在此拦)。
run('commands reject pastes that cannot fit the argument limit', () => {
  const payloadWith = (editor, ...blocks) => {
    const content = doc({ type: 'text', text: editor }, ...blocks);
    return { text: appendPastedTextToSubmission(editor, content), composer_content: content };
  };
  const route = (payload) => inputRouteForText(payload.text);
  const small = payloadWith('/goal 修复登录', inline('e'.repeat(1000)));
  assert.equal(pasteTooLongForCommand(route(small), small), null);
  const big = payloadWith('/goal 修复登录', inline('字'.repeat(2000)));
  assert.deepEqual(pasteTooLongForCommand(route(big), big), { command: 'goal', limitBytes: GOAL_OBJECTIVE_MAX_BYTES });
  const withFile = payloadWith('/goal 修复登录', fileBlock());
  assert.deepEqual(pasteTooLongForCommand(route(withFile), withFile), { command: 'goal', limitBytes: GOAL_OBJECTIVE_MAX_BYTES });
  const btwSmall = payloadWith('/btw 这是什么', inline('e'.repeat(3000)));
  assert.equal(pasteTooLongForCommand(route(btwSmall), btwSmall), null);
  const btwBig = payloadWith('/btw 这是什么', inline('e'.repeat(20000)));
  assert.deepEqual(pasteTooLongForCommand(route(btwBig), btwBig), { command: 'btw', limitBytes: SIDE_QUESTION_MAX_BYTES });
  const btwFile = payloadWith('/side 这是什么', fileBlock());
  assert.deepEqual(pasteTooLongForCommand(route(btwFile), btwFile), { command: 'side', limitBytes: SIDE_QUESTION_MAX_BYTES });
  const compact = payloadWith('/compact', fileBlock());
  assert.equal(pasteTooLongForCommand(route(compact), compact), null);
  const typedOnly = { text: `/goal ${'字'.repeat(2000)}`, composer_content: doc({ type: 'text', text: `/goal ${'字'.repeat(2000)}` }) };
  assert.equal(pasteTooLongForCommand(route(typedOnly), typedOnly), null, 'no paste block: the server keeps validating as before');
});

// 触发场景:块的增删改与提交正文拼接。
// 期望:追加块在末尾;替换换 key 且位置不变,新文本为空即删除;顶层 payload.text 与
// composerContentSubmissionText 逐字节一致(块总在编辑器内容之后)。
run('block editing helpers and submission text', () => {
  let content = appendPastedTextPart(doc({ type: 'text', text: '分析' }), 'L1\nL2', 'k1');
  content = appendPastedTextPart(content, 'M', 'k2');
  assert.deepEqual(pasteBlocksOf(content).map((block) => [block.id, block.kind]), [['k1', 'inline'], ['k2', 'inline']]);
  assert.equal(appendPastedTextToSubmission('分析', content), composerContentSubmissionText(content));
  assert.equal(appendPastedTextToSubmission('', content), 'L1\nL2\n\nM');
  const replaced = replacePastedTextPart(content, 'k1', 'NEW', 'k3');
  assert.deepEqual(replaced.parts.map((part) => part.key || part.type), ['text', 'k3', 'k2']);
  assert.deepEqual(replacePastedTextPart(content, 'k1', '').parts.map((part) => part.key || part.type), ['text', 'k2']);
  assert.deepEqual(removePastedTextPart(content, 'k2').parts.map((part) => part.key || part.type), ['text', 'k1']);
  const withFile = normalizeComposerContent({ ...content, parts: [...content.parts, fileBlock()] });
  assert.deepEqual(pasteBlocksOf(withFile).at(-1), { id: 'pf-1', kind: 'file', part: withFile.parts.at(-1) });
  assert.deepEqual(removePastedTextPart(withFile, 'att-1').parts.length, 3, 'file blocks can be removed by attachment id');
  assert.equal(createPastedTextPart('x').key.startsWith('paste-'), true);
  assert.notEqual(createPastedTextPart('x').key, createPastedTextPart('x').key);
});

// 触发场景:队列编辑框 / 输入框只把编辑器内容交给 Slate,保存时再把块合并回来。
// 期望:withoutPasteBlocks 剥掉两种块;withPasteBlocksFrom 以 source 的块为准、按原顺序追加在编辑器内容之后。
run('editor projection round trip keeps blocks from the source', () => {
  const source = doc({ type: 'text', text: 'old' }, inline('A', 'a'), fileBlock(), inline('B', 'b'));
  const editor = withoutPasteBlocks(source);
  assert.deepEqual(editor.parts, [{ type: 'text', text: 'old' }]);
  const merged = withPasteBlocksFrom(doc({ type: 'text', text: 'new' }), source);
  assert.deepEqual(merged.parts.map((part) => part.key || part.text), ['new', 'a', 'pf-1', 'b']);
  assert.equal(composerContentText(merged), 'new');
});

// 触发场景:cwd 输入历史、首页新建会话的标题。
// 期望:历史只记编辑器文本(不含拼上去的粘贴正文);标题种子优先编辑器文本(截 200 字符),
// 编辑器为空时用第一个块的标题。
run('history text and title seed use editor text first', () => {
  const content = doc({ type: 'text', text: '分析下面日志' }, inline('ERROR boot\nline 2', 'p'));
  const payload = { text: appendPastedTextToSubmission('分析下面日志', content), composer_content: content };
  assert.equal(inputHistoryTextForPayload(payload), '分析下面日志');
  const blockOnly = doc(inline('\n\n  ERROR   boot failed  \nline 2', 'p'));
  const blockPayload = { text: appendPastedTextToSubmission('', blockOnly), composer_content: blockOnly };
  assert.equal(inputHistoryTextForPayload(blockPayload), '');
  assert.equal(inputHistoryTextForPayload({ text: 'plain' }), 'plain');
  assert.equal(sessionTitleSeedForPayload(payload), '分析下面日志');
  assert.equal(sessionTitleSeedForPayload(blockPayload), 'ERROR boot failed');
  assert.equal(sessionTitleSeedForPayload({ text: '', composer_content: doc(fileBlock()) }), '日志');
  assert.equal(sessionTitleSeedForPayload({ text: 'a'.repeat(500) }).length, 200);
});

// 触发场景:卡片标题、统计与排队卡片的概览文本。
// 期望:标题取前 2KB 里第一条非空行、压缩空白、按码点截断;全空白时用默认标题;统计按码点计、
// 末尾换行不算一行;概览把两种块都显示成 [粘贴的文本] 且长度有界。
run('titles, stats and bounded display text', () => {
  assert.equal(pastedTextTitle('\n  hello   world \nsecond'), 'hello world');
  assert.equal(pastedTextTitle('😀'.repeat(100), 10), `${'😀'.repeat(10)}…`);
  assert.equal(pastedTextTitle(`${' '.repeat(3000)}late`), '粘贴的文本', 'only the first 2KB is scanned');
  assert.deepEqual(pastedTextStats('a😀\nb\n'), { chars: 5, lines: 2 });
  assert.deepEqual(pastedTextStats(''), { chars: 0, lines: 0 });
  const content = doc({ type: 'text', text: '看下' }, inline('x'.repeat(100000), 'p'), fileBlock());
  assert.equal(composerContentDisplayText(content), '看下\n\n[粘贴的文本]\n\n[粘贴的文本]');
  assert.ok(composerContentDisplayText(doc({ type: 'text', text: 'y'.repeat(100000) }), undefined, { maxChars: 1000 }).length <= 1000);
});

// 触发场景:旧长文本入框(旧草稿 / fork 回填 / 上箭头翻旧历史)。
// 期望:没有 composer_content 且达到折叠阈值 → 折叠;结构化纯文本只有 >= 2 万字符才折叠(本版之前
// 大段粘贴也存成 text 部件);带路径 / 附件等引用的内容不折叠(不丢结构)。
run('legacy text fold decision', () => {
  assert.equal(legacyTextNeedsFold(lines(20)), true);
  assert.equal(legacyTextNeedsFold('short'), false);
  assert.equal(legacyTextNeedsFold(lines(20), doc({ type: 'text', text: lines(20) })), false);
  const huge = 'z'.repeat(20000);
  assert.equal(legacyTextNeedsFold(huge, doc({ type: 'text', text: huge })), true);
  assert.equal(legacyTextNeedsFold(huge, doc({ type: 'text', text: huge }, fileBlock({ paste: undefined }))), false);
});

// 触发场景:打开首页 / 会话,旧版本留下的几 MB 草稿全文被折叠成文件块、上传还在进行中。
// 期望:折叠块仍在内容里且没有 id → pending(草稿保存必须跳过,保留服务端旧全文);某一段
// 还没 id 也算 pending;全部拿到 id、或块被用户删掉、或没有 guard → 不再 pending。
// 回归 bug 表现:折叠后 250~350ms 草稿就被覆盖成「空文本 + 无 id 附件」,上传完成前刷新 /
// 关页 / 上传失败,卡片显示「已丢失」,旧草稿全文不可恢复。
run('legacy fold upload pending guard', () => {
  const guard = { localIds: ['pf-a', 'pf-b'] };
  const part = (key, id = '') => ({ type: 'attachment', key, ...(id ? { id } : {}), name: `${key}.txt`, kind: 'file' });
  assert.equal(legacyFoldUploadPending(guard, doc(part('pf-a'), part('pf-b'))), true);
  assert.equal(legacyFoldUploadPending(guard, doc(part('pf-a', 'att-a'), part('pf-b'))), true);
  assert.equal(legacyFoldUploadPending(guard, doc(part('pf-a', 'att-a'), part('pf-b', 'att-b'))), false);
  assert.equal(legacyFoldUploadPending(guard, doc({ type: 'text', text: 'user deleted the blocks' })), false);
  // 与折叠无关的其它未上传附件不影响判定。
  assert.equal(legacyFoldUploadPending(guard, doc(part('other'))), false);
  assert.equal(legacyFoldUploadPending(null, doc(part('pf-a'))), false);
  assert.equal(legacyFoldUploadPending({ localIds: [] }, doc(part('pf-a'))), false);
  assert.equal(legacyFoldUploadPending(guard, null), false);
});

// 触发场景:打开卡片时决定从哪里读文本。
// 期望:内联块直接给 text;有内存 File 优先 File;工作区草稿附件由 scope + id 计算 URL(不读记录的
// blob_url);会话附件优先记录 blob_url,否则按 sessionId 拼。
run('paste block text source', () => {
  assert.deepEqual(pasteBlockTextSource({ kind: 'inline', part: inline('T', 'k') }), { text: 'T' });
  const file = { name: 'p.txt' };
  assert.deepEqual(pasteBlockTextSource({ part: fileBlock(), resource: { file } }), { file });
  assert.deepEqual(
    pasteBlockTextSource({ part: fileBlock({ id: 'd1', store: 'workspace_draft', store_scope: '__no_workspace__' }),
      resource: { blob_url: '/api/sessions/.workspace-draft/attachments/d1/blob' } }),
    { url: '/api/workspaces/__no_workspace__/draft/attachments/d1/blob' },
  );
  assert.deepEqual(pasteBlockTextSource({ part: fileBlock(), resource: { blob_url: '/api/sessions/child/attachments/att-1/blob' } }, { sessionId: 'parent' }),
    { url: '/api/sessions/child/attachments/att-1/blob' });
  assert.deepEqual(pasteBlockTextSource({ part: fileBlock() }, { sessionId: 's 1' }), { url: '/api/sessions/s%201/attachments/att-1/blob' });
  assert.equal(pasteBlockTextSource({ part: fileBlock({ id: '' }) }), null);
});

// 触发场景:常量与 api.js 超时对齐。期望:粘贴上传 / 读取用 5 分钟有限超时。
run('upload timeout constant is re-exported', () => {
  assert.equal(PASTED_TEXT_UPLOAD_TIMEOUT_MS, 5 * 60 * 1000);
});
