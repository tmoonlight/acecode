# 验证记录

日期：2026-10-09，当前 master 检出。

## 已完成

- `cmake --build build --config Release --target acecode_unit_tests --parallel 1`：在 `scripts/dev_windows_env.bat` 初始化的 MSVC 环境中构建成功。首次新增测试误用 `chat(..., true)`，修正为已有 `chat_for_compaction(..., nullptr)` 后增量重编成功。
- `build/tests/Release/acecode_unit_tests.exe --gtest_filter=SavedModelsTest.*:SavedModelsEditor*.*:ModelCatalogHandler.*:ModelsHandler.*:ModelProfileRuntimeOptions.*:OpenAiResponsesProviderTest.*:ModelConnectionTest.* --gtest_color=no`：7 个 suite、174 项全部通过。
- `node web/src/lib/modelSettings.test.js`：包含 ACEModel 目录默认值、批量保存、编辑及服务商切换的回归通过。
- `pnpm test`：3,080 项 Web 检查通过。
- `pnpm build`：Vite 构建和正则兼容检查通过。
- `powershell -NoProfile -ExecutionPolicy Bypass -File installer/windows/seed_acemodel.test.ps1`：新建、升级、重复运行保留明确协议、无关配置保留均通过。
- Headless Edge 使用真实 `ModelProfileDialog` 与当前 Vite 源码：4 项交互通过，覆盖默认 Responses 和三模型保存、编辑/检测/保存协议一致、明确 Chat Completions 回填后再切回 Responses、切换 Provider 恢复默认；无 pageerror。
- `python scripts/refactor/check_layer_libraries.py --build-dir build`：生产库分层检查通过。
- `openspec validate default-acemodel-responses-protocol --strict`：通过。
- `git diff --check`：通过。

## 验证边界

HTTP 回归使用本机 fixture，确认实际端点和请求体；浏览器检查使用 fixture API。未发送真实 ACEModel 付费请求。已验证源代码与 Web 构建，未更新当前运行的已安装 Desktop 软件。当前仓库规则忽略 `openspec/` 中新建文件，变更文档保存在本地既定目录中。

构建出现既有共享 Intermediate 目录警告；使用串行构建避免并发链接冲突。未提交或发布本次改动。
