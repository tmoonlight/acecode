#include "theme_drafts.hpp"

#include "image/image_processor.hpp"
#include "utils/atomic_file.hpp"
#include "utils/sha256.hpp"
#include "utils/utf8_path.hpp"
#include "utils/uuid.hpp"

#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <set>

namespace acecode::themes {
namespace fs = std::filesystem;
using nlohmann::json;
namespace {
constexpr std::size_t kImageLimit = 16 * 1024 * 1024;
const std::map<std::string, std::string> kExtraBackgrounds = {
    {"session_background", "session-background.png"},
    {"user_message_background", "user-message-background.png"},
};

std::shared_ptr<std::mutex> draft_mutex(const fs::path& path) {
    static std::mutex guard;
    static std::map<std::string, std::weak_ptr<std::mutex>> locks;
    std::lock_guard<std::mutex> lock(guard);
    for (auto it = locks.begin(); it != locks.end();) {
        if (it->second.expired()) it = locks.erase(it); else ++it;
    }
    auto& slot = locks[path_to_utf8(fs::absolute(path).lexically_normal())];
    auto mutex = slot.lock();
    if (!mutex) { mutex = std::make_shared<std::mutex>(); slot = mutex; }
    return mutex;
}

std::string read_bytes(const fs::path& path, std::size_t limit) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec) || ec || fs::file_size(path, ec) > limit || ec)
        throw ThemeError(422, "THEME_RESOURCE_UNAVAILABLE", "Theme resource is missing or too large", path_to_utf8(path));
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw ThemeError(422, "THEME_RESOURCE_UNAVAILABLE", "Could not read theme resource", path_to_utf8(path));
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

void save(const fs::path& path, const std::string& bytes) {
    if (!atomic_write_file(path_to_utf8(path), bytes))
        throw ThemeError(500, "THEME_SAVE_FAILED", "Could not save theme draft", path_to_utf8(path));
}

std::string palette_digest(const json& draft) {
    json palette = {{"name", draft.at("name")}, {"mode", draft.at("mode")},
        {"colors", draft.at("colors")}};
    // Preserve existing approvals when continuing a draft without overrides.
    if (draft.contains("appearance")) palette["appearance"] = draft.at("appearance");
    return sha256_hex(palette.dump());
}

std::string prototype_digest(const json& draft) {
    json prototype = {{"palette", palette_digest(draft)},
        {"background", draft.at("background_sha256")}, {"preview", draft.at("preview_sha256")}};
    // Absent optional resources preserve the digest of previously approved drafts.
    for (const auto& [kind, filename] : kExtraBackgrounds)
        if (draft.contains(kind + "_sha256")) prototype[kind] = draft.at(kind + "_sha256");
    return sha256_hex(prototype.dump());
}

bool palette_approved(const json& draft) {
    return draft.value("palette_approval", "") == palette_digest(draft);
}

json describe(const json& draft) {
    json result = {{"draft_id", draft.at("draft_id")}, {"name", draft.at("name")},
        {"mode", draft.at("mode")}, {"colors", draft.at("colors")}};
    if (draft.contains("appearance")) result["appearance"] = draft.at("appearance");
    if (draft.contains("installed")) {
        result.update(draft.at("installed"));
        result["stage"] = "installed";
        result["next_action"] = "done";
    } else if (!palette_approved(draft)) {
        result["stage"] = "palette_pending";
        result["next_action"] = "Show the palette and call palette for explicit user confirmation.";
    } else if (draft.value("prototype_approval", "").empty() ||
               draft.at("prototype_approval") != prototype_digest(draft)) {
        result["stage"] = "prototype_pending";
        result["next_action"] = "Prepare and show the selected backgrounds and UI preview (HTML/browser is supported), then call prototype for user confirmation.";
    } else {
        result["stage"] = "ready";
        result["next_action"] = "install";
    }
    return result;
}

bool confirmed(const json& response, const std::string& id, const std::string& label) {
    if (!response.is_object() || response.value("cancelled", false) ||
        response.value("timed_out", false) || response.value("auto_answered", false) ||
        !response.contains("answers") || !response["answers"].is_array() || response["answers"].size() != 1) return false;
    const auto& answer = response["answers"][0];
    return answer.is_object() && answer.value("question_id", "") == id &&
        answer.value("custom_text", "").empty() && answer.contains("selected") &&
        answer["selected"] == json::array({label});
}

