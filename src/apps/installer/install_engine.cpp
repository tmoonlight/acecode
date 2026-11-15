#include "install_engine.hpp"

#include "platform/install_record.hpp"
#include "utils/encoding.hpp"
#include "utils/utf8_path.hpp"
#include "windows_shell.hpp"

#include <array>
#include <cctype>
#include <fstream>
#include <mutex>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace acecode::installer {
namespace {

namespace fs = std::filesystem;

constexpr const char* kStartMenuShortcut = "ACECode.lnk";
constexpr std::array<const char*, 4> kLegacyGroupShortcuts{
    "ACECode.lnk", "ACECode 终端.lnk", "卸载 ACECode.lnk", "Uninstall ACECode.lnk"};
constexpr const char* kUninstallCopyPrefix = "acecode-uninstall-";

std::mutex& log_mutex() {
    static std::mutex mu;
    return mu;
}

unsigned long current_pid() {
#ifdef _WIN32
    return static_cast<unsigned long>(::GetCurrentProcessId());
#else
    return 0;
#endif
}

bool is_uninstaller_image(const std::string& image) {
    const auto slash = image.find_last_of("\\/");
    std::string name = slash == std::string::npos ? image : image.substr(slash + 1);
    for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return name == kUninstallerFileName;
}

Outcome failure(FailureKind kind, std::string detail = {}) {
    setup_log("failed: " + detail);
    return Outcome{false, kind, std::move(detail)};
}

std::string error_text(const std::error_code& ec) {
    return ec ? ensure_utf8(ec.message()) + " (" + std::to_string(ec.value()) + ")"
              : std::string();
}

// 安装目录里的文件路径可能超过 MAX_PATH:删除时走扩展长度路径(只用于 IO)。
void remove_tree(const fs::path& path, std::error_code& ec) {
    fs::remove_all(to_extended_length_path(path), ec);
}

// 扩展长度路径不进日志和界面:去掉 \\?\ 前缀再显示。
std::string display_path(const fs::path& path) {
    std::string text = path_to_utf8(path);
    if (text.rfind("\\\\?\\UNC\\", 0) == 0) return "\\\\" + text.substr(8);
    if (text.rfind("\\\\?\\", 0) == 0) return text.substr(4);
    return text;
}

enum class TargetKind { absent, empty, recorded, acecode, foreign };

TargetKind inspect_target(const fs::path& target) {
    std::error_code ec;
    if (!fs::exists(target, ec)) return TargetKind::absent;
    if (!fs::is_directory(target, ec)) return TargetKind::foreign;
    if (fs::is_empty(target, ec)) return TargetKind::empty;
    if (install_record::read_marker(target)) return TargetKind::recorded;
    if (fs::is_regular_file(target / kCliExecutable, ec)) return TargetKind::acecode;
    return TargetKind::foreign;
}

// 旧目录不是安装程序装的(便携 zip 解出来的),里面可能有用户自己放的文件:留成备份。
void retire_previous(const fs::path& previous, TargetKind kind, const std::string& name) {
    std::error_code ec;
    if (!fs::exists(previous, ec)) return;
    const bool legacy_inno = fs::exists(previous / "unins000.dat", ec);
    if (kind == TargetKind::recorded || kind == TargetKind::empty || legacy_inno) {
        remove_tree(previous, ec);
        if (ec) setup_log("could not remove " + path_to_utf8(previous) + ": " + error_text(ec));
        return;
    }
    const fs::path backup = previous.parent_path() /
        path_from_utf8(name + ".backup-" + shell::today_install_date());
    fs::rename(previous, backup, ec);
    setup_log("kept the previous folder as " + path_to_utf8(backup) + " " + error_text(ec));
}

bool write_bytes(const fs::path& path, std::string_view bytes, std::string* error) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.close();
    if (!out) {
        if (error) *error = "failed to write " + path_to_utf8(path);
        return false;
    }
    return true;
}

class ExtractBridge final : public ExtractObserver {
public:
    explicit ExtractBridge(StepObserver& observer) : observer_(observer) {}
    void on_extract_progress(std::uint64_t done, std::uint64_t total) override {
        observer_.on_step(InstallStep::extracting,
                          total ? static_cast<double>(done) / static_cast<double>(total) : 1.0);
    }

private:
    StepObserver& observer_;
};

