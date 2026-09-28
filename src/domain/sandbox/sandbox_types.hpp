#pragma once

// 沙盒子系统的基础枚举(openspec add-auto-mode-sandbox / align-codex-sandboxing)。
// 单独放一个轻量头,让 ToolContext(tool_executor.hpp,被全仓库 include)只需要
// 它和策略结构,不用拖进分类器 / 规则 / 决策表。

#include <string>

namespace acecode::sandbox {

// 一次 bash 执行的沙盒模式。FullAccess = 不沙盒。
enum class SandboxMode { FullAccess, WorkspaceWrite, ReadOnly };

inline const char* sandbox_mode_name(SandboxMode m) {
    switch (m) {
        case SandboxMode::FullAccess:     return "full-access";
        case SandboxMode::WorkspaceWrite: return "workspace-write";
        case SandboxMode::ReadOnly:       return "read-only";
    }
    return "full-access";
}

// WindowsMxc 是给微软 MXC(AppContainer / 进程安全环境)后端留的口子
// (align-codex-sandboxing D9):枚举、配置与探测入口已就位,本构建的探测
// 恒返回不可用。
enum class BackendKind { None, WindowsRestrictedToken, WindowsMxc, MacosSeatbelt, LinuxBwrap };

inline const char* backend_kind_name(BackendKind kind) {
    switch (kind) {
        case BackendKind::None:                   return "none";
        case BackendKind::WindowsRestrictedToken: return "restricted-token";
        case BackendKind::WindowsMxc:             return "mxc";
        case BackendKind::MacosSeatbelt:          return "seatbelt";
        case BackendKind::LinuxBwrap:             return "bwrap";
    }
    return "none";
}

// config.sandbox.windows_backend 的取值。
enum class WindowsBackendChoice { RestrictedToken, Mxc };

inline const char* windows_backend_choice_name(WindowsBackendChoice choice) {
    return choice == WindowsBackendChoice::Mxc ? "mxc" : "restricted-token";
}

// 文件系统条目的访问级别(对齐 Codex FileSystemSandboxEntry::access)。
enum class FsAccess { Read, Write, Deny };

inline const char* fs_access_name(FsAccess access) {
    switch (access) {
        case FsAccess::Read:  return "read";
        case FsAccess::Write: return "write";
        case FsAccess::Deny:  return "deny";
    }
    return "read";
}

} // namespace acecode::sandbox
