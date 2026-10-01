#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>

namespace acecode::upgrade {

std::optional<std::string> parse_executable_version_output(const std::string& output);

// 探测只为拦住「起不来 / 卡死」的坏包,不是性能门槛:正常情况下子进程一退出就
// 立刻返回,超时值只在真卡住时才会被用满。企业机器的杀软会在新 exe 第一次执行
// 时整包扫描(Defender 的云端首见拦截可以把执行挂起到 60 秒),曾经的 5 秒在
// 这类机器上稳定超时 —— 反馈 ZHAOZEYIN831:0.9.26 → 0.9.27 下载、校验、解压都
// 成功,最后因为 `acecode.exe --version` 5 秒内没返回而整体回滚。
inline constexpr std::chrono::milliseconds kExecutableVersionProbeTimeout =
    std::chrono::seconds(120);

// Direct child process, bounded stdout and wall time, no command shell.
bool verify_executable_version(
    const std::filesystem::path& executable, const std::string& expected_version,
    std::string* error,
    std::chrono::milliseconds timeout = kExecutableVersionProbeTimeout);

} // namespace acecode::upgrade
