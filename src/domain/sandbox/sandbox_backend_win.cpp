// Windows 沙盒后端:WRITE_RESTRICTED 受限令牌 + 工作区 ACE(openspec
// add-auto-mode-sandbox,对齐 Codex 的 unelevated Windows 沙盒)。
//
// 原理:带 WRITE_RESTRICTED 的令牌做写类访问检查时,除了正常的用户 / 组 SID
// 检查外,还要再用"限制 SID 列表"过一遍,两次都过才放行。列表 =
// {Everyone, 登录会话 SID, ACECode 合成 SID}:Everyone 与登录 SID 保住窗口站 /
// 管道 / 临时对象这些基础设施,合成 SID 只在我们打过 ACE 的目录(可写根)上有
// 数据写权限。读不受影响;DELETE / DELETE_CHILD 不完整受此限制,即便加拒绝 ACE
// 仍可能删除、改名外部/受保护项(与 Codex unelevated 同样,见 docs/sandbox.md)。
// 不降完整性级别(低完整性会
// 把所有中完整性对象的写全拒掉,连工作区也写不了)。不断网(需要管理员建防火墙
// 规则,对齐 Codex unelevated 档)。Everyone 可写的目录是已知绕过面。

#ifdef _WIN32

#include "sandbox_backend.hpp"

#include "utils/encoding.hpp"
#include "utils/logger.hpp"
#include "utils/sha1.hpp"

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>

#include <chrono>
#include <algorithm>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace acecode::sandbox {

