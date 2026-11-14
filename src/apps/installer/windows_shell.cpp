#include "windows_shell.hpp"
#include "platform/invite_protocol.hpp"
#include "utils/sha256.hpp"
#include <cstdlib>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <iterator>
#endif

namespace acecode::installer::shell {

namespace {
std::string invite_registry_sandbox() {
    const auto* test = std::getenv("ACECODE_SETUP_TEST_ROOT");
    return test && *test ? "Software\\ACECodeSetupTest\\" + sha256_hex(test).substr(0, 16) + "\\Classes\\acecode" : "";
}
}
bool register_invite_protocol(const std::string& executable) {
    return platform::register_invite_protocol(executable, invite_registry_sandbox());
}
bool unregister_invite_protocol(const std::string& executable) {
    return platform::unregister_invite_protocol(executable, invite_registry_sandbox());
}

#ifdef _WIN32
namespace {

std::wstring wide(const std::string& text) {
    if (text.empty()) return {};
    const int length = ::MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                             static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring out(static_cast<std::size_t>(length), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(),
                          length);
    return out;
}

std::string narrow(const std::wstring& text) {
    if (text.empty()) return {};
    const int length = ::WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                             static_cast<int>(text.size()), nullptr, 0,
                                             nullptr, nullptr);
    if (length <= 0) return {};
    std::string out(static_cast<std::size_t>(length), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(),
                          length, nullptr, nullptr);
    return out;
}

std::string system_message(DWORD code) {
    wchar_t* buffer = nullptr;
    const DWORD length = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring text = length && buffer ? std::wstring(buffer, length) : std::wstring();
    if (buffer) ::LocalFree(buffer);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ')) {
        text.pop_back();
    }
    return narrow(text) + " (" + std::to_string(code) + ")";
}

std::string known_folder(REFKNOWNFOLDERID id) {
    PWSTR path = nullptr;
    std::string out;
    if (SUCCEEDED(::SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &path)) && path) {
        out = narrow(path);
    }
    if (path) ::CoTaskMemFree(path);
    return out;
}

// 测试沙盒:设置 ACECODE_SETUP_TEST_ROOT 后,卸载登记、用户 PATH、开始菜单、桌面与默认
// 安装位置都落在沙盒里,在开发机上跑真实安装 / 卸载也不会动到本机已有的 ACECode。
const std::string& sandbox_root() {
    static const std::string root = [] {
        wchar_t buffer[MAX_PATH * 2] = {};
        const DWORD length = ::GetEnvironmentVariableW(L"ACECODE_SETUP_TEST_ROOT", buffer,
                                                       static_cast<DWORD>(std::size(buffer)));
        return length > 0 && length < std::size(buffer)
                   ? trim_windows_dir(narrow(std::wstring(buffer, length)))
                   : std::string();
    }();
    return root;
}

std::string sandbox_folder(const char* name) {
    const std::string dir = join_windows_path(sandbox_root(), name);
    ::SHCreateDirectoryExW(nullptr, wide(dir).c_str(), nullptr);
    return dir;
}

std::wstring environment_key() {
    return sandbox_root().empty() ? std::wstring(L"Environment")
                                  : std::wstring(L"Software\\ACECode-setup-test\\Environment");
}

// 目录联接(例如 C:\Users\x 指向 N:\Users\x)下同一个文件有两种写法,比较前都换成真实路径。
std::wstring final_path(const std::wstring& path) {
    HANDLE file = ::CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    wchar_t buffer[MAX_PATH * 2] = {};
    const DWORD length = ::GetFinalPathNameByHandleW(file, buffer,
                                                     static_cast<DWORD>(std::size(buffer)),
                                                     FILE_NAME_NORMALIZED);
    ::CloseHandle(file);
    if (length == 0 || length >= std::size(buffer)) return {};
    std::wstring out(buffer, length);
    if (out.rfind(L"\\\\?\\UNC\\", 0) == 0) return L"\\\\" + out.substr(8);
    if (out.rfind(L"\\\\?\\", 0) == 0) return out.substr(4);
    return out;
}

