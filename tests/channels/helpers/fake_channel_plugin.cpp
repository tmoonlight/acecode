// 测试用的外部平台插件(openspec add-channel-platform-plugins D3):走真实的进程与管道,
// 按参数模拟正常收发、需要登录、初始化后崩溃、协议版本不兼容。所有收到的帧追加写进 --log 文件。
//
//   --account <id>        报告的本人账号(默认 me)
//   --name <名字>          报告的显示名(默认 测试用户)
//   --log <文件>           记录收到的每一帧(每行一个 JSON)
//   --counter <文件>       每次启动在文件里追加一行,测试据此数重启次数
//   --crash-times <N>      前 N 次启动在回复 initialize 后立即退出
//   --version <N>          initialize 报告的协议版本(默认 1)
//   --login-required       初始化后报告需要登录;收到 action login 后再报告已连接
//   --no-inbound           初始化后不主动发入站消息
//   --spawn-holder <毫秒>  启动时拉起一个继承 stdin/stdout 的子进程,睡这么久再退出
//                          (模拟插件拉起的常驻服务:插件退出后管道写端仍被它持有)
//   --hold <毫秒>          只睡这么久就退出,不读写任何东西(--spawn-holder 拉起的子进程用)

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

using json = nlohmann::json;

struct Options {
    std::string account = "me";
    std::string name = "\xE6\xB5\x8B\xE8\xAF\x95\xE7\x94\xA8\xE6\x88\xB7";  // 测试用户
    std::string log;
    std::string counter;
    int crash_times = 0;
    int version = 1;
    bool login_required = false;
    bool inbound = true;
    std::string spawn_holder_ms;
};

// 拉起一个继承标准句柄的子进程;它不碰管道,只是让写端在插件退出后仍然打开。
void spawn_holder(const char* self, const std::string& ms) {
#ifdef _WIN32
    // _spawnv 按空格拼命令行,程序路径要自己加引号。
    const std::string quoted = std::string("\"") + self + "\"";
    const char* args[] = {quoted.c_str(), "--hold", ms.c_str(), nullptr};
    _spawnv(_P_NOWAIT, self, args);
#else
    if (::fork() == 0) {
        ::execl(self, self, "--hold", ms.c_str(), static_cast<char*>(nullptr));
        ::_exit(127);
    }
#endif
}

void emit(const json& frame) {
    // argv 在 Windows 上按 ANSI 代码页解码,中文参数可能不是合法 UTF-8;替换掉而不是让 dump 抛异常退出。
    std::cout << frame.dump(-1, ' ', false, json::error_handler_t::replace) << '\n';
    std::cout.flush();
}

void log_frame(const Options& options, const json& frame) {
    if (options.log.empty()) return;
    std::ofstream out(options.log, std::ios::binary | std::ios::app);
    out << frame.dump() << '\n';
}

int start_count(const Options& options) {
    if (options.counter.empty()) return 1;
    {
        std::ofstream out(options.counter, std::ios::binary | std::ios::app);
        out << "start\n";
    }
    std::ifstream in(options.counter, std::ios::binary);
    int lines = 0;
    std::string line;
    while (std::getline(in, line)) ++lines;
    return lines;
}

void connected(const Options& options) {
    emit({{"event", "status"},
          {"params", {{"state", "connected"}, {"account", options.account}, {"display_name", options.name}}}});
}

void send_inbound(const Options& options, const json& settings) {
    emit({{"event", "inbound"},
          {"params",
           {{"address", {{"kind", "private"}, {"chat", options.account}, {"sender", options.account}}},
            {"message_id", "m1"},
            {"text", "hello"},
            {"platform", "spoofed"},
            {"timestamp_ms", 1761300000000LL}}}});
    for (const auto& group : settings.value("watched_groups", json::array())) {
        if (!group.is_string()) continue;
        emit({{"event", "inbound"},
              {"params",
               {{"address", {{"kind", "group"}, {"chat", group}, {"sender", "u2"}}},
                {"message_id", "g1"},
                {"text", "@\xE6\xB5\x8B\xE8\xAF\x95\xE7\x94\xA8\xE6\x88\xB7 ping"},
                {"mentioned", true},
                {"sender_name", "Bob"},
                {"chat_name", "\xE6\xB5\x8B\xE8\xAF\x95\xE7\xBE\xA4"}}}});  // 测试群
    }
}

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string{}; };
        if (arg == "--account") options.account = next();
        else if (arg == "--name") options.name = next();
        else if (arg == "--log") options.log = next();
        else if (arg == "--counter") options.counter = next();
        else if (arg == "--crash-times") options.crash_times = std::stoi(next());
        else if (arg == "--version") options.version = std::stoi(next());
        else if (arg == "--login-required") options.login_required = true;
        else if (arg == "--no-inbound") options.inbound = false;
        else if (arg == "--spawn-holder") options.spawn_holder_ms = next();
        else if (arg == "--hold") {
            // 换到临时目录再睡:继承来的 cwd 是测试的临时目录,占着它测试就删不掉。
            std::error_code ec;
            std::filesystem::current_path(std::filesystem::temp_directory_path(ec), ec);
            std::this_thread::sleep_for(std::chrono::milliseconds(std::stoi(next())));
            return 0;
        }
    }
    if (!options.spawn_holder_ms.empty()) spawn_holder(argv[0], options.spawn_holder_ms);
    const int started = start_count(options);

    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const auto frame = json::parse(line, nullptr, false);
        if (!frame.is_object()) return 2;
        log_frame(options, frame);
        const auto method = frame.value("method", std::string{});
        const bool has_id = frame.contains("id");
        const auto params = frame.value("params", json::object());
        const auto reply = [&](const json& result) {
            if (has_id) emit({{"id", frame["id"]}, {"ok", true}, {"result", result}});
        };
        if (method == "initialize") {
            reply({{"protocol_version", options.version},
                   {"capabilities", {{"max_text_units", 3000}, {"supports_typing", true}}}});
            if (started <= options.crash_times) return 3;
            if (options.login_required) {
                emit({{"event", "status"},
                      {"params",
                       {{"state", "failed"},
                        {"retry_stopped", true},
                        {"account", options.account},
                        {"detail", "need login"},
                        {"extra", {{"login_required", true}}}}}});
                continue;
            }
            connected(options);
            if (options.inbound) send_inbound(options, params.value("settings", json::object()));
        } else if (method == "send_text") {
            const auto text = params.value("text", std::string{});
            if (text == "reject") {
                reply({{"outcome", "failed"}, {"error", "rejected by platform"}});
            } else {
                reply({{"outcome", "sent"}});
            }
        } else if (method == "send_file") {
            reply({{"outcome", "failed"}, {"error", "files unsupported"}});
        } else if (method == "download") {
            reply({{"ok", false}, {"error", "no media"}});
        } else if (method == "configure") {
            reply(json::object());
        } else if (method == "action") {
            const auto name = params.value("name", std::string{});
            reply({{"name", name}});
            if (name == "login") connected(options);
        } else if (method == "shutdown") {
            reply(json::object());
            return 0;
        } else if (has_id) {
            emit({{"id", frame["id"]}, {"ok", false}, {"error", "unknown method"}});
        }
    }
    return 0;
}