namespace {

constexpr DWORD kAllowMask = FILE_GENERIC_READ | FILE_GENERIC_WRITE | FILE_GENERIC_EXECUTE |
                             DELETE;
// 拒绝 ACE 只能含写类权限:FILE_GENERIC_WRITE 里混着 READ_CONTROL / SYNCHRONIZE,
// 拒绝了它们连打开读都失败。
constexpr DWORD kDenyMask = FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA |
                            FILE_WRITE_ATTRIBUTES | DELETE | FILE_DELETE_CHILD |
                            WRITE_DAC | WRITE_OWNER;

struct SidBuffer {
    std::vector<unsigned char> bytes;
    PSID get() { return bytes.empty() ? nullptr : bytes.data(); }
    bool valid() const { return !bytes.empty(); }
};

SidBuffer copy_sid(PSID sid) {
    SidBuffer out;
    if (!sid || !IsValidSid(sid)) return out;
    const DWORD len = GetLengthSid(sid);
    out.bytes.resize(len);
    if (!CopySid(len, out.bytes.data(), sid)) out.bytes.clear();
    return out;
}

SidBuffer everyone_sid() {
    SidBuffer out;
    out.bytes.resize(SECURITY_MAX_SID_SIZE);
    DWORD len = SECURITY_MAX_SID_SIZE;
    if (!CreateWellKnownSid(WinWorldSid, nullptr, out.bytes.data(), &len)) {
        out.bytes.clear();
        return out;
    }
    out.bytes.resize(len);
    return out;
}

unsigned char hex_nibble(char c) {
    if (c >= '0' && c <= '9') return static_cast<unsigned char>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<unsigned char>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return static_cast<unsigned char>(c - 'A' + 10);
    return 0;
}

// 合成 SID:与 Windows 服务 SID 同一派生规则(S-1-5-80-<SHA1(大写 UTF-16LE 名)
// 切 5 个小端 DWORD>)。确定性、不对应任何真实账户、`sc showsid` 可复算。
SidBuffer synthetic_sid(const SandboxPolicy& policy) {
    // ACL 会留在目录上,所以身份必须绑定完整策略;只读令牌永远不能携带写策略的 SID。
    std::vector<std::string> boundaries;
    for (const auto& root : policy.writable_roots) {
        boundaries.push_back("allow:" + root.root);
        for (const auto& ro : root.read_only_subpaths) boundaries.push_back("deny:" + ro);
    }
    std::sort(boundaries.begin(), boundaries.end());
    std::wstring name = L"ACECODE-SANDBOX-V2:" + utf8_to_wide(sandbox_mode_name(policy.mode));
    for (const auto& boundary : boundaries) {
        name += L"\n" + utf8_to_wide(boundary);
    }
    // Windows 路径不区分 ASCII 大小写,同一边界的不同拼写使用同一身份。
    for (wchar_t& c : name) {
        if (c >= L'a' && c <= L'z') c -= L'a' - L'A';
        if (c == L'/') c = L'\\';
    }
    const std::string bytes(reinterpret_cast<const char*>(name.data()),
                            name.size() * sizeof(wchar_t));
    const std::string hex = sha1_hex(bytes);
    unsigned char digest[20] = {};
    for (std::size_t i = 0; i < 20 && 2 * i + 1 < hex.size(); ++i) {
        digest[i] = static_cast<unsigned char>((hex_nibble(hex[2 * i]) << 4) | hex_nibble(hex[2 * i + 1]));
    }
    DWORD sub[5];
    for (std::size_t i = 0; i < 5; ++i) {
        sub[i] = static_cast<DWORD>(digest[4 * i]) |
                 (static_cast<DWORD>(digest[4 * i + 1]) << 8) |
                 (static_cast<DWORD>(digest[4 * i + 2]) << 16) |
                 (static_cast<DWORD>(digest[4 * i + 3]) << 24);
    }
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    PSID sid = nullptr;
    if (!AllocateAndInitializeSid(&nt, 6, 80, sub[0], sub[1], sub[2], sub[3], sub[4], 0, 0, &sid)) {
        return {};
    }
    SidBuffer out = copy_sid(sid);
    FreeSid(sid);
    return out;
}

SidBuffer logon_sid(HANDLE token) {
    DWORD len = 0;
    GetTokenInformation(token, TokenGroups, nullptr, 0, &len);
    if (len == 0) return {};
    std::vector<unsigned char> buf(len);
    if (!GetTokenInformation(token, TokenGroups, buf.data(), len, &len)) return {};
    auto* groups = reinterpret_cast<TOKEN_GROUPS*>(buf.data());
    for (DWORD i = 0; i < groups->GroupCount; ++i) {
        if ((groups->Groups[i].Attributes & SE_GROUP_LOGON_ID) == SE_GROUP_LOGON_ID) {
            return copy_sid(groups->Groups[i].Sid);
        }
    }
    return {};
}

std::string win_error_text(DWORD code) {
    LPWSTR buffer = nullptr;
    const DWORD n = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::string text = "error " + std::to_string(code);
    if (n && buffer) {
        std::wstring w(buffer, n);
        while (!w.empty() && (w.back() == L'\r' || w.back() == L'\n' || w.back() == L' ')) w.pop_back();
        text += ": " + wide_to_utf8(w);
    }
    if (buffer) LocalFree(buffer);
    return text;
}

bool ace_present(PACL dacl, PSID sid, DWORD mask, bool deny, bool is_dir) {
    if (!dacl) return false;
    ACL_SIZE_INFORMATION info{};
    if (!GetAclInformation(dacl, &info, sizeof(info), AclSizeInformation)) return false;
    for (DWORD i = 0; i < info.AceCount; ++i) {
        LPVOID raw = nullptr;
        if (!GetAce(dacl, i, &raw)) continue;
        auto* header = static_cast<ACE_HEADER*>(raw);
        if (header->AceFlags & INHERITED_ACE) continue;
        if (deny && header->AceType == ACCESS_DENIED_ACE_TYPE) {
            auto* ace = static_cast<ACCESS_DENIED_ACE*>(raw);
            if (!EqualSid(&ace->SidStart, sid)) continue;
            if ((ace->Mask & mask) != mask) continue;
        } else if (!deny && header->AceType == ACCESS_ALLOWED_ACE_TYPE) {
            auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(raw);
            if (!EqualSid(&ace->SidStart, sid)) continue;
            if ((ace->Mask & mask) != mask) continue;
        } else {
            continue;
        }
        if (is_dir) {
            const BYTE want = OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE;
            if ((header->AceFlags & want) != want) continue;
        }
        return true;
    }
    return false;
}

bool apply_ace(const std::wstring& path, PSID sid, DWORD mask, bool deny, bool is_dir,
               std::string* error) {
    PACL old_dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    DWORD rc = GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                     nullptr, nullptr, &old_dacl, nullptr, &sd);
    if (rc != ERROR_SUCCESS) {
        if (error) *error = "GetNamedSecurityInfo(" + wide_to_utf8(path) + ") " + win_error_text(rc);
        return false;
    }
    if (ace_present(old_dacl, sid, mask, deny, is_dir)) {
        LocalFree(sd);
        return true;
    }
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    GetSecurityDescriptorControl(sd, &control, &revision);