void remove_start_menu_shortcuts(const std::string& dir) {
    const std::string programs = shell::programs_menu_dir();
    if (programs.empty()) return;
    shell::remove_shortcut_if_inside(join_windows_path(programs, kStartMenuShortcut), dir);
    shell::remove_shortcut_if_inside(
        join_windows_path(programs, std::string(kTerminalShortcutZh) + ".lnk"), dir);
    shell::remove_shortcut_if_inside(
        join_windows_path(programs, std::string(kTerminalShortcutEn) + ".lnk"), dir);
}

void cleanup_legacy_inno(const std::string& dir) {
    const auto location = shell::uninstall_entry_location(kLegacyInnoUninstallKey);
    if (!location || !install_record::same_install_dir(*location, dir)) return;
    shell::delete_uninstall_entry(kLegacyInnoUninstallKey);
    const std::string programs = shell::programs_menu_dir();
    if (!programs.empty()) {
        const std::string group = join_windows_path(programs, kProductName);
        for (const char* name : kLegacyGroupShortcuts) {
            shell::remove_shortcut_if_inside(join_windows_path(group, name), dir);
        }
        std::error_code ec;
        fs::remove(path_from_utf8(group), ec);
    }
    setup_log("removed the legacy Inno Setup registration");
}

Outcome integrate(const std::string& dir,
                  const Payload& payload,
                  const InstallRequest& request,
                  std::uint64_t installed_bytes,
                  StepObserver& observer) {
    observer.on_step(InstallStep::integrating, 0.0);
    UninstallEntryInput entry;
    entry.install_dir = dir;
    entry.version = payload.info.version;
    entry.installed_bytes = installed_bytes + payload.uninstaller.size();
    entry.install_date = shell::today_install_date();
    std::string error;
    if (!shell::write_uninstall_entry(install_record::kUninstallKeyName,
                                      uninstall_entry_values(entry), &error)) {
        return failure(FailureKind::write, error);
    }
    observer.on_step(InstallStep::integrating, 0.3);

    std::error_code ec;
    const std::string desktop_exe = join_windows_path(dir, kDesktopExecutable);
    if (!shell::register_invite_protocol(desktop_exe))
        return failure(FailureKind::write, "failed to register ACECode invitation protocol");
    const std::string cli_exe = join_windows_path(dir, kCliExecutable);
    const std::string app_exe =
        fs::is_regular_file(path_from_utf8(desktop_exe), ec) ? desktop_exe : cli_exe;
    const std::string programs = shell::programs_menu_dir();
    if (!programs.empty()) {
        remove_start_menu_shortcuts(dir);
        if (!shell::create_shortcut(join_windows_path(programs, kStartMenuShortcut), app_exe, dir,
                                    kProductName, &error) ||
            !shell::create_shortcut(
                join_windows_path(programs, request.terminal_shortcut_name + ".lnk"), cli_exe,
                shell::user_profile_dir(), request.terminal_shortcut_name, &error)) {
            return failure(FailureKind::write, error);
        }
    }
    const std::string desktop = shell::desktop_dir();
    if (!desktop.empty()) {
        const std::string link = join_windows_path(desktop, kStartMenuShortcut);
        if (request.desktop_shortcut) {
            if (!shell::create_shortcut(link, app_exe, dir, kProductName, &error)) {
                return failure(FailureKind::write, error);
            }
        } else {
            shell::remove_shortcut_if_inside(link, dir);
        }
    }
    observer.on_step(InstallStep::integrating, 0.6);

    const bool path_ok = request.add_to_path ? shell::add_to_user_path(dir, &error)
                                             : shell::remove_from_user_path(dir, &error);
    if (!path_ok) return failure(FailureKind::write, error);
    cleanup_legacy_inno(dir);
    shell::broadcast_environment_change();
    observer.on_step(InstallStep::finished, 1.0);
    return Outcome{true, FailureKind::none, {}};
}

}  // namespace

std::string build_target() {
#if defined(_M_ARM64) || defined(__aarch64__)
    return "windows-arm64";
#else
    return "windows-x64";
#endif
}

