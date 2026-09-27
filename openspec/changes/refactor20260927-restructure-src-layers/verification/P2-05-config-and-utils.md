# P2-05 config 与 utils 拆分验证记录

2026-09-28,分支 `refactor20260927/P2-05`,工作区 `N:/Users/shao/acecode-p2-05`。
用户确认原 Claude 任务及子任务已停止后,由 Codex 接手。主线 `0c0e37cb` 已通过 `f2ef7eaf` 合入;接手前的源码修改保留并继续审查。

## 范围与实现

- 权限模式枚举和模式名转换移到 `config/vocab/permission_mode.hpp`;配置不再依赖完整的 `PermissionManager`。真正使用该类的 `sandbox/exec_permission.hpp` 显式包含它。
- models.dev 的纯构造和格式化移到 config,全局 registry 缓存移到 `provider/models_dev_catalog_cache`。移除原来只有声明、无定义和调用的 `provider_catalog_snapshot`。
- `state_file` 的五组业务逻辑全部迁回所属模块:模型探测缓存到 provider,搜索地区缓存到 tool,活跃 / 首页工作区到 desktop,斜杠命令用量到 tui。底层只管理状态文件、同步事务和锁。JSON 字段、容错、暂停写入和失败返回语义保持。
- 安全写、MCP 配置拦截移到 `tool/safe_text_write`;四处带工具名的报错提示移到 `tool/text_file_errors`。编解码层返回结构化错误,工具层使用当前模型工具名映射生成原有文案;Web 只改 include 和调用点。
- 已有纯搬迁覆盖 file_operations / tool_errors / tool_args_parser、token_tracker、path_validator / shell guard / interaction_mode、upgrade HTTP / semver、theme_id / pointer_appearance 等 P2-05 项目。旧路径不保留转发头。
- 状态与安全写测试随职责拆分,保留原 suite / case 名;增加两项工具别名与禁止有损写入的回归。原并发测试扩展为五组模块混合写入与暂停写入的检查。

## 范围偏差的处理

接手时的未提交修改只抽出了模型探测缓存,理由是其余四组没有向上 include,并将其延期到 P4 / 二期。这未完整落实原任务的职责划分。按用户再次确认,五组全部在 P2-05 完成(D25)。

原修改还把工具名作为参数传给底层,让带工具名的文案继续留在 utils,并使 Web / resume 使用默认 `file_read`。本次将文案真正上移;Web 和模型工具沿用原来的动态名称,纯内部的恢复读取不展示错误文案。

## 本地构建与测试

使用独立的全新构建目录 `build-p2-codex`,只读复用 canonical master 的 vcpkg 依赖。先在本工作区完成 `pnpm install --frozen-lockfile` / `pnpm build`,再使用 VS 2022 x64 开发环境配置 CMake:

```text
cmake -S . -B build-p2-codex -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DACECODE_BUILD_DESKTOP=ON -DVCPKG_MANIFEST_INSTALL=OFF -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake -DVCPKG_INSTALLED_DIR=N:/Users/shao/acecode/build/vcpkg_installed -DVCPKG_TARGET_TRIPLET=x64-windows-static -DVCPKG_MANIFEST_FEATURES=tests
cmake --build build-p2-codex --target acecode acecode-desktop acecode_unit_tests computer_use_native_smoke computer_use_broker_smoke agent_browser_host_smoke agent_browser_pointer_demo acecode_upgrade_restart_smoke --parallel 10
```

| 检查 | 结果 |
|---|---|
| CLI / Desktop / GoogleTest / 五个 EXCLUDE_FROM_ALL 冒烟目标构建 | 通过;冒烟程序仅构建,此处不声称已做交互验证 |
| 定向 GoogleTest | 151 项通过,0 失败;覆盖状态、搜索地区、编码 / 文案、权限与 MCP |
| 完整 GoogleTest 与 G0 用例清单对照 | 通过:列出 5117 / 执行 5116 / 跳过 9 / 失败 0,退出码 0;相对同机 P2-02 后的 master 只增加本次两项回归,原用例和 SKIP 用例集合均未增删 |
| 前端 `pnpm test` / `pnpm build` | 通过 |
| `python -m unittest discover -s scripts/refactor/tests -p 'test_*.py'` | 74 项通过 |
| `normalize_includes.py --scope src --check` / `--scope tests --check` | 通过 |
| `validate_map.py --strict` / `check_file_size.py --strict` / `check_ownership.py --strict` | 通过,未提高基线 |
| `check_layers.py --enforce-parent-includes` | 通过已启用的阻断项;全量过渡报告 88 项,config / utils 的反向依赖已消失;其余属于后续 P2 |
| `openspec validate refactor20260927-restructure-src-layers --strict` | 通过 |
| `git diff --check` | 通过 |
| 帮助站点生成 | 从 group3 / group4 源文件重新生成 49 篇,生成值与当前 sources.json 一致 |

GoogleTest 的 HOME / USERPROFILE / APPDATA / LOCALAPPDATA / TEMP / TMP 都指向仓库外的独立目录,未使用用户的运行时配置。首轮目录在用户 TEMP 下嵌套过深,触发 Windows seed 复制路径过长以及窄屏路径文本截断;首轮记录保留为 `p2-05-gtest-long-path.json`。改用 `N:/ac-p205-0928/` 后,同一二进制的 10 项失败用例全部通过(2.797 秒),无代码修补或断言放宽。完整复跑使用单独的短路径 home/tmp,结果 5116 项执行、9 项跳过、0 失败,退出码 0;日志为 `p2-05-gtest-run.log`,清单为 `p2-05-gtest.json`。

