import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';
import {
  INLINE_VISUAL_LIBRARIES,
  inlineVisualLibraryCode,
  isInlineVisualLibrary,
  loadInlineVisualLibrary,
} from './inlineVisualLibraries.js';

async function run(name, fn) {
  try {
    await fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

const read = (relative) => readFileSync(new URL(relative, import.meta.url), 'utf8');

// 触发场景(openspec add-inline-visuals D12):框内脚本引用 Chart.js / d3,外层要从这里拿本地副本。
// 期望行为:只认这两个名字;原型链上的名字(__proto__、constructor)与 echarts 一律不认,
// 免得框内发来的任意名字取到奇怪的东西。
await run('只认内置的两个库名', () => {
  assert.deepEqual(INLINE_VISUAL_LIBRARIES, ['chart.js', 'd3']);
  for (const name of INLINE_VISUAL_LIBRARIES) assert.equal(isInlineVisualLibrary(name), true, name);
  for (const name of ['echarts', 'Chart.js', '__proto__', 'constructor', 'toString', '', null, 42]) {
    assert.equal(isInlineVisualLibrary(name), false, String(name));
  }
});

// 触发场景:库代码以内联脚本执行,末尾的 sourceMappingURL 注释会让开发者工具去请求 .map(404)。
// 期望行为:只去掉末尾那一行注释,代码其它部分一字不改。
await run('去掉末尾的 sourceMappingURL 注释', () => {
  assert.equal(inlineVisualLibraryCode('a();\n//# sourceMappingURL=chart.umd.min.js.map\n'), 'a();\n');
  assert.equal(inlineVisualLibraryCode('a();'), 'a();');
  assert.equal(inlineVisualLibraryCode(null), '');
});

// 触发场景:加载本地副本(这里用注入的加载函数代替 Vite 的 ?raw 动态导入)。
// 期望行为:拿到模块的默认导出并去掉 map 注释;未知名字返回 null;加载失败把错误抛给调用方
// (外层据此回空,框内改走 CDN)。
await run('按名字加载并处理失败', async () => {
  const loaders = {
    'chart.js': async () => ({ default: 'window.Chart = 1;\n//# sourceMappingURL=x.map' }),
    d3: async () => { throw new Error('chunk failed'); },
  };
  assert.equal(await loadInlineVisualLibrary('chart.js', loaders), 'window.Chart = 1;\n');
  assert.equal(await loadInlineVisualLibrary('echarts', loaders), null);
  await assert.rejects(loadInlineVisualLibrary('d3', loaders), /chunk failed/);
});

// 触发场景:打包进来的就是 package.json 写死的那一版,且浏览器构建在没有 module / exports 的
// 环境里(隔离框里的内联脚本就是这样)求值后挂出全局 Chart / d3。
// 回归:换版本或换文件名后,模型照规范写的 new Chart(...) / d3.select(...) 会在框里报未定义。
await run('内置副本版本固定且能挂出全局变量', () => {
  const manifest = JSON.parse(read('../../package.json'));
  assert.equal(manifest.dependencies['chart.js'], '4.5.1');
  assert.equal(manifest.dependencies.d3, '7.9.0');
  const chart = vm.createContext({});
  vm.runInContext(inlineVisualLibraryCode(read('../../node_modules/chart.js/dist/chart.umd.min.js')), chart);
  assert.equal(typeof chart.Chart, 'function');
  assert.equal(chart.Chart.version, '4.5.1');
  const d3 = vm.createContext({});
  vm.runInContext(read('../../node_modules/d3/dist/d3.min.js'), d3);
  assert.equal(typeof d3.d3.select, 'function');
  assert.equal(d3.d3.version, '7.9.0');
});
