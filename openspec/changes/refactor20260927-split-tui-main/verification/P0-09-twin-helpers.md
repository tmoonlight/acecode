# P0-09 去掉 main.cpp 与 tui_helpers.cpp 孪生 helper 的验证记录

实现提交 `320fe1d7`(认领 `89b0b3f7`),基线为合并 Phase 0 各分支后的 master
`d47a8ace`。全部改动只动 `src/main.cpp`、`src/tui/terminal_key_event.hpp`、
`src/tui/tui_helpers.cpp` 的一条注释、`CLAUDE.md` 一段说明,并新增一个测试文件。

## 改动范围

- `main.cpp` 8601 → 8099 行(−573 / +71)。删除的 file-static 块是原 716–1150 行
  (合并后 714–1150 行)的 26 个定义:`EN_THINKING_PHRASES` / `ZH_THINKING_PHRASES`
  两个数组与 24 个函数;另删 `is_terminal_key` / `is_terminal_codepoint` 两个包装的
  声明与定义、从未使用的 `contains_box` lambda、重复的 `#include "tui_state.hpp"`。
- 删除前逐个定义与 `tui_helpers.cpp` 比对(脚本按大括号截取定义,剥掉
  `acecode::` / `ftxui::` / `tui::` 限定、默认实参与注释后比较):22 个逐字相同,
  `renderable_tool_summary_line` 与 `render_tool_result_lines_preserving_breaks`
  只差注释与限定写法,两个短语数组按字符串序列比较 md5 相同。渲染行为不变。
- 调用点改写:`is_terminal_key` 23 处、`is_terminal_codepoint` 11 处改为
  `tui::matches_terminal_*`(包装原本就只是转发,默认忽略 CapsLock / NumLock 的语义
  由被调函数的默认实参保持);`kTerminalCtrl/Shift/Alt` 20 处与 `is_alt_v_event` /
  `is_alt_a_event` 2 处改为 `tui::` 限定,定义挪进 `tui/terminal_key_event.hpp`
  (`inline constexpr` / `inline` 函数,与原 file-static 版本逐字同义);孪生 helper 的
  28 处调用改为 `tui::` 限定。
- 负载 chip 的写入目标 `g_model_load_percent.store(pct)` 改为
  `acecode::tui::g_model_load_percent`,与 `tui_helpers.cpp` 里 `render_model_load_chip`
  读的是同一个原子。
- 保留 200–203 行的两条前置声明(MR-12,等 B-04 外提 status_line 后再删)。
- `CLAUDE.md`「两份同名实现」一段改为指向唯一实现;`tui_helpers.cpp:497` 的
  "Keep in sync with the file-static twin" 注释改写(保持 2 行,避免触发 R12 棘轮)。

## 新增测试

`tests/tui/model_load_chip_test.cpp`(3 条,中文注释写明触发场景 / 期望 / 回归表现
「负载 chip 永不显示或停在旧值」):写入 `acecode::tui::g_model_load_percent` 后
`render_model_load_chip()` 渲染出 `NN%`;更新后下一帧可见新值;−1 时为空。
进程级原子用 RAII 守卫恢复。

## 验证

全新目录 `build-p0merge`(Ninja、MSVC 19.4x、Release、`BUILD_TESTING=ON`、
`ACECODE_BUILD_DESKTOP=ON`,只读复用主仓 vcpkg 依赖树):

| 检查 | 结果 |
|---|---|
| `cmake --build build-p0merge --target acecode` | 成功(main.cpp 重编 + 链接) |
| `cmake --build build-p0merge --target acecode_unit_tests` | 成功 |
| `--gtest_filter=ModelLoadChip.*:TerminalKeyEvent*.*:RegularSidebar*.*:AgentLoopTurnSteering.*:…`(与 P0-11 用例同批) | 42 / 42 通过 |
| `check_file_size.py --strict` | 通过(main.cpp 8099 < 基线 8827;tui_helpers.cpp 1386 = 基线) |
| `check_ownership.py --strict`、`check_layers.py`、`validate_map.py --strict` | 通过 / 报告模式 0 新增 |
| `git diff --check` | 通过 |

Windows 全量单测与四平台 CI 结果统一记在 restructure 的
`verification/P0-acceptance.md`(post-p0 采集)。

## 未由本记录覆盖的手工项

- 任务里「手工逐个比对底栏的 chip(token、缓存命中、模型负载)」与「跑手工清单第 2、3
  小节」需要真人在终端里操作;本次只以逐函数比对证明渲染代码逐字相同,并用单测
  钉住负载 chip 的数据通路。请用户在方便时跑一次 TUI 核对底栏。
