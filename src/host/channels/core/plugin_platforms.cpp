#include "plugin_platforms.hpp"

#include "utils/utf8_path.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <system_error>

namespace acecode::channels::core {
namespace {

constexpr std::uintmax_t kMaxManifestBytes = 64 * 1024;
constexpr std::size_t kMaxLabelBytes = 64;
constexpr std::size_t kMaxArgs = 32;
constexpr std::size_t kMaxArgBytes = 4096;
constexpr const char* kManifestDirToken = "${manifest_dir}";

bool fail(std::string* error, const std::string& message) {
    if (error) *error = message;
    return false;
}

std::string replace_all(std::string text, const std::string& from, const std::string& to) {
    for (std::size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size()))
        text.replace(at, from.size(), to);
    return text;
}

} // namespace

std::optional<PlatformSpec> parse_plugin_manifest(const nlohmann::json& manifest,
                                                  const std::filesystem::path& manifest_dir, std::string* error) {
    const auto reject = [error](const std::string& message) {
        fail(error, message);
        return std::optional<PlatformSpec>{};
    };
    if (!manifest.is_object()) return reject("清单不是 JSON 对象");
    if (manifest.value("schema", std::string{}) != kPluginManifestSchema)
        return reject(std::string("schema 必须是 ") + kPluginManifestSchema);

    const auto name_it = manifest.find("name");
    if (name_it == manifest.end() || !name_it->is_string() || !valid_platform_name(name_it->get<std::string>()))
        return reject("name 只能由小写字母、数字和连字符组成,以字母开头,2-32 个字符");
    const auto name = name_it->get<std::string>();
    if (is_builtin_platform(name)) return reject("name 与内置平台重名:" + name);

    const auto label_it = manifest.find("label");
    if (label_it == manifest.end() || !label_it->is_string() || label_it->get<std::string>().empty() ||
        label_it->get<std::string>().size() > kMaxLabelBytes)
        return reject("label 必须是 1-64 字节的字符串");

    const auto account = manifest.value("account", std::string{});
    if (account != "self") return reject("account 目前只支持 self");

    std::error_code ec;
    const auto dir = std::filesystem::absolute(manifest_dir, ec).lexically_normal();
    if (ec) return reject("无法解析清单目录");
    const auto dir_utf8 = path_to_utf8(dir);

    const auto command_it = manifest.find("command");
    if (command_it == manifest.end() || !command_it->is_string() || command_it->get<std::string>().empty())
        return reject("缺少 command");
    auto command = path_from_utf8(command_it->get<std::string>());
    if (command.is_relative()) command = dir / command;
    command = command.lexically_normal();
    if (!std::filesystem::is_regular_file(command, ec)) return reject("command 指向的程序不存在");

    PluginLaunch launch;
    launch.manifest_dir = dir_utf8;
    launch.command = path_to_utf8(command);
    if (const auto args_it = manifest.find("args"); args_it != manifest.end()) {
        if (!args_it->is_array() || args_it->size() > kMaxArgs) return reject("args 必须是最多 32 项的字符串数组");
        for (const auto& arg : *args_it) {
            if (!arg.is_string() || arg.get<std::string>().size() > kMaxArgBytes)
                return reject("args 必须是最多 32 项的字符串数组");
            launch.args.push_back(replace_all(arg.get<std::string>(), kManifestDirToken, dir_utf8));
        }
    }

    PlatformSpec spec;
    spec.name = name;
    spec.label = label_it->get<std::string>();
    spec.owner_binding = OwnerBinding::Self;
    spec.plugin = std::move(launch);
    if (const auto features = manifest.find("features"); features != manifest.end() && features->is_array()) {
        for (const auto& feature : *features)
            if (feature.is_string() && feature.get<std::string>() == "watched_groups") spec.watched_groups = true;
    }
    return spec;
}

PluginScanResult scan_plugin_platforms(const std::filesystem::path& connectors_root) {
    PluginScanResult result;
    std::error_code ec;
    if (!std::filesystem::is_directory(connectors_root, ec)) return result;
    std::vector<std::filesystem::path> dirs;
    for (const auto& entry : std::filesystem::directory_iterator(connectors_root, ec)) {
        std::error_code entry_ec;
        if (entry.is_directory(entry_ec)) dirs.push_back(entry.path());
    }
    std::sort(dirs.begin(), dirs.end());
    for (const auto& dir : dirs) {
        const auto path = dir / kPluginManifestFile;
        std::error_code file_ec;
        if (!std::filesystem::is_regular_file(path, file_ec)) continue;
        const auto where = path_to_utf8(path);
        const auto size = std::filesystem::file_size(path, file_ec);
        if (file_ec || size > kMaxManifestBytes) {
            result.errors.push_back(where + ": 清单不可读或超过 64 KiB");
            continue;
        }
        std::ifstream in(path, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const auto json = nlohmann::json::parse(text, nullptr, false);
        if (json.is_discarded()) {
            result.errors.push_back(where + ": 不是合法的 JSON");
            continue;
        }
        std::string error;
        auto spec = parse_plugin_manifest(json, dir, &error);
        if (!spec) {
            result.errors.push_back(where + ": " + error);
            continue;
        }
        const bool duplicate = std::any_of(result.platforms.begin(), result.platforms.end(),
                                           [&spec](const PlatformSpec& other) { return other.name == spec->name; });
        if (duplicate) {
            result.errors.push_back(where + ": 与已注册的插件重名:" + spec->name);
            continue;
        }
        result.platforms.push_back(std::move(*spec));
    }
    return result;
}

} // namespace acecode::channels::core
