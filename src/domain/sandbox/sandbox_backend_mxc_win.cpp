// Windows MXC 后端口子(openspec align-codex-sandboxing D9)。
//
// 微软 MXC(github.com/microsoft/mxc,MIT)在零售版 Windows 11 上只能落到
// 「AppContainer + DACL」档:能真断网、删除 / 改名默认拒绝,但要管理员跑一次
// host-prep(系统盘根 ACE + NUL 设备描述符,后者每次开机重置),而且每次运行
// 都要给整棵可写树打可继承 ACE 再撤掉。Codex 自己只接 25H2+ 的内核原生档。
//
// 本构建不捆绑 MXC:这里只保留探测入口,让 `config.sandbox.windows_backend = "mxc"`
// 有明确的不可用原因,而不是静默退回受限令牌。接入时需要实现两件事:
//   1. probe_windows_mxc():探测 wxc-exec / processmodel 是否可用;
//   2. bash_tool 的 Windows 分支按 BackendKind::WindowsMxc 把 SandboxPolicy 翻译成
//      MXC ExecutionRequest(writable_roots → readwritePaths,readable_roots →
//      readonlyPaths,denied_paths → deniedPaths,network_access → capabilities)。

#ifdef _WIN32

#include "sandbox_backend.hpp"

namespace acecode::sandbox {

BackendProbe probe_windows_mxc() {
    BackendProbe probe;
    probe.kind = BackendKind::WindowsMxc;
    probe.available = false;
    probe.network_enforced = true;
    probe.network_best_effort = false;
    probe.reason = "MXC backend is not bundled in this build; set config.sandbox.windows_backend to "
                   "\"restricted-token\" or leave it unset";
    return probe;
}

} // namespace acecode::sandbox

#endif // _WIN32
