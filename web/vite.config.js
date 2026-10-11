import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';
import tailwindcss from '@tailwindcss/vite';
import { viteSingleFile } from 'vite-plugin-singlefile';
import localizeStaticCopyBabelPlugin from './scripts/localize-static-copy-babel.mjs';

const daemonPort = process.env.ACECODE_DAEMON_PORT || '28080';
const daemonToken = process.env.ACECODE_DAEMON_TOKEN;
const daemonHttpTarget = `http://127.0.0.1:${daemonPort}`;
const daemonWsTarget = `ws://127.0.0.1:${daemonPort}`;
const daemonProxyHeaders = daemonToken ? { 'X-ACECode-Token': daemonToken } : undefined;

// daemon 在 build 时把 web/dist/ 嵌入二进制(见 cmake/acecode_embed_assets.cmake)。
// 用 viteSingleFile 把 JS/CSS 全部 inline 进 index.html — 一来 daemon 只需要
// serve 单个文件,二来绕开 Crow keep-alive 在多资源连接复用时丢 Content-Type
// 的已知问题(同一 TCP 连接的第二个请求会回空 body / 空头)。
// 代价:bundle 不能拆 chunk。原生资源生成器压缩存储产物,静态资源路由协商
// gzip 传输;单文件布局仍保留,拆 chunk 需要另行验证上述连接兼容性。
//
// dev 模式下跑 `pnpm dev` 起 Vite 5173；/api 与 /ws 默认代理到 127.0.0.1:28080。
// scripts/dev_web 会为它启动的 Vite 进程设置 ACECODE_DAEMON_PORT，以代理到该工作树的 daemon。
export default defineConfig({
  plugins: [
    react({ babel: { plugins: [localizeStaticCopyBabelPlugin] } }),
    tailwindcss(),
    viteSingleFile(),
  ],
  build: {
    outDir: 'dist',
    emptyOutDir: true,
    sourcemap: false,
    target: 'es2020',
    cssCodeSplit: false,
    assetsInlineLimit: 100 * 1024 * 1024, // 全部 inline
    rollupOptions: {
      output: {
        manualChunks: undefined,
        inlineDynamicImports: true,
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
