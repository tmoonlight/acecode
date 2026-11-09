import assert from 'node:assert/strict';
import { enqueueQueuedInput } from './chatInputQueue.js';
import { composerContentFromText, composerContentText } from './composerContent.js';
import { prependComposerContent, withdrawnSteerComposerState } from './steerWithdrawal.js';

function run(name, test) { test(); console.log(`[pass] ${name}`); }

function queuedItem(payload, sessionId = 's1') {
  return enqueueQueuedInput(null, { sessionId, payload }).items[0];
}

run('撤回插话:输入框空着时退回的消息原样回到输入框', () => {
  // 场景:回复中按 Enter 插了一句「改成蓝色」,模型还没读到就点了气泡上的叉。
  // 期望:这句话回到输入框,而不是消失(回归:曾经撤回后直接删掉,要重打一遍)。
  const item = queuedItem({ text: '改成蓝色', composer_content: composerContentFromText('改成蓝色') });
  const state = withdrawnSteerComposerState(item, { currentText: '' });
  assert.equal(composerContentText(state.content), '改成蓝色');
  assert.deepEqual(state.resources, []);
  assert.deepEqual(state.contexts, []);
});

run('撤回插话:输入框已有内容时退回的消息放前面,换行接原内容', () => {
  // 场景:撤回前已经在输入框里打了下一句。
  // 期望:与 Claude Code 把排队消息拉回输入框一致——退回的在前,换一行接现有内容,两段都不丢。
  const item = queuedItem({ text: '先跑测试' });
  const state = withdrawnSteerComposerState(item, {
    currentContent: composerContentFromText('再看构建'),
  });
  assert.equal(composerContentText(state.content), '先跑测试\n再看构建');
});

run('撤回插话:附件块与上下文芯片一起回来,同一个芯片不重复', () => {
  // 场景:插话带了一张已上传的图片和一个浏览器上下文芯片;输入框里正好也挂着同一个芯片。
  const content = {
    version: 1,
    parts: [
      { type: 'attachment', key: 'img-1', id: 'att-9', name: 'shot.png', kind: 'image', mime_type: 'image/png' },
      { type: 'text', text: '看这张图' },
    ],
  };
  const browser = { type: 'browser', id: 'ctx-1', local_id: 'ctx-1', content: 'page' };
  const item = queuedItem({
    text: '看这张图',
    attachments: [{ id: 'att-9' }],
    contexts: [browser, { type: 'selection', content: '选中的段落' }],
    composer_content: content,
  }, 's1');
  const state = withdrawnSteerComposerState(item, {
    currentText: '',
    currentContexts: [browser],
    sessionId: 's1',
  });
  // 附件块保留在内容里,并给出能在输入框显示缩略图的资源(带本会话的 blob 地址)。
  assert.equal(state.content.parts[0].type, 'attachment');
  assert.equal(state.resources.length, 1);
  assert.equal(state.resources[0].id, 'att-9');
  assert.equal(state.resources[0].local_id, 'img-1');
  assert.ok(state.resources[0].blob_url.includes('/api/sessions/s1/attachments/att-9/blob'));
  // 芯片:退回的在前,已在输入框里的同一芯片不再重复;没有 id 的芯片补一个本地 id。
  assert.equal(state.contexts.length, 2);
  assert.equal(state.contexts[0].id, 'ctx-1');
  assert.ok(state.contexts[1].local_id);
});

run('拼接规则:任一侧为空就取另一侧,只有附件也算有内容', () => {
  const text = (value) => composerContentText(value);
  assert.equal(text(prependComposerContent(composerContentFromText(''), composerContentFromText('b'))), 'b');
  assert.equal(text(prependComposerContent(composerContentFromText('a'), null)), 'a');
  // 场景:输入框里只有一个附件(没文字) → 不算空,退回的文字放前面并换行,附件保留。
  const attachmentOnly = { version: 1, parts: [{ type: 'attachment', key: 'k', id: 'f1', name: 'a.txt', kind: 'file' }] };
  const merged = prependComposerContent(composerContentFromText('前'), attachmentOnly);
  assert.equal(merged.parts.at(-1).type, 'attachment');
  assert.equal(text(merged), '前\n');
  // 拿不到消息(条目已不存在)→ 不改输入框。
  assert.equal(withdrawnSteerComposerState(null), null);
});
