import assert from 'node:assert/strict';
import { flattenCommands } from './slashCommands.js';
import {
  COMPOSER_ATTACHMENT_TAG,
  COMPOSER_COMMAND_TAG,
  COMPOSER_EXTERNAL_SYNC_ACTIONS,
  COMPOSER_PATH_TAG,
  COMPOSER_SESSION_TAG,
  appendComposerLocalEcho,
  classifyComposerExternalSync,
  clipboardHasRichText,
  clipboardHasTextFormat,
  composerAdjacentAttachmentKey,
  composerAdjacentTagDeletionRange,
  composerAttachmentItemsSignature,
  composerAttachmentTagsSignature,
  composerDocumentFromText,
  composerDocumentWithSynchronizedAttachments,
  composerDocumentWithSynchronizedLeadingCommand,
  composerPlainTextOffsetForPoint,
  composerPlainTextRangeFromSelection,
  composerPointForPlainTextOffset,
  composerSelectionFromPlainTextRange,
  composerTextFromDocument,
  isComposerImageAttachment,
  normalizeComposerPlainText,
  plainTextFromClipboardData,
  plainTextFromClipboardHtml,
  richComposerModelFromText,
  richComposerTextFromModel,
} from './richComposerModel.js';
import { formatSessionReferenceToken } from './sessionReference.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

const COMMANDS = flattenCommands({
  builtins: [
    { name: 'init', description: 'Generate AGENT.md' },
    { name: 'goal', description: 'Manage thread goal' },
  ],
  commands: [{ name: 'opsx-apply', description: 'Apply OpenSpec change' }],
  skills: [{ name: 'openspec-explore', description: 'Explore a change' }],
});

run('normalizeComposerPlainText normalizes CRLF and CR to LF', () => {
  assert.equal(normalizeComposerPlainText('a\r\nb\rc'), 'a\nb\nc');
});

run('rich composer model round-trips empty and multiline plain text', () => {
  assert.equal(richComposerTextFromModel(richComposerModelFromText('', COMMANDS)), '');
  const text = 'first\nsecond\nthird';
  assert.deepEqual(richComposerModelFromText(text, COMMANDS), {
    kind: 'plain',
    text,
    command: null,
    rest: text,
  });
  assert.equal(richComposerTextFromModel(richComposerModelFromText(text, COMMANDS)), text);
});

run('recognized leading command becomes command model and serializes with slash', () => {
  const model = richComposerModelFromText('/openspec-explore investigate input', COMMANDS);
  assert.equal(model.kind, 'command');
  assert.equal(model.command.token, '/openspec-explore');
  assert.equal(model.command.name, 'openspec-explore');
  assert.equal(model.rest, ' investigate input');
  assert.equal(richComposerTextFromModel(model), '/openspec-explore investigate input');
});

run('recognized opencode command keeps command kind', () => {
  const model = richComposerModelFromText('/opsx-apply change-123', COMMANDS);
  assert.equal(model.kind, 'command');
  assert.equal(model.command.name, 'opsx-apply');
  assert.equal(model.command.kind, 'command');
  assert.equal(model.rest, ' change-123');
});

run('unknown leading command remains plain text', () => {
  const text = '/not-known investigate input';
  const model = richComposerModelFromText(text, COMMANDS);
  assert.equal(model.kind, 'plain');
  assert.equal(model.command, null);
  assert.equal(richComposerTextFromModel(model), text);
});

