#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace acecode::desktop {

inline constexpr int kDesktopStartupProgressVersion = 1;
inline constexpr const char* kDesktopStartupProgressEvent =
    "acecode:desktop-startup-progress";

struct DesktopStartupEvent {
    std::uint64_t sequence = 0;
    std::string stage;
    std::string message;
    std::string source;
    std::uint64_t elapsed_ms = 0;
    std::optional<double> frontend_ms;
    bool terminal = false;
};

class DesktopStartupTimeline {
public:
    using NowMsFn = std::function<std::uint64_t()>;

    explicit DesktopStartupTimeline(NowMsFn now_ms = {});

    DesktopStartupEvent record(
        std::string stage,
        std::string message,
        std::string source = "native",
        bool terminal = false,
        std::optional<double> frontend_ms = std::nullopt);

    bool empty() const;
    const DesktopStartupEvent* latest() const;
    const std::vector<DesktopStartupEvent>& history() const;
    std::string snapshot_json() const;

private:
    NowMsFn now_ms_;
    std::uint64_t started_ms_ = 0;
    std::uint64_t last_elapsed_ms_ = 0;
    std::uint64_t next_sequence_ = 1;
    std::vector<DesktopStartupEvent> history_;
};

struct FrontendStartupMilestone {
    std::string stage;
    std::optional<double> performance_ms;
};

std::optional<FrontendStartupMilestone> parse_frontend_startup_milestone(
    const std::string& args_json,
    std::string* error = nullptr);

bool is_frontend_startup_stage(const std::string& stage);
bool is_terminal_startup_stage(const std::string& stage);
std::string desktop_startup_stage_message(
    const std::string& stage,
    const std::string& locale);

} // namespace acecode::desktop
