// Small, signed CLI entry for the deduplicated macOS update archive.
// exec preserves terminal I/O, argument boundaries, signals and exit status.
#include <cerrno>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <vector>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace {
std::filesystem::path launcher_path() {
#ifdef __APPLE__
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> path(size);
    if (_NSGetExecutablePath(path.data(), &size) != 0) return {};
    return std::filesystem::canonical(path.data());
#else
    // Linux permits exercising the actual exec path in portable packaging tests.
    return std::filesystem::canonical("/proc/self/exe");
#endif
}
} // namespace

int main(int argc, char** argv) {
    try {
        const auto parent = launcher_path().parent_path();
        const bool bundled = parent.filename() == "MacOS" &&
                             parent.parent_path().filename() == "Contents" &&
                             parent.parent_path().parent_path().filename() == "ACECode.app";
        const auto daemon = bundled ? parent / "acecode-daemon"
            : parent / "ACECode.app" / "Contents" / "MacOS" / "acecode-daemon";
        const std::string executable = daemon.string();
        std::vector<char*> arguments;
        arguments.reserve(static_cast<std::size_t>(argc) + 1);
        arguments.push_back(const_cast<char*>(executable.c_str()));
        for (int i = 1; i < argc; ++i) arguments.push_back(argv[i]);
        arguments.push_back(nullptr);
        ::execv(executable.c_str(), arguments.data());
        const int code = errno;
        std::fprintf(stderr, "ACECode CLI: cannot start %s: %s\n",
                     executable.c_str(), std::strerror(code));
        return code == ENOENT ? 127 : 126;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "ACECode CLI: cannot locate the installed application: %s\n", error.what());
        return 126;
    }
}
