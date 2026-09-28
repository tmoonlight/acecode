// 沙盒后端的纯字符串部分:Seatbelt policy / sandbox-exec argv / bwrap argv /
// 子进程环境变量。所有平台都编译,单测跨平台跑。

#include "sandbox_backend.hpp"

#include "utils/utf8_path.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <system_error>
#include <vector>

namespace acecode::sandbox {

namespace {

// 移植自 Codex codex-rs/sandboxing/src/seatbelt_base_policy.sbpl(deny-default,
// 放行进程 / sysctl / mach-lookup / pty 等基础项)。文件读策略不在这里:按
// 全盘 / 受限读在 build_seatbelt_policy 里单独拼(align-codex-sandboxing D8)。
const char* kSeatbeltBasePolicy = R"SBPL((version 1)
; ACECode exec sandbox. Base rules ported from openai/codex seatbelt_base_policy.sbpl,
; which itself follows Chromium's macOS sandbox policy.
(deny default)

; child processes inherit the policy of their parent
(allow process-exec)
(allow process-fork)
(allow signal (target same-sandbox))
(allow process-info* (target same-sandbox))

(allow file-write-data
  (require-all
    (path "/dev/null")
    (vnode-type CHARACTER-DEVICE)))
(allow file-write* (literal "/dev/null") (literal "/dev/tty") (literal "/dev/dtracehelper"))

(allow sysctl-read
  (sysctl-name "hw.activecpu")
  (sysctl-name "hw.busfrequency_compat")
  (sysctl-name "hw.byteorder")
  (sysctl-name "hw.cacheconfig")
  (sysctl-name "hw.cachelinesize_compat")
  (sysctl-name "hw.cpufamily")
  (sysctl-name "hw.cpufrequency_compat")
  (sysctl-name "hw.cputype")
  (sysctl-name "hw.l1dcachesize_compat")
  (sysctl-name "hw.l1icachesize_compat")
  (sysctl-name "hw.l2cachesize_compat")
  (sysctl-name "hw.l3cachesize_compat")
  (sysctl-name "hw.logicalcpu_max")
  (sysctl-name "hw.machine")
  (sysctl-name "hw.model")
  (sysctl-name "hw.memsize")
  (sysctl-name "hw.ncpu")
  (sysctl-name "hw.nperflevels")
  (sysctl-name-prefix "hw.optional.arm.")
  (sysctl-name-prefix "hw.optional.armv8_")
  (sysctl-name "hw.packages")
  (sysctl-name "hw.pagesize_compat")
  (sysctl-name "hw.pagesize")
  (sysctl-name "hw.physicalcpu")
  (sysctl-name "hw.physicalcpu_max")
  (sysctl-name "hw.logicalcpu")
  (sysctl-name "hw.cpufrequency")
  (sysctl-name "hw.tbfrequency_compat")
  (sysctl-name "hw.vectorunit")
  (sysctl-name "machdep.cpu.brand_string")
  (sysctl-name "kern.argmax")
  (sysctl-name "kern.hostname")
  (sysctl-name "kern.maxfilesperproc")
  (sysctl-name "kern.maxproc")
  (sysctl-name "kern.osproductversion")
  (sysctl-name "kern.osrelease")
  (sysctl-name "kern.ostype")
  (sysctl-name "kern.osvariant_status")
  (sysctl-name "kern.osversion")
  (sysctl-name "kern.secure_kernel")
  (sysctl-name "kern.sysv.semmns")
  (sysctl-name "kern.usrstack64")
  (sysctl-name "kern.version")
  (sysctl-name "sysctl.proc_cputype")
  (sysctl-name "vm.loadavg")
  (sysctl-name-prefix "hw.perflevel")
  (sysctl-name-prefix "kern.proc.pgrp.")
  (sysctl-name-prefix "kern.proc.pid.")
  (sysctl-name-prefix "net.routetable.")
)
(allow sysctl-write
  (sysctl-name "kern.grade_cputype"))

(allow iokit-open
  (iokit-registry-entry-class "RootDomainUserClient")
)
(allow mach-lookup
  (global-name "com.apple.system.opendirectoryd.libinfo")
  (global-name "com.apple.PowerManagement.control")
  (global-name "com.apple.system.logger")
)
(allow ipc-posix-sem)
(allow ipc-posix-shm-read-data
  ipc-posix-shm-write-create
  ipc-posix-shm-write-unlink
  (ipc-posix-name-regex #"^/__KMP_REGISTERED_LIB_[0-9]+$"))

(allow pseudo-tty)
(allow file-read* file-write* file-ioctl (literal "/dev/ptmx"))
(allow file-read* file-write*
  (require-all
    (regex #"^/dev/ttys[0-9]+")
    (extension "com.apple.sandbox.pty")))
(allow file-ioctl (regex #"^/dev/ttys[0-9]+"))
)SBPL";

// 移植自 Codex seatbelt_network_policy.sbpl:放行网络时追加。
const char* kSeatbeltNetworkPolicy = R"SBPL(
; network access enabled
(allow network-outbound)
(allow network-inbound)
(allow system-socket)
(allow system-socket
  (require-all
    (socket-domain AF_SYSTEM)
    (socket-protocol 2)
  )
)
(allow mach-lookup
    (global-name "com.apple.bsd.dirhelper")
    (global-name "com.apple.system.opendirectoryd.membership")
    (global-name "com.apple.SecurityServer")
    (global-name "com.apple.networkd")
    (global-name "com.apple.ocspd")
    (global-name "com.apple.trustd.agent")
    (global-name "com.apple.SystemConfiguration.DNSConfiguration")
    (global-name "com.apple.SystemConfiguration.configd")
)
(allow sysctl-read
  (sysctl-name-regex #"^net.routetable")
)
)SBPL";

// 移植自 Codex seatbelt_preferences_policy.sbpl:cfprefsd 能把可读根之外的数据
// 透出来,所以只在全盘可读时追加。
const char* kSeatbeltPreferencesPolicy = R"SBPL(
; Preferences IPC can expose data outside the filesystem read roots.
; Include this policy only when filesystem reads are unrestricted.
(allow ipc-posix-shm-read* (ipc-posix-name-prefix "apple.cfprefs."))
(allow mach-lookup
  (global-name "com.apple.cfprefsd.daemon")
  (global-name "com.apple.cfprefsd.agent")
  (local-name "com.apple.cfprefsd.agent"))
(allow user-preference-read)
)SBPL";

// 移植自 Codex seatbelt_read_only_platform_defaults.sbpl:受限读时让进程还能
// 加载系统框架 / 动态库、解析 /etc、打开终端设备。
const char* kSeatbeltReadOnlyPlatformDefaults = R"SBPL(
; macOS platform defaults included when filesystem reads are restricted.
(allow file-read* file-test-existence
  (subpath "/Library/Apple")
  (subpath "/Library/Filesystems/NetFSPlugins")
  (subpath "/Library/Preferences/Logging")
  (subpath "/private/var/db/DarwinDirectory/local/recordStore.data")
  (subpath "/private/var/db/timezone")
  (subpath "/usr/lib")
  (subpath "/usr/share")
  (subpath "/Library/Preferences")
  (subpath "/var/db")
  (subpath "/private/var/db"))
(allow file-map-executable
  (subpath "/Library/Apple/System/Library/Frameworks")
  (subpath "/Library/Apple/System/Library/PrivateFrameworks")
  (subpath "/Library/Apple/usr/lib")
  (subpath "/System/Library/Extensions")
  (subpath "/System/Library/Frameworks")
  (subpath "/System/Library/PrivateFrameworks")
  (subpath "/System/Library/SubFrameworks")
  (subpath "/System/iOSSupport/System/Library/Frameworks")
  (subpath "/System/iOSSupport/System/Library/PrivateFrameworks")
  (subpath "/System/iOSSupport/System/Library/SubFrameworks")
  (subpath "/usr/lib"))
(allow file-read* file-test-existence
  (subpath "/Library/Apple/System/Library/Frameworks")
  (subpath "/Library/Apple/System/Library/PrivateFrameworks")
  (subpath "/Library/Apple/usr/lib")
  (subpath "/System/Library/Frameworks")
  (subpath "/System/Library/PrivateFrameworks")
  (subpath "/System/Library/SubFrameworks")
  (subpath "/System/iOSSupport/System/Library/Frameworks")
  (subpath "/System/iOSSupport/System/Library/PrivateFrameworks")
  (subpath "/System/iOSSupport/System/Library/SubFrameworks")
  (subpath "/usr/lib"))
(allow system-mac-syscall (mac-policy-name "vnguard"))
(allow system-mac-syscall
  (require-all
    (mac-policy-name "Sandbox")
    (mac-syscall-number 67)))
(allow file-read-metadata file-test-existence
  (literal "/etc")
  (literal "/tmp")
  (literal "/var")
  (literal "/private/etc/localtime"))
(allow file-read-metadata file-test-existence
  (path-ancestors "/System/Volumes/Data/private"))
(allow file-read* file-test-existence
  (literal "/"))
(allow system-fsctl (fsctl-command FSIOC_CAS_BSDFLAGS))
(allow file-read* file-test-existence
  (literal "/dev/autofs_nowait")
  (literal "/dev/random")
  (literal "/dev/urandom")
  (literal "/private/etc/master.passwd")
  (literal "/private/etc/passwd")
  (literal "/private/etc/protocols")
  (literal "/private/etc/services"))
(allow file-read* file-test-existence file-write-data
  (literal "/dev/null")
  (literal "/dev/zero"))
(allow file-read-data file-test-existence file-write-data
  (subpath "/dev/fd"))
(allow file-read* file-test-existence file-write-data file-ioctl
  (literal "/dev/dtracehelper"))
(allow file-read* (subpath "/etc"))
(allow file-read* (subpath "/private/etc"))
(allow file-read* file-test-existence
  (literal "/System/Library/CoreServices")
  (literal "/System/Library/CoreServices/.SystemVersionPlatform.plist")
  (literal "/System/Library/CoreServices/SystemVersion.plist"))
(allow file-read-metadata (subpath "/var"))
(allow file-read-metadata (subpath "/private/var"))
(allow iokit-open
  (iokit-registry-entry-class "RootDomainUserClient"))
(allow mach-lookup (global-name "com.apple.system.opendirectoryd.libinfo"))
(allow mach-lookup
  (global-name "com.apple.analyticsd")
  (global-name "com.apple.analyticsd.messagetracer")
  (global-name "com.apple.appsleep")
  (global-name "com.apple.bsd.dirhelper")
  (global-name "com.apple.diagnosticd")
  (global-name "com.apple.dt.automationmode.reader")
  (global-name "com.apple.espd")
  (global-name "com.apple.logd")
  (global-name "com.apple.logd.events")
  (global-name "com.apple.runningboard")
  (global-name "com.apple.secinitd")
  (global-name "com.apple.system.DirectoryService.libinfo_v1")
  (global-name "com.apple.system.logger")
  (global-name "com.apple.system.notification_center")
  (global-name "com.apple.system.opendirectoryd.membership")
  (global-name "com.apple.trustd")
  (global-name "com.apple.trustd.agent")
  (global-name "com.apple.xpc.activity.unmanaged"))
(allow network-outbound (literal "/private/var/run/syslog"))
(allow ipc-posix-shm-read*
  (ipc-posix-name "apple.shm.notification_center"))
(allow file-read*
  (literal "/private/var/db/eligibilityd/eligibility.plist"))
(allow mach-lookup (global-name "com.apple.audio.audiohald"))
(allow mach-lookup (global-name "com.apple.audio.AudioComponentRegistrar"))
(allow mach-lookup (global-name "com.apple.PowerManagement.control"))
(allow file-read-data (subpath "/bin"))
(allow file-read-metadata (subpath "/bin"))
(allow file-read-data (subpath "/sbin"))
(allow file-read-metadata (subpath "/sbin"))
(allow file-read-data (subpath "/usr/bin"))
(allow file-read-metadata (subpath "/usr/bin"))
(allow file-read-data (subpath "/usr/sbin"))
(allow file-read-metadata (subpath "/usr/sbin"))
(allow file-read-data (subpath "/usr/libexec"))
(allow file-read-metadata (subpath "/usr/libexec"))
(allow file-read* (subpath "/Library/Preferences"))
(allow file-read* (subpath "/opt/homebrew/lib"))
(allow file-read* (subpath "/usr/local/lib"))
(allow file-read* (regex "^/dev/fd/(0|1|2)$"))
(allow file-write* (regex "^/dev/fd/(1|2)$"))
(allow file-read* file-write* (literal "/dev/null"))
(allow file-read* file-write* (literal "/dev/tty"))
(allow file-read-metadata (literal "/dev"))
(allow file-read-metadata (regex "^/dev/.*$"))
(allow file-read-metadata (literal "/dev/stdin"))
(allow file-read-metadata (literal "/dev/stdout"))
(allow file-read-metadata (literal "/dev/stderr"))
(allow file-read-metadata (regex "^/dev/tty[^/]*$"))
(allow file-read-metadata (regex "^/dev/pty[^/]*$"))
(allow file-read* file-write* (regex "^/dev/ttys[0-9]+$"))
(allow file-read* file-write* (literal "/dev/ptmx"))
(allow file-ioctl (regex "^/dev/ttys[0-9]+$"))
(allow file-read-metadata (literal "/System/Volumes") (vnode-type DIRECTORY))
(allow file-read-metadata (literal "/System/Volumes/Data") (vnode-type DIRECTORY))
(allow file-read-metadata (literal "/System/Volumes/Data/Users") (vnode-type DIRECTORY))
(allow file-read* (extension "com.apple.app-sandbox.read"))
(allow file-read* file-write* (extension "com.apple.app-sandbox.read-write"))
)SBPL";

std::string root_param(std::size_t i) { return "WRITABLE_ROOT_" + std::to_string(i); }
std::string excluded_param(std::size_t i, std::size_t j) {
    return "WRITABLE_ROOT_" + std::to_string(i) + "_EXCLUDED_" + std::to_string(j);
}
std::string readable_param(std::size_t i) { return "READABLE_ROOT_" + std::to_string(i); }
std::string denied_param(std::size_t i) { return "DENIED_PATH_" + std::to_string(i); }
std::string ancestor_param(std::size_t i) { return "PROTECTED_ANCESTOR_" + std::to_string(i); }

// `#"..."` 是 SBPL 的原始字符串(反斜杠不转义),正则里的 `\.` 必须原样保留;
// 只有双引号需要转义(与 Codex seatbelt.rs 一致)。
std::string sbpl_escape(const std::string& text) {
    std::string out;
    for (char c : text) {
        if (c == '"') out += '\\';
        out += c;
    }
    return out;
}

std::string regex_escape(char c) {
    static const std::string meta = R"(\.^$|()[]{}*+?)";
    std::string out;
    if (meta.find(c) != std::string::npos) out += '\\';
    out += c;
    return out;
}

// 只读子路径的祖先目录(到可写根为止,不含根本身):改名它们等于把受保护
// 子树搬出 carveout。
std::vector<std::string> protected_ancestors(const SandboxPolicy& policy) {
    std::set<std::string> ancestors;
    for (const auto& root : policy.writable_roots) {
        const std::filesystem::path base = path_from_utf8(root.root);
        for (const auto& ro : root.read_only_subpaths) {
            std::filesystem::path parent = path_from_utf8(ro).parent_path();
            while (!parent.empty() && parent != base && parent != parent.parent_path()) {
                const auto relative = parent.lexically_relative(base);
                if (relative.empty() || *relative.begin() == "..") break;
                ancestors.insert(path_to_utf8(parent));
                parent = parent.parent_path();
            }
        }
    }
    return {ancestors.begin(), ancestors.end()};
}

bool env_key_equals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'a' && x <= 'z') x = static_cast<char>(x - 'a' + 'A');
        if (y >= 'a' && y <= 'z') y = static_cast<char>(y - 'a' + 'A');
        if (x != y) return false;
    }
    return true;
}

std::string reorder_pathext(const std::string& pathext) {
    const std::string source = pathext.empty() ? ".COM;.EXE;.BAT;.CMD" : pathext;
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= source.size()) {
        std::size_t end = source.find(';', start);
        if (end == std::string::npos) end = source.size();
        if (end > start) parts.push_back(source.substr(start, end - start));
        if (end == source.size()) break;
        start = end + 1;
    }
    std::vector<std::string> front, rest;
    for (const auto& part : parts) {
        if (env_key_equals(part, ".BAT") || env_key_equals(part, ".CMD")) front.push_back(part);
        else rest.push_back(part);
    }
    std::string out;
    for (const auto& part : front) { if (!out.empty()) out += ';'; out += part; }
    for (const auto& part : rest) { if (!out.empty()) out += ';'; out += part; }
    return out;
}

} // namespace

