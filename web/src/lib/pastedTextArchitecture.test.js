// 「粘贴的文本」块的架构守卫(第 2 条反馈 f300:超长粘贴把整段塞进 Slate 导致卡死)。
//
// 组件没有 DOM 测试环境,这里按源码结构守住几条容易被后续改动悄悄打穿的约束:
//  - 所有文本入口先过 foldLargePaste,再碰编辑器;
//  - 交给 RichComposer 的附件必须剥掉粘贴资源;
//  - 首页草稿附件只有一个导入点,且在真正发送 / 插话之前;
//  - 被否决的「超预算剥块 + 内存缓存」方案不得回流。
import assert from 'node:assert/strict';
import { readdirSync, readFileSync, statSync } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';

function source(path) {
  return readFileSync(fileURLToPath(new URL(path, import.meta.url)), 'utf8');
}

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

// 取 start 标记到 end 标记之间的源码;任一标记缺失直接失败,避免空串让断言静默通过。
function between(text, start, end, from = 0) {
  const begin = text.indexOf(start, from);
  assert.ok(begin >= 0, `missing marker: ${start}`);
  const finish = text.indexOf(end, begin + start.length);
  assert.ok(finish > begin, `missing end marker after ${start}: ${end}`);
  return text.slice(begin, finish);
}

function before(text, first, second, message) {
  const a = text.indexOf(first);
  const b = text.indexOf(second);
  assert.ok(a >= 0, `missing: ${first}`);
  assert.ok(b >= 0, `missing: ${second}`);
  assert.ok(a < b, message || `${first} must appear before ${second}`);
}

const richComposer = source('../components/RichComposer.jsx');
const inputBar = source('../components/InputBar.jsx');
const queueCardList = source('../components/QueueCardList.jsx');
const pastedTextDialog = source('../components/PastedTextDialog.jsx');
const chatView = source('../components/ChatView.jsx');
const message = source('../components/Message.jsx');
const subagentPanel = source('../components/SubagentPanel.jsx');
const app = source('../App.jsx');

// 触发场景:用户粘贴 / 拖放 / 从另一个输入框复制一段几 MB 的文本。
// 期望行为:四个入口都先调用 foldLargePaste,命中阈值就交给父组件变成卡片,不碰 Slate。
// 回归 bug 表现:只拦了普通粘贴,文件传输通道(InputBar 总是传 onPasteFilesystemItems,
// 普通粘贴实际走它)、结构化剪贴板或 text/plain 拖放仍把全文插进编辑器,页面卡死。
run('RichComposer:四个文本入口都先过 foldLargePaste 再插入编辑器', () => {
  const applyPaste = between(richComposer, 'const applyPlainTextPaste = useCallback(', 'const handleContextPasteAction');
  before(applyPaste, 'foldLargePaste(text)', 'insertPlainText(editor', 'applyPlainTextPaste must fold before inserting');

  const transferInsert = between(richComposer, 'insertText(text) {', 'reserveAttachments()');
  before(transferInsert, 'foldLargePaste(text)', 'insertPlainText(editor', 'transfer.insertText must fold before inserting');

  const clipboard = between(richComposer, 'const handleClipboardPaste = useCallback(', 'const markPasteHandled');
  before(clipboard, 'takeLargeClipboardParts(copiedContent, foldLargePaste)',
    'insertComposerContent(editor, copiedContent', 'structured clipboard must fold before inserting');

  const drop = between(richComposer, 'const handleDrop = useCallback(', 'return (');
  assert.match(drop, /getData\?\.\('text\/plain'\)/);
  assert.match(drop, /foldLargePaste\(text\)/);
  assert.match(drop, /application\/x-slate-fragment/);

  // 父组件返回 false 才照常插入;阈值判断与 onLargeTextPaste 都在同一个入口函数里。
  const fold = between(richComposer, 'const foldLargePaste = useCallback(', 'const applyPlainTextPaste');
  assert.match(fold, /shouldFoldPastedText\(normalizedText\)/);
  assert.match(fold, /handler\(normalizedText\) !== false/);
});

