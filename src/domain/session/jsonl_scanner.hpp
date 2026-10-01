#pragma once

#include "session_file_reader.hpp"
#include <optional>

namespace acecode {

struct JsonlRecord {
    std::uint64_t offset = 0;
    std::uint64_t end = 0;
    std::string text;
    bool terminated = false;
};

// Synchronous borrowed reader, bounded to its captured file size. The scanner
// retains at most one 64 KiB block plus the current record, including long rows.
class JsonlScanner {
public:
    JsonlScanner(const SessionFileReader& reader, bool reverse,
                 std::optional<std::uint64_t> position = {});
    std::optional<JsonlRecord> next();

private:
    void ensure_block(std::uint64_t position);
    const SessionFileReader& reader_;
    bool reverse_;
    std::uint64_t position_;
    std::uint64_t block_start_ = 0;
    std::string block_;
};

} // namespace acecode