std::string seatbelt_regex_for_glob(const std::string& pattern, bool subtree) {
    if (pattern.empty()) return {};
    std::string regex = "^";
    bool saw_glob = false;
    int alternate_depth = 0;
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        const char c = pattern[i];
        if (c == '*') {
            saw_glob = true;
            if (i + 1 < pattern.size() && pattern[i + 1] == '*') {
                ++i;
                if (i + 1 < pattern.size() && pattern[i + 1] == '/') {
                    ++i;
                    regex += "(.*/)?";
                } else {
                    regex += ".*";
                }
            } else {
                regex += "[^/]*";
            }
        } else if (c == '?') {
            saw_glob = true;
            regex += "[^/]";
        } else if (c == '\\') {
            if (i + 1 < pattern.size()) regex += regex_escape(pattern[++i]);
            else regex += "\\\\";
        } else if (c == '{') {
            saw_glob = true;
            ++alternate_depth;
            regex += '(';
        } else if (c == '}' && alternate_depth > 0) {
            --alternate_depth;
            regex += ')';
        } else if (c == ',' && alternate_depth > 0) {
            regex += '|';
        } else if (c == '[') {
            const std::size_t close = pattern.find(']', i + 1);
            if (close == std::string::npos) {
                regex += "\\[";
                continue;
            }
            saw_glob = true;
            std::string cls = pattern.substr(i + 1, close - i - 1);
            regex += '[';
            if (!cls.empty() && cls[0] == '!') { regex += '^'; cls.erase(0, 1); }
            else if (!cls.empty() && cls[0] == '^') { regex += "\\^"; cls.erase(0, 1); }
            for (char cc : cls) {
                if (cc == '\\') regex += "\\\\";
                else regex += cc;
            }
            regex += ']';
            i = close;
        } else if (c == ']') {
            saw_glob = true;
            regex += "\\]";
        } else {
            regex += regex_escape(c);
        }
    }
    for (int i = 0; i < alternate_depth; ++i) regex += ')';
    if (!saw_glob && subtree) regex += "(/.*)?";
    regex += '$';
    return regex;
}

