import assert from 'node:assert/strict';
import {
  createAgentBrowserConsoleContext,
  createAgentBrowserElementContext,
} from './agentBrowserChatContext.js';
import { normalizeComposerContext } from './selectionChatContext.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('selected browser elements become bounded structured composer contexts', () => {
  const context = createAgentBrowserElementContext({
    name: 'button#submit.primary',
    url: 'https://example.com/form',
    outerHTML: '<button id="submit" class="primary">Send</button>',
    innerText: 'Send',
    computedStyle: 'display: inline-block;',
    ancestors: [
      { tagName: 'html' },
      { tagName: 'body' },
      { tagName: 'button', id: 'submit', classNames: ['primary'] },
    ],
    dimensions: { top: 10.2, left: 20.8, width: 100, height: 32 },
  }, { page_id: 'page-1', title: 'Example' }, 100);
  assert.equal(context.type, 'browser');
  assert.equal(context.kind, 'element');
  assert.equal(context.label, 'button#submit.primary');
  assert.equal(context.source.page_id, 'page-1');
  assert.match(context.content, /html > body > button#submit\.primary/);
  assert.match(context.content, /Outer HTML/);
  assert.match(context.content, /width: 100px/);
  assert.equal(normalizeComposerContext(context).content, context.content);
});

run('console snapshots become composer contexts and empty logs are ignored', () => {
  assert.equal(createAgentBrowserConsoleContext({ logs: '' }), null);
  const context = createAgentBrowserConsoleContext({
    page_id: 'page-2',
    url: 'https://example.com',
    title: 'Example',
    logs: '[log] ready\n[error] failed',
  }, 200);
  assert.equal(context.kind, 'console');
  assert.equal(context.label, '控制台日志');
  assert.match(context.content, /\[error\] failed/);
  assert.equal(normalizeComposerContext(context).source.url, 'https://example.com');
});