struct KeyGuard {
    HKEY key = nullptr;
    ~KeyGuard() {
        if (key) ::RegCloseKey(key);
    }
};

struct HandleGuard {
    HANDLE handle = nullptr;
    ~HandleGuard() {
        if (handle && handle != INVALID_HANDLE_VALUE) ::CloseHandle(handle);
    }
};

template <typename T>
struct ComGuard {
    T* ptr = nullptr;
    ~ComGuard() {
        if (ptr) ptr->Release();
    }
};

// 快捷方式是 COM 对象,调用线程必须先进 COM 套间。安装界面把安装放在工作线程上跑,那条线程
// 从没初始化过 COM:开发机上常有别的组件先建好多线程套间,线程被隐式并进去所以能用;干净的
// 机器上 CoCreateInstance 直接返回 CO_E_NOTINITIALIZED。每次调用自己进出一次套间;线程已在
// 另一种套间里(RPC_E_CHANGED_MODE)时 COM 照样可用,只是不配对退出。
class ComApartment {
public:
    ComApartment()
        : result_(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)) {}
    ~ComApartment() {
        if (SUCCEEDED(result_)) ::CoUninitialize();
    }
    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;

private:
    HRESULT result_;
};

std::string hresult_text(HRESULT hr) {
    char buffer[16] = {};
    std::snprintf(buffer, sizeof(buffer), "0x%08lX", static_cast<unsigned long>(hr));
    return buffer;
}

std::wstring uninstall_key(const std::string& key_name) {
    const char* parent = sandbox_root().empty()
                             ? "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\"
                             : "Software\\ACECode-setup-test\\Uninstall\\";
    return wide(std::string(parent) + key_name);
}

std::optional<std::wstring> query_string(HKEY key, const wchar_t* name, DWORD* type_out) {
    DWORD type = 0;
    DWORD bytes = 0;
    if (::RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ)) {
        return std::nullopt;
    }
    std::wstring value(bytes / sizeof(wchar_t) + 1, L'\0');
    if (::RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(value.data()),
                           &bytes) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    value.resize(bytes / sizeof(wchar_t));
    while (!value.empty() && value.back() == L'\0') value.pop_back();
    if (type_out) *type_out = type;
    return value;
}

std::optional<std::string> read_uninstall_string(const std::string& key_name,
                                                 const wchar_t* value_name) {
    KeyGuard key;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, uninstall_key(key_name).c_str(), 0, KEY_QUERY_VALUE,
                        &key.key) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    const auto value = query_string(key.key, value_name, nullptr);
    if (!value) return std::nullopt;
    return narrow(*value);
}

struct UserPath {
    std::string value;
    DWORD type = REG_EXPAND_SZ;
};

bool read_user_path(UserPath* out) {
    KeyGuard key;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, environment_key().c_str(), 0, KEY_QUERY_VALUE,
                        &key.key) != ERROR_SUCCESS) {
        return true;
    }
    DWORD type = REG_EXPAND_SZ;
    const auto value = query_string(key.key, L"Path", &type);
    if (value) {
        out->value = narrow(*value);
        out->type = type;
    }
    return true;
}

bool write_user_path(const UserPath& path, std::string* error) {
    KeyGuard key;
    LONG status = ::RegCreateKeyExW(HKEY_CURRENT_USER, environment_key().c_str(), 0, nullptr, 0,
                                    KEY_SET_VALUE, nullptr, &key.key, nullptr);
    if (status != ERROR_SUCCESS) {
        if (error) *error = "failed to open HKCU\\Environment: " + system_message(status);
        return false;
    }
    const std::wstring value = wide(path.value);
    status = ::RegSetValueExW(key.key, L"Path", 0, path.type,
                              reinterpret_cast<const BYTE*>(value.c_str()),
                              static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    if (status != ERROR_SUCCESS) {
        if (error) *error = "failed to update the user PATH: " + system_message(status);
        return false;
    }
    return true;
}

std::wstring lower(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    std::replace(text.begin(), text.end(), L'/', L'\\');
    return text;
}

bool has_prefix(const std::wstring& path, const std::wstring& dir) {
    if (dir.empty()) return false;
    const std::wstring base = lower(dir.back() == L'\\' ? dir : dir + L"\\");
    const std::wstring candidate = lower(path);
    return candidate.size() > base.size() && candidate.compare(0, base.size(), base) == 0;
}

bool is_inside(const std::string& path, const std::string& dir) {
    const std::wstring raw_dir = wide(trim_windows_dir(dir));
    const std::wstring raw_path = wide(path);
    if (has_prefix(raw_path, raw_dir)) return true;
    const std::wstring real_dir = final_path(raw_dir);
    const std::wstring real_path = final_path(raw_path);
    return has_prefix(raw_path, real_dir) || has_prefix(real_path, raw_dir) ||
           has_prefix(real_path, real_dir);
}

}  // namespace