std::string build_seatbelt_policy(const SandboxPolicy& policy) {
    std::string out = kSeatbeltBasePolicy;

    // ---- 读 ----
    if (policy.full_disk_read()) {
        out += "\n; allow read-only file operations\n(allow file-read*)\n";
    } else {
        out += "\n; restricted read roots\n";
        for (std::size_t i = 0; i < policy.readable_roots.size(); ++i) {
            out += "(allow file-read* (subpath (param \"" + readable_param(i) + "\")))\n";
        }
    }

    // ---- 写 ----
    if (policy.mode == SandboxMode::WorkspaceWrite) {
        for (std::size_t i = 0; i < policy.writable_roots.size(); ++i) {
            const auto& root = policy.writable_roots[i];
            out += "(allow file-write* (require-all (subpath (param \"" + root_param(i) + "\"))";
            for (std::size_t j = 0; j < root.read_only_subpaths.size(); ++j) {
                out += " (require-not (subpath (param \"" + excluded_param(i, j) + "\")))";
                out += " (require-not (literal (param \"" + excluded_param(i, j) + "\")))";
            }
            out += "))\n";
            // 沙盒里的进程不能把下一次策略要复用的权限边界本身改名 / 删除。
            out += "(deny file-write-unlink (require-all (literal (param \"" + root_param(i) +
                   "\")) (vnode-type DIRECTORY)))\n";
        }
    }

    if (policy.network_access) out += kSeatbeltNetworkPolicy;
    if (policy.full_disk_read()) out += kSeatbeltPreferencesPolicy;
    else out += kSeatbeltReadOnlyPlatformDefaults;

    // ---- deny(放在 allow 之后,后出现的规则优先)----
    for (std::size_t i = 0; i < policy.denied_paths.size(); ++i) {
        out += "(deny file-read* file-write* (subpath (param \"" + denied_param(i) + "\")))\n";
        out += "(deny file-read* file-write* (literal (param \"" + denied_param(i) + "\")))\n";
    }
    for (const auto& glob : policy.denied_globs) {
        const std::string regex = seatbelt_regex_for_glob(glob, /*subtree=*/true);
        if (regex.empty()) continue;
        out += "(deny file-read* (regex #\"" + sbpl_escape(regex) + "\"))\n";
        out += "(deny file-write* (regex #\"" + sbpl_escape(regex) + "\"))\n";
        // glob 静态前缀的祖先目录不能被改名,否则匹配路径整体搬出 glob 的作用域。
        // 有意偏离 Codex:它对 `**` 覆盖的每一层目录都拒绝 unlink(`^/Users/u/.*$`),
        // 等于禁止删除 / 改名家目录下任何目录;改名 `**` 之下的目录并不会让匹配
        // 路径离开作用域,所以只保护含 glob 元字符的组件之前的祖先。
        std::filesystem::path ancestor = std::filesystem::path(glob).parent_path();
        while (!ancestor.empty() && ancestor != ancestor.parent_path()) {
            const std::string text = ancestor.generic_string();
            if (text.find_first_of("*?[{") == std::string::npos) {
                const std::string exact = seatbelt_regex_for_glob(text, /*subtree=*/false);
                if (!exact.empty()) {
                    out += "(deny file-write-unlink (require-all (vnode-type DIRECTORY) (regex #\"" +
                           sbpl_escape(exact) + "\")))\n";
                }
            }
            ancestor = ancestor.parent_path();
        }
    }
    // 只读子路径的祖先目录:改名它们等于把受保护子树搬出 carveout,放最后。
    const auto ancestors = protected_ancestors(policy);
    for (std::size_t i = 0; i < ancestors.size(); ++i) {
        out += "(deny file-write-unlink (require-all (vnode-type DIRECTORY) (literal (param \"" +
               ancestor_param(i) + "\"))))\n";
    }
    return out;
}

