import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

function source(relativePath) {
  return fs.readFileSync(path.join(srcRoot, relativePath), 'utf8').replace(/\r\n?/g, '\n');
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

test('markdown classifies thread:// before rejecting unknown schemes', () => {
  const fileLink = source('lib/fileLink.js');
  const classify = fileLink.slice(
    fileLink.indexOf('export function classifyFileLink'),
    fileLink.indexOf('// Windows 盘符绝对路径'),
  );
  assert.match(classify, /parseThreadLink\(href\)/);
  assert.ok(
    classify.indexOf('parseThreadLink') < classify.indexOf('WIN_ABS') || !classify.includes('WIN_ABS'),
    'thread links must be recognized before generic scheme rejection',
  );
});

test('markdown renders session chips with NewSession icon and data attributes', () => {
  const markdown = source('lib/markdown.js');
  assert.match(markdown, /info\.kind === 'session'/);
  assert.match(markdown, /data-session-id/);
  assert.match(markdown, /ace-cmd-token ace-thread-link/);
  assert.match(markdown, /NewSession\.svg/);
  assert.match(markdown, /md\.linkify\.add\('thread:'/);
});

test('App intercepts session chips and opens the target session', () => {
  const app = source('App.jsx');
  assert.match(app, /import \{ threadSessionTargetFromClickEvent \} from '\.\/lib\/fileLink\.js';/);
  assert.match(app, /threadSessionTargetFromClickEvent\(event\)/);
  assert.match(app, /resumeAndOpenSession\(target\)/);
  assert.match(app, /window\.addEventListener\('click', onClick, true\)/);
});