std::string local_app_data_dir() {
    return sandbox_root().empty() ? known_folder(FOLDERID_LocalAppData)
                                  : sandbox_folder("LocalAppData");
}
std::string programs_menu_dir() {
    return sandbox_root().empty() ? known_folder(FOLDERID_Programs) : sandbox_folder("Programs");
}
std::string desktop_dir() {
    return sandbox_root().empty() ? known_folder(FOLDERID_Desktop) : sandbox_folder("Desktop");
}
std::string user_profile_dir() { return known_folder(FOLDERID_Profile); }

std::string temp_dir() {
    wchar_t buffer[MAX_PATH + 2] = {};
    const DWORD length = ::GetTempPathW(MAX_PATH + 1, buffer);
    return trim_windows_dir(narrow(std::wstring(buffer, length)));
}

std::string current_executable_path() {
    std::wstring buffer(MAX_PATH, L'\0');
    while (true) {
        const DWORD length = ::GetModuleFileNameW(nullptr, buffer.data(),
                                                  static_cast<DWORD>(buffer.size()));
        if (length == 0) return {};
        if (length < buffer.size()) {
            buffer.resize(length);
            return narrow(buffer);
        }
        buffer.resize(buffer.size() * 2);
    }
}

bool write_uninstall_entry(const std::string& key_name,
                           const std::vector<RegistryValue>& values,
                           std::string* error) {
    KeyGuard key;
    LONG status = ::RegCreateKeyExW(HKEY_CURRENT_USER, uninstall_key(key_name).c_str(), 0,
                                    nullptr, 0, KEY_SET_VALUE, nullptr, &key.key, nullptr);
    if (status != ERROR_SUCCESS) {
        if (error) *error = "failed to create the uninstall entry: " + system_message(status);
        return false;
    }
    for (const auto& value : values) {
        const std::wstring name = wide(value.name);
        if (value.is_dword) {
            const DWORD number = value.number;
            status = ::RegSetValueExW(key.key, name.c_str(), 0, REG_DWORD,
                                      reinterpret_cast<const BYTE*>(&number), sizeof(number));
        } else {
            const std::wstring text = wide(value.text);
            status = ::RegSetValueExW(key.key, name.c_str(), 0, REG_SZ,
                                      reinterpret_cast<const BYTE*>(text.c_str()),
                                      static_cast<DWORD>((text.size() + 1) * sizeof(wchar_t)));
        }
        if (status != ERROR_SUCCESS) {
            if (error) *error = "failed to write " + value.name + ": " + system_message(status);
            return false;
        }
    }
    return true;
}

std::optional<std::string> uninstall_entry_location(const std::string& key_name) {
    return read_uninstall_string(key_name, L"InstallLocation");
}

std::optional<std::string> uninstall_entry_version(const std::string& key_name) {
    return read_uninstall_string(key_name, L"DisplayVersion");
}

bool delete_uninstall_entry(const std::string& key_name) {
    const LONG status = ::RegDeleteTreeW(HKEY_CURRENT_USER, uninstall_key(key_name).c_str());
    return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
}

bool add_to_user_path(const std::string& dir, std::string* error) {
    UserPath path;
    read_user_path(&path);
    const std::string next = path_list_with(path.value, dir);
    if (next == path.value) return true;
    path.value = next;
    return write_user_path(path, error);
}