std::vector<std::string> build_seatbelt_argv(const SandboxPolicy& policy) {
    std::vector<std::string> argv;
    argv.push_back("/usr/bin/sandbox-exec");
    argv.push_back("-p");
    argv.push_back(build_seatbelt_policy(policy));
    if (!policy.full_disk_read()) {
        for (std::size_t i = 0; i < policy.readable_roots.size(); ++i) {
            argv.push_back("-D" + readable_param(i) + "=" + policy.readable_roots[i]);
        }
    }
    if (policy.mode == SandboxMode::WorkspaceWrite) {
        for (std::size_t i = 0; i < policy.writable_roots.size(); ++i) {
            const auto& root = policy.writable_roots[i];
            argv.push_back("-D" + root_param(i) + "=" + root.root);
            for (std::size_t j = 0; j < root.read_only_subpaths.size(); ++j) {
                argv.push_back("-D" + excluded_param(i, j) + "=" + root.read_only_subpaths[j]);
            }
        }
    }
    for (std::size_t i = 0; i < policy.denied_paths.size(); ++i) {
        argv.push_back("-D" + denied_param(i) + "=" + policy.denied_paths[i]);
    }
    const auto ancestors = protected_ancestors(policy);
    for (std::size_t i = 0; i < ancestors.size(); ++i) {
        argv.push_back("-D" + ancestor_param(i) + "=" + ancestors[i]);
    }
    argv.push_back("--");
    return argv;
}

