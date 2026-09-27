#pragma once

// lsp 侧对 platform/process 原语的兼容别名(refactor20260927 P2-03)。
// 子进程 spawn(原 lsp_process 的 LspProcess / LspSpawnOptions)与 PATH 探测
// (原 lsp_which)已下沉到 platform/process,这里保留原来的 lsp:: 名字,
// lsp 内部调用点逐步改用 platform:: 名字;lsp 之外的新代码直接用 platform::。

#include "platform/process/piped_process.hpp"
#include "platform/process/which.hpp"

namespace acecode::lsp {

using LspProcess = platform::PipedProcess;
using LspSpawnOptions = platform::SpawnOptions;
using FileExistsFn = platform::FileExistsFn;
using platform::quote_windows_arg;
using platform::which;
using platform::which_in;

} // namespace acecode::lsp
