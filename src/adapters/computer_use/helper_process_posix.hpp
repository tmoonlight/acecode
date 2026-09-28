#pragma once

#ifdef __APPLE__
#include <string>
#include <vector>
#include <sys/types.h>

namespace acecode::computer_use {

// Nonblocking private transport. Its owner serializes access, including close.
// Only the child endpoints survive exec; no shell or PATH lookup is involved.
class PosixHelperProcess {
public:
    ~PosixHelperProcess();
    PosixHelperProcess() = default;
    PosixHelperProcess(const PosixHelperProcess&) = delete;
    PosixHelperProcess& operator=(const PosixHelperProcess&) = delete;
    bool start(const std::string& executable, const std::vector<std::string>& arguments = {});
    bool alive();
    bool started() const { return pid_ > 0; }
    void terminate();
    void close();
    ssize_t write(const char* bytes, std::size_t size);
    ssize_t read(char* bytes, std::size_t size);
    int descriptor() const { return socket_; }
private:
    pid_t pid_ = -1;
    int socket_ = -1;
};

std::string computer_use_helper_path();
} // namespace acecode::computer_use
#endif