json ask(const ThemeDraftStore::Confirm& confirm, const std::string& id,
         const std::string& question, const std::string& label) {
    if (!confirm) return {{"cancelled", true}, {"reason", "Interactive confirmation is unavailable."}};
    return confirm(json::array({{{"id", id}, {"text", question}, {"header", "AI主题"},
        {"multiSelect", false}, {"options", json::array({
            {{"label", label}, {"value", label}, {"description", "使用当前展示的方案继续。"}},
            {{"label", "修改"}, {"value", "修改"}, {"description", "说明修改意见后重新预览。"}}
        })}}}));
}

std::string input_png(const fs::path& path, int max_edge = 0, std::size_t limit = kImageLimit) {
    image::ImageNormalizeOptions options;
    options.force_png = true;
    options.max_edge = max_edge;
    options.final_max_bytes = limit;
    auto result = image::normalize_image_bytes(read_bytes(path, kImageLimit), "", options);
    if (!result.ok || result.bytes.empty()) throw ThemeError(422, "THEME_INVALID_IMAGE",
        "Could not decode theme image: " + result.error, path_to_utf8(path));
    return std::move(result.bytes);
}

void check_keys(const json& args, const std::string& action) {
    std::set<std::string> allowed = {"action", "draft_id"};
    if (action == "palette") allowed.insert({"name", "mode", "colors", "appearance"});
    else if (action == "prototype") allowed.insert({"background_path", "preview_path", "session_background_path", "user_message_background_path"});
    else if (action != "install" && action != "status")
        throw ThemeError(400, "THEME_INVALID_ACTION", "Unknown theme_create action");
    for (const auto& item : args.items()) {
        if (!allowed.count(item.key())) throw ThemeError(400, "THEME_INVALID_ARGUMENT", "Unexpected argument: " + item.key());
    }
}

json read_draft(const fs::path& path, const std::string& id, const std::string& session) {
    const auto draft = json::parse(read_bytes(path, 128 * 1024));
    if (draft.at("schema_version") != 1 || draft.at("draft_id") != id || draft.at("session_id") != session)
        throw ThemeError(403, "THEME_DRAFT_OWNER_MISMATCH", "Theme draft belongs to another session");
    if (draft.contains("appearance") && !valid_theme_appearance(draft.at("appearance")))
        throw ThemeError(422, "THEME_INVALID_APPEARANCE", "Theme draft has invalid appearance overrides");
    return draft;
}
} // namespace

ThemeDraftStore::ThemeDraftStore(fs::path root) : root_(std::move(root)) {}

