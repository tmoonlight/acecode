# P0-04 CMake 源文件护栏验证

本记录只确认已经执行的本机检查。四平台正式 G0 与远端 CI 尚待 P0-07
采集，P0-04 任务保持未勾选。

## 实现边界

- `acecode_require_sources` 对空清单、不存在的文件、目录和非字面路径报
  `FATAL_ERROR`，用于全部显式源码清单和 REMOVE_ITEM 的输入。
- `acecode_set_source_define` 先确认文件存在，再保留 APPEND 语义设置源属性。
  Deepin manifest、channel asset 路径和升级测试 fixture 的定义保持原值。
- `acecode_assert_known_roots` 读取同一份 `src/layers.tsv`；过渡期间显式
  `ALLOW_LEGACY` 读取迁移映射。P3 M2 必须移除该参数，拒绝旧目录回流。
- `ACECODE_TUI_DIRS` 与 `ACECODE_TUI_TESTABLE_SUBSETS` 分开维护；目录缺失、
  整体为空、剔除 testable 后为空均失败。commands/resume/path_reference/
  markdown 与三个指定的独立 helper 已预登记，不改变当前目标归属。
- `cmake/acecode_source_paths.cmake` 导出多个 target 共用的源路径和全部
  EXCLUDE_FROM_ALL 冒烟源码清单。11 个生产 `.mm` 即使在 Windows 配置也
  检查存在；OBJCXX 语言与编译选项仍按原有平台条件设置。
- 仅修改 CMake 与护栏测试；生产 C++、既有 GTest 注册、运行期行为均未修改。

## Windows 实测

工作树：`C:/Users/shaoh/.codex/worktrees/refactor-cmake-guards/acecode`。
改动前固定 base：`12052b9e`；其 C++ 源码、测试与 CMake 内容和原始 G0
源码 `3ddb7d43` 相同。最新主线 `6decc859` 已同步，只增加经过复验的
快照路径修复，没有改变 CMake 或生产源码。

MSVC 19.38、Windows x64、Ninja、Release、`BUILD_TESTING=ON`、
`ACECODE_BUILD_DESKTOP=ON`。两个独立全新目录分别为
`build-p0-04-before`、`build-p0-04-after`。使用已有 vcpkg 安装树作为只读
依赖输入，`VCPKG_MANIFEST_INSTALL=OFF`，不使用主仓的构建输出。

每个目录都先用 `cmake_target_snapshot.py --query` 写入 File API query，
再 configure；随后执行：

```text
python scripts/refactor/cmake_target_snapshot.py --build-dir build-p0-04-before --output build-p0-04-before/target-snapshot.json
python scripts/refactor/cmake_target_snapshot.py --build-dir build-p0-04-after --compare build-p0-04-before/target-snapshot.json --output build-p0-04-after/target-snapshot.json
```

结果：**59 个 target、3535 个元组全部一致**。target 依赖、源码、语言、
编译定义、编译选项、generated 标志均未发生差异；重复元组保留计数。
六个 Windows 可用的 EXCLUDE_FROM_ALL 冒烟目标都在快照中。
`acecode` 保留 13 个 TUI-only `.cpp`，`acecode_testable` 保留 41 个
可测 TUI/markdown `.cpp`，`acecode-desktop` 没有取得这些源文件。

在真实工作树依次执行四次故障注入，每次在 `finally` 中恢复原始字节：

| 故障 | configure 结果 |
|---|---|
| NATIVE_BRIDGE 清单的 config.cpp 改成不存在的路径 | FATAL，指出清单与缺失路径 |
| manifest 源属性指向不存在的文件 | FATAL，指出 source compile definition |
| 不在 Windows 编译的 native_macos.mm 路径拼错 | FATAL，指出 shared target source paths |
| 清空 ACECODE_TUI_DIRS | FATAL，指出 TUI source set is empty |

恢复后 configure 成功，再次快照比较零差异。原始日志存于
`build-p0-04-after/fault-*.log`，没有将构建日志提交到仓库。

## 回归测试与快照工具修正

```text
python -m unittest discover -s scripts/refactor/tests -p test_*.py
Ran 36 tests
OK
```

其中五项新 CMake 护栏测试覆盖缺失/空源清单、源属性保真、属性路径
拼错、最终布局拒绝旧根和类似前缀、过渡布局只接受登记路径。

实际双目录比较发现 File API 对 source 内的 build 产物使用相对路径；
原采集器只规范化绝对路径，会把构建目录名称变化误报成源码变化。
另一个真实 Visual Studio configure fixture 发现 ZERO_CHECK 的重生成
规则包含绝对构建目录的哈希。修复已独立作为 P0-03 后续提交
`6decc859` 合入主线：保留生成规则和元组计数，仅消除构建位置差异。
三个新增采集器回归测试同时验证双 fresh build、路径边界和重复计数。

`git diff --check` 通过。系列不变量 §7.3 第 9 条的 Windows 构建边界
由完整 File API 比较守护；macOS 的 OBJCXX、Deepin 定义及另外平台
快照仍需真实 CI 复核，不能由 Windows 结果推断为通过。

## 同步主线后的原始 G0 对照

合入主线 `8acc3863` 后，在空闲工作树
`C:/Users/shaoh/.codex/worktrees/refactor-tools/acecode` 检出本任务分支，
验证提交为 `129f5106`。原已验证版本 `0a56e436` 到该提交的 CMake、
src 与 tests 输入没有变化；迁移工具随主线同步。

再次使用全新 `build-p0-04-latest` 进行 Desktop ON / Release 配置，
32.578 秒完成，返回 0。此次直接对照 P0-07 在固定原始源码
`3ddb7d433f280181292e5cd2eede612b17cc8b42` 上实际采集的 File API：
59 个 target、3535 个元组仍逐项相同。不是用另一份当前工作树配置
冒充原始 G0。完整比较摘要、输入 hash 与配置命令见
[P0-04-windows-g0-diff.json](P0-04-windows-g0-diff.json)。

原始 G0 测试阶段的临时目录隔离缺陷不影响这份配置期目标快照；测试
证据正在另行补采，本记录不据此宣称全平台 G0 已完成。合入主线后的
Python 工具集 56 项全部通过（50.446 秒）。未运行或声称完成新的
C++ 全量构建，任务仍等待其余平台和远端 CI 验收。
