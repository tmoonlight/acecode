# P0-05 测试路径验证

## 范围与实测差异

- 实施起点：`2637bc98`，任务分支 `refactor20260927/P0-05`。
- 提交前已将主线 `37737a59` fast-forward 合入本任务分支，再构建并复验；未修改 master。
- 七个指定测试文件实际含 **15 处**固定层数的仓库根定位，其中 `default_skill_seeder_test.cpp` 为 9 处；任务中的 17 为旧盘点数字。另替换 `channel_boundary_guard_test.cpp` 的专用查找器，共 16 处调用统一 helper。
- `tests/test_support/repo_root.hpp` 从源文件或目录向上查找，要求同时存在 `CMakeLists.txt` 与 `.git`，支持普通仓库的 `.git` 目录和 worktree 的 `.git` 文件。没有根标记时抛错，不回退到当前工作目录。
- `bridge_test` 在判断 `node_modules` 前先验证脚本和 package manifest；缺依赖可以 SKIP，受跟踪资产路径错误必须 FAIL。
- 边界测试的每个扫描入口都断言存在且含文件。旧 `openspec/changes/add-remote-control` 未被 Git 跟踪，也没有归档或 specs 对应路径；现在扫描现存的远程控制提案 `openspec/changes/add-rc-session-navigation`。只保留相关提案的守护范围，不把其它历史文档或未跟踪提案纳入。
- `tests/CMakeLists.txt` 只增加测试根目录这一行 include 路径。P1-01 无须重复添加；没有增加、移除或改名 GTest 用例。

## 本机环境与独立构建

- Windows x64，Visual Studio 2022 Enterprise，MSVC `19.38.33133.0`，工具目录 `14.38.33130`。
- 使用本任务工作树的 `build-p0-05`，不在主仓构建，也不终止用户进程。
- vcpkg 根：`C:/Users/shaoh/AppData/Local/acecode-dev/vcpkg`。
- 只读复用已安装依赖：`C:/Users/shaoh/acecode/build/windows-x64-desktop-release/vcpkg_installed`，triplet 为 `x64-windows-static`。`VCPKG_MANIFEST_INSTALL=OFF` 保证配置不向该目录安装或更新依赖。
- 先在本工作树执行 `git submodule update --init --recursive`。没有修改子模块内容。

在工作树根的 PowerShell 中加载 MSVC 环境：

```powershell
$aceDevEnvironment = & $env:ComSpec /d /c 'scripts\dev_windows_env.bat --print-env'
if ($LASTEXITCODE -ne 0) { throw 'MSVC environment initialization failed' }
foreach ($aceEnvLine in $aceDevEnvironment) {
    $aceEnvPair = $aceEnvLine -split '=', 2
    if ($aceEnvPair.Length -eq 2 -and $aceEnvPair[0] -notmatch '^=') {
        [Environment]::SetEnvironmentVariable($aceEnvPair[0], $aceEnvPair[1], 'Process')
    }
}

cmake -S . -B build-p0-05 -G Ninja -DCMAKE_BUILD_TYPE=Release `
  '-DCMAKE_MAKE_PROGRAM=C:/Program Files/Microsoft Visual Studio/2022/Enterprise/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe' `
  '-DCMAKE_TOOLCHAIN_FILE=C:/Users/shaoh/AppData/Local/acecode-dev/vcpkg/scripts/buildsystems/vcpkg.cmake' `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static `
  '-DVCPKG_INSTALLED_DIR=C:/Users/shaoh/acecode/build/windows-x64-desktop-release/vcpkg_installed' `
  -DVCPKG_MANIFEST_INSTALL=OFF -DBUILD_TESTING=ON
