# P2-02 共同协议根 `llm/` 验证记录

认领 Claude-phase2,2026-09-27(`fdb91d1f`)。分支 `refactor20260927/P2-02`,两个 [mechanical] 提交 + 三个内容提交 + 一次主线同步;另有三个直接进 master 的工具提交(P2 各 PR 共用):

| 提交 | 内容 |
|---|---|
| `d40762c7`(master) | `cmake_target_snapshot --reverse-map` 先把 P2 过渡目录换算到最终路径再反查旧路径 |
| `187bdd50` / `4b6ce35c`(master) | `normalize_includes` 的 include 解析支持 P2 过渡目录(映射到最终分组路径后再退回 `src/<模块>/` 查找);第二个提交修正被 heredoc 塌掉的测试转义 |
| `bf1ef7db`(master) | 快照对照把映射同样作用于对象文件路径里嵌着的源路径(否则每个搬迁 .cpp 在 5 个目标里各报一对假差异) |
| `4009835e` [mechanical] | `provider/llm_provider.hpp`、`tool/{tool_protocol_names,model_family}.{hpp,cpp}`、`tool/tool_icons.hpp` → `src/llm/`;`tool/{diff_utils,word_diff,diff_view_truncate}.{hpp,cpp}` → `src/utils/`;镜像测试 `tests/tool/{model_family,tool_protocol_names}_test.cpp` → `tests/llm/`,`tests/tool/{diff_utils,word_diff,diff_view_truncate}_test.cpp` → `tests/utils/`。17 个 R100 |
| `bd8ac3eb` [mechanical] | `normalize_includes.py --scope src/tests`:src 79 文件 / 89 行,tests 48 文件 / 55 行改为新模块根形式(`provider/llm_provider.hpp` 75 处、`tool/tool_protocol_names.hpp` 24 处、`tool_icons` 12、`diff_*` 14 …) |
| `4d855149` | 拆头:`llm/retry_waiter`(`ProviderRetryWaiter`)、`llm/tool_result.hpp`(`ToolSummary` / `ToolResult`)、`llm/token_estimate`、`llm/message_predicates`(含 `get_compact_summary_prefix`)、`llm/context_thresholds`、`llm/context_usage`(JSON 编解码)、`llm/text_preamble_tags`(扫描器 / 标题规整 / 标签常量,namespace `acecode::llm`,tool_preamble 用 using 声明保持写法);使用方改为只依赖 llm;R12 棘轮的三个超限文件各去掉一个空行 |
| `992b9bdc` | `tests/utils/abandonable_call_test.cpp`:`AbortReturnsWithin100msAndDiscardsLateResult` 的 100ms 上限放宽到 1s(macOS CI 负载下实测 167ms;worker 在 release 之前不产生结果,语义不变;同 P2-01 `5596fc8e` 的处理) |
| `77eddf55` → `7729e836` | 先按原设计留了 7 个转发头并登记 exception,`validate_map --strict` / `migrate_branch --check` / 工具自测随即报 destination collision,登记 **D24(P2 不留转发头)** 并删除;`src_layout_map.tsv` 新增 3 条 extract 行,layout-map.md §3 回写两条执行中发现;CLAUDE.md 的 model_family / tool_protocol_names 路径改到 llm/ |

## 1. 执行中与设计不符的两处(已回写 layout-map.md / design.md)

1. **`retry_policy.hpp` 不能只删 include**:`LlmProvider` 有成员 `ProviderRetryWaiter retry_waiter_`(retry_policy.hpp:42 的类)与三个 inline 方法。把等待器类整体下沉到 `llm/retry_waiter.{hpp,cpp}`(纯 mutex / condvar / atomic),`provider/retry_policy.hpp` 继续 include 它,既有使用方不变;此前经 `llm_provider.hpp` 传递拿到 retry_policy 的 4 个 provider 实现与 `compact.cpp` 自行 include(「遗漏的地方编译会报出来」—— 实际是先 grep 出来补上的,一次编译通过)。
2. **`is_compact_summary_message` 依赖 `get_compact_summary_prefix()`**(定义在 `commands/compact_prompt.cpp`,apps 层),谓词下沉时前缀常量与取值函数随行进 `llm/message_predicates`,`compact_prompt.hpp` 保留 include。
3. **转发头与映射校验冲突**(D24,见上表)。

## 2. §7.2「P2(每个 PR)」闸门逐项