run('composer external sync defers stale controlled text through IME settlement', () => {
  const baseline = 'asd';
  const committed = 'asd中文';
  assert.deepEqual(classifyComposerExternalSync({
    compositionProtected: true,
    currentText: committed,
    nextText: baseline,
    lastExternalText: baseline,
  }), {
    action: COMPOSER_EXTERNAL_SYNC_ACTIONS.DEFER,
    acknowledgedEchoCount: 0,
  });

  const localEchoes = appendComposerLocalEcho([], committed);
  assert.deepEqual(classifyComposerExternalSync({
    currentText: committed,
    nextText: baseline,
    lastExternalText: baseline,
    localEchoes,
  }), {
    action: COMPOSER_EXTERNAL_SYNC_ACTIONS.IGNORE_STALE,
    acknowledgedEchoCount: 0,
  });
  assert.deepEqual(classifyComposerExternalSync({
    currentText: committed,
    nextText: committed,
    lastExternalText: baseline,
    localEchoes,
  }), {
    action: COMPOSER_EXTERNAL_SYNC_ACTIONS.ACCEPT,
    acknowledgedEchoCount: 1,
  });
});

run('composer external sync ignores earlier rapid local echoes without losing the latest text', () => {
  let localEchoes = [];
  localEchoes = appendComposerLocalEcho(localEchoes, '中');
  localEchoes = appendComposerLocalEcho(localEchoes, '中文');
  localEchoes = appendComposerLocalEcho(localEchoes, '中文输入');

  assert.deepEqual(classifyComposerExternalSync({
    currentText: '中文输入',
    nextText: '中文',
    lastExternalText: '',
    localEchoes,
  }), {
    action: COMPOSER_EXTERNAL_SYNC_ACTIONS.IGNORE_STALE,
    acknowledgedEchoCount: 2,
  });
  assert.deepEqual(appendComposerLocalEcho(['一', '二'], '三', 2), ['二', '三']);
});

run('composer external sync makes the latest session generation authoritative', () => {
  assert.equal(classifyComposerExternalSync({
    generationChanged: true,
    currentText: '会话 A 草稿',
    nextText: '会话 B 草稿',
    lastExternalText: '会话 A 草稿',
    localEchoes: ['会话 A 新输入'],
  }).action, COMPOSER_EXTERNAL_SYNC_ACTIONS.REPLACE);

  assert.equal(classifyComposerExternalSync({
    generationChanged: true,
    currentText: '相同文字',
    nextText: '相同文字',
  }).action, COMPOSER_EXTERNAL_SYNC_ACTIONS.REPLACE);

  assert.equal(classifyComposerExternalSync({
    compositionProtected: true,
    generationChanged: true,
    currentText: '旧会话 composition',
    nextText: '',
  }).action, COMPOSER_EXTERNAL_SYNC_ACTIONS.DEFER);
  assert.equal(classifyComposerExternalSync({
    generationChanged: true,
    currentText: '旧会话 composition',
    nextText: '',
  }).action, COMPOSER_EXTERNAL_SYNC_ACTIONS.REPLACE);
});

run('composer external sync accepts unchanged English and replaces novel external drafts', () => {
  assert.equal(classifyComposerExternalSync({
    currentText: 'plain English',
    nextText: 'plain English',
  }).action, COMPOSER_EXTERNAL_SYNC_ACTIONS.ACCEPT);
  assert.equal(classifyComposerExternalSync({
    currentText: 'local draft',
    nextText: 'external draft',
    lastExternalText: 'previous draft',
  }).action, COMPOSER_EXTERNAL_SYNC_ACTIONS.REPLACE);
});

run('composer image classification respects explicit attachment kind before MIME fallback', () => {
  assert.equal(isComposerImageAttachment({ kind: 'image', mime_type: 'image/png' }), true);
  assert.equal(isComposerImageAttachment({ mime_type: 'image/jpeg' }), true);
  assert.equal(isComposerImageAttachment({ kind: 'file', mime_type: 'image/svg+xml' }), false);
  assert.equal(isComposerImageAttachment({ kind: 'file', mime_type: 'application/pdf' }), false);
});