json ThemeDraftStore::execute(const json& args, const std::string& session_id,
                              const std::string& cwd, const Confirm& confirm) {
    if (session_id.empty()) throw ThemeError(400, "THEME_SESSION_REQUIRED", "An active session is required");
    if (!args.is_object()) throw ThemeError(400, "THEME_INVALID_ARGUMENT", "Expected theme_create arguments object");
    const auto action = args.at("action").get<std::string>();
    check_keys(args, action);
    auto id = args.value("draft_id", std::string{});
    if (action == "status" && id.empty()) {
        json drafts = json::array();
        std::error_code ec;
        fs::directory_iterator it(root_ / "drafts", ec), end;
        for (; !ec && it != end; it.increment(ec)) {
            const auto item_id = path_to_utf8(it->path().filename());
            if (!is_local_theme("ai-" + item_id)) continue;
            try {
                const auto mutex = draft_mutex(it->path());
                std::lock_guard<std::mutex> lock(*mutex);
                drafts.push_back(describe(read_draft(it->path() / "draft.json", item_id, session_id)));
            } catch (...) { /* Other sessions and incomplete drafts stay private. */ }
        }
        return {{"drafts", drafts}, {"next_action", "Resume a listed draft, or call palette to create one."}};
    }
    const bool creating = id.empty() && action == "palette";
    if (creating) id = generate_uuid();
    if (!is_local_theme("ai-" + id)) throw ThemeError(400, "THEME_INVALID_DRAFT_ID", "A valid draft_id is required");
    const auto directory = root_ / "drafts" / id;
    const auto path = directory / "draft.json";
    const auto mutex = draft_mutex(directory);
    // Keep this specific draft locked across the callback. A concurrent update
    // cannot replace the resources while a human is approving the prior view.
    std::lock_guard<std::mutex> lock(*mutex);
    json draft = creating ? json{{"schema_version", 1}, {"draft_id", id}, {"session_id", session_id}}
        : read_draft(path, id, session_id);
    if (action == "status") return describe(draft);
    if (draft.contains("installed") && action != "install")
        throw ThemeError(409, "THEME_DRAFT_INSTALLED", "Create a new palette draft to revise an installed theme");
    if (action == "palette") {
        const auto name = args.at("name").get<std::string>();
        const auto mode = args.at("mode").get<std::string>();
        if (args.contains("appearance") && !valid_theme_appearance(args.at("appearance")))
            throw ThemeError(422, "THEME_INVALID_APPEARANCE",
                "Appearance accepts supported HEX colors, boolean extend_to_titlebar and numeric opacity values from 0 to 1");
        if (name.empty() || name.size() > 256 || name.find_first_not_of(" \t\r\n") == std::string::npos ||
            std::any_of(name.begin(), name.end(), [](unsigned char c) { return c < 32 || c == 127; }) ||
            (mode != "light" && mode != "dark") || !valid_theme_colors(args.at("colors")))
            throw ThemeError(422, "THEME_INVALID_PALETTE", "Provide a name, light/dark mode and all 28 HEX colors");
        draft["name"] = name;
        draft["mode"] = mode;
        draft["colors"] = args.at("colors");
        if (args.contains("appearance")) draft["appearance"] = args.at("appearance");
        else draft.erase("appearance");
        draft["palette_approval"] = "";
        draft["prototype_approval"] = "";
        draft.erase("background_sha256");
        draft.erase("preview_sha256");
        for (const auto& [kind, filename] : kExtraBackgrounds) draft.erase(kind + "_sha256");
        save(path, draft.dump(2));
        const auto question_id = "theme-palette-" + id;
        const auto response = ask(confirm, question_id, "确认「" + name + "」当前展示的色系？", "确认色系");
        if (confirmed(response, question_id, "确认色系")) {
            draft["palette_approval"] = palette_digest(draft);
            save(path, draft.dump(2));
        }
        auto result = describe(draft);
        result["confirmed"] = palette_approved(draft);
        if (response.contains("answers")) result["feedback"] = response["answers"];
        if (!result["confirmed"].get<bool>()) {
            result["confirmation_status"] = response.value("timed_out", false) ? "timed_out" :
                response.value("cancelled", false) ? "cancelled" : "changes_requested";
            result["next_action"] = "Address user feedback if provided; otherwise wait. Show a revised palette and confirm it before generating images.";
        }
        return result;
    }
    if (!palette_approved(draft)) throw ThemeError(409, "THEME_PALETTE_CONFIRMATION_REQUIRED", "Confirm the current palette first");
    if (action == "prototype") {
        const auto resolve = [&](const std::string& key) {
            auto file = path_from_utf8(args.at(key).get<std::string>());
            if (file.empty()) throw ThemeError(400, "THEME_INVALID_ARGUMENT", std::string(key) + " is required");
            if (file.is_relative()) file = path_from_utf8(cwd) / file;
            return file;
        };
        const auto background = input_png(resolve("background_path"));
        const auto preview = input_png(resolve("preview_path"));
        std::map<std::string, std::string> extra;
        std::size_t total = background.size();
        for (const auto& [kind, filename] : kExtraBackgrounds) {
            if (!args.contains(kind + "_path")) continue;
            auto bytes = input_png(resolve(kind + "_path"));
            total += bytes.size();
            if (total > kImageLimit) throw ThemeError(422, "THEME_INVALID_IMAGE", "Combined theme backgrounds exceed 16 MiB");
            extra.emplace(kind, std::move(bytes));
        }
        draft["prototype_approval"] = "";
        save(path, draft.dump(2));
        save(directory / "background.png", background);
        save(directory / "preview.png", preview);
        draft["background_sha256"] = sha256_hex(background);
        draft["preview_sha256"] = sha256_hex(preview);
        for (const auto& [kind, filename] : kExtraBackgrounds) {
            draft.erase(kind + "_sha256");
            const auto found = extra.find(kind);
            if (found == extra.end()) continue;
            save(directory / filename, found->second);
            draft[kind + "_sha256"] = sha256_hex(found->second);
        }
        save(path, draft.dump(2));
        const auto question_id = "theme-prototype-" + id;
        const auto response = ask(confirm, question_id, "确认「" + draft.at("name").get<std::string>() +
            "」当前展示的原型，并制作主题？", "确认原型并生成");
        if (confirmed(response, question_id, "确认原型并生成")) {
            draft["prototype_approval"] = prototype_digest(draft);
            save(path, draft.dump(2));
        }
        auto result = describe(draft);
        result["confirmed"] = !draft.value("prototype_approval", "").empty();
        if (response.contains("answers")) result["feedback"] = response["answers"];
        if (!result["confirmed"].get<bool>()) {
            result["confirmation_status"] = response.value("timed_out", false) ? "timed_out" :
                response.value("cancelled", false) ? "cancelled" : "changes_requested";
            result["next_action"] = "Address user feedback if provided; otherwise wait. Show a revised prototype and confirm it before installing.";
        }
        return result;
    }
    if (draft.value("prototype_approval", "").empty() ||
        draft.at("prototype_approval") != prototype_digest(draft))
        throw ThemeError(409, "THEME_PROTOTYPE_CONFIRMATION_REQUIRED", "Confirm the current prototype first");
    const auto background = read_bytes(directory / "background.png", kImageLimit);
    std::map<std::string, std::string> extra;
    bool resources_changed = sha256_hex(background) != draft.at("background_sha256") ||
        sha256_hex(read_bytes(directory / "preview.png", kImageLimit)) != draft.at("preview_sha256");
    for (const auto& [kind, filename] : kExtraBackgrounds) {
        if (!draft.contains(kind + "_sha256")) continue;
        auto bytes = read_bytes(directory / filename, kImageLimit);
        if (sha256_hex(bytes) != draft.at(kind + "_sha256")) resources_changed = true;
        extra.emplace(filename, std::move(bytes));
    }
    if (resources_changed) {
        draft["prototype_approval"] = "";
        save(path, draft.dump(2));
        throw ThemeError(409, "THEME_PROTOTYPE_CHANGED", "Prototype resources changed; show and confirm them again");
    }
    ThemeStore store(root_, std::string{});
    if (draft.contains("installed")) {
        const auto& result = draft.at("installed");
        const auto installed = store.definition(result.at("id"));
        if (installed.at("colors") != draft.at("colors") || installed.at("name") != draft.at("name") ||
            installed.at("mode") != draft.at("mode") ||
            installed.value("appearance", json{}) != draft.value("appearance", json{}) ||
            installed.at("background").at("sha256") != draft.at("background_sha256"))
            throw ThemeError(409, "THEME_VERSION_CONFLICT", "Installed theme differs from the approved draft");
        for (const auto& [kind, filename] : kExtraBackgrounds)
            if (installed.contains(kind) != draft.contains(kind + "_sha256") ||
                (installed.contains(kind) && installed.at(kind).at("sha256") != draft.at(kind + "_sha256")))
                throw ThemeError(409, "THEME_VERSION_CONFLICT", "Installed background differs from the approved draft");
        if (fs::is_regular_file(path_from_utf8(result.at("package_path").get<std::string>()))) return describe(draft);
    }
    const auto thumbnail = input_png(directory / "background.png", 240, 256 * 1024);
    json definition = {{"schema_version", 1}, {"id", "ai-" + id}, {"version", "1.0.0"},
        {"name", draft.at("name")}, {"mode", draft.at("mode")}, {"colors", draft.at("colors")},
        {"background", {{"bytes", background.size()}, {"sha256", sha256_hex(background)}}},
        {"thumbnail", {{"bytes", thumbnail.size()}, {"sha256", sha256_hex(thumbnail)}}}};
    if (draft.contains("appearance")) definition["appearance"] = draft.at("appearance");
    for (const auto& [kind, filename] : kExtraBackgrounds)
        if (extra.count(filename)) definition[kind] = {{"bytes", extra.at(filename).size()}, {"sha256", draft.at(kind + "_sha256")}};
    draft["installed"] = store.install_local(definition, background, thumbnail, extra);
    save(path, draft.dump(2));
    return describe(draft);
}

} // namespace acecode::themes
