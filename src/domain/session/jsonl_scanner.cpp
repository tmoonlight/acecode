#include "jsonl_scanner.hpp"

#include <algorithm>
#include <vector>

namespace acecode {
namespace {
constexpr std::uint64_t block_size = 64 * 1024;
}

JsonlScanner::JsonlScanner(const SessionFileReader& reader, bool reverse,
                           std::optional<std::uint64_t> position)
    : reader_(reader), reverse_(reverse),
      position_(position.value_or(reverse ? reader.size() : 0)) {
    if (position_ > reader.size()) throw HistorySnapshotChanged();
}

void JsonlScanner::ensure_block(std::uint64_t position) {
    if (!block_.empty() && position >= block_start_ &&
        position < block_start_ + block_.size()) return;
    block_start_ = (position / block_size) * block_size;
    block_ = reader_.read(block_start_, block_size);
}

std::optional<JsonlRecord> JsonlScanner::next() {
    if (reverse_) {
        if (position_ == 0) return std::nullopt;
        JsonlRecord record;
        record.end = position_;
        ensure_block(position_ - 1);
        record.terminated = block_[static_cast<std::size_t>(position_ - 1 - block_start_)] == '\n';
        if (record.terminated) --position_;
        std::vector<std::string> pieces;
        std::size_t length = 0;
        while (position_ > 0) {
            ensure_block(position_ - 1);
            const auto end = static_cast<std::size_t>(position_ - block_start_);
            const auto newline = block_.rfind('\n', end - 1);
            const auto begin = newline == std::string::npos ? 0 : newline + 1;
            pieces.push_back(block_.substr(begin, end - begin));
            length += end - begin;
            position_ = block_start_ + begin;
            if (newline != std::string::npos) break;
        }
        record.offset = position_;
        record.text.reserve(length);
        for (auto it = pieces.rbegin(); it != pieces.rend(); ++it) record.text += *it;
        return record;
    }
    if (position_ >= reader_.size()) return std::nullopt;
    JsonlRecord record;
    record.offset = position_;
    while (position_ < reader_.size()) {
        ensure_block(position_);
        const auto begin = static_cast<std::size_t>(position_ - block_start_);
        const auto newline = block_.find('\n', begin);
        const auto end = newline == std::string::npos ? block_.size() : newline;
        record.text.append(block_, begin, end - begin);
        position_ = block_start_ + end;
        if (newline != std::string::npos) {
            ++position_;
            record.terminated = true;
            break;
        }
    }
    record.end = position_;
    return record;
}

} // namespace acecode
