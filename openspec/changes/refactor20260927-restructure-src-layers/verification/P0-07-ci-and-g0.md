# P0-07 CI 与基线 G0 验证记录

原认领人 Codex-testpaths 在另一台机器上采过一次 Windows 的原始 G0(P0-04 / P0-08 /
P0-11 的记录都以它为对照),但没有推送分支;2026-09-27 由 Claude-phase0 接手
(`e712690b`),在 Phase 0 各分支合入 master 之后完成。相关提交:`d47a8ace`(两份
工作流)、`3426fbe0`(对照工具与 D23)、`6396b884` / `989b1a0b`(清单工具落盘日志、
ctest 隔离模式)。

## 1. test.yml 的 layer-lint 作业

`.github/workflows/test.yml` 新增 `layer-lint (report mode)`,`unit-tests` 作业
`needs` 它,因此排在 C++ 构建之前;`pull_request` 与 `push: master` 都触发,即
「四类 lint 基线已接入 PR」。

| 步骤 | 命令 | 阻断? |
|---|---|---|
| 分层 R1–R14 | `check_layers.py --output layers.json` | 否(报告模式,当前 1326 项违规待 P1/P2 消化) |
| 文档路径 | `check_doc_paths.py --output doc-paths.json` | 否(80 项既有失效路径) |
| include 规范化 | `normalize_includes.py --check --scope src/tests` | 否(P1 才处理,失败只打 notice) |
| 行数棘轮 R12 | `check_file_size.py --strict` | **是**(按 `scripts/layers/size_baseline.txt` non-increasing) |
| 所有权棘轮 R15 | `check_ownership.py --strict` | **是**(按 `scripts/layers/ownership_baseline.json`) |
| 迁移映射 | `validate_map.py --strict` | **是** |
| 工具自测 | `python -m unittest discover -s scripts/refactor/tests` | **是** |

报告以 artifact `layer-lint-reports` 发布。首次在 master 上运行:run 36292991045
(`d47a8ace`)通过;之后每次推送都绿。

## 2. 只能手动触发的 refactor-matrix 工作流

`.github/workflows/refactor-matrix.yml`,`workflow_dispatch` 输入:`source_ref`
(要采集的源码提交,默认当前 ref)、`label`(artifact 标签)、`run_tests`、
`include_deepin`。设计要点:

- **工具与源码分离**:工具(`scripts/`)恒取工作流所在提交,源码可以指向更早的基线
  提交(原始 G0 = `3ddb7d43`,那时 `scripts/refactor` 还不存在);源码检出到
  `source/`,构建目录 `source/build`。
- **四个平台**:windows-2022(MSVC + Ninja,x64-windows-static)、macos-15(arm64-osx)、
  ubuntu-22.04(x64-linux)三者 Release + `BUILD_TESTING=ON` + Desktop ON,先写
  File API query 再 configure,采集目标快照,构建 acecode / acecode-desktop /
  acecode_unit_tests,运行全量清单;linux-deepin-x64 走 package.yml 同款
  `buildpack-deps:buster` 容器(MinSizeRel、`BUILD_TESTING=OFF`、DTK SDK),容器里的
  Python 太旧,只上传 File API reply,由后续 ubuntu 作业转成快照。
- **清单采集经 ctest 逐用例独立进程**(`gtest_inventory.py --via-ctest`):首轮用单进程
  gtest 时,macOS 上原始树挂起、合并树 3 分 41 秒后崩溃且没有任何 XML;ctest 模式下
  崩溃 / 挂起(`--test-timeout 900`)只记为该用例的 failure,其余结果照常入档,CTest 的
  JUnit 报告给出 SKIP(来自 gtest_discover_tests 的 SKIP_REGULAR_EXPRESSION)与失败。
  完整运行输出落在 artifact 的 `gtest-run.log`。
- **Linux 另外采集四类 lint 报告**(源码取 `--repo source`),`continue-on-error`:
  原始基线提交上没有 `src/layers.tsv`,该步骤在那种 ref 上允许失败。
- 每个 artifact 带 `provenance.json`(平台、源码 / 工具提交、run id、cmake 版本、
  configure 参数)。原本就失败的用例只记入清单,不作为阻断条件。

## 3. 对照规则与工具

design.md §9 **D23**:原始 G0 固定在 `3ddb7d43`;P0 之后的目标快照与原始 G0 逐元组
比较时,只允许「移除 = `src_layout_map.tsv` 的 `delete` 行(P0-08 的 20 个文件)及其
生成对象」与「新增 = P2-01 的 6 个原语文件(File API 会把显式登记的头文件也列进
target 源清单)或 `acecode_unit_tests` 下的 `tests/` 新源」两类差异,target 集合不得增减。
`scripts/refactor/compare_snapshots.py` 实现该判定(`unexpected` 即退出码 1),
gtest 清单差异只报告。单测 `scripts/refactor/tests/test_compare_snapshots.py`(6 条)、
`test_gtest_inventory.py`(3 条)。

P0 验收完成时的快照另存为 `baseline/g0/post-p0/`,P1 起以它为对照;原始 G0 只用于追溯。

## 4. 采集运行

| run | label | 源码 | 说明 |
|---|---|---|---|
| 36292991337 | g0-original | `3ddb7d43` | 单进程 gtest;Deepin / Linux / Windows 完成,macOS 挂起 |
| 36292993278 | p0-merged | `d47a8ace` | 合并树;Linux 5024 / 0 失败,macOS 单进程崩溃 |
| 36294465653 | g0-post-p0 | `6396b884` | 单进程 gtest,交叉对照 |
| 36294798063 | g0-original-v2 | `3ddb7d43` | ctest 隔离模式,不含 Deepin(已有) |
| 36294799561 | g0-post-p0-v2 | `989b1a0b` | ctest 隔离模式,**正式 post-p0** |

采集结果、四平台对照与归档清单见 `baseline/g0/README.md` 与 `P0-acceptance.md`。