// 触发场景:粘贴 1 MB 文本 → 上传完成,服务端记录整体替换了本地资源(paste 标记随之丢失)。
// 回归 bug 表现:RichComposer 把这个「没见过、文档里也没有」的资源插成编辑器里的附件标签。
// 期望行为:InputBar 与排队编辑框交给 RichComposer 的附件都经 editorAttachmentResources。
run('InputBar / 排队编辑框:粘贴资源不进编辑器,卡片条渲染 PastedTextCard', () => {
  assert.match(inputBar, /const editorAttachmentItems = useMemo\(\s*\(\) => editorAttachmentResources\(attachmentItems, composerContent\)/);
  assert.match(inputBar, /attachments=\{editorAttachmentItems\}/);
  assert.match(inputBar, /onLargeTextPaste=\{onLargeTextPaste \? handleEditorLargeTextPaste : undefined\}/);
  assert.match(inputBar, /<PastedTextCard/);
  assert.match(inputBar, /hasNonTextContent: pasteBlocks\.length > 0/);
  assert.match(inputBar, /historyPointer: histPtr,/);
  assert.match(inputBar, /onLargeTextPaste\(entryText, \{ deferUpload: true \}\)/);

  assert.match(queueCardList, /editorAttachmentResources\(resources, card\.composerContent\)/);
  assert.match(queueCardList, /attachments=\{editorResources\}/);
  assert.match(queueCardList, /composerContent=\{editorContent\}/);
  assert.match(queueCardList, /onLargeTextPaste=/);
  assert.match(queueCardList, /<PastedTextCard/);
  assert.match(queueCardList, /withPasteBlocksFrom\(nextContent, previous\)/);
});

// 触发场景:打开一个 30 万字符的粘贴块编辑。
// 期望行为:编辑框是不受控原生 textarea(受控 value 每次按键都要把全文过一遍 React);
// 编辑过后 Esc 不关,点背景始终不关。
run('PastedTextDialog:不受控 textarea + Modal + 编辑过后 Esc 不关', () => {
  assert.match(pastedTextDialog, /<Modal/);
  assert.match(pastedTextDialog, /dismissOnEscape=\{!dirty\}/);
  assert.match(pastedTextDialog, /dismissOnBackdrop=\{false\}/);
  const textarea = between(pastedTextDialog, '<textarea', '/>');
  assert.doesNotMatch(textarea, /\svalue=/);
  assert.match(textarea, /spellCheck=\{false\}/);
});

// 触发场景:首页粘贴 3 MB → 落在工作区草稿附件区 → 发送 / 排队出队 / /turn 插话。
// 期望行为:草稿附件 id 在进入任何会话请求之前经唯一导入点复制成会话附件;路由看
// 整个 payload(编辑器为空、只有粘贴块时一律普通消息)。
run('ChatView:正文拼接、唯一导入点与按 payload 路由', () => {
  const normalize = between(chatView, 'function normalizeComposerPayload(', 'function pastedTextFilesForPlan(');
  assert.match(normalize, /text: appendPastedTextToSubmission\(sessionReferences\.displayText, content\)/);
  assert.match(normalize, /store: item\.store, store_scope: item\.store_scope/);

  const send = between(chatView, 'const sendInputOrBuiltin = useCallback(', 'const submit = useStableCallback(');
  before(send, 'materializeWorkspaceDraftPastes(', 'api.sendInput(');
  assert.match(send, /inputRouteForPayload\(requestPayload\)/);

  const turn = between(chatView, "if (route.kind === 'turn_steer') {", 'const isBuiltin = ');
  before(turn, 'materializeWorkspaceDraftPastes(', 'api.interruptTurn(');

  const submit = between(chatView, 'const submit = useStableCallback(', 'const drainQueuedInput = useStableCallback(');
  assert.match(submit, /const route = inputRouteForPayload\(payload\)/);
  before(submit, "route.kind === 'paste_too_long'", "route.kind === 'desktop_feedback'");
  before(submit, "route.kind === 'paste_too_long'", "route.kind === 'side_question'");
  assert.match(submit, /sessionTitleSeedForPayload\(payload\)/);
  assert.match(submit, /commitDeferredPastes\(\)/);
  // cwd 历史只记编辑器文本:/turn、/feedback、/btw 也不例外(route 里的文本已拼上内联块正文)。
  // 回归 bug 表现:`/turn 按这个改` + 200 KiB 内联块整段进 input_history.jsonl,上箭头翻回时
  // 连同 /turn 被折叠成粘贴块,发出去成了普通消息。
  assert.doesNotMatch(chatView, /recordInputHistory\(route\.display_text\)/);
  assert.match(submit, /recordHistory: true,\s*historyText,/);
  const side = between(chatView, 'const runSideQuestion = useStableCallback(', 'const openSideQuestionComposer');
  assert.match(side, /typeof historyText === 'string' \? historyText :/);

  const drain = between(chatView, 'const drainQueuedInput = useStableCallback(', 'sendInputOrBuiltin(targetSid, sendPayload)');
  assert.match(drain, /inputRouteForPayload\(/);
  assert.doesNotMatch(chatView, /inputRouteForText\(/, '所有路由都按 payload 判定');

  // 首页草稿附件留在 payload.attachments 里才算 extras,判定函数本身不看粘贴块。
  const extras = between(chatView, 'function payloadHasExtras(', '\n}');
  assert.doesNotMatch(extras, /composerContentHasPastedText/);
});

// 触发场景:粘贴 1 MB → 上传完成。
// 回归 bug 表现:上传回填用服务端记录整体替换资源,paste 描述丢失,persistOne 也拿不到
// 描述(上传请求不带 origin,模型收到的是普通附件引用)。
// 期望行为:persistOne 从 File 上取描述(WeakMap),上传体带 origin/paste,回填资源带 paste;
// 粘贴分段顺序上传(并发会把服务端峰值内存放大到 GB 级)。
run('ChatView:上传从 File 取粘贴描述,回填资源带 paste,分段顺序上传', () => {
  const persist = between(chatView, 'const persistMediaFilesToSession = useCallback(', 'const handleMediaFiles = useStableCallback(');
  assert.match(persist, /const paste = pastedTextFileMeta\(file\)/);
  assert.match(persist, /\.\.\.\(paste \? pastedTextUploadBody\(paste\) : \{\}\)/);
  assert.match(persist, /timeoutMs: PASTED_TEXT_UPLOAD_TIMEOUT_MS/);
  assert.match(persist, /\{ \.\.\.uploadedItem, preview_url: item\.preview_url \|\| '', \.\.\.\(paste \? \{ paste \} : \{\}\) \}/);
  assert.match(persist, /for \(const index of pasteIndexes\)/);

  const draftUpload = between(chatView, 'const persistPastedTextToWorkspaceDraft = useStableCallback(', 'const uploadPasteReservations = useStableCallback(');
  assert.match(draftUpload, /api\.uploadWorkspaceDraftAttachment\(scope,/);
  assert.match(draftUpload, /onHomeComposerDraftPatch\?\.\(hash, \(draft\) => reconcileHomeDraftUpload\(draft, uploadedItem, scope\), api\)/);
  assert.match(app, /onHomeComposerDraftPatch=\{patchHomeComposerDraft\}/);
  assert.match(app, /homeDraftStore\.patch\(client, workspaceHash, updater\)/);
});

// 触发场景:打开首页 / 会话,旧版本留下的几 MB 草稿全文(没有 composer_content)被折叠成文件块,
// 上传要好几秒。
// 期望行为:restoreComposerDraft 以 legacyDraft 调用分类,暂存前登记 legacyFoldGuardRef;首页草稿
// 写入、会话草稿定时保存、切走时的收尾保存三处都在 legacyFoldUploadPending 为真时跳过,
// 服务端草稿里的旧全文保留到块拿到 id。
// 回归 bug 表现:折叠后 250~350ms 草稿就被覆盖成「空文本 + 无 id 附件」,上传完成前刷新 / 关页 /
// 上传失败,卡片显示「已丢失」,旧草稿全文不可恢复。
run('ChatView:旧长文本折叠的文件块上传完成前不覆盖服务端草稿', () => {
  const restore = between(chatView, 'const restoreComposerDraft = useCallback(', 'const clearAttachmentReservations');
  assert.match(restore, /legacyFoldGuardRef\.current = null/);
  assert.match(restore, /handleLargeTextPasteRef\.current\(storedText, \{ legacyDraft: true \}\)/);

  const classify = between(chatView, 'const handleLargeTextPaste = useStableCallback(', 'handleLargeTextPasteRef.current = handleLargeTextPaste');
  before(classify, 'if (legacyDraft) legacyFoldGuardRef.current =', 'stagePastedTextFiles(reservedFiles',
    'guard must be registered before staging triggers the home draft write');

  const change = between(chatView, 'const handleComposerChange = useCallback(', 'const restoreComposerDraft = useCallback(');
  before(change, 'legacyFoldUploadPending(legacyFoldGuardRef.current, snapshot.composer_content)', 'onHomeComposerDraftChange?.(');

  const cleanup = between(chatView, 'if (!targetSid || !targetKey || !composerDirtyRef.current) return;', '};');
  before(cleanup, 'legacyFoldUploadPending(legacyFoldGuardRef.current, content)', 'persistDraftValue(');

  const autosave = between(chatView, 'const content = reconcileComposerContentAttachments(composerContent, composerAttachments);', 'const timer = setTimeout(');
  assert.match(autosave, /if \(legacyFoldUploadPending\(legacyFoldGuardRef\.current, content\)\) return undefined;/);
});

// 回归守卫:被否决的方案(超过预算就把块从草稿里剥掉、只放内存缓存,刷新即丢)不得回流。
run('被否决的「剥块 + 内存缓存」方案不在源码里', () => {
  const root = fileURLToPath(new URL('..', import.meta.url));
  const offenders = [];
  const walk = (dir) => {
    for (const name of readdirSync(dir)) {
      const path = join(dir, name);
      if (statSync(path).isDirectory()) {
        if (name !== 'node_modules') walk(path);
        continue;
      }
      if (!/\.(jsx?|mjs)$/.test(name) || name === 'pastedTextArchitecture.test.js') continue;
      const text = readFileSync(path, 'utf8');
      if (/strippedPasteCache|composerContentForDraftPersistence/.test(text)) offenders.push(path);
    }
  };
  walk(root);
  assert.deepEqual(offenders, []);
});

// 触发场景:切进 f300(一条 2400 万字符的旧消息),或在后台任务面板打开子会话里的文件块。
// 期望行为:气泡只接收预览文本;读取附件正文走 context 提供的 loader(远程 Web 需要
// token 头),主会话与子会话各自提供自己连接的 loader。
run('Message.jsx 只渲染预览文本,附件正文 loader 由 ChatView / SubagentPanel 提供', () => {
  assert.match(message, /<UserMessageBody content=\{messagePreview\.text\} \/>/);
  assert.doesNotMatch(message, /<UserMessageBody content=\{content\}/);
  assert.match(message, /useContext\(AttachmentTextLoaderContext\)/);
  assert.match(chatView, /<AttachmentTextLoaderContext\.Provider value=\{api\.readAttachmentText\}>/);
  assert.match(subagentPanel, /<AttachmentTextLoaderContext\.Provider value=/);
  // 输入框的卡片对话框不在 transcript 的 Provider 里,loader 显式传入。
  assert.match(chatView, /attachmentTextLoader: api\.readAttachmentText/);
  assert.match(inputBar, /loader=\{attachmentTextLoader\}/);
});
