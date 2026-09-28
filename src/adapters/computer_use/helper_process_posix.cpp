#include "helper_process_posix.hpp"

#ifdef __APPLE__
#include <cerrno>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;

namespace acecode::computer_use {

std::string computer_use_helper_path() {
    std::uint32_t length = 0;
    _NSGetExecutablePath(nullptr, &length);
    std::vector<char> path(length);
    if (_NSGetExecutablePath(path.data(), &length) != 0) return {};
    std::error_code error;
    auto executable = std::filesystem::canonical(path.data(), error);
    if (error) return {};
    return (executable.parent_path() / "acecode-computer-use").string();
}

PosixHelperProcess::~PosixHelperProcess() { close(); }

bool PosixHelperProcess::start(const std::string& executable, const std::vector<std::string>& arguments) {
    close();
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) return false;
    // Daemon launches can have closed stdio. Keep both private endpoints above
    // 2 so child dup2/close actions cannot accidentally close its new stdin.
    for (int& descriptor : sockets) {
        if (descriptor >= 3) continue;
        const int replacement = fcntl(descriptor, F_DUPFD_CLOEXEC, 3);
        if (replacement < 0) { ::close(sockets[0]); ::close(sockets[1]); return false; }
        ::close(descriptor);
        descriptor = replacement;
    }
    const auto fail = [&] { ::close(sockets[0]); ::close(sockets[1]); return false; };
    const int yes = 1;
    if (fcntl(sockets[0], F_SETFD, FD_CLOEXEC) < 0 || fcntl(sockets[1], F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(sockets[0], F_SETFL, O_NONBLOCK) < 0 ||
        setsockopt(sockets[0], SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes)) != 0) return fail();
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    if (posix_spawn_file_actions_init(&actions) != 0) return fail();
    if (posix_spawnattr_init(&attributes) != 0) {
        posix_spawn_file_actions_destroy(&actions);
        return fail();
    }
    int result = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT);
    if (!result) result = posix_spawn_file_actions_adddup2(&actions, sockets[1], STDIN_FILENO);
    if (!result) result = posix_spawn_file_actions_adddup2(&actions, sockets[1], STDOUT_FILENO);
    if (!result) result = posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    if (!result) result = posix_spawn_file_actions_addclose(&actions, sockets[0]);
    if (!result) result = posix_spawn_file_actions_addclose(&actions, sockets[1]);
    std::vector<std::string> storage{executable};
    storage.insert(storage.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    for (auto& value : storage) argv.push_back(value.data());
    argv.push_back(nullptr);
    if (!result) result = posix_spawn(&pid_, executable.c_str(), &actions, &attributes, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    if (result) { pid_ = -1; errno = result; return fail(); }
    ::close(sockets[1]);
    socket_ = sockets[0];
    return true;
}

bool PosixHelperProcess::alive() {
    if (pid_ <= 0) return false;
    int status = 0;
    pid_t result;
    do { result = waitpid(pid_, &status, WNOHANG); } while (result < 0 && errno == EINTR);
    if (result == 0) return true;
    if (result == pid_ || (result < 0 && errno == ECHILD)) pid_ = -1;
    return false;
}

void PosixHelperProcess::terminate() {
    if (alive()) kill(pid_, SIGTERM);
}

void PosixHelperProcess::close() {
    if (pid_ > 0) {
        terminate();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
        while (alive() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (pid_ > 0) {
            kill(pid_, SIGKILL);
            int status;
            while (waitpid(pid_, &status, 0) < 0 && errno == EINTR) {}
            pid_ = -1;
        }
    }
    if (socket_ >= 0) ::close(socket_);
    socket_ = -1;
}

ssize_t PosixHelperProcess::write(const char* bytes, std::size_t size) {
    return send(socket_, bytes, size, 0);
}
ssize_t PosixHelperProcess::read(char* bytes, std::size_t size) {
    return recv(socket_, bytes, size, 0);
}
} // namespace acecode::computer_use
#endif