std::optional<Payload> load_embedded_payload(PayloadProblem* problem, std::string* detail) {
    const auto report = [problem, detail](PayloadProblem kind, std::string text) {
        if (problem) *problem = kind;
        if (detail) *detail = std::move(text);
        return std::optional<Payload>();
    };
#ifdef _WIN32
    const auto resource = [](const char* name) -> std::string_view {
        const std::wstring wide_name(name, name + std::char_traits<char>::length(name));
        HRSRC found = ::FindResourceW(nullptr, wide_name.c_str(), MAKEINTRESOURCEW(10));
        if (!found) return {};
        HGLOBAL loaded = ::LoadResource(nullptr, found);
        const DWORD size = ::SizeofResource(nullptr, found);
        const auto* bytes = loaded ? static_cast<const char*>(::LockResource(loaded)) : nullptr;
        return bytes ? std::string_view(bytes, size) : std::string_view();
    };
    Payload payload;
    const std::string_view info_text = resource(kPayloadInfoResourceName);
    payload.archive = resource(kPayloadResourceName);
    payload.uninstaller = resource(kUninstallerResourceName);
    if (info_text.empty() || payload.archive.empty() || payload.uninstaller.empty()) {
        return report(PayloadProblem::missing, "this build has no embedded ACECode package");
    }
    std::string error;
    auto info = parse_payload_info(std::string(info_text), &error);
    if (!info) return report(PayloadProblem::damaged, error);
    if (info->payload_size != payload.archive.size()) {
        return report(PayloadProblem::damaged, "embedded package size does not match");
    }
    if (info->target != build_target()) {
        return report(PayloadProblem::wrong_target, info->target);
    }
    payload.info = *info;
    if (problem) *problem = PayloadProblem::none;
    return payload;
#else
    return report(PayloadProblem::missing, "the installer only supports Windows");
#endif
}

std::optional<ExistingInstall> find_existing_install() {
    if (const auto location = shell::uninstall_entry_location(install_record::kUninstallKeyName)) {
        const std::string dir = trim_windows_dir(*location);
        if (const auto record = install_record::read_marker(path_from_utf8(dir))) {
            return ExistingInstall{dir, record->version, false, record->add_to_path,
                                   record->desktop_shortcut};
        }
    }
    if (const auto location = shell::uninstall_entry_location(kLegacyInnoUninstallKey)) {
        const std::string dir = trim_windows_dir(*location);
        std::error_code ec;
        if (fs::is_regular_file(path_from_utf8(join_windows_path(dir, kCliExecutable)), ec)) {
            ExistingInstall existing;
            existing.install_dir = dir;
            existing.version = shell::uninstall_entry_version(kLegacyInnoUninstallKey).value_or("");
            existing.legacy = true;
            existing.add_to_path = shell::user_path_contains(dir);
            const auto desktop_target = shell::shortcut_target(
                join_windows_path(shell::desktop_dir(), kStartMenuShortcut));
            existing.desktop_shortcut = desktop_target.has_value();
            return existing;
        }
    }
    const std::string fallback = default_install_dir(shell::local_app_data_dir());
    if (const auto record = install_record::read_marker(path_from_utf8(fallback))) {
        return ExistingInstall{fallback, record->version, false, record->add_to_path,
                               record->desktop_shortcut};
    }
    return std::nullopt;
}