std::vector<std::string> build_bwrap_argv(const SandboxPolicy& policy) {
    std::vector<std::string> argv;
    argv.push_back("bwrap");
    argv.push_back("--unshare-user");
    argv.push_back("--unshare-pid");
    argv.push_back("--unshare-ipc");
    argv.push_back("--new-session");
    argv.push_back("--cap-drop"); argv.push_back("ALL");
    argv.push_back("--ro-bind"); argv.push_back("/"); argv.push_back("/");
    argv.push_back("--dev"); argv.push_back("/dev");
    argv.push_back("--proc"); argv.push_back("/proc");
    if (policy.mode == SandboxMode::WorkspaceWrite) {
        for (const auto& root : policy.writable_roots) {
            argv.push_back("--bind"); argv.push_back(root.root); argv.push_back(root.root);
        }
        // 后绑定的覆盖先绑定的:只读子路径必须排在可写根之后。不存在的路径
        // (deny 名单落在可写根下的那些)跳过:bwrap 对缺失源路径直接报错。
        std::error_code exists_ec;
        for (const auto& root : policy.writable_roots) {
            for (const auto& ro : root.read_only_subpaths) {
                if (!std::filesystem::exists(path_from_utf8(ro), exists_ec) || exists_ec) continue;
                argv.push_back("--ro-bind"); argv.push_back(ro); argv.push_back(ro);
            }
        }
    }
    // deny 路径:目录用空 tmpfs 遮住,文件用 /dev/null 遮住;不存在的跳过。
    // (Linux 的受限读 / glob 展开不在本期范围,见 docs/sandbox.md。)
    std::error_code ec;
    for (const auto& denied : policy.denied_paths) {
        const auto p = path_from_utf8(denied);
        if (std::filesystem::is_directory(p, ec) && !ec) {
            argv.push_back("--tmpfs"); argv.push_back(denied);
        } else if (std::filesystem::exists(p, ec) && !ec) {
            argv.push_back("--ro-bind"); argv.push_back("/dev/null"); argv.push_back(denied);
        }
    }
    if (!policy.network_access) argv.push_back("--unshare-net");
    argv.push_back("--die-with-parent");
    argv.push_back("--");
    return argv;
}

