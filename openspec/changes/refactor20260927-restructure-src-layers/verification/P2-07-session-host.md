# P2-07 会话编排层迁移验证

任务认领由 PR #85 合入 master,基线 `fd6f96955540bf2492c2306a21fb1ff73a47e987`,前置 P2-06 已由 PR #83 验收。工作分支为 `refactor20260927/P2-07`,复用已合入、干净且无活动构建/运行进程的原 P2-04 工作区;原工作区内已有构建目录保留,本任务使用全新目录。

## 实现边界

- `35ef1d9e`:将 /init 提示词和骨架归入 prompt/init_prompt,将 LSP 状态文本与子命令分派归入 lsp/lsp_status_text,将生成标题校验、净化及终端标题净化归入 session/session_title_text。命令注册、宿主编排和终端 OS 操作分别保留在各自职责层。七组提取函数与原始文本逐项核对一致;标题请求的空白检查共用提取后的纯函数,语义不变。
- `dfe8a5c1`:27 个文件纯 `git mv`,全部 R100,0 行增删。20 个源/头文件归到 session_host,五个宿主测试与两个纯逻辑测试同步镜像归位。
- `88d0e6d2`:39 个 src/tests 文件的 95 处 include 路径改写,逐文件增加行数 = 删除行数 = 修改的 include 行数。补充 init_prompt 和 terminal_title 纯逻辑测试映射,保留 P2-03 中间路径别名供旧分支迁移。
- 不在旧路径留下转发头。SessionRegistry 对文件工具恢复的依赖由后继 P2-08 的 tool/file_state_restore 拆分处理;不借本次移动改变现有线程、回调或服务租约语义。
- 当前文档更新移动路径;帮助站点从 group7.py 重新生成 49 篇文章及辅助清单,实际差异仅 sources.json 中一条源码路径。已将两个机械提交登记到 .git-blame-ignore-revs,并向 branch-inventory.md 追加九个遗留 ref 的迁移提示。

## 静态检查

src / tests include 规范化二次运行均为 0 改动。映射 strict、已有分层阻断项、行数与所有权 strict 通过;分层违规从 83 降到 79,未增加例外。下降项为 session_registry 对 commands 的两条依赖,以及 session_manager / session_storage 对标题生成宿主头的两条依赖。apply_model_to_session 上移后不再构成 provider 对 agent_loop 的物理反向依赖;尚存的裸 agent_loop.hpp include 由 P2-08 的整体归位处理。OpenSpec strict 通过。

完整 `pnpm test` 与 `pnpm build` 均通过。

## 待完成验证

此文件记录实施中的状态,任务复选框仍未勾选。全新目录原生构建、File API 归属对照、完整与定向单测、四平台构建与既有失败复核完成后补充结果;未验收前不将采集工作流成功等同于单测全绿。