    EXPLICIT_ACCESS_W ea{};
    ea.grfAccessPermissions = mask;
    ea.grfAccessMode = deny ? DENY_ACCESS : GRANT_ACCESS;
    ea.grfInheritance = is_dir ? SUB_CONTAINERS_AND_OBJECTS_INHERIT : NO_INHERITANCE;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType = TRUSTEE_IS_UNKNOWN;
    ea.Trustee.ptstrName = static_cast<LPWSTR>(sid);
    PACL fresh = nullptr;
    rc = SetEntriesInAclW(1, &ea, old_dacl, &fresh);
    if (rc != ERROR_SUCCESS) {
        if (error) *error = "SetEntriesInAcl(" + wide_to_utf8(path) + ") " + win_error_text(rc);
        LocalFree(sd);
        return false;
    }
    // 保住 DACL 的"是否继承父目录"状态:不传这一位,SetNamedSecurityInfo 会把
    // 受保护的 DACL 重新打开继承。
    SECURITY_INFORMATION si = DACL_SECURITY_INFORMATION |
        ((control & SE_DACL_PROTECTED) ? PROTECTED_DACL_SECURITY_INFORMATION
                                       : UNPROTECTED_DACL_SECURITY_INFORMATION);
    const auto started = std::chrono::steady_clock::now();
    rc = SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT, si,
                               nullptr, nullptr, fresh, nullptr);
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    LocalFree(fresh);
    LocalFree(sd);
    if (rc != ERROR_SUCCESS) {
        if (error) *error = "SetNamedSecurityInfo(" + wide_to_utf8(path) + ") " + win_error_text(rc);
        return false;
    }
    LOG_INFO(std::string("[sandbox] ") + (deny ? "deny" : "allow") + " ACE applied to " +
             wide_to_utf8(path) + " in " + std::to_string(elapsed_ms) + "ms");
    return true;
}

bool is_directory(const std::wstring& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

bool path_exists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::mutex& grants_mutex() {
    static std::mutex mu;
    return mu;
}

} // namespace

std::string synthetic_sid_string(const SandboxPolicy& policy) {
    SidBuffer sid = synthetic_sid(policy);
    if (!sid.valid()) return {};
    LPWSTR text = nullptr;
    if (!ConvertSidToStringSidW(sid.get(), &text)) return {};
    std::string out = wide_to_utf8(text);
    LocalFree(text);
    return out;
}

void* create_restricted_token(const SandboxPolicy& policy, std::string* error) {
    HANDLE base = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY | TOKEN_ADJUST_DEFAULT, &base)) {
        if (error) *error = "OpenProcessToken " + win_error_text(GetLastError());
        return nullptr;
    }
    SidBuffer everyone = everyone_sid();
    SidBuffer logon = logon_sid(base);
    SidBuffer synthetic = synthetic_sid(policy);
    if (!everyone.valid() || !synthetic.valid() || !logon.valid()) {
        if (error) *error = "failed to build restricting SIDs";
        CloseHandle(base);
        return nullptr;
    }
    std::vector<SID_AND_ATTRIBUTES> restricting;
    restricting.push_back({everyone.get(), 0});
    if (logon.valid()) restricting.push_back({logon.get(), 0});
    restricting.push_back({synthetic.get(), 0});

    HANDLE restricted = nullptr;
    const BOOL ok = CreateRestrictedToken(base, WRITE_RESTRICTED | DISABLE_MAX_PRIVILEGE | LUA_TOKEN,
                                          0, nullptr, 0, nullptr,
                                          static_cast<DWORD>(restricting.size()), restricting.data(),
                                          &restricted);
    const DWORD last = ok ? 0 : GetLastError();
    CloseHandle(base);
    if (!ok) {
        if (error) *error = "CreateRestrictedToken " + win_error_text(last);
        return nullptr;
    }
    // 管道/命名对象的新建默认 DACL 必须能通过限制 SID 检查,否则 cmd/PowerShell
    // 可能在 DLL 初始化或创建管线阶段退出。仅修改新令牌,不修改宿主令牌。
    std::vector<EXPLICIT_ACCESS_W> entries(restricting.size());
    for (std::size_t i = 0; i < restricting.size(); ++i) {
        entries[i].grfAccessPermissions = GENERIC_ALL;
        entries[i].grfAccessMode = GRANT_ACCESS;
        entries[i].grfInheritance = NO_INHERITANCE;
        entries[i].Trustee.TrusteeForm = TRUSTEE_IS_SID;
        entries[i].Trustee.TrusteeType = TRUSTEE_IS_UNKNOWN;
        entries[i].Trustee.ptstrName = static_cast<LPWSTR>(restricting[i].Sid);
    }
    PACL default_dacl = nullptr;
    const DWORD acl_error = SetEntriesInAclW(static_cast<ULONG>(entries.size()), entries.data(), nullptr, &default_dacl);
    TOKEN_DEFAULT_DACL dacl_info{default_dacl};
    if (acl_error != ERROR_SUCCESS ||
        !SetTokenInformation(restricted, TokenDefaultDacl, &dacl_info, sizeof(dacl_info))) {
        if (error) *error = "Cannot set restricted token default DACL: " +
            win_error_text(acl_error == ERROR_SUCCESS ? GetLastError() : acl_error);
        if (default_dacl) LocalFree(default_dacl);
        CloseHandle(restricted);
        return nullptr;
    }
    LocalFree(default_dacl);
    return restricted;
}

