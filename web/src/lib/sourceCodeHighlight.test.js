import assert from 'node:assert/strict';
import { langForFile } from './lang.js';
import { highlightSourceForFile } from './sourceCodeHighlight.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('代码文件按扩展名生成 highlight.js token', () => {
  const cpp = highlightSourceForFile('src/example.cpp', 'const bool ready = true;');
  assert.equal(cpp.language, 'cpp');
  assert.match(cpp.html, /class="hljs-/);

  const java = highlightSourceForFile('Main.java', 'public class Main {}');
  assert.equal(java.language, 'java');
  assert.match(java.html, /class="hljs-/);
});

run('纯文本和未知文件只转义原文且不自动着色', () => {
  for (const path of ['notes.txt', 'README.unknown']) {
    const plain = highlightSourceForFile(path, '<script>alert("x")</script>');
    assert.equal(plain.language, '');
    assert.equal(plain.html, '&lt;script&gt;alert("x")&lt;/script&gt;');
    assert.doesNotMatch(plain.html, /hljs-|<span/);
  }
});

run('特殊代码文件名和常见扩展映射到已注册语言', () => {
  assert.equal(langForFile('C:\\repo\\CMakeLists.txt'), 'cmake');
  assert.equal(langForFile('Dockerfile'), 'dockerfile');
  assert.equal(langForFile('styles/main.scss'), 'scss');
  assert.equal(langForFile('notes.TXT'), '');
});