bool remove_from_user_path(const std::string& dir, std::string* error) {
    UserPath path;
    read_user_path(&path);
    const std::string next = path_list_without(path.value, dir);
    if (next == path.value) return true;
    path.value = next;
    return write_user_path(path, error);
}

bool user_path_contains(const std::string& dir) {
    UserPath path;
    read_user_path(&path);
    return path_list_contains(path.value, dir);
}

void broadcast_environment_change() {
    if (!sandbox_root().empty()) return;
    DWORD_PTR result = 0;
    ::SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                          reinterpret_cast<LPARAM>(L"Environment"), SMTO_ABORTIFHUNG, 2000,
                          &result);
}

bool create_shortcut(const std::string& link_path,
                     const std::string& target,
                     const std::string& working_dir,
                     const std::string& description,
                     std::string* error) {
    const ComApartment apartment;
    ComGuard<IShellLinkW> link;
    HRESULT hr = ::CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_IShellLinkW, reinterpret_cast<void**>(&link.ptr));
    if (FAILED(hr) || !link.ptr) {
        if (error) *error = "failed to create a shell link object: " + hresult_text(hr);
        return false;
    }
    const std::wstring target_w = wide(target);
    link.ptr->SetPath(target_w.c_str());
    link.ptr->SetWorkingDirectory(wide(working_dir).c_str());
    link.ptr->SetDescription(wide(description).c_str());
    link.ptr->SetIconLocation(target_w.c_str(), 0);
    ComGuard<IPersistFile> file;
    hr = link.ptr->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&file.ptr));
    if (FAILED(hr) || !file.ptr) {
        if (error) *error = "failed to save the shortcut";
        return false;
    }
    hr = file.ptr->Save(wide(link_path).c_str(), TRUE);
    if (FAILED(hr)) {
        if (error) *error = "failed to save " + link_path + ": " +
                            system_message(static_cast<DWORD>(hr));
        return false;
    }
    return true;
}

std::optional<std::string> shortcut_target(const std::string& link_path) {
    const ComApartment apartment;
    ComGuard<IShellLinkW> link;
    if (FAILED(::CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IShellLinkW, reinterpret_cast<void**>(&link.ptr))) ||
        !link.ptr) {
        return std::nullopt;
    }
    ComGuard<IPersistFile> file;
    if (FAILED(link.ptr->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&file.ptr))) ||
        !file.ptr || FAILED(file.ptr->Load(wide(link_path).c_str(), STGM_READ))) {
        return std::nullopt;
    }
    wchar_t buffer[MAX_PATH * 2] = {};
    if (FAILED(link.ptr->GetPath(buffer, static_cast<int>(std::size(buffer)), nullptr,
                                 SLGP_RAWPATH))) {
        return std::nullopt;
    }
    return narrow(buffer);
}

void remove_shortcut_if_inside(const std::string& link_path, const std::string& dir) {
    const auto target = shortcut_target(link_path);
    if (target && is_inside(*target, dir)) ::DeleteFileW(wide(link_path).c_str());
}

std::vector<RunningProcess> processes_running_from(const std::string& dir) {
    std::vector<RunningProcess> found;
    HandleGuard snapshot;
    snapshot.handle = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot.handle == INVALID_HANDLE_VALUE) return found;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    const DWORD self = ::GetCurrentProcessId();
    for (BOOL ok = ::Process32FirstW(snapshot.handle, &entry); ok;
         ok = ::Process32NextW(snapshot.handle, &entry)) {
        if (entry.th32ProcessID == self || entry.th32ProcessID == 0) continue;
        HandleGuard process;
        process.handle = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                       entry.th32ProcessID);
        if (!process.handle) continue;
        wchar_t image[MAX_PATH * 2] = {};
        DWORD size = static_cast<DWORD>(std::size(image));
        if (!::QueryFullProcessImageNameW(process.handle, 0, image, &size)) continue;
        const std::string path = narrow(std::wstring(image, size));
        if (is_inside(path, dir)) found.push_back({entry.th32ProcessID, path});
    }
    return found;
}

