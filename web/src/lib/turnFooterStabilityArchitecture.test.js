import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

function source(relativePath) {
  return fs.readFileSync(path.join(srcRoot, relativePath), 'utf8');
}

function between(text, start, end) {
  const startIndex = text.indexOf(start);
  const endIndex = text.indexOf(end, startIndex);
  assert.notEqual(startIndex, -1, `missing start marker: ${start}`);
  assert.notEqual(endIndex, -1, `missing end marker: ${end}`);
  return text.slice(startIndex, endIndex);
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

run('ChatView defers only the active turn footer from the live busy state', () => {
  const chat = source('components/ChatView.jsx');
  assert.match(
    chat,
    /buildAssistantRunDirectives\(renderedItems, \{ deferLastFooter: busy \}\)/,
  );
  assert.match(chat, /\[renderedItems, busy\]/);
});

run('completion summary renders its entire footer only when it owns the settled turn', () => {
  const renderer = source('components/TranscriptItems.jsx');
  const completionBlock = between(
    renderer,
    'export function CompletionSummaryBlock({',
    'export function TerminationNoticeBlock',
  );

  assert.match(completionBlock, /showFooter = true/);
  assert.match(completionBlock, /\{showFooter && \(\s*<div className="min-h-6 flex items-center gap-1">/);
  assert.match(completionBlock, /<MessageActions/);
  assert.match(
    renderer,
    /showFooter=\{!nested && capabilities\.showMessageFooters && directive\?\.showFooter === true\}/,
  );
});

run('terminal notice and error rows reuse message actions only as terminal owners', () => {
  const renderer = source('components/TranscriptItems.jsx');
  const message = source('components/Message.jsx');
  const terminationBlock = between(
    renderer,
    'export function TerminationNoticeBlock({',
    'function TranscriptItem',
  );
  const errorRow = between(
    message,
    'function ErrorRow({',
    'export const Message',
  );

  assert.match(terminationBlock, /showFooter = false/);
  assert.match(terminationBlock, /<MessageActions/);
  assert.match(terminationBlock, /messageId=\{forkMessageId\}/);
  assert.match(errorRow, /\{showFooter && \(/);
  assert.match(errorRow, /<MessageActions/);
  assert.match(
    renderer,
    /const directive = nested \? undefined : directives\.get\(item\.id\)/,
  );
  assert.match(
    renderer,
    /showFooter=\{!nested && capabilities\.showMessageFooters && directive\?\.showFooter === true\}/,
  );
});

run('expanded activity details never create an intermediate turn footer', () => {
  const expandedItems = source('components/TranscriptItems.jsx');

  assert.match(expandedItems, /<TranscriptItems[\s\S]*?nested/);
  assert.match(expandedItems, /showFooter=\{!nested && capabilities\.showMessageFooters/);
  assert.match(expandedItems, /const showFooter = !nested[\s\S]*?capabilities\.showMessageFooters/);
});

console.log('turnFooterStabilityArchitecture tests passed');