run('Slate composer document round-trips command, path tags, and multiline text', () => {
  const text = '/openspec-explore inspect @src/main.cpp now\nthen @"docs/file name.md" and @"C:/Outside Folder/"';
  const document = composerDocumentFromText(text, COMMANDS);
  assert.equal(composerTextFromDocument(document), text);
  assert.equal(document.length, 2);
  assert.equal(document[0].children.some((child) => child.type === COMPOSER_COMMAND_TAG), true);
  assert.equal(document[0].children.some((child) => child.type === COMPOSER_PATH_TAG), true);
  const quoted = document[1].children.find((child) => child.type === COMPOSER_PATH_TAG);
  assert.equal(quoted.token, '@"docs/file name.md"');
  assert.equal(quoted.path, 'docs/file name.md');
  const absolute = document[1].children.filter((child) => child.type === COMPOSER_PATH_TAG)[1];
  assert.equal(absolute.token, '@"C:/Outside Folder/"');
  assert.equal(absolute.path, 'C:/Outside Folder/');
  assert.equal(absolute.directory, true);
});

run('attachment tags stay in Slate while contributing zero plain-text characters', () => {
  const attachments = [
    {
      local_id: 'local-image',
      name: 'screen.png',
      kind: 'image',
      preview_url: 'blob:screen',
      source_path: 'C:/repo/screen.png',
      uploading: true,
    },
    {
      id: 'att-notes',
      name: 'notes.txt',
      kind: 'file',
      blob_url: '/api/attachments/att-notes/blob',
    },
  ];
  const text = '/init inspect @src/main.cpp';
  const document = composerDocumentFromText(text, COMMANDS, attachments);
  const tags = document[0].children.filter((child) => child.type === COMPOSER_ATTACHMENT_TAG);

  assert.equal(composerTextFromDocument(document), text);
  assert.deepEqual(tags.map((tag) => tag.attachmentKey), ['local-image', 'att-notes']);
  assert.equal(tags[0].uploading, true);
  assert.equal(tags[0].sourcePath, 'C:/repo/screen.png');
  assert.equal(composerAttachmentTagsSignature(document), composerAttachmentItemsSignature(attachments));
});

run('attachment synchronization preserves live unfinished text and updates tag metadata', () => {
  const liveDocument = composerDocumentFromText('continue @src/', COMMANDS);
  const uploading = [{ local_id: 'local-1', name: 'report.md', kind: 'file', uploading: true }];
  const withUpload = composerDocumentWithSynchronizedAttachments(liveDocument, uploading);
  const ready = [{
    local_id: 'local-1',
    id: 'att-ready',
    name: 'report.md',
    kind: 'file',
    blob_url: '/api/attachments/att-ready/blob',
  }];
  const synchronized = composerDocumentWithSynchronizedAttachments(withUpload, ready);

  assert.equal(composerTextFromDocument(synchronized), 'continue @src/');
  assert.equal(synchronized[0].children.some((child) => child.type === COMPOSER_PATH_TAG), false);
  const tag = synchronized[0].children.find((child) => child.type === COMPOSER_ATTACHMENT_TAG);
  assert.equal(tag.attachmentId, 'att-ready');
  assert.equal(tag.uploading, false);
  assert.equal(composerAttachmentTagsSignature(synchronized), composerAttachmentItemsSignature(ready));
});

run('offset zero lands after leading attachments and Backspace resolves the nearest key', () => {
  const attachments = [
    { id: 'att-first', name: 'first.txt', kind: 'file' },
    { id: 'att-last', name: 'last.png', kind: 'image' },
  ];
  const document = composerDocumentFromText('type here', COMMANDS, attachments);
  const caret = composerSelectionFromPlainTextRange(document, 0, 0);

  assert.equal(composerPlainTextOffsetForPoint(document, caret.anchor), 0);
  assert.equal(composerAdjacentAttachmentKey(document, caret, 'backward'), 'att-last');
  assert.equal(composerAdjacentAttachmentKey(document, caret, 'forward'), '');
  const lastAttachmentIndex = document[0].children
    .map((child) => child.type)
    .lastIndexOf(COMPOSER_ATTACHMENT_TAG);
  assert.ok(caret.anchor.path[1] > lastAttachmentIndex);
});

