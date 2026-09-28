<!-- refactor-layout-map sha256:e2eb7cc27deba8a1e0bfb8fa3e6771094a4ccb875d33cab20578289cd5198965 -->
源码路径迁移请按 `scripts/refactor/src_layout_map.tsv` 换算；本设计中的历史路径保留。

## Context

会话 `20260924-072030-b1fe` 的元数据标记为 `no_workspace=true`，工作目录为 `C:/Users/shao/.acecode/cache/no-workspace/20260924-072030-b1fe`。轮次净差异使用 `pelican-bicycle.html` 等相对路径。日志显示该会话结束后被工作区 `f1ee51683484c65b` 的恢复接口两次报告成功，但该工作区存储中没有该会话元数据。`SessionRegistry::resume` 对已运行的 ID 直接返回成功，工作区路由未检查元数据是否存在，响应于是带回错误的工作区目录。`ChatView` 的文件预览使用导航 ref 的工作目录，差异预览只读会话变更记录。

## Goals / Non-Goals

**Goals:** 工作区路由验证会话归属；导航遇到错误工作区提示时恢复无工作区会话的真实目录；覆盖该相对文件打开场景。

**Non-Goals:** 改变轮次差异格式、文件内容 API、列表布局或历史文件已删除时的预览错误语义。

## Decisions

- 工作区恢复路由先验证所选工作区内的会话元数据存在、不是无工作区会话，且同 ID 的运行中会话没有归属其他工作区，再调用会话客户端。不能把 `resume_session` 的成功值当归属证明，因为已运行 ID 会立即返回成功。
- 桌面导航在工作区恢复返回 404 时尝试兼容恢复接口，仅当它明确返回 `no_workspace=true` 才接受并用返回的 `working_cwd` 生成会话 ref；其他失败保留原错误。正常工作区恢复仍只发原请求。
- 跨工作区整页跳转的 URL 使用恢复后的规范会话 ref，避免把陈旧的工作区 hash 再写回页面。

## Risks / Trade-offs

- [旧 daemon 仍会错误地返回工作区恢复成功] → 修复需要前后端一起构建部署；回归测试验证新服务端的拒绝行为。
- [404 也可能表示真正不存在的会话] → 仅在兼容接口明确证实无工作区归属时回退，否则保持 404。

## Validation

- `node web/src/lib/sessionJump.test.js`、`pnpm test`、`pnpm build` 通过。
- `cmake --build build --config Release --target acecode_unit_tests` 通过；定向运行无工作区会话及正常工作区恢复的两个 Web smoke test，均通过。
- `openspec validate fix-turn-file-open-session-root --strict` 和 `git diff --check` 通过。