Outcome install(const Payload& payload, const InstallRequest& request, StepObserver& observer) {
    observer.on_step(InstallStep::preparing, 0.0);
    const std::string dir = trim_windows_dir(request.install_dir);
    setup_log("install " + payload.info.version + " (" + payload.info.target + ") into " + dir);
    if (!is_acceptable_install_dir(dir)) return failure(FailureKind::invalid_dir, dir);
    if (const auto running = shell::processes_running_from(dir); !running.empty()) {
        return failure(FailureKind::running, running.front().image);
    }
    if (sha256_of(payload.archive) != payload.info.payload_sha256 ||
        sha256_of(payload.uninstaller) != payload.info.uninstaller_sha256) {
        return failure(FailureKind::damaged_payload, "sha256 mismatch");
    }

    const fs::path target = path_from_utf8(dir);
    const fs::path parent = target.parent_path();
    const std::string name = path_to_utf8(target.filename());
    std::error_code ec;
    fs::create_directories(parent, ec);
    if (ec) return failure(FailureKind::write, path_to_utf8(parent) + ": " + error_text(ec));
    const TargetKind kind = inspect_target(target);
    if (kind == TargetKind::foreign) return failure(FailureKind::folder_not_empty, dir);

    const fs::path staging = parent / path_from_utf8(name + ".setup-new");
    fs::path previous = parent / path_from_utf8(name + ".setup-old");
    remove_tree(staging, ec);
    if (fs::exists(previous, ec)) {
        remove_tree(previous, ec);
        if (ec) {
            previous = parent / path_from_utf8(name + ".setup-old-" + shell::today_install_date() +
                                               "-" + std::to_string(current_pid()));
        }
    }
    observer.on_step(InstallStep::preparing, 1.0);

    ExtractBridge bridge(observer);
    const ExtractResult extracted = extract_archive(payload.archive, staging, &bridge);
    if (!extracted.ok) {
        remove_tree(staging, ec);
        return failure(FailureKind::extract, extracted.error);
    }
    std::string error;
    if (!fs::is_regular_file(staging / kCliExecutable, ec)) {
        remove_tree(staging, ec);
        return failure(FailureKind::damaged_payload, "package has no acecode.exe");
    }
    install_record::InstallRecord record;
    record.version = payload.info.version;
    record.target = payload.info.target;
    record.add_to_path = request.add_to_path;
    record.desktop_shortcut = request.desktop_shortcut;
    record.installed_at = shell::now_iso8601();
    if (!write_bytes(staging / kUninstallerFileName, payload.uninstaller, &error) ||
        !install_record::write_marker(staging, record, &error)) {
        remove_tree(staging, ec);
        return failure(FailureKind::write, error);
    }

    observer.on_step(InstallStep::replacing, 0.0);
    const bool had_previous = kind != TargetKind::absent;
    if (had_previous) {
        fs::rename(target, previous, ec);
        if (ec) {
            remove_tree(staging, ec);
            return failure(FailureKind::dir_in_use, dir + ": " + error_text(ec));
        }
    }
    fs::rename(staging, target, ec);
    if (ec) {
        const std::string reason = error_text(ec);
        if (had_previous) fs::rename(previous, target, ec);
        return failure(FailureKind::dir_in_use, dir + ": " + reason);
    }
    observer.on_step(InstallStep::replacing, 0.6);
    if (had_previous) retire_previous(previous, kind, name);
    observer.on_step(InstallStep::replacing, 1.0);

    const Outcome outcome = integrate(dir, payload, request, extracted.bytes, observer);
    if (outcome.ok) setup_log("install finished");
    return outcome;
}

Outcome uninstall(const std::string& install_dir, StepObserver& observer) {
    const std::string dir = trim_windows_dir(install_dir);
    setup_log("uninstall " + dir);
    const fs::path target = path_from_utf8(dir);
    if (!is_acceptable_install_dir(dir) || !install_record::read_marker(target)) {
        return failure(FailureKind::no_record, dir);
    }
    // 静默卸载时,安装目录里的 uninstall.exe 在等这个临时副本跑完,它不算「ACECode 在运行」。
    for (const auto& process : shell::processes_running_from(dir)) {
        if (!is_uninstaller_image(process.image)) {
            return failure(FailureKind::running, process.image);
        }
    }
    observer.on_step(InstallStep::preparing, 0.0);

    remove_start_menu_shortcuts(dir);
    shell::remove_shortcut_if_inside(join_windows_path(shell::desktop_dir(), kStartMenuShortcut),
                                     dir);
    std::string error;
    shell::remove_from_user_path(dir, &error);
    shell::unregister_invite_protocol(join_windows_path(dir, kDesktopExecutable));
    const auto location = shell::uninstall_entry_location(install_record::kUninstallKeyName);
    if (!location || install_record::same_install_dir(*location, dir)) {
        shell::delete_uninstall_entry(install_record::kUninstallKeyName);
    }
    shell::broadcast_environment_change();
    observer.on_step(InstallStep::preparing, 1.0);

    std::error_code ec;
    std::vector<fs::path> files;
    const fs::path target_io = to_extended_length_path(target);
    for (fs::recursive_directory_iterator it(target_io, fs::directory_options::skip_permission_denied,
                                             ec), end;
         !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec) || it->is_symlink(ec)) files.push_back(it->path());
    }
    std::string first_failure;
    // 静默卸载的原启动器仍映射着 uninstall.exe。移动映射中的 exe 只能在同一卷内
    // 改名:自选安装盘可能与 TEMP 不同,因此退场目录放在安装目录旁边。
    const fs::path trash = target.parent_path() /
        (std::string(kUninstallCopyPrefix) + std::to_string(current_pid()) + "-trash");
    for (std::size_t i = 0; i < files.size(); ++i) {
        fs::remove(files[i], ec);
        if (ec) {
            // 正在运行的 exe 删不掉但能在同一磁盘上改名:挪进临时目录,下次运行时清理。
            std::error_code move_ec;
            fs::create_directories(trash, move_ec);
            fs::rename(files[i], trash / (std::to_string(i) + "-" + path_to_utf8(files[i].filename())),
                       move_ec);
            if (!move_ec) ec.clear();
        }
        if (ec && first_failure.empty()) {
            first_failure = display_path(files[i]) + ": " + error_text(ec);
        }
        observer.on_step(InstallStep::extracting,
                         static_cast<double>(i + 1) / static_cast<double>(files.size()));
    }
    remove_tree(target, ec);
    std::error_code exists_ec;
    if (ec || fs::exists(target, exists_ec)) {
        return failure(FailureKind::remove,
                       first_failure.empty() ? dir + ": " + error_text(ec) : first_failure);
    }
    observer.on_step(InstallStep::finished, 1.0);
    setup_log("uninstall finished");
    return Outcome{true, FailureKind::none, {}};
}