run('Slate composer document round-trips a stable session tag', () => {
  const token = formatSessionReferenceToken({
    id: 'session-1',
    title: '修复引用菜单',
    workspace_hash: 'workspace-a',
    workspaceName: 'ACECode',
  }, { trailingSpace: false });
  const text = `compare ${token} now`;
  const document = composerDocumentFromText(text, COMMANDS);
  assert.equal(composerTextFromDocument(document), text);
  const tag = document[0].children.find((child) => child.type === COMPOSER_SESSION_TAG);
  assert.ok(tag);
  assert.equal(tag.sessionId, 'session-1');
  assert.equal(tag.workspaceHash, 'workspace-a');
  assert.equal(tag.title, '修复引用菜单');
  assert.equal(tag.workspaceName, 'ACECode');
});

run('session tags use the existing atomic deletion behavior', () => {
  const token = formatSessionReferenceToken({
    id: 'session-2',
    title: '上下文来源',
    no_workspace: true,
    workspace_name: '任务',
  }, { trailingSpace: false });
  const text = `use ${token} next`;
  const document = composerDocumentFromText(text, COMMANDS);
  const start = text.indexOf(token);
  const before = composerSelectionFromPlainTextRange(document, start, start);
  assert.deepEqual(composerAdjacentTagDeletionRange(document, before, 'forward'), {
    start,
    end: start + token.length + 1,
  });
});

run('Slate composer keeps unfinished directory queries and non-reference at-signs editable', () => {
  const unfinished = composerDocumentFromText('continue @src/', COMMANDS);
  const email = composerDocumentFromText('mail a@b.com', COMMANDS);
  assert.equal(composerTextFromDocument(unfinished), 'continue @src/');
  assert.equal(composerTextFromDocument(email), 'mail a@b.com');
  assert.equal(unfinished.flatMap((block) => block.children).some((child) => child.type === COMPOSER_PATH_TAG), false);
  assert.equal(email.flatMap((block) => block.children).some((child) => child.type === COMPOSER_PATH_TAG), false);
});

run('leading command synchronization preserves an unfinished path query as text', () => {
  const text = '/openspec-explore continue @src/';
  const liveDocument = [{
    type: 'paragraph',
    children: [{ text }],
  }];
  const synchronized = composerDocumentWithSynchronizedLeadingCommand(liveDocument, text, COMMANDS);
  assert.equal(composerTextFromDocument(synchronized), text);
  assert.equal(synchronized[0].children.some((child) => child.type === COMPOSER_COMMAND_TAG), true);
  assert.equal(synchronized[0].children.some((child) => child.type === COMPOSER_PATH_TAG), false);
});

run('plain-text offsets clamp around inline void tags and preserve newlines', () => {
  const text = '/openspec-explore @src/main.cpp\nnext';
  const document = composerDocumentFromText(text, COMMANDS);
  const commandEnd = '/openspec-explore'.length;
  const pathStart = commandEnd + 1;
  const pathEnd = pathStart + '@src/main.cpp'.length;
  const secondLineStart = pathEnd + 1;

  const beforeCommand = composerPointForPlainTextOffset(document, 0, 'backward');
  assert.equal(composerPlainTextOffsetForPoint(document, beforeCommand), 0);
  const insideCommandBackward = composerPointForPlainTextOffset(document, 4, 'backward');
  const insideCommandForward = composerPointForPlainTextOffset(document, 4, 'forward');
  assert.equal(composerPlainTextOffsetForPoint(document, insideCommandBackward), 0);
  assert.equal(composerPlainTextOffsetForPoint(document, insideCommandForward), commandEnd);

  const insidePathBackward = composerPointForPlainTextOffset(document, pathStart + 2, 'backward');
  const insidePathForward = composerPointForPlainTextOffset(document, pathStart + 2, 'forward');
  assert.equal(composerPlainTextOffsetForPoint(document, insidePathBackward), pathStart);
  assert.equal(composerPlainTextOffsetForPoint(document, insidePathForward), pathEnd);
  assert.equal(
    composerPlainTextOffsetForPoint(document, composerPointForPlainTextOffset(document, secondLineStart)),
    secondLineStart,
  );
});

