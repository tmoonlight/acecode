#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>
#include <nlohmann/json.hpp>

namespace acecode::share {

struct ShareRecord {
    std::string id;
    std::string secret;
    std::string session_id;
    std::string cwd;
    std::string owner;
    std::string title;
    bool include_tools = false;
    bool allow_questions = true;
    bool revoked = false;
    std::int64_t created_at = 0;
    std::int64_t expires_at = 0;
    std::int64_t question_day = 0;
    int question_count = 0;
    std::string credential() const { return id + "." + secret; }
};

struct ShareAuthorization {
    std::optional<ShareRecord> share;
    std::string error;
};

class ShareStore {
public:
    explicit ShareStore(std::string data_dir);
    ShareRecord create(ShareRecord record, std::int64_t now);
    std::vector<ShareRecord> list() const;
    std::optional<ShareRecord> find(const std::string& id) const;
    bool revoke(const std::string& id);
    bool has_active(std::int64_t now) const;
    ShareAuthorization authorize(const std::string& credential, std::int64_t now) const;
    // Empty means admitted; quota is durably claimed before calling the model.
    std::string begin_question(const std::string& id, std::int64_t now);
    void end_question(const std::string& id);
    void append_question(const std::string& id, const nlohmann::json& record);
    nlohmann::json questions(const std::string& id) const;
private:
    std::string directory_;
    std::string path_;
    mutable std::mutex questions_mu_;
    std::unordered_set<std::string> active_questions_;
};

nlohmann::json share_public_json(const ShareRecord& record);

} // namespace acecode::share