std::vector<std::pair<std::string, std::string>> offline_environment_overrides(
    const std::string& denybin_dir, const std::string& base_path, const std::string& base_pathext) {
    std::vector<std::pair<std::string, std::string>> env;
    const std::string dead_proxy = "http://127.0.0.1:9";
    env.emplace_back("ACECODE_SANDBOX_NETWORK_DISABLED", "1");
    env.emplace_back("SBX_NONET_ACTIVE", "1");
    for (const char* name : {"HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY", "GIT_HTTP_PROXY", "GIT_HTTPS_PROXY"}) {
        env.emplace_back(name, dead_proxy);
    }
    env.emplace_back("NO_PROXY", "localhost,127.0.0.1,::1");
    env.emplace_back("PIP_NO_INDEX", "1");
    env.emplace_back("PIP_DISABLE_PIP_VERSION_CHECK", "1");
    env.emplace_back("NPM_CONFIG_OFFLINE", "true");
    env.emplace_back("CARGO_NET_OFFLINE", "true");
    env.emplace_back("GIT_SSH_COMMAND", "cmd /c exit 1");
    env.emplace_back("GIT_ALLOW_PROTOCOLS", "");
    if (!denybin_dir.empty()) {
        std::string path = denybin_dir;
        if (!base_path.empty()) path += ";" + base_path;
        env.emplace_back("PATH", path);
        env.emplace_back("PATHEXT", reorder_pathext(base_pathext));
    }
    return env;
}

