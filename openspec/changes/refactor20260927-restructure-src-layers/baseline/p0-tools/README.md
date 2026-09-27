# P0-03 工具验收与源码基线

本目录是 P0-03 的源码报告与工具验收证据,不是 P0-07 的四平台 G0。

- 固定 master base:`3ddb7d433f280181292e5cd2eede612b17cc8b42`(包含 P0-05)。
- 工具实现:`b0572c09`;合入该 master 后的复验提交:`2d3af9f0f970cdd74f6f1122ea6031421c4ccc72`。
- base 的 src tree:`496d7b36cf07f38ccdae31cec04e17e7957096f0`。
- base 的 tests tree:`bd25d5c71301b89d52f3b02a2bc8e90d19e5a32c`。
- 对 base 的 src/tests 差异仅新增 `src/layers.tsv`;没有修改运行期 C++ 或既有 C++ 测试。

## 实测结果

| 报告 | 结果 | 命令退出码 |
|---|---|---:|
| layers.json | 1473 个 src/tests C++ 文件;2753 条可解析的 src 内 include 边;1326 项违规 | 0,报告模式 |
| size.json | 31 个非豁免超限文件进入棘轮;另有 web 4 个、stb 3 个超限豁免 | 0,严格比较已记录基线 |
| ownership.json | 五类指标及补充裸句柄指标见下表;原语例外按精确作用域计数 | 0,严格比较已记录基线 |
| doc-paths.json | 检查 1109 处文档路径,80 项既有失效路径 | 0,报告模式 |
| map.json | 1081 个文件受搬迁映射影响;无目标碰撞/无非法路径/无未知目标模块 | 0,严格校验 |
| includes-src.json | 1124 行 `../` 分布在 366 个文件;连同子目录相对写法共需改 1152 行、370 个文件;解析错误为 0 | 1,检查出待规范化行 |
| includes-tests.json | 16 行 `../`,15 个文件;解析错误为 0 | 1,检查出待规范化行 |
| line-identity.json | 6952 行 AgentLoop、8827 行 main 全覆盖,没有丢行/重复/内容差异 | 0 |

行数总数是 **38**,不沿用任务里「37」的调研估计。31 个非豁免文件全部写入
`scripts/layers/size_baseline.txt`,没有通过移除真实超限项凑数。

分层违规项:R1=18、R2=11、R3=5、R5=6、R8=1267、R9=10、R10=1、R14=8;
R6/R7/R11/R12/R13 为 0。R4 在设计中没有定义,显式保留。一个 include
同时违反不同规则会计为多项;这不是“1326 条不同依赖边”。

| 所有权度量 | 数量 |
|---|---:|
| 裸 new / delete | 10 / 7 |
| detach | 16 |
| std::thread 资源声明/构造候选 | 104 |
| 可能逃逸的 `[this]` / `[&]` 回调候选 | 338 |
| set_*(T*) 延迟注入候选 | 34 |
| 补充:裸句柄候选 | 59 |

另外 284 处可证明在当前调用内结束的同步 lambda 单列在 `synchronous_captures`,
不与长寿回调混算。词法分析无法证明的回调标为 `requires-review`,仍进入棘轮。
裸句柄是所有权设计 §5 的第六行,母任务原文只列五类,故作为补充显式报告。
原语例外登记在 `layers.tsv` 的 `ownership_allow` 行,限定文件、作用域、指标、
数量、owner 与理由;业务目录没有获得整目录例外。

## 工具验收

在合入上述 master 后运行:

```text
python -m unittest discover -s scripts/refactor/tests -p test_*.py -v
Ran 28 tests in 23.237s
OK
```

用例覆盖:

- 真正的 `.claude/worktrees`、`.worktrees`、`.acecode/worktrees` 三类嵌套
  Git worktree 不被遍历或修改;未跟踪文件、gitlink 不作为源文件。
- 远端跟踪分支即使没有本地分支也被盘点;补丁等价提交与独有 src/tests 改动分开;
  子模块自身的脏状态不隐藏。
- CRLF/LF 混排、末尾无换行、平台 `#if` include、幂等、歧义拒绝;
  P1 先搬 helper 后,裸名和 `../` 引用仍能归一到 `test_support/`。
- 正式规则的反向依赖、语义禁止、apps 平级、窄边界、第三方封闭、唯一解析、
  旧根回流、单出口、行数增长、例外到期、test helper 放错目录均有故障注入。
- 原语合法的单次 detach 可登记;同函数第二次 detach 或同文件其它函数仍报告。
- 原行丢失、重复映射、错误目标内容均失败;本目录 identity 映射仅用于 P0 原行
  基线,不代表任何拆分任务已经完成。
- 实际 CMake C++ 项目 configure 与 File API 读取,确认 EXCLUDE_FROM_ALL 的
  smoke target 存在,并保留 `SOURCE_DEFINE` 和 `TARGET_DEFINE`;
  丢失一个 target/source 元组会在比较中出现。
- CMake define 中的 source/build 绝对路径仅在完整边界规范化,不会误改 `repo2`。
- gtest 参数化名称、真实 SKIP 理由、disabled/notrun 与 executed 清单分开。

`check_layers --enforce-parent-includes` 在此 base 返回 1,证明 P1 闸门会阻断
当前仍存在的 `../`。完整 R8 的裸根头与 version 名称歧义将在 P2 消除,
不能把完整 `--enforce R8` 提前当作 P1 唯一闸门。

## 后续阶段的证据边界

P0-07 仍需在固定原始 base、四个平台的全新 build 目录上采集 ACECode 本体的
File API 快照、全量 gtest/ctest 清单及实际 SKIP。本次 CMake fixture 与 gtest
XML 解析测试验证工具行为,没有伪装成 ACECode 四平台构建结果。后续新增 RAII
测试也不能混入原始 G0。文档路径 80 项及其它违规只作为当前报告基线,
不会因本任务完成而自动视为后续整改已通过。