bool ensure_windows_acl_grants(const SandboxPolicy& policy, std::string* error) {
    if (policy.mode != SandboxMode::WorkspaceWrite) return true;
    for (const auto& root : policy.writable_roots) {
        for (const auto& sub : root.read_only_subpaths) {
            const auto relative = std::filesystem::path(utf8_to_wide(sub)).lexically_relative(
                std::filesystem::path(utf8_to_wide(root.root)));
            if (relative.empty() || *relative.begin() == L"..") {
                if (error) *error = "Protected path resolves outside sandbox root: " + sub;
                return false;
            }
        }
    }
    SidBuffer sid = synthetic_sid(policy);
    if (!sid.valid()) {
        if (error) *error = "failed to build the sandbox SID";
        return false;
    }
    std::lock_guard<std::mutex> lk(grants_mutex());
    for (const auto& root : policy.writable_roots) {
        const std::wstring wroot = utf8_to_wide(root.root);
        if (!is_directory(wroot)) {
            if (error) *error = "Sandbox writable root is unavailable: " + root.root;
            return false;
        }
        {
            // ACL 不授予父目录 DELETE_CHILD / 修改 ACL 的权限。
            // WRITE_RESTRICTED 不保证执行 DELETE 检查,这些 ACE 不是删除隔离承诺。
            if (!apply_ace(wroot, sid.get(), FILE_DELETE_CHILD | WRITE_DAC | WRITE_OWNER,
                           true, true, error)) return false;
            if (!apply_ace(wroot, sid.get(), DELETE, true, false, error)) return false;
            if (!apply_ace(wroot, sid.get(), kAllowMask, /*deny=*/false, is_directory(wroot), error)) {
                return false;
            }
        }
        for (const auto& sub : root.read_only_subpaths) {
            const std::wstring wsub = utf8_to_wide(sub);
            if (!path_exists(wsub)) {
                // git / rules 这类自家受保护路径由 prepare_request 预建,消失了就是
                // 异常;deny 名单落在可写根下的路径(比如把家目录当工作区时的
                // `~/.ssh`)不存在是常态,跳过即可 —— 不能为了打拒绝 ACE 去创建它。
                const std::wstring name = std::filesystem::path(wsub).filename().wstring();
                const bool own_protected = name == L"rules" || name == L"hooks" || name == L"modules" ||
                                           name == L"config" || name == L"config.worktree" || name == L".git";
                if (!own_protected) continue;
                if (error) *error = "Sandbox protected path disappeared: " + sub;
                return false;
            }
            if (!apply_ace(wsub, sid.get(), kDenyMask, /*deny=*/true, is_directory(wsub), error)) {
                return false;
            }
            // 描述敏感祖先的目标 ACL;本后端仍有已记录的删除/改名限制。
            auto parent = std::filesystem::path(wsub).parent_path();
            const auto root_path = std::filesystem::path(wroot);
            while (!parent.empty() && parent != root_path && parent != parent.parent_path()) {
                const auto relative = parent.lexically_relative(root_path);
                if (relative.empty() || *relative.begin() == L"..") break;
                if (!apply_ace(parent.wstring(), sid.get(), DELETE, true, false, error)) return false;
                parent = parent.parent_path();
            }
        }
    }
    return true;
}

