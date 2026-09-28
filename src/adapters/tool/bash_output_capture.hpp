#pragma once

#include <cstddef>
#include <fstream>
#include <string>
#include <string_view>

namespace acecode {

inline constexpr std::size_t kBashInlineOutputLimitBytes = 100 * 1024;

// Captures already-decoded UTF-8 subprocess output. Large preserved results
// spill to disk while the process is running; standalone results keep the
// legacy head/tail preview without ever retaining the discarded middle.
class BashOutputCapture {
public:
    explicit BashOutputCapture(bool preserve_full_output,
                               std::string tool_results_dir = {});

    bool append(std::string_view text);
    std::string finish();

    bool failed() const { return !error_.empty(); }
    bool truncated() const { return truncated_; }
    std::size_t total_bytes() const { return total_bytes_; }
    std::size_t retained_bytes() const { return output_.size() + tail_.size(); }
    const std::string& error() const { return error_; }

private:
    bool start_file();
    bool write(std::string_view text);
    void fail(const std::string& reason);

    bool preserve_full_output_;
    bool truncated_ = false;
    bool finished_ = false;
    std::size_t total_bytes_ = 0;
    std::string tool_results_dir_;
    std::string output_;
    std::string tail_;
    std::string filepath_;
    std::string error_;
    std::ofstream file_;
};

} // namespace acecode