bool ensure_denybin_stubs(const std::string& dir, std::string* error) {
    if (dir.empty()) {
        if (error) *error = "denybin directory is empty";
        return false;
    }
    std::error_code ec;
    const auto base = path_from_utf8(dir);
    std::filesystem::create_directories(base, ec);
    if (ec) {
        if (error) *error = "cannot create " + dir + ": " + ec.message();
        return false;
    }
    for (const char* tool : {"ssh", "scp"}) {
        for (const char* ext : {".cmd", ".bat"}) {
            const auto stub = base / (std::string(tool) + ext);
            if (std::filesystem::exists(stub, ec) && !ec) continue;
            std::ofstream out(stub, std::ios::binary);
            if (!out) {
                if (error) *error = "cannot write " + path_to_utf8(stub);
                return false;
            }
            out << "@echo off\r\nexit /b 1\r\n";
        }
    }
    return true;
}

std::vector<std::pair<std::string, std::string>> sandbox_environment(
    BackendKind kind, const SandboxPolicy& policy, bool network_enforced,
    const std::string& denybin_dir, const std::string& base_path, const std::string& base_pathext) {
    std::vector<std::pair<std::string, std::string>> env;
    env.emplace_back("ACECODE_SANDBOX", backend_kind_name(kind));
    if (kind == BackendKind::WindowsRestrictedToken &&
        policy.mode == SandboxMode::WorkspaceWrite && !policy.temporary_directory.empty()) {
        for (const char* name : {"TEMP", "TMP", "TMPDIR"}) {
            env.emplace_back(name, policy.temporary_directory);
        }
    }
    if (network_enforced && !policy.network_access) {
        env.emplace_back("ACECODE_SANDBOX_NETWORK_DISABLED", "1");
    } else if (kind == BackendKind::WindowsRestrictedToken && !policy.network_access) {
        std::string path = base_path;
        std::string pathext = base_pathext;
        if (path.empty()) {
            if (const char* value = std::getenv("PATH")) path = value;
        }
        if (pathext.empty()) {
            if (const char* value = std::getenv("PATHEXT")) pathext = value;
        }
        const auto offline = offline_environment_overrides(denybin_dir, path, pathext);
        env.insert(env.end(), offline.begin(), offline.end());
    }
    return env;
}

} // namespace acecode::sandbox
