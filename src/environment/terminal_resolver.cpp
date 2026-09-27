#include "terminal_resolver.hpp"

#include "hooks/hook_config.hpp"
#include "platform/process/process_runner.hpp"
#include "utils/logger.hpp"

#include <algorithm>
#include <cctype>

namespace acecode::environment {

namespace {

std::string first_line(const std::string& text) {
    const auto pos = text.find_first_of("\r\n");
    return pos == std::string::npos ? text : text.substr(0, pos);
}

bool contains(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

}  // namespace

LaunchProbe default_launch_probe(int timeout_ms) {
    return [timeout_ms](const std::string& program,
                        const std::vector<std::string>& args) {
        LaunchProbeResult out;
        platform::ProcessSpec spec;
        spec.command = program;
        spec.args = args;
        HookProcessOptions options;
        options.timeout_ms = timeout_ms;
        options.max_stdout_bytes = 4096;
        options.max_stderr_bytes = 4096;
        options.append_output_truncation_notice = false;
        const HookProcessResult r = run_hook_process(spec, "", "", options);
        if (!r.started) {
            out.error = r.error.empty() ? std::string("failed to start") : r.error;
            return out;
        }
        if (r.timed_out) {
            out.error = "timed out after " + std::to_string(timeout_ms) + "ms";
            return out;
        }
        if (r.exit_code != 0) {
            out.error = "exit code " + std::to_string(r.exit_code);
            const std::string detail = first_line(r.stderr_text.empty() ? r.stdout_text
                                                                        : r.stderr_text);
            if (!detail.empty()) out.error += ": " + detail;
            return out;
        }
        out.ok = true;
        return out;
    };
}

std::vector<std::string> platform_fallback_order() {
#ifdef _WIN32
    return {"powershell", "git-bash", "cmd"};
#else
    return {"shell", "bash", "zsh", "fish"};
#endif
}

std::string console_command_for(TerminalFamily family, const std::string& program) {
    switch (family) {
        case TerminalFamily::Bash:
            return quote_shell_path_if_needed(program) + " --login -i";
        case TerminalFamily::Cmd:
        case TerminalFamily::PowerShell:
            return quote_shell_path_if_needed(program);
        case TerminalFamily::Posix:
            return program;
    }
    return program;
}

TerminalResolution resolve_terminal(const ConsoleConfig& console,
                                    const ShellProbe& probe,
                                    const LaunchProbe& launch) {
    TerminalResolution res;
    res.configured_id = console.default_shell;

    for (const auto& o : detect_console_shells(console.shell_paths, probe)) {
        TerminalCandidate c;
        c.id = o.id;
        c.label = o.label;
        c.family = terminal_family_for_id(o.id);
        c.configured_path = o.configured_path;
        c.detected_path = o.detected_path;
        c.available = o.available;
        c.needs_path = o.needs_path;
        c.program = o.program;
        // 显式路径永远排第一(哪怕不存在 —— 探测阶段会把"配置路径不存在"报成原因),
        // 然后是目录层面选出的程序,最后是同类备选(pwsh → powershell.exe)。
        if (!c.configured_path.empty()) c.programs.push_back(c.configured_path);
        if (!o.program.empty() && !contains(c.programs, o.program)) c.programs.push_back(o.program);
        for (const auto& fb : o.fallback_programs) {
            if (!fb.empty() && !contains(c.programs, fb)) c.programs.push_back(fb);
        }
        res.candidates.push_back(std::move(c));
    }

    auto find_candidate = [&](const std::string& id) -> TerminalCandidate* {
        for (auto& c : res.candidates) if (c.id == id) return &c;
        return nullptr;
    };

    // Preserve an older console.shell override until the user selects a type.
    std::string legacy_program;
    std::string legacy_id;
    if (console.default_shell.empty() && !console.shell.empty()) {
        const auto first = console.shell.find_first_not_of(" \t");
        if (first != std::string::npos) {
            const bool quoted = console.shell[first] == '"';
            const auto start = first + (quoted ? 1 : 0);
            const auto end = console.shell.find_first_of(quoted ? "\"" : " \t", start);
            legacy_program = console.shell.substr(start, end == std::string::npos ? end : end - start);
        }
#ifdef _WIN32
        auto name = legacy_program.substr(legacy_program.find_last_of("/\\") + 1);
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        legacy_id = name.find("pwsh") != std::string::npos || name.find("powershell") != std::string::npos
            ? "powershell" : name.find("bash") != std::string::npos ? "git-bash" : "cmd";
#else
        legacy_id = "shell";
#endif
        if (!legacy_program.empty()) {
            if (auto* candidate = find_candidate(legacy_id)) candidate->programs.insert(candidate->programs.begin(), legacy_program);
        }
    }

    std::vector<std::string> order;
    if (!legacy_program.empty()) order.push_back(legacy_id);
    if (!console.default_shell.empty() && find_candidate(console.default_shell)) {
        order.push_back(console.default_shell);
    }
    for (const auto& id : platform_fallback_order()) {
        if (find_candidate(id) && !contains(order, id)) order.push_back(id);
    }
    for (const auto& c : res.candidates) {
        if (!contains(order, c.id)) order.push_back(c.id);
    }

    std::string reasons;
    auto add_reason = [&](const std::string& text) {
        if (!reasons.empty()) reasons += "; ";
        reasons += text;
    };
    const std::string preferred = order.empty() ? std::string{} : order.front();

    for (const auto& id : order) {
        TerminalCandidate* c = find_candidate(id);
        if (!c) continue;
        if (c->programs.empty()) {
            add_reason(c->label + ": not installed");
            c->probe_error = "not installed";
            continue;
        }
        bool first_program = true;
        for (const auto& program : c->programs) {
            std::string failure;
            if (program == c->configured_path && !probe.exists(program)) {
                failure = "configured path not found";
            } else {
                c->probed = true;
                const LaunchProbeResult r = launch(program, probe_arguments(c->family));
                if (r.ok) {
                    c->usable = true;
                    c->program = program;
                    res.resolved.id = id;
                    res.resolved.family = c->family;
                    res.resolved.program = program;
                    res.resolved.console_command = console_command_for(c->family, program);
                    if (id == legacy_id && program == legacy_program) res.resolved.console_command = console.shell;
                    res.resolved.usable = true;
                    const bool is_preferred = (id == preferred) && first_program;
                    res.resolved.fallback_reason = is_preferred ? std::string{} : reasons;
                    return res;
                }
                failure = r.error.empty() ? std::string("launch failed") : r.error;
            }
            const std::string line = c->label + " (" + program + "): " + failure;
            add_reason(line);
            if (!c->probe_error.empty()) c->probe_error += "; ";
            c->probe_error += program + ": " + failure;
            first_program = false;
        }
    }

    res.resolved.usable = false;
    res.resolved.fallback_reason = reasons.empty() ? std::string("no terminal candidates")
                                                   : reasons;
    LOG_WARN("[terminal] no usable terminal found: " + res.resolved.fallback_reason);
    return res;
}

TerminalResolution resolve_terminal(const ConsoleConfig& console) {
    return resolve_terminal(console, default_shell_probe(), default_launch_probe());
}

}  // namespace acecode::environment
