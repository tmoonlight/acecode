# P3-01 演练验证

目的:在临时 worktree 里从固定 base 生成 M1 / M2 / M2b / M3,跑通 §7.2「P3」一行里 Windows 本机能做的闸门(D26),记录每一步的耗时来估算 P3-02 的冻结窗口。演练分支不推送;正式搬迁在窗口内于最新 master 上重新生成。

- base:master `a3e2d204`(P2-08 已合入,Phase 2 全部勾选;adopt-ownership P2-01 已合入)。
- worktree:`N:/Users/shao/acecode-p3-rehearsal`(仓库外),全新构建目录 `build-p3`,子模块另行 `git submodule update --init --recursive`,`web/dist` 由本树 `pnpm build` 生成。
- 工具:`scripts/refactor/apply_layout.py`(P3-01 前置,2026-09-28 新增)+ `scripts/refactor/apply_include_roots.py`(本次演练沉淀的 M2 结构改动脚本,幂等)。

## 生成的提交与耗时

| 步 | 命令 | 结果 | 耗时 |
|---|---|---|---|
| M1 `[no-build]` | `apply_layout.py move --phase P3 --commit` | 1003 个文件全部 R100,0 行内容改动;stb → `external/stb`;搬空的 68 个目录删除;更早阶段 0 行未完成 | 21 秒 |
| M2 `[mechanical]` 机械部分 | `apply_layout.py rewrite --phase P3 --commit` | 6 个构建文件(CMakeLists.txt、tests/CMakeLists.txt、cmake/acecode_source_guards / source_paths / winpty.cmake、tests/cpp_source_paths.json)+ 195 份文档,help 站点重生成 49 篇;src/tests 只改 3 行 include(`image/stb/*` → `stb/*`)+ tests/CMakeLists.txt 3 行 + tests/README.md 5 行 + cpp_source_paths.json 19 行;blob 保持 LF | 17 秒 |
| M2 `[mechanical]` 结构部分 | `apply_include_roots.py` | 新增 `ACECODE_INCLUDE_ROOTS`(6 个分组根 + `external` + `generated`,configure 时断言目录存在),替换 18 处 `${CMAKE_SOURCE_DIR}/src` include 根(根 CMake 5、tests 12、acecode_desktop.cmake 1),`acecode_assert_known_roots` 去掉 `ALLOW_LEGACY`;`ACECODE_TUI_DIRS` 已由映射改写为 `src/apps/tui` | 1 秒 |
| M2b | `apply_layout.py seed --seed-version 2026-09-28.1 --commit` | 两个 SKILL.md 的 6 处路径、seed.version、MANIFEST bundle_version 与两个 skill_md_sha256 一起更新;测试里没有硬编码的旧 bundle 版本字面量 | 3 秒 |
| M3 | `apply_layout.py blame --revs … --commit` | 三个机械提交登记进 `.git-blame-ignore-revs` | 1 秒 |

## 闸门(Windows 本机,D26)

- M1 逐文件 R100:`git diff -M100% --name-status` 全部 `R100`(工具在搬迁后自行核对,非 R100 即失败)。
- `normalize_includes --check`:0 改动、0 错误(模块根形式的 include 在分组搬迁下不变,P1-01 的设计目的在这里兑现)。
- `check_layers --layout final --enforce-parent-includes`:**0**(R1–R14 全为 0;P2-08 登记的 3 条 R3 例外仍在压制 tool_preamble 的三个消费方)。
- `validate_map --strict`、`check_file_size`、`check_ownership`:0。
- `check_doc_paths`:归档的 M2 后报告为 100 项,主要为历史文档和无后缀模块引用;后续 seed 路径另已迁移。路径说明由 P4-02 完成,一期最终静态验收统一归零。
- 前端:本树 `pnpm install --frozen-lockfile` 后 `pnpm test` 通过(架构测试读取改写后的 `tests/cpp_source_paths.json`),`pnpm build` 通过。
- 既有演练日志已核对:MSVC 2022 / Ninja / Release 全新构建通过,首次 configure/build/smoke/models-registry 退出码均为 0;加入 stb 头清单和 M2c 后的构建退出码同样全部为 0。首次完整构建耗时 5 分 08 秒。
- 归档 full.json:清单 5117、执行 5116、9 SKIP,首次有 1 个 ChannelBoundaryGuard 路径失败。M2c 修正 5 处扫描路径后,guard.json 实际运行 8 个守护/定向用例,0 失败、0 SKIP;未把该次定向复跑写成再次全量通过。最后的 Windows 全量按 D27 在一期实现结束后统一执行。
- targets-compare2.json 已核对:59 个目标;targets 和 tuples 的 added/removed 均为 0。stb 三个头以 ACECODE_THIRDPARTY_HEADERS 挂回原目标,未丢失头文件元组。

## 未做 / 留待正式搬迁与 5.4

- macOS / Linux / Deepin 构建、package.yml、Deepin `current_target()`、verify-package、TUI / desktop / 控制台冒烟五项:按 D26 留 tasks.md 5.4 与用户手工核对。
- 演练用的三个机械提交 SHA 只在演练分支有效,正式搬迁 M3 按新 SHA 重登记。

## P3-02 正式搬迁的执行清单(窗口内按顺序,预计 30–45 分钟)

1. 公告已在 CLAUDE.md / AGENTS.md(2026-09-29 06:00–18:00);进入窗口后先 `git fetch`,在最新 master 上打 tag `pre-src-layout`。
2. 新 worktree(或复用演练 worktree `reset --hard master` 后 `clean -fd`),依次:`apply_layout.py move --phase P3 --commit` → `rewrite --phase P3 --commit` → `apply_include_roots.py` 并 `git commit --amend`(并入 M2)或单独提交 → `seed --seed-version <当天>.1 --commit` → `blame --revs <M1> <M2> --commit`。
3. 闸门:上表全部重跑;全新目录构建 + 全量档单测 + 快照对照(以窗口前 master 的快照为基线,反查换算后 removed 必须 0)。
4. `--no-ff` 合入 master,打 tag `post-src-layout`,推送,解除冻结并更新公告;`.git-blame-ignore-revs` 已在 M3 登记。
5. 窗口期间 master 若被推进:丢弃已生成的提交,在新 master 上重新生成(全部由脚本完成,约 1 分钟)。

## Codex 接续(2026-09-28)

Claude 已按用户要求停止。以上构建退出码、full.json、guard.json、targets-compare2.json 和静态报告均于接续时只读核对,证据目录为 `N:/acecode-gtest-iso/p3/`;不重复执行已完成的演练。原执行清单中的分支、reset/clean、逐步提交及多平台验证不用于本次 D27 批量实施。后续在 master 上保留现有改动并重新生成未提交布局,末尾统一验收与交付。