bool launch_installed_app(const std::string& install_dir) {
    std::error_code ec;
    const std::string desktop = join_windows_path(install_dir, kDesktopExecutable);
    if (fs::is_regular_file(path_from_utf8(desktop), ec)) {
        return shell::launch(desktop, {}, install_dir);
    }
    return shell::launch(join_windows_path(install_dir, kCliExecutable), {},
                         shell::user_profile_dir());
}

std::optional<std::string> resolve_uninstall_dir(const InstallerArgs& args) {
    if (!args.install_dir.empty()) return trim_windows_dir(args.install_dir);
    const fs::path self = path_from_utf8(shell::current_executable_path());
    if (install_record::read_marker(self.parent_path())) {
        return trim_windows_dir(path_to_utf8(self.parent_path()));
    }
    if (const auto location = shell::uninstall_entry_location(install_record::kUninstallKeyName)) {
        if (install_record::read_marker(path_from_utf8(*location))) {
            return trim_windows_dir(*location);
        }
    }
    return std::nullopt;
}

std::optional<std::string> prepare_uninstall_copy(std::string* error) {
#ifdef _WIN32
    const fs::path temp = path_from_utf8(shell::temp_dir());
    const fs::path folder =
        temp / (std::string(kUninstallCopyPrefix) + std::to_string(::GetCurrentProcessId()));
    std::error_code ec;
    fs::create_directories(folder, ec);
    const fs::path copy = folder / "acecode-uninstall.exe";
    fs::copy_file(path_from_utf8(shell::current_executable_path()), copy,
                  fs::copy_options::overwrite_existing, ec);
    if (ec) {
        if (error) *error = "failed to prepare the uninstaller: " + error_text(ec);
        return std::nullopt;
    }
    return path_to_utf8(copy);
#else
    if (error) *error = "the installer only supports Windows";
    return std::nullopt;
#endif
}

void cleanup_stale_uninstall_copies(const std::string& install_dir) {
#ifdef _WIN32
    std::vector<fs::path> roots{path_from_utf8(shell::temp_dir())};
    if (!install_dir.empty() && is_acceptable_install_dir(install_dir)) {
        roots.push_back(path_from_utf8(install_dir).parent_path());
    }
    const std::string own =
        std::string(kUninstallCopyPrefix) + std::to_string(::GetCurrentProcessId());
    // 正在运行的卸载副本所在的文件夹(以启动它的原进程 PID 命名)也要跳过。
    const std::string running_from =
        path_to_utf8(path_from_utf8(shell::current_executable_path()).parent_path().filename());
    for (const auto& root : roots) {
        std::error_code ec;
        for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
            const std::string name = path_to_utf8(it->path().filename());
            if (name.rfind(kUninstallCopyPrefix, 0) != 0 || name == own || name == running_from) {
                continue;
            }
            std::error_code ignored;
            remove_tree(it->path(), ignored);
        }
    }
#else
    (void)install_dir;
#endif
}

std::string setup_log_path() {
    const std::string temp = shell::temp_dir();
    return temp.empty() ? std::string() : join_windows_path(temp, "acecode-setup.log");
}

void setup_log(const std::string& line) {
    const std::string path = setup_log_path();
    if (path.empty()) return;
    std::lock_guard<std::mutex> lock(log_mutex());
    std::ofstream out(path_from_utf8(path), std::ios::binary | std::ios::app);
    out << shell::now_iso8601() << " " << line << "\n";
}

}  // namespace acecode::installer