## CMake 目标与编译归属

通过 File API 采集实际配置。与 `N:/Users/shao/acecode-p2-shared/master-windows-targets.json` 比较时,两侧都用 `translate_for_comparison(reverse=True)` 归回旧路径;仅将同一个 vcpkg 安装目录的绝对 / 源根相对前缀统一。没有忽略 target、定义、选项或源文件差异。

- 目标数 59 → 59,目标依赖没有变化;Desktop 不链接 `acecode_testable`。
- 编译元组 3572 → 3622。既有元组删除数为 0;搬迁文件的归属、语言、定义和选项一致。
- 增加 50 个元组,与逐项白名单完全一致:7 个拆出实现文件、8 个声明头、5 个拆分测试文件、6 个 testable 实现分别被 5 个原消费目标引用的 30 个对象元组。
- 6 个通用可测试实现进入 `acecode_testable`;`desktop/workspace_state.cpp` 进入原有的 `acecode_desktop_support`;TUI 用量实现已登记 `ACECODE_TUI_TESTABLE_SUBSETS`。
- 新增编译元组均沿用对应目标既有的编译属性;未出现新的编译选项或宏。

## 迁移检查与未完成验收

`migrate_branch.py --check` 已执行。它检查整个最终目录方案,当前返回 1:仍报告尚未进行的 P3 分组搬迁 / 文档迁移,以及后续 P2 的分层问题。其 include 规范化、映射碰撞、所有权、seed 完整性子检查均为 0 项。独立的过渡布局检查如上,不能把整体返回 1 记为已通过。

按设计中的文档安排,seed 中权限头路径与其版本 / 哈希将在 P3 M2b 一起更新;不单独修改 seed 的生成契约。P2-05 的九个遗留 ref 迁移提示登记在 branch-inventory.md。

源码提交 `8b182f184e57d139e703ceb794ffd3d1f2eaf558` 已触发 [四平台 CI](https://github.com/tmoonlight/acecode/actions/runs/36334375820),启用完整测试及 Deepin 构建。四平台构建现均通过,测试结果见下表;本任务仍不提前勾选验收。完整构建日志、定向 XML、完整清单、目标对照与各检查 JSON 保存在仓库外 `N:/Users/shao/AppData/Local/Temp/codex-refactor20260928/`。

## 四平台原始交付复核与集成版本

源码 `8b182f18` 的四平台结果:

| 平台 | target / 编译元组 | 列出 / 执行 / SKIP / 失败 |
|---|---|---|
| Windows x64 | 59 / 3622 | 5117 / 5110 / 10 / 5 |
| Linux x64 | 50 / 3536 | 5037 / 5028 / 16 / 0 |
| macOS arm64 | 57 / 3681 | 5042 / 5035 / 14 / 11 |
| Deepin x64 | 13 / 1393 | 不启用测试 |

目标和既有编译元组无删除。相对 post-p0,三个测试平台各增加 93 个元组(前序 P2-02 的 43 + 本任务的 50);Deepin 增加 40(前序 19 + 本任务 21)。新增实现、头、拆分测试和生成对象均逐项登记,编译属性沿用所属目标。

三个测试平台均只增加本任务的两项回归用例,没有删除用例或增加 SKIP。macOS 的 10 项原基线失败与 P1 / P2-02 已登记的 SSE 时序抖动仍保留。Windows 的 4 项原基线失败之外,多一项 `AgentLoopTurnSteering.InterruptStartsStructuredTurnBeforeOrdinaryQueue`:等待第二次 provider 调用的 250ms 断言失败。该用例正文未改动(本任务只改 permissions 头路径),在同一源码的本机全量及 Linux / macOS CI 中通过;本机原二进制追加 20 次运行全部通过(`p2-05-steering-repeat.log`)。保留该次 CI 失败,继续用集成版本核对,不删除断言或扩充跳过条件。

Deepin 的 provenance 中 `source_revision` 为空,原因与 P2-03 / P2-04 相同:容器 Git 报 dubious ownership。已从 build job 108662299627 的 checkout 日志核对完整的 `8b182f184e57d139e703ceb794ffd3d1f2eaf558`,与 source_ref 相同;没有用空字段证明来源。

集成提交 `4a3366e68b48cfd9dd1b9d2653925d9ef01f0e0c` 接入 P2-04 `aa6872f9`(含已合入 master 的 P2-03),六处冲突只合并两边的模块路径与 include 责任,五组状态逻辑和工具错误适配保持。补齐 P2-02 / P2-03 / P2-04 六个纯搬迁、纯 include 提交在 `.git-blame-ignore-revs` 的登记。

集成后 src / tests include、映射、分层阻断项、行数、所有权、OpenSpec strict、前端完整测试均通过。全新本机构建 `build/p2-05-integrated-4a3366e6` 已完成 CLI / Desktop / GoogleTest / 五个冒烟目标共 1058 步,退出码 0。File API 为 59 target / 3623 元组,相对原始交付只多前序 P2-03 的别名头,无目标变更或既有元组删除、编译属性变化;等价报告为仓库外 `p2-05-integrated-target-equivalence.json`。

集成版本完整本机测试正在独立短路径 `N:/a05i/` 执行;[四平台 CI 36336950956](https://github.com/tmoonlight/acecode/actions/runs/36336950956) 正在执行,Deepin 配置与构建已完成。P2-04 验收并合入后再合入本任务。
