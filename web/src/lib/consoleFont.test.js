import assert from 'node:assert/strict';
import { CONSOLE_FONT_SIZE, resolveConsoleFont } from './consoleFont.js';

function run(name, fn) {
  fn();
  console.log(`[pass] ${name}`);
}

function fontDocument(measure) {
  let font;
  return {
    // Availability checks alone accept proportional substitutes on WebKit.
    fonts: { check() { throw new Error('Font availability is not a width test'); } },
    createElement(tag) {
      assert.equal(tag, 'canvas');
      return {
        getContext(type) {
          assert.equal(type, '2d');
          return {
            set font(value) { font = value; },
            measureText(character) {
              const [, weight, size, family] = /^(normal|bold) ([\d.]+)px (.+)$/.exec(font);
              return { width: measure(family, weight, character, Number(size)) };
            },
          };
        },
      };
    },
  };
}

const proportional = (_, __, character) => character === 'W' ? 11 : 4;

run('console font preserves a preferred face when normal and bold are monospaced', () => {
  const document = fontDocument((family, _, __, size) => {
    assert.equal(family, '"SF Mono"');
    assert.equal(size, CONSOLE_FONT_SIZE);
    return 8;
  });
  assert.equal(resolveConsoleFont(document), '"SF Mono"');
});

run('console font skips proportional substitutions and returns only the usable family', () => {
  const document = fontDocument((family, weight, character) =>
    family === '"DejaVu Sans Mono"' ? 8 : proportional(family, weight, character));
  assert.equal(resolveConsoleFont(document), '"DejaVu Sans Mono"');
});

run('console font uses a bare generic fallback when all named faces are unsuitable', () => {
  assert.equal(resolveConsoleFont(fontDocument(proportional)), 'monospace');
});

run('console font rejects proportional bold substitutions', () => {
  const document = fontDocument((family, weight, character) => {
    if (family === '"SF Mono"') return weight === 'normal' || character === 'W' ? 8 : 4;
    return family === '"Cascadia Code"' ? 7 : proportional(family, weight, character);
  });
  assert.equal(resolveConsoleFont(document), '"Cascadia Code"');
});

run('console font rejects different advances between normal and bold', () => {
  const document = fontDocument((family, weight) =>
    family === '"SF Mono"' ? (weight === 'bold' ? 9 : 8) : 7);
  assert.equal(resolveConsoleFont(document), '"Cascadia Code"');
});

run('console font checks punctuation and spaces as well as letters', () => {
  for (const badCharacter of [' ', '.', '[']) {
    const document = fontDocument((family, _, character) =>
      family === '"SF Mono"' && character === badCharacter ? 4 : 8);
    assert.equal(resolveConsoleFont(document), '"Cascadia Code"');
  }
});

run('console font accepts harmless fractional noise and uses the requested size', () => {
  const document = fontDocument((_, weight, character, size) => {
    assert.equal(size, 15.5);
    return 9.125 + (weight === 'bold' || character === 'i' ? 0.001 : 0);
  });
  assert.equal(resolveConsoleFont(document, 15.5), '"SF Mono"');
});

run('console font rejects non-finite and non-positive measurements', () => {
  for (const invalid of [NaN, Infinity, 0, -1]) {
    const document = fontDocument(family => family === '"SF Mono"' ? invalid : 8);
    assert.equal(resolveConsoleFont(document), '"Cascadia Code"');
  }
});

run('console font continues after a candidate measurement fails', () => {
  const document = fontDocument(family => {
    if (family === '"SF Mono"') throw new Error('Font measurement unavailable');
    return 8;
  });
  assert.equal(resolveConsoleFont(document), '"Cascadia Code"');
});

run('console font gracefully handles unavailable measurement and invalid sizes', () => {
  assert.equal(resolveConsoleFont(undefined), 'monospace');
  assert.equal(resolveConsoleFont({ createElement() { throw new Error('No canvas'); } }), 'monospace');
  assert.equal(resolveConsoleFont({ createElement: () => ({ getContext: () => null }) }), 'monospace');
  for (const size of [0, -1, NaN, Infinity]) {
    assert.equal(resolveConsoleFont(fontDocument(() => 8), size), 'monospace');
  }
});

run('console font does not retain stale choices across documents or later probes', () => {
  assert.equal(resolveConsoleFont(fontDocument(() => 8)), '"SF Mono"');
  assert.equal(resolveConsoleFont(fontDocument(proportional)), 'monospace');
});