cmake --build build-p0-05 --target acecode_unit_tests --parallel 3
```

本任务只验证测试路径，未构建前端；CMake 使用现有的最小 Web 资产回退页。模型目录 HTTP 用例不依赖前端页面。

## 验证证据

- 对所有 Git 跟踪的测试源文件，将修改前后的 `TEST`、`TEST_F`、`TEST_P`、typed test 与实例化注册表达式逐文件比较，**5,043 条全部一致**。
- 在独立构建目录分别编译八个测试源的修改前版本和最终版本，执行 `acecode_unit_tests --gtest_list_tests`，**5,033 个运行时用例名称及顺序完全一致**。比较时移除 `# GetParam()` 注释里的运行时指针地址，避免 ASLR 导致误报；不删除或过滤任何用例名。
- 原生 C++ 探针验证了默认 helper 起点、源文件起点与目录起点；将真实测试源复制到更深的临时目录后，两种 `.git` 标记均能找到同一个仓库根。嵌套目录单独存在 `CMakeLists.txt` 不会误判为根。从仓库外的临时工作目录运行也能定位正确。
- 没有标记、仅有 `CMakeLists.txt`、仅有 `.git` 三种情况均抛错并返回非零退出码。
- 将真实 `ChannelBoundaryGuard` 源复制到更深目录，使用 MSVC 与同一 GTest 库编译运行：1 个用例通过。
- 对同一边界用例注入缺失扫描根和空扫描根：均返回退出码 1，并分别给出 `Scan root missing`、`Scan root contains no files`。测试数据使用系统临时目录，编译输出留在独立构建目录。
- 全新目录的 `acecode_unit_tests` 完整构建成功，994 个构建步骤通过。相关 **119 个用例通过，0 失败、0 SKIP**；范围包含本任务修改涉及的全部测试套件、修改的模型目录 HTTP 用例和 7 个提示词字节稳定性守护用例。
- 使用真实测试二进制执行 bridge 故障注入，每次都检查退出码与 GTest XML：

  | 状态 | 退出码 | 失败数 | SKIP 数 |
  |---|---:|---:|---:|
  | 脚本与 manifest 存在，移走 `node_modules` | 0 | 0 | 1 |
  | 缺失脚本，同时缺 `node_modules` | 1 | 1 | 0 |
  | 缺失 manifest，同时缺 `node_modules` | 1 | 1 | 0 |
  | `node_modules` 为普通文件 | 1 | 1 | 0 |
  | `npm ci --no-audit --no-fund` 安装固定版本依赖后 | 0 | 0 | 0 |

  故障注入只临时移动本工作树的资产文件和依赖目录，并在 `finally` 中恢复；没有触碰用户运行中的进程。正常启动用例使用 `--setup-only`，不连接账号。
- `git diff --check` 通过。

相关测试复验命令：

```powershell
./build-p0-05/tests/acecode_unit_tests.exe `
  '--gtest_filter=AiThemeSeedTest.*:ChannelBoundaryGuard.*:ChannelBridge.*:DefaultSkillSeedRegistryTest.*:DefaultSkillSeederTest.*:HookRegistry.*:ModelCatalogHandler.*:ModelsHandler.*:WebServerHttp.ModelCatalogRoutesReturnBoundedCanonicalLocalData:AgentLoopTermination.RequestPrefixIsByteStableAcrossIterationsInATurn:SystemPromptTest.*ByteStable*:SystemPromptTest.NonGptModelStateIsByteIdenticalToLegacyPrompt' `
  '--gtest_output=xml:build-p0-05/targeted-results.xml'
```

本机复核产物均位于独立构建目录：`targeted-results-after-master.xml`、`gtest-inventory-{baseline,final}.names.txt`、`runtime-inventory-comparison.log`、`repo-path-checks.log`、`bridge-path-checks.log`。构建日志和临时探针不提交到仓库。

本任务不修改生产运行时逻辑。涉及的系列不变量守护为提示词缓存前缀的字节稳定性；关联验证中一并运行 `RequestPrefixIsByteStableAcrossIterationsInATurn`、`SystemPromptTest` 的 byte-stable 用例与 `NonGptModelStateIsByteIdenticalToLegacyPrompt`。
