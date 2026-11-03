import assert from 'node:assert/strict';
import path from 'node:path';
import { pathToFileURL } from 'node:url';
import { transformSync, types } from '@babel/core';
import localizeStaticCopyBabelPlugin, { jsxTextEdges } from '../../scripts/localize-static-copy-babel.mjs';
import { sourceCatalogs } from './sourceCatalog.generated.js';
import { applyLocalePreference } from './index.js';

const sourceRoot = path.resolve(process.cwd(), 'src');
const i18nModuleUrl = pathToFileURL(path.join(sourceRoot, 'i18n/index.js')).href;
const englishByCopy = new Map(
  Object.entries(sourceCatalogs['zh-CN']).map(([key, text]) => [text, sourceCatalogs['en-US'][key]]),
);

// 把最外层 JSX 元素换成 React 实际收到的 children 数组(Babel 自己的 JSX 空白规则),
// 嵌套元素只留 `<标签>` 占位,便于比较文本节点两侧的空白。
function jsxChildrenArrayPlugin() {
  function flatten(node) {
    return types.react.buildChildren(node).map((child) => (types.isJSXElement(child)
      ? types.stringLiteral(`<${child.openingElement.name.name}>`)
      : child));
  }
  return {
    visitor: {
      JSXElement: {
        exit(path) {
          if (path.parentPath.isJSXElement() || path.parentPath.isJSXFragment()) return;
          path.replaceWith(types.arrayExpression(flatten(path.node)));
        },
      },
    },
  };
}

async function renderChildren(jsx, { localize, locale = 'zh-CN' } = {}) {
  const source = `export function render(count) { return ${jsx}; }`;
  const code = transformSync(source, {
    filename: path.join(sourceRoot, 'components/JsxSpacingFixture.jsx'),
    configFile: false,
    babelrc: false,
    parserOpts: { plugins: ['jsx'] },
    plugins: localize
      ? [localizeStaticCopyBabelPlugin, jsxChildrenArrayPlugin]
      : [jsxChildrenArrayPlugin],
  }).code.replace('"/src/i18n/index.js"', JSON.stringify(i18nModuleUrl));
  const module = await import(`data:text/javascript;base64,${Buffer.from(code).toString('base64')}`);
  await applyLocalePreference(locale, { cache: false });
  try {
    return module.render(3);
  } finally {
    await applyLocalePreference('zh-CN', { cache: false });
  }
}

// 本地化前后中文渲染必须逐字一致:插件只把文案换成翻译调用,不能凭空多出或吞掉空格。
async function assertSameAsReact(name, jsx, expected) {
  const plain = await renderChildren(jsx, { localize: false });
  assert.deepEqual(plain, expected, `${name}: fixture must describe React's own rendering`);
  assert.deepEqual(await renderChildren(jsx, { localize: true }), plain, `${name}: localized zh-CN children`);
}

// 场景:按钮文字单独占一行,收尾标签前有缩进。
// 期望:只渲染「重置」。回归:旧规则把 "\n      " 里的缩进当成同行空格,
// 渲染成「重置 」,inline-flex 居中按钮的文字因此左偏半个空格。
await assertSameAsReact('multi-line button copy', `<button>
      重置
    </button>`, ['重置']);
assert.deepEqual(
  await renderChildren(`<button>
      重置
    </button>`, { localize: true, locale: 'en-US' }),
  [englishByCopy.get('重置')],
  'multi-line button copy must not gain a trailing space in English either',
);

// 场景:多行文本没有任何缩进。期望:同样不带首尾空格。
await assertSameAsReact('multi-line copy without indentation', `<button>
重置
</button>`, ['重置']);

// 场景:文案与表达式写在同一行,两侧空格属于正文。期望:照旧保留,前后各一个空格。
await assertSameAsReact('same-line copy around an expression', '<span>设置 {count} 重置</span>', ['设置 ', 3, ' 重置']);

// 场景:只有前导同行空格 / 只有尾随同行空格。期望:只保留写出来的那一侧。
await assertSameAsReact('leading same-line space only', '<span>{count} 重置</span>', [3, ' 重置']);
await assertSameAsReact('trailing same-line space only', '<span>重置 {count}</span>', ['重置 ', 3]);

// 场景:表达式后面先有空格再换行,文案在下一行。
// 期望:贴着换行的空白丢弃。回归:旧的前导规则同样会把它当成同行空格补出「 重置」。
await assertSameAsReact(
  'spaces before a line break on the leading side',
  '<span>{count}  \n      重置\n    </span>',
  [3, '重置'],
);

// 场景:多行文本后面紧跟另一个元素。期望:两者之间没有空格(与 React 一致);
// 真需要空格的地方必须在源码里显式写 {' '}。
await assertSameAsReact('multi-line copy followed by an element', `<p>
      重置
      <b>x</b>
    </p>`, ['重置', '<b>']);

// 场景:同行边缘有制表符或多个空格。期望:按 Babel 规则保留实际宽度(制表符记为空格)。
await assertSameAsReact('same-line tab and repeated spaces', '<span>{count}\t重置  </span>', [3, ' 重置  ']);

// 场景:同行边缘是 &nbsp;。期望:不间断空格作为正文保留(旧规则会把它和文案一起裁掉)。
await assertSameAsReact('non-breaking space edge', '<span>&nbsp;重置</span>', [' 重置']);

// 场景:直接核对边缘计算。期望:只有缩进包围时没有边缘,同行尾随空格算作尾部边缘。
assert.deepEqual(jsxTextEdges(types, '\n      重置\n    ', '重置'), { leading: '', trailing: '' });
assert.deepEqual(jsxTextEdges(types, '文件位置： ', '文件位置：'), { leading: '', trailing: ' ' });

console.log('[pass] localized JSX text keeps React whitespace semantics');
