import assert from 'node:assert/strict';
import { fromMarkdown } from 'mdast-util-from-markdown';
import { gfmFromMarkdown } from 'mdast-util-gfm';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

// Guards the patched mdast-util-gfm-autolink-literal dependency
// (see patches/mdast-util-gfm-autolink-literal@2.0.1.patch): the email
// autolink regex must keep working without a lookbehind so legacy WebKit
// engines (Safari < 16.4) can parse the module.
function extractLinks(markdown) {
  const tree = fromMarkdown(markdown, { mdastExtensions: [gfmFromMarkdown()] });
  const links = [];
  (function walk(nodes) {
    for (const node of nodes) {
      if (node.type === 'link') {
        links.push({ url: node.url, text: node.children.map((c) => c.value).join('') });
      }
      if (Array.isArray(node.children)) walk(node.children);
    }
  })(tree.children);
  return links;
}

run('GFM email autolink links after whitespace', () => {
  const links = extractLinks('联系 foo@bar.com 了解更多');
  assert.equal(links.length, 1);
  assert.equal(links[0].url, 'mailto:foo@bar.com');
  assert.equal(links[0].text, 'foo@bar.com');
});

run('GFM email autolink links at start of text', () => {
  const links = extractLinks('foo@bar.com');
  assert.equal(links.length, 1);
  assert.equal(links[0].url, 'mailto:foo@bar.com');
});

run('GFM email autolink links after punctuation', () => {
  for (const source of ['(foo@bar.com)', '（foo@bar.com）', '"foo@bar.com"']) {
    const links = extractLinks(source);
    assert.equal(links.length, 1, `expected link in ${source}`);
    assert.equal(links[0].url, 'mailto:foo@bar.com');
  }
});

run('GFM email autolink refuses slash-prefixed emails', () => {
  const links = extractLinks('see path/user@example.com for details');
  assert.deepEqual(links, []);
});

run('GFM email autolink refuses emails attached to CJK characters', () => {
  const links = extractLinks('中文foo@bar.com');
  assert.deepEqual(links, []);
});

run('GFM email autolink absorbs preceding latin letters into the address', () => {
  // Matches upstream behavior: the leftmost atext run is absorbed, and the
  // `^` branch of the original lookbehind allowed this case as well.
  const links = extractLinks('abcfoo@bar.com tail');
  assert.equal(links.length, 1);
  assert.equal(links[0].url, 'mailto:abcfoo@bar.com');
});

run('GFM URL autolink still works alongside email autolink', () => {
  const links = extractLinks('visit https://example.com or mail foo@bar.com');
  assert.equal(links.length, 2);
  assert.deepEqual(
    links.map((l) => l.url),
    ['https://example.com', 'mailto:foo@bar.com']
  );
});
