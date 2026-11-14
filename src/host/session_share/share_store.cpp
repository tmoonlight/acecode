#include "share_store.hpp"
#include "share/invite_link.hpp"
#include "platform/crypto/secure_random.hpp"
#include "utils/atomic_file.hpp"
#include "utils/state_file.hpp"
#include "utils/utf8_path.hpp"

#include <fstream>
#include <stdexcept>

namespace acecode::share {
namespace {
using nlohmann::json;

json serialize(const ShareRecord& r) {
    return {{"id", r.id}, {"secret", r.secret}, {"session_id", r.session_id},
        {"cwd", r.cwd}, {"owner", r.owner}, {"title", r.title},
        {"include_tools", r.include_tools}, {"allow_questions", r.allow_questions},
        {"revoked", r.revoked}, {"created_at", r.created_at}, {"expires_at", r.expires_at},
        {"question_day", r.question_day}, {"question_count", r.question_count}};
}

ShareRecord deserialize(const json& j) {
    ShareRecord r;
    r.id = j.at("id"); r.secret = j.at("secret"); r.session_id = j.at("session_id");
    r.cwd = j.at("cwd"); r.owner = j.at("owner"); r.title = j.at("title");
    r.include_tools = j.at("include_tools"); r.allow_questions = j.at("allow_questions");
    r.revoked = j.at("revoked"); r.created_at = j.at("created_at"); r.expires_at = j.at("expires_at");
    r.question_day = j.value("question_day", std::int64_t{0});
    r.question_count = j.value("question_count", 0);
    if (!valid_share_credential(r.credential())) throw std::invalid_argument("invalid_share");
    return r;
}

bool same_secret(std::string_view a, std::string_view b) {
    // Always compare the fixed-size credential field, including length errors.
    volatile unsigned int mismatch = static_cast<unsigned int>(a.size() ^ b.size());
    for (std::size_t i = 0; i < 32; ++i)
        mismatch |= static_cast<unsigned int>((i < a.size() ? a[i] : 0) ^ (i < b.size() ? b[i] : 0));
    return mismatch == 0;
}
} // namespace

ShareStore::ShareStore(std::string data_dir)
    : directory_(path_to_utf8(path_from_utf8(data_dir) / "shares")),
      path_(path_to_utf8(path_from_utf8(directory_) / "shares.json")) {}

ShareRecord ShareStore::create(ShareRecord record, std::int64_t now) {
    if (record.session_id.empty() || record.expires_at <= now)
        throw std::invalid_argument("invalid_share");
    const auto bytes = platform::secure_random_bytes(8);
    record.secret = platform::secure_random_token(32);
    if (bytes.size() != 8 || record.secret.size() != 32) throw std::runtime_error("random_failed");
    constexpr char hex[] = "0123456789abcdef";
    record.id.clear();
    for (unsigned char byte : bytes) { record.id += hex[byte >> 4]; record.id += hex[byte & 15]; }
    record.created_at = now;
    record.revoked = false;
    record.question_day = now / 86400;
    record.question_count = 0;
    if (!update_state_json_at(path_, [record](json& state) {
        state[record.id] = serialize(record); return true;
    })) throw std::runtime_error("share_save_failed");
    return record;
}

std::vector<ShareRecord> ShareStore::list() const {
    std::vector<ShareRecord> records;
    const auto state = read_state_json_at(path_);
    for (const auto& item : state.items()) {
        try {
            auto record = deserialize(item.value());
            if (record.id == item.key()) records.push_back(std::move(record));
        } catch (const std::exception&) { /* Corrupt records never grant access. */ }
    }
    return records;
}

std::optional<ShareRecord> ShareStore::find(const std::string& id) const {
    const auto state = read_state_json_at(path_);
    const auto it = state.find(id);
    if (it == state.end()) return std::nullopt;
    try { auto r = deserialize(*it); if (r.id == id) return r; } catch (const std::exception&) {}
    return std::nullopt;
}

bool ShareStore::revoke(const std::string& id) {
    const auto result = update_state_json_at(path_, [id](json& state) {
        auto it = state.find(id);
        if (it == state.end() || !it->is_object()) return false;
        (*it)["revoked"] = true; return true;
    });
    if (!result) throw std::runtime_error("share_save_failed");
    return result.changed;
}

bool ShareStore::has_active(std::int64_t now) const {
    for (const auto& record : list()) if (!record.revoked && record.expires_at > now) return true;
    return false;
}

ShareAuthorization ShareStore::authorize(const std::string& credential, std::int64_t now) const {
    if (credential.empty()) return {{}, "missing_share"};
    if (!valid_share_credential(credential)) return {{}, "invalid_share"};
    auto record = find(credential.substr(0, 16));
    if (!record || !same_secret(credential.substr(17), record->secret)) return {{}, "unknown_share"};
    if (record->revoked) return {{}, "revoked"};
    if (record->expires_at <= now) return {{}, "expired"};
    return {std::move(record), {}};
}

std::string ShareStore::begin_question(const std::string& id, std::int64_t now) {
    std::lock_guard<std::mutex> lock(questions_mu_);
    if (active_questions_.count(id)) return "question_busy";
    const auto result = update_state_json_at(path_, [id, now](json& state) {
        if (!state.contains(id)) return false;
        auto r = deserialize(state.at(id));
        if (r.revoked || r.expires_at <= now || !r.allow_questions) return false;
        if (r.question_day != now / 86400) { r.question_day = now / 86400; r.question_count = 0; }
        if (r.question_count >= 30) return false;
        ++r.question_count; state[id] = serialize(r); return true;
    });
    if (!result) return "share_save_failed";
    if (!result.state.contains(id)) return "unknown_share";
    const auto record = deserialize(result.state.at(id));
    if (record.revoked) return "revoked";
    if (record.expires_at <= now) return "expired";
    if (!record.allow_questions) return "read_only";
    if (!result.changed) return "daily_limit";
    active_questions_.insert(id);
    return {};
}

void ShareStore::end_question(const std::string& id) {
    std::lock_guard<std::mutex> lock(questions_mu_);
    active_questions_.erase(id);
}

nlohmann::json ShareStore::questions(const std::string& id) const {
    if (!find(id)) return json::array();
    std::lock_guard<std::mutex> lock(questions_mu_);
    json result = json::array();
    std::ifstream input(path_from_utf8(directory_) / (id + ".asks.jsonl"));
    for (std::string line; std::getline(input, line);) {
        auto record = json::parse(line, nullptr, false);
        if (record.is_object()) result.push_back(std::move(record));
    }
    return result;
}

void ShareStore::append_question(const std::string& id, const json& record) {
    if (!find(id)) throw std::invalid_argument("unknown_share");
    std::lock_guard<std::mutex> lock(questions_mu_);
    const auto path = path_from_utf8(directory_) / (id + ".asks.jsonl");
    std::vector<std::string> lines;
    { std::ifstream input(path); for (std::string line; std::getline(input, line);) lines.push_back(std::move(line)); }
    lines.push_back(record.dump());
    std::string content;
    const auto start = lines.size() > 200 ? lines.size() - 200 : 0;
    for (auto i = start; i < lines.size(); ++i) content += lines[i] + '\n';
    if (!atomic_write_file(path_to_utf8(path), content)) throw std::runtime_error("share_log_failed");
}

nlohmann::json share_public_json(const ShareRecord& record) {
    auto result = serialize(record);
    result.erase("secret"); result.erase("cwd");
    result["mode"] = record.allow_questions ? "interactive" : "read_only";
    return result;
}

} // namespace acecode::share