run('plain-text range mapping preserves collapsed and backward selections around tags', () => {
  const text = '/openspec-explore @src/main.cpp';
  const document = composerDocumentFromText(text, COMMANDS);
  const pathStart = '/openspec-explore '.length;
  const pathEnd = text.length;

  const collapsed = composerSelectionFromPlainTextRange(document, pathStart + 2, pathStart + 2);
  assert.deepEqual(collapsed.anchor, collapsed.focus);
  assert.equal(composerPlainTextOffsetForPoint(document, collapsed.anchor), pathEnd);

  const backward = composerSelectionFromPlainTextRange(document, pathStart, pathEnd, 'backward');
  assert.deepEqual(composerPlainTextRangeFromSelection(document, backward), {
    start: pathStart,
    end: pathEnd,
    direction: 'backward',
  });
});

run('atomic deletion range includes a tag and its following separator', () => {
  const text = '/openspec-explore continue @src/main.cpp now';
  const document = composerDocumentFromText(text, COMMANDS);
  const commandEnd = '/openspec-explore'.length;
  const afterCommandSpace = composerSelectionFromPlainTextRange(
    document,
    commandEnd + 1,
    commandEnd + 1,
  );
  assert.deepEqual(composerAdjacentTagDeletionRange(
    document,
    afterCommandSpace,
    'backward',
  ), {
    start: 0,
    end: commandEnd + 1,
  });

  const pathStart = text.indexOf('@src/main.cpp');
  const beforePath = composerSelectionFromPlainTextRange(document, pathStart, pathStart);
  assert.deepEqual(composerAdjacentTagDeletionRange(document, beforePath, 'forward'), {
    start: pathStart,
    end: pathStart + '@src/main.cpp '.length,
  });
});

run('clipboard rich text detection is based on clipboard types', () => {
  assert.equal(clipboardHasRichText({ types: ['text/plain'] }), false);
  assert.equal(clipboardHasRichText({ types: ['text/html', 'text/plain'] }), true);
  assert.equal(clipboardHasRichText({ types: ['TEXT/RTF'] }), true);
});

run('plainTextFromClipboardData prefers text/plain and normalizes newlines', () => {
  const clipboardData = {
    types: ['text/html', 'text/plain'],
    getData(type) {
      if (type === 'text/html') return '<b>hello</b>';
      if (type === 'text/plain') return 'hello\r\nworld';
      return '';
    },
  };
  assert.equal(plainTextFromClipboardData(clipboardData), 'hello\nworld');
});

run('Slate composer defers a command tag until the command has a whitespace boundary', () => {
  const unfinished = composerDocumentFromText('/goal', COMMANDS);
  const continued = composerDocumentFromText('/goal继续输入', COMMANDS);
  const committed = composerDocumentFromText('/goal ', COMMANDS);
  const committedWithTab = composerDocumentFromText('/goal\tcontinue', COMMANDS);
  const committedWithBreak = composerDocumentFromText('/goal\ncontinue', COMMANDS);

  assert.equal(unfinished[0].children.some((child) => child.type === COMPOSER_COMMAND_TAG), false);
  assert.equal(continued[0].children.some((child) => child.type === COMPOSER_COMMAND_TAG), false);
  assert.equal(committed[0].children.some((child) => child.type === COMPOSER_COMMAND_TAG), true);
  assert.equal(committedWithTab[0].children.some((child) => child.type === COMPOSER_COMMAND_TAG), true);
  assert.equal(committedWithBreak[0].children.some((child) => child.type === COMPOSER_COMMAND_TAG), true);
  assert.equal(composerTextFromDocument(unfinished), '/goal');
  assert.equal(composerTextFromDocument(continued), '/goal继续输入');
  assert.equal(composerTextFromDocument(committed), '/goal ');
  assert.equal(composerTextFromDocument(committedWithTab), '/goal\tcontinue');
  assert.equal(composerTextFromDocument(committedWithBreak), '/goal\ncontinue');
});