namespace {

bool start_process(const std::string& executable,
                   const std::vector<std::string>& args,
                   const std::string& working_dir,
                   PROCESS_INFORMATION* info) {
    std::string command = quote_windows_arg(executable);
    for (const auto& arg : args) command += " " + quote_windows_arg(arg);
    std::wstring command_w = wide(command);
    const std::wstring exe_w = wide(executable);
    const std::wstring cwd_w = wide(working_dir);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    if (!::CreateProcessW(exe_w.c_str(), command_w.data(), nullptr, nullptr, FALSE, 0, nullptr,
                          cwd_w.empty() ? nullptr : cwd_w.c_str(), &startup, info)) {
        return false;
    }
    ::CloseHandle(info->hThread);
    return true;
}

}  // namespace

bool launch(const std::string& executable,
            const std::vector<std::string>& args,
            const std::string& working_dir) {
    PROCESS_INFORMATION info{};
    if (!start_process(executable, args, working_dir, &info)) return false;
    ::CloseHandle(info.hProcess);
    return true;
}

std::optional<unsigned long> launch_and_wait(const std::string& executable,
                                             const std::vector<std::string>& args,
                                             const std::string& working_dir) {
    PROCESS_INFORMATION info{};
    if (!start_process(executable, args, working_dir, &info)) return std::nullopt;
    ::WaitForSingleObject(info.hProcess, INFINITE);
    DWORD code = 1;
    ::GetExitCodeProcess(info.hProcess, &code);
    ::CloseHandle(info.hProcess);
    return static_cast<unsigned long>(code);
}

void wait_for_process_exit(unsigned long pid, unsigned long timeout_ms) {
    if (pid == 0) return;
    HandleGuard process;
    process.handle = ::OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (process.handle) ::WaitForSingleObject(process.handle, timeout_ms);
}

std::string today_install_date() {
    SYSTEMTIME now{};
    ::GetLocalTime(&now);
    return format_install_date(now.wYear, now.wMonth, now.wDay);
}

std::string now_iso8601() {
    SYSTEMTIME now{};
    ::GetSystemTime(&now);
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "%04u-%02u-%02uT%02u:%02u:%02uZ", now.wYear,
                  now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    return buffer;
}

#else

std::string local_app_data_dir() { return {}; }
std::string programs_menu_dir() { return {}; }
std::string desktop_dir() { return {}; }
std::string temp_dir() { return {}; }
std::string user_profile_dir() { return {}; }
std::string current_executable_path() { return {}; }
bool write_uninstall_entry(const std::string&, const std::vector<RegistryValue>&,
                           std::string* error) {
    if (error) *error = "the installer only supports Windows";
    return false;
}
std::optional<std::string> uninstall_entry_location(const std::string&) { return std::nullopt; }
std::optional<std::string> uninstall_entry_version(const std::string&) { return std::nullopt; }
bool delete_uninstall_entry(const std::string&) { return false; }
bool add_to_user_path(const std::string&, std::string* error) {
    if (error) *error = "the installer only supports Windows";
    return false;
}
bool remove_from_user_path(const std::string&, std::string*) { return false; }
bool user_path_contains(const std::string&) { return false; }
void broadcast_environment_change() {}
bool create_shortcut(const std::string&, const std::string&, const std::string&,
                     const std::string&, std::string* error) {
    if (error) *error = "the installer only supports Windows";
    return false;
}
std::optional<std::string> shortcut_target(const std::string&) { return std::nullopt; }
void remove_shortcut_if_inside(const std::string&, const std::string&) {}
std::vector<RunningProcess> processes_running_from(const std::string&) { return {}; }
bool launch(const std::string&, const std::vector<std::string>&, const std::string&) {
    return false;
}
std::optional<unsigned long> launch_and_wait(const std::string&, const std::vector<std::string>&,
                                             const std::string&) {
    return std::nullopt;
}
void wait_for_process_exit(unsigned long, unsigned long) {}
std::string today_install_date() { return {}; }
std::string now_iso8601() { return {}; }

#endif

}  // namespace acecode::installer::shell
