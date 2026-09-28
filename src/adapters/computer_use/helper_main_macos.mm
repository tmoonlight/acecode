#include "macos_native.hpp"
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <sys/file.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

int computer_use_protocol_main();

int main(int argc, char** argv) {
    @autoreleasepool {
        using namespace acecode::computer_use::macos;
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        const pid_t parent = getppid();
        if (parent <= 1) return 2;
        signal(SIGTERM, SIG_IGN);
        signal(SIGINT, SIG_IGN);
        signal(SIGPIPE, SIG_IGN);
        dispatch_queue_t watchdog = dispatch_queue_create("dev.acecode.computer-use.lifetime", DISPATCH_QUEUE_SERIAL);
        auto stop = ^{ revoke_input(); std::_Exit(0); };
        dispatch_source_t parent_source = dispatch_source_create(DISPATCH_SOURCE_TYPE_PROC, parent, DISPATCH_PROC_EXIT, watchdog);
        dispatch_source_t term_source = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGTERM, 0, watchdog);
        dispatch_source_t interrupt_source = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGINT, 0, watchdog);
        if (!parent_source || !term_source || !interrupt_source) return 2;
        for (dispatch_source_t source : {parent_source, term_source, interrupt_source}) {
            dispatch_source_set_event_handler(source, stop); dispatch_resume(source);
        }
        if (getppid() != parent) return 2;
        const bool probe = argc == 2 && std::string(argv[1]) == "--permissions";
        const bool request = argc == 3 && std::string(argv[1]) == "--request-permission";
        int lease = -1;
        if (!probe && !request) {
            if (argc != 1) return 2;
            char directory[4096]{};
            const auto count = confstr(_CS_DARWIN_USER_TEMP_DIR, directory, sizeof(directory));
            if (!count || count > sizeof(directory)) return 2;
            const std::string path = std::string(directory) + "acecode-computer-use.lock";
            lease = open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
            struct stat status{};
            if (lease < 0 || fstat(lease, &status) != 0 || !S_ISREG(status.st_mode) || status.st_uid != getuid() || status.st_nlink != 1 || flock(lease, LOCK_EX | LOCK_NB) != 0) {
                std::string ignored; std::getline(std::cin, ignored);
                std::cout << json{{"protocol_version", 1}, {"success", false}, {"output", {{"error", "COMPUTER_USE_BUSY"},
                    {"message", "Another ACECode instance owns this desktop."}}}}.dump() << '\n' << std::flush;
                if (lease >= 0) close(lease);
                return 2;
            }
        }
        const std::string permission = request ? argv[2] : "";
        std::thread worker([probe, request, permission] {
            @autoreleasepool {
                int code = 0;
                if (probe || request) {
                    try { std::cout << json{{"protocol_version", 1}, {"success", true}, {"output", permissions(permission)}}.dump() << '\n' << std::flush; }
                    catch (const Error& error) { std::cout << json{{"protocol_version", 1}, {"success", false}, {"output", {{"error", error.code}, {"message", error.what()}}}}.dump() << '\n' << std::flush; code = 1; }
                } else code = computer_use_protocol_main();
                revoke_input();
                dispatch_async(dispatch_get_main_queue(), ^{ pointer_hide(); std::exit(code); });
            }
        });
        worker.detach();
        [NSApp run];
        revoke_input();
        if (lease >= 0) close(lease);
        return 0;
    }
}