run('leading command synchronization commits and restores tags at the whitespace boundary', () => {
  const plain = [{ type: 'paragraph', children: [{ text: '/goal ' }] }];
  const committed = composerDocumentWithSynchronizedLeadingCommand(plain, '/goal ', COMMANDS);
  assert.equal(committed[0].children.some((child) => child.type === COMPOSER_COMMAND_TAG), true);
  assert.equal(composerTextFromDocument(committed), '/goal ');

  const withoutBoundary = structuredClone(committed);
  withoutBoundary[0].children[withoutBoundary[0].children.length - 1].text = '';
  const uncommitted = composerDocumentWithSynchronizedLeadingCommand(
    withoutBoundary,
    '/goal',
    COMMANDS,
  );
  assert.equal(uncommitted[0].children.some((child) => child.type === COMPOSER_COMMAND_TAG), false);
  assert.equal(composerTextFromDocument(uncommitted), '/goal');
});

run('clipboard text format detection covers Windows-compatible text payloads', () => {
  assert.equal(clipboardHasTextFormat({ types: ['text/plain'] }), true);
  assert.equal(clipboardHasTextFormat({ types: ['TEXT'] }), true);
  assert.equal(clipboardHasTextFormat({ types: ['text/html'] }), true);
  assert.equal(clipboardHasTextFormat({ types: ['image/png'] }), false);
});

run('plainTextFromClipboardData falls back to the legacy Windows text alias', () => {
  const clipboardData = {
    types: ['text/plain'],
    getData(type) {
      if (type === 'text/plain') throw new Error('first format unavailable');
      if (type === 'text') return 'first\rsecond';
      return '';
    },
  };
  assert.equal(plainTextFromClipboardData(clipboardData), 'first\nsecond');
});

run('HTML-only clipboard data is converted to normalized plain text', () => {
  const textNode = (nodeValue) => ({ nodeType: 3, nodeValue });
  const elementNode = (tagName, childNodes = []) => ({ nodeType: 1, tagName, childNodes });
  const clipboardData = {
    types: ['text/html'],
    getData(type) {
      if (type === 'text/html') return '<div>styled<br>line</div>';
      return '';
    },
  };
  assert.equal(clipboardHasRichText(clipboardData), true);
  assert.equal(plainTextFromClipboardData(clipboardData, {
    parseHtml(html) {
      assert.equal(html, '<div>styled<br>line</div>');
      return {
        body: elementNode('BODY', [
          elementNode('DIV', [textNode('styled'), elementNode('BR'), textNode('line')]),
        ]),
      };
    },
  }), 'styled\nline');
});

run('HTML clipboard conversion preserves block boundaries and ignores active content', () => {
  const textNode = (nodeValue) => ({ nodeType: 3, nodeValue });
  const elementNode = (tagName, childNodes = []) => ({ nodeType: 1, tagName, childNodes });
  const parseHtml = () => ({
    body: elementNode('BODY', [
      elementNode('DIV', [textNode('first')]),
      elementNode('P', [textNode('second'), elementNode('BR'), textNode('line')]),
      elementNode('SCRIPT', [textNode('doNotPaste()')]),
    ]),
  });

  assert.equal(plainTextFromClipboardHtml('<ignored>', { parseHtml }), 'first\nsecond\nline');
});

run('plainTextFromClipboardHtml fails closed when parsing is unavailable', () => {
  assert.equal(plainTextFromClipboardHtml('<b>styled</b>', { parseHtml: () => null }), '');
  assert.equal(plainTextFromClipboardHtml('<b>styled</b>', {
    parseHtml() { throw new Error('invalid clipboard html'); },
  }), '');
});
