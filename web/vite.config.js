import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';
import tailwindcss from '@tailwindcss/vite';
import localizeStaticCopyBabelPlugin from './scripts/localize-static-copy-babel.mjs';

const daemonPort = process.env.ACECODE_DAEMON_PORT || '28080';
const daemonToken = process.env.ACECODE_DAEMON_TOKEN;
const daemonHttpTarget = `http://127.0.0.1:${daemonPort}`;
const daemonWsTarget = `ws://127.0.0.1:${daemonPort}`;
const daemonProxyHeaders = daemonToken ? { 'X-ACECode-Token': daemonToken } : undefined;

// 首屏就要执行的第三方库收进一个 vendor chunk:应用代码改动时它的内容哈希不变,
// 浏览器继续命中缓存。只列确定会在启动路径上执行的包;按需加载的库(mermaid /
// shiki / katex / office 预览 / xterm / react-joyride)靠动态 import() 自然拆分,
// 不要把它们加进来 —— 加进 vendor 就等于让它们随首屏一起下载。
const EAGER_VENDOR_PACKAGES = new Set([
  'react',
  'react-dom',
  'scheduler',
  'react-i18next',
  'i18next',
  'slate',
  'slate-dom',
  'slate-react',
  'slate-history',
  'markdown-it',
  'markdown-it-task-lists',
  'markdown-it-cjk-friendly',
  'highlight.js',
  'diff',
  'diff2html',
  'clsx',
]);

export function nodePackageName(id) {
  const normalized = String(id || '').replaceAll('\\', '/');
  const marker = normalized.lastIndexOf('/node_modules/');
  if (marker < 0) return '';
  const parts = normalized.slice(marker + '/node_modules/'.length).split('/');
  if (!parts[0]) return '';
  return parts[0].startsWith('@') && parts[1] ? `${parts[0]}/${parts[1]}` : parts[0];
}

export function manualChunkFor(id) {
  const name = nodePackageName(id);
  if (name && EAGER_VENDOR_PACKAGES.has(name)) return 'vendor';
  return undefined;
}

// daemon 在 build 时把 web/dist/ 嵌入二进制(见 cmake/acecode_embed_assets.cmake)。
//
// 产物布局:index.html 只保留入口引用,JS / CSS / 字体按内容哈希落到
// dist/assets/<name>-<hash>.<ext>。daemon 的静态路由(src/apps/web/routes/routes_misc.cpp)
// 对 assets/ 下的文件回 `Cache-Control: immutable`,对 index.html 与其它资源回
// `no-cache` + ETag(命中 If-None-Match 即 304)。首屏只加载入口 chunk 与 vendor;
// mermaid、shiki、katex、office 预览、xterm、新手引导都是动态 import() 按需加载。
//
// 历史:曾用 vite-plugin-singlefile 把全部 JS/CSS inline 进 index.html,理由是
// Crow 1.3.2 的 catchall 路由会污染同一 keep-alive 连接上的下一个请求(第二个响应
// 丢 Content-Type / 空 body)。静态路由早已改成按路径层数显式注册,/assets/<file>
// 这类两段路径不经过 catchall;tests/web/web_server_smoke_test.cpp 的
// StaticAssetsKeepAlive* 用例在同一连接上连续取 / 、资源与 /api/health 守着这条。
//
// dev 模式下跑 `pnpm dev` 起 Vite 5173；/api 与 /ws 默认代理到 127.0.0.1:28080。
// scripts/dev_web 会为它启动的 Vite 进程设置 ACECODE_DAEMON_PORT，以代理到该工作树的 daemon。
export default defineConfig({
  plugins: [
    react({ babel: { plugins: [localizeStaticCopyBabelPlugin] } }),
    tailwindcss(),
  ],
  build: {
    outDir: 'dist',
    emptyOutDir: true,
    sourcemap: false,
    target: 'es2020',
    cssCodeSplit: true,
    // 超过 4 KiB 的图片 / 字体落成独立文件(KaTeX 的 60 个字体曾以 base64 内联进
    // CSS,占 1.4 MB,只有 trajectory 视图渲染公式时才用得到)。
    assetsInlineLimit: 4096,
    chunkSizeWarningLimit: 2000,
    rollupOptions: {
      output: {
        manualChunks: manualChunkFor,
      },
    },
  },
  server: {
    port: 5173,
    proxy: {
      '/api': { target: daemonHttpTarget, changeOrigin: true, headers: daemonProxyHeaders },
      '/ws':  { target: daemonWsTarget,   ws: true,         changeOrigin: true, headers: daemonProxyHeaders },
    },
  },
});
