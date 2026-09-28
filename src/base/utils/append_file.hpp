#pragma once

#include <cstdint>
#include <filesystem>
#include <string_view>

namespace acecode {

// A file sink with OS append positioning across independent processes. Callers
// serialize access to this object and submit each complete record in one call.
class AppendFile {
public:
    AppendFile() = default;
    ~AppendFile();
    AppendFile(const AppendFile&) = delete;
    AppendFile& operator=(const AppendFile&) = delete;

    bool open(const std::filesystem::path& path);
    void close();
    bool is_open() const { return handle_ != -1; }
    bool append(std::string_view record);

private:
    std::intptr_t handle_ = -1;
};

} // namespace acecode
