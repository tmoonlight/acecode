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

## 本机原生验证

固定源码 `a5cad80df6897fe7ec22ba4bdbdfba5999aefec5`,使用 MSVC 2022 x64 / Ninja / Release,全新目录 `build/p2-07-session-host`,BUILD_TESTING 与 Desktop 均开启。CLI、Desktop、acecode_unit_tests 与五个 EXCLUDE_FROM_ALL 冒烟目标均构建成功;Desktop 的 File API 依赖清单不含 acecode_testable。

CMake File API 为 59 个目标、3658 个元组,对照 P2-06 的 59 / 3637,原有目标、文件归属和编译参数均保持,无删除元组。21 个新增元组只对应 init_prompt、lsp_status_text 与 session_title_text 三组源/头和生成对象。

独立 HOME/TEMP 下完整 gtest 清单 5117,实际运行 5116,9 SKIP,0 失败,退出码 0;用例名和 SKIP 集合相对 P2-06 完全一致。本次涉及的会话宿主、子代理、线程工具、任务建议、模型切换、标题、LSP、init_prompt 与 Web 服务共 18 个测试套件、428 个用例,全部实际执行且 0 SKIP。完整测试已覆盖这些定向用例,没有另行重复运行。

CTest 注册条目仍为 5121。原始展示名中 42 项含参数指针地址或 `GetParam()` 对象字节,重新链接后这些展示数据不同;只移除该原始对象展示注释、归一化 `pointing to` 前的指针值后,保留测试标识与字面参数的条目多重集相同。gtest 的原始规范用例名直接逐项相同,未按模糊前缀合并或隐藏用例。

`AgentLoopTermination.RequestPrefixIsByteStableAcrossIterationsInATurn` 与 `SystemPromptTest.NonGptModelStateIsByteIdenticalToLegacyPrompt` 均实际执行并通过。

`migrate_branch.py --check` 已执行,过渡布局仍返回非零:documentation 102、layers 2341、migration_paths 60;include_normalization、map、ownership、seed 为 0。这里包含尚未执行的 P3 最终分组规则;当前迁移模式的实际分层违规为 79,不将此报告当作 P3 验收。

## 四平台验证(D26 之前已触发的一次运行)

[refactor-matrix 36372667024](https://github.com/tmoonlight/acecode/actions/runs/36372667024) 固定上述源码。Deepin 已完成构建,File API 为 13 个目标、1409 个元组;对照 P2-06,原有目标/元组均保持,仅新增三组提取实现的 9 个元组。该平台不运行单测。

Deepin 的 provenance 源码字段仍因容器 Git 归属检查为空;额外核对已完成 job `108772039048` 的 checkout 命令、HEAD 提示和紧接 `git log -1 --format=%H` 的完整 SHA,均为固定源码,不使用空字段作为来源证明。

该运行随后全部完成:windows-x64、linux-x64、macos-arm64 与 Deepin(configure + build、snapshot)五个 job 均为 success。按 D26(2026-09-28)逐任务验收只做 Windows 本机,三个测试平台的快照、用例清单与既有失败复核没有逐项对照,统一留到 tasks.md 5.4「多平台补验」。

## D26 接手核对(Claude,2026-09-28)

原 Codex 会话停在等待四平台结果的阶段(用户反映验证成本过高),本节按 design.md §7.4 的 B 档在 Windows 本机复核,不再触发新的 CI:

- 静态闸门(分支 `a5cad80d`):`normalize_includes --check` src / tests 均 0 改动、0 错误;`check_layers --enforce-parent-includes` 79 项(master 83);`validate_map --strict`、`check_file_size`、`check_ownership` 均 0;`check_doc_paths` 102 项,较 master 多 23 项,全部是 `docs/superpowers/plans|specs/2026-05-09-model-selection*`、`docs/reviews/pr-24-*` 等历史文档里的 `apply_model_to_session` / `session_registry` 旧路径,P3 M2 的文档改写会统一换算(apply_layout 预演已覆盖 54 份文档),不在本任务改写历史记录。
- 快速档单测:用上文全新目录 `build/p2-07-session-host` 的 `acecode_unit_tests.exe`,`run_fast_tests.py --profile fast`(6 分片、隔离 HOME/TEMP、对照 P2-05 二进制的全量记录):清单 5117,执行 4515,9 SKIP,0 失败,601 条慢套件用例未运行(已如实记录),27 秒。守护测试 `AgentLoopTermination.RequestPrefixIsByteStableAcrossIterationsInATurn` 与 `SystemPromptTest.*ByteStable*` 单独补跑通过。
- target 快照:对 master(`fd6f9695`,configure-only File API)与本分支的快照都按映射表反查换算后对照:59 个目标无增删;新增元组只有 init_prompt、lsp_status_text、session_title_text 三组源/头及其 `.obj` 在各链接目标里的引用;另有 25 对 `nlohmann_json.natvis` 差异是主检出记成相对路径、worktree 记成绝对路径的已知形态差异,1 对 `terminal_title_test.cpp` 差异来自本分支补充的映射行(master 侧映射表还没有该行),两者都不是归属变化。
- 合入前已把 master(D26 决策、apply_layout 工具)合进本分支;src / tests / CMake 相对验证源码无差异。
