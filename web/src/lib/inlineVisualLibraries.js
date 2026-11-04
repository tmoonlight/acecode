// 对话内可视化的内置图表库(openspec add-inline-visuals D12,docs/inline-visuals.md)。
//
// 隔离框里的脚本引用 Chart.js 4 / d3 7 的 CDN 地址时,框内运行时向外层要代码(library 消息),
// 外层用这里按需加载的本地副本回复,框内换成内联脚本执行 —— 断网、内网也能画,隔离框的
// 内容安全策略一条都不用放宽(框里的代码仍然请求不到本机 daemon)。
//
// 两个包都没有在 exports 里导出浏览器构建文件,所以按相对路径导入;`?raw` 让 Vite 把文件
// 原样打成独立 chunk,首屏不加载。版本在 web/package.json 里写死(chart.js 4.5.1、d3 7.9.0)。
const LOADERS = Object.freeze({
  'chart.js': () => import('../../node_modules/chart.js/dist/chart.umd.min.js?raw'),
  d3: () => import('../../node_modules/d3/dist/d3.min.js?raw'),
});

export const INLINE_VISUAL_LIBRARIES = Object.freeze(Object.keys(LOADERS));

// 去掉末尾的 sourceMappingURL 注释:代码是内联执行的,开发者工具按它去找 .map 只会报 404。
export function inlineVisualLibraryCode(raw) {
  return String(raw || '').replace(/\n\/\/# sourceMappingURL=[^\n]*\s*$/, '\n');
}

export function isInlineVisualLibrary(name) {
  return typeof name === 'string' && Object.prototype.hasOwnProperty.call(LOADERS, name);
}

const cache = new Map();

// 同一页面里多张图共用一份代码;加载失败不缓存,下一张图重试。
export function loadInlineVisualLibrary(name, loaders = LOADERS) {
  if (!Object.prototype.hasOwnProperty.call(loaders, name)) return Promise.resolve(null);
  if (!cache.has(name) || loaders !== LOADERS) {
    const pending = Promise.resolve()
      .then(() => loaders[name]())
      .then((module) => inlineVisualLibraryCode(module?.default ?? module))
      .catch((error) => {
        if (loaders === LOADERS) cache.delete(name);
        throw error;
      });
    if (loaders !== LOADERS) return pending;
    cache.set(name, pending);
  }
  return cache.get(name);
}