bool remove_windows_acl_grants(const std::string& path, const SandboxPolicy& policy, std::string* error) {
    SidBuffer sid = synthetic_sid(policy);
    if (!sid.valid()) {
        if (error) *error = "failed to build the sandbox SID";
        return false;
    }
    const std::wstring wpath = utf8_to_wide(path);
    PACL old_dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    DWORD rc = GetNamedSecurityInfoW(wpath.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                     nullptr, nullptr, &old_dacl, nullptr, &sd);
    if (rc != ERROR_SUCCESS) {
        if (error) *error = "GetNamedSecurityInfo " + win_error_text(rc);
        return false;
    }
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    GetSecurityDescriptorControl(sd, &control, &revision);
    EXPLICIT_ACCESS_W ea{};
    ea.grfAccessMode = REVOKE_ACCESS;
    ea.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType = TRUSTEE_IS_UNKNOWN;
    ea.Trustee.ptstrName = static_cast<LPWSTR>(sid.get());
    PACL fresh = nullptr;
    rc = SetEntriesInAclW(1, &ea, old_dacl, &fresh);
    if (rc != ERROR_SUCCESS) {
        if (error) *error = "SetEntriesInAcl " + win_error_text(rc);
        LocalFree(sd);
        return false;
    }
    SECURITY_INFORMATION si = DACL_SECURITY_INFORMATION |
        ((control & SE_DACL_PROTECTED) ? PROTECTED_DACL_SECURITY_INFORMATION
                                       : UNPROTECTED_DACL_SECURITY_INFORMATION);
    rc = SetNamedSecurityInfoW(const_cast<LPWSTR>(wpath.c_str()), SE_FILE_OBJECT, si,
                               nullptr, nullptr, fresh, nullptr);
    LocalFree(fresh);
    LocalFree(sd);
    if (rc != ERROR_SUCCESS) {
        if (error) *error = "SetNamedSecurityInfo " + win_error_text(rc);
        return false;
    }
    return true;
}

void* create_process_tree_job() {
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) return nullptr;
    // 刻意不设 KILL_ON_JOB_CLOSE:正常结束时关闭句柄不能连带杀掉后台孙进程
    // (agent-browser 起的 Chrome 之类)。只有超时 / 中止才 TerminateJobObject。
    // 也不能设 SILENT_BREAKAWAY_OK —— 它会让所有孙进程静默脱离 Job,杀树就只剩
    // 直接子进程(实测 cmd → ping 时 Job 里只剩 cmd)。
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = 0;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        CloseHandle(job);
        return nullptr;
    }
    return job;
}

bool assign_process_to_job(void* job, void* process) {
    if (!job || !process) return false;
    return AssignProcessToJobObject(static_cast<HANDLE>(job), static_cast<HANDLE>(process)) != FALSE;
}

void terminate_job_tree(void* job) {
    if (job) TerminateJobObject(static_cast<HANDLE>(job), 1);
}

void close_job(void* job) {
    if (job) CloseHandle(static_cast<HANDLE>(job));
}

BackendProbe probe_backend(WindowsBackendChoice windows_backend) {
    if (windows_backend == WindowsBackendChoice::Mxc) return probe_windows_mxc();
    BackendProbe probe;
    probe.kind = BackendKind::WindowsRestrictedToken;
    probe.network_enforced = false;
    probe.network_best_effort = true;
    std::string error;
    SandboxPolicy read_only;
    read_only.mode = SandboxMode::ReadOnly;
    void* token = create_restricted_token(read_only, &error);
    if (token) {
        CloseHandle(static_cast<HANDLE>(token));
        probe.available = true;
    } else {
        probe.available = false;
        probe.reason = error;
    }
    return probe;
}

} // namespace acecode::sandbox

#endif // _WIN32