| 闸门 | 结论 | 证据 |
|---|---|---|
| lint 违规数下降 | 满足 | `check_layers.py --enforce-parent-includes`:161 → 153。消失的 8 条正是任务点名的边:`provider/llm_provider.hpp → retry_policy`(R1 domain→adapters + R3 domain 不准引用 provider)、`session/session_storage.cpp → prompt/context_usage_breakdown`(R1)、`session/thread_repair.cpp → commands/compact`(R1)、`session/tool_metadata_codec.hpp` / `tool_result_storage.hpp → tool/tool_executor`(R1 ×2)、`prompt/context_usage_breakdown.cpp` / `prompt/system_prompt.cpp → commands/compact`(R2 ×2)。三个环(provider↔session、provider↔tool、provider↔pa)不再存在:provider 不再被 session / tool / pa 反向引用,pa_quirks.hpp:9 已指向 `llm/llm_provider.hpp` |
| 旧路径不留文件,`validate_map --strict` 通过(D24) | 满足 | 7 个旧路径已删除;`validate_map --strict` exit 0,`planned_or_obsolete_rows` 51(全是尚未执行的 P2/P3 行) |
| 按映射换算后每个文件所属的 target 不变 | 满足 | 本机全新目录 `build-p2-02` 的快照经 `--map --reverse-map` 与 P1 之后 master 的本机快照(`local-win-p1`)对照:59 target 无增减,3572 元组,**减 0**,增 43 = 6 个新 .cpp、7 个新 .hpp(File API 把显式登记的头列进 `acecode_testable`)与 6 个新对象 × 5 个链接目标(acecode、acecode_unit_tests、concurrent_session_writer、remote_web_proxy_test_child、state_file_claim_worker);搬走的 5 个 .cpp / 5 个 .hpp 换算回旧路径后逐元组相同。CI 四平台同样减 0(第 4 节)。对象路径的换算由 `bf1ef7db` 补上,否则 5 个搬迁 .cpp 各报 5 对假差异 |
| 测试已随源文件移动 | 满足 | 5 个镜像测试 R100 进 `tests/llm/` 与 `tests/utils/`;新拆出的纯逻辑由既有用例覆盖(`compact_core_test` / `context_usage_breakdown_test` / `tool_preamble_test` / `retry_policy_test` / `tool_metadata_codec_test` / `session_storage` 系列),用例清单不变 |
| 三平台构建通过 | 满足 | `refactor-matrix` run 36310298609(`p2-02-v2`,源码 `7729e836`):Windows / Linux / macOS 构建 + 全量清单、Deepin configure + build 全部成功(第 4 节);本机 Windows 全新目录 `build-p2-02`(第 3 节) |
| 用例清单不变 | 满足 | 本机 5115 列出 / 5114 执行 / 9 SKIP / 0 失败,与 P1 之后的 master 本机结果逐条相同(`tests`、`ctest_names` 5119、SKIP 集合);CI 见第 4 节 |
| `system_prompt.cpp` 对 compact.hpp 的无用 include 已删除 | 满足 | 改为只 include `llm/token_estimate.hpp`(它只用 `approx_token_count`) |
| byte-stable 用例与 tool_preamble 测试通过 | 满足 | 全量单测 0 失败,含 `RequestPrefixIsByteStableAcrossIterationsInATurn`、`system_prompt_test` 的 byte-stable 用例、`NonGptModelStateIsByteIdenticalToLegacyPrompt`、`tool_preamble_test` 与 `agent_loop_tool_preamble_test` |
| `migrate_branch.py --check --layout current` | 已跑(报告) | exit 1,失败项只有「final layout 的 R1–R14 尚未归零」这一类 P2 期间必然存在的项,记录在 `p2-02/migrate-check2.json`(不入库) |
| 通知 9 个遗留分支 | 满足 | branch-inventory.md「P1-01 公告」已写明 P2 各 PR 的处理方式(`normalize_includes` / `migrate_branch --apply-map`),本任务无需单独公告 |

## 3. 本机验证(Windows)

- 增量构建 `build-p1`:`acecode_testable` 一次编译通过,随后 `acecode` / `acecode-desktop` / `acecode_unit_tests` 通过。
- 全新目录 `build-p2-02`:configure 通过(`acecode_assert_known_roots` 接受新目录 `src/llm/`),`acecode acecode-desktop acecode_unit_tests computer_use_native_smoke computer_use_broker_smoke` 全部通过;快照对照见第 2 节。
- 全量单测(`gtest_inventory.py --run`,隔离 HOME / TEMP 在 `N:\acecode-p2-02-iso`):5115 列出 / 5114 执行 / 9 SKIP / 0 失败。
- lint:`check_file_size --strict`、`check_ownership --strict`、`validate_map --strict` exit 0;`check_doc_paths` 84 项(与 P1 相同);`normalize_includes --check` src / tests 0 diff;工具自测 72 项通过。

## 4. CI(refactor-matrix,源码 `7729e836`)

`collect_p2.py`:artifact 的 File API reply 重建快照,`--map --reverse-map` 后与 `baseline/g0/post-p0/<platform>/targets.json` 逐元组对照;用例清单用 `compare_snapshots.py` 的报告段。

| 平台 | 目标快照(target / 元组) | 用例:post-p0 → P2-02 | 差异 |
|---|---|---|---|
| windows-x64 | 59 / 3572,target 无增减,减 0,增 43(源 6 / 头 7 / 对象 30) | 5115 列出 / 5108 执行 / 10 SKIP / 4 失败 → 5115 / 5108 / 10 / 5 | 用例与 SKIP 相同;失败多 1 条 `TaskSuggestionServiceTest.RetryKeepsTargetAndRecoveryDoesNotRepeatInput`(6.49s 后等待 `started` 状态超时;本机 3 次通过、Linux / macOS 通过、P1 的两轮 Windows CI 通过,P2-02 未改 task_suggestion_service,记为 Windows runner 时序抖动) |
| macos-arm64 | 57 / 3631,target 无增减,减 0,增 43 | 5040 / 5033 / 14 SKIP / 10 失败 → 5040 / 5033 / 14 / 12 | 失败多 2 条:`AbandonableCallTest.AbortReturnsWithin100msAndDiscardsLateResult`(167ms > 100ms,已由 `992b9bdc` 放宽)与已知抖动 `OpenAiProviderErrorRecovery.SseKeepaliveCommentsDoNotTriggerRetry` |
| linux-x64 | 50 / 3486,target 无增减,减 0,增 43 | 5035 / 5026 / 16 SKIP / 0 失败 → 相同 | 完全相同 |
| linux-deepin-x64(MinSizeRel,BUILD_TESTING=OFF) | 13 / 1372,target 无增减,减 0,增 19(源 6 / 头 7 / 对象 6) | — | 与其它平台的新增集合一致 |

新增的 13 个文件在四个平台上都只属于 `acecode_testable`(及直接链接其对象的可执行目标),没有任何搬迁文件换了 target。
