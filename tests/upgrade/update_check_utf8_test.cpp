// 本文件覆盖升级检查链路(acecode::upgrade::check_for_update 及其上游)在出错时产出的
// 文本编码:UpdateCheckResult 的 error / log_error 会被 GET /api/update/status 原样序列化,
// nlohmann::json::dump() 默认是严格模式,任何一个非 UTF-8 字节都会抛 type_error.316,
// 整个请求变成 500。所以这些文本必须在产生处就转成合法 UTF-8。
//
// 场景:
//   1. 清单服务器返回 GBK 正文(网关 / 代理的中文错误页,不是 JSON):
//      parse_update_manifest 的 "invalid JSON: ..." 会回显出错位置的正文字节。
//   2. 清单文件被按 GBK 保存(字符串值里是 GBK 字节):解析错误回显的 token 同样带 GBK。
//   3. 诊断日志目录建不出来:DiagnosticLog::error() 拼进去的 ec.message() 在中文 Windows
//      上走 ANSI 代码页(GBK)。
//   4. check_for_update 端到端:GBK 正文 + 日志目录失败同时出现,error(含 with_location
//      追加的日志失败说明)与 log_error 都要能按严格模式序列化。
//
// 场景 1、2、4 的 GBK 字节来自 HTTP 正文,任何平台都能复现修复前的失败;场景 3 的 OS 文本
// 只有中文 Windows 才是 GBK,英文系统与 Linux / macOS 上它本来就是 ASCII,用例照常通过。

#include "config/config.hpp"
#include "upgrade/check.hpp"
#include "upgrade/diagnostics.hpp"
#include "upgrade/manifest.hpp"
#include "utils/encoding.hpp"

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <thread>

using namespace std::chrono_literals;

namespace {

// 「该页面无法访问」的 GBK 编码。首字节 0xB8 在 UTF-8 里是孤立的续字节,
// 正是用户报告里 "invalid UTF-8 byte ...: 0xB8" 那一类字节。
const std::string kGbkErrorPage =
    "\xB8\xC3\xD2\xB3\xC3\xE6\xCE\xDE\xB7\xA8\xB7\xC3\xCE\xCA";

// 「重要提示」的 GBK 编码:0xD6 是合法的 UTF-8 双字节首字节,紧跟的 0xD8 却不是续字节,
// 解析器在第二个字节处报错,回显的 token 里带着这两个 GBK 字节。
const std::string kGbkNotes = "\xD6\xD8\xD2\xAA\xCC\xE1\xCA\xBE";

struct TempDir {
    std::filesystem::path path;

    TempDir() {
        std::random_device rd;
        path = std::filesystem::temp_directory_path() /
               ("acecode-update-utf8-" + std::to_string(rd()));
        std::filesystem::create_directories(path);
    }

    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

// 只服务 /aceupdate.json 的本地清单服务器,正文与 Content-Type 由用例指定。
struct LocalManifestServer {
    httplib::Server svr;
    int port = 0;
    std::thread th;

    LocalManifestServer(std::string body, std::string content_type) {
        svr.Get("/aceupdate.json",
                [body, content_type](const httplib::Request&, httplib::Response& res) {
                    res.set_content(body, content_type);
                });
        port = svr.bind_to_any_port("127.0.0.1");
        th = std::thread([this] { svr.listen_after_bind(); });
        for (int i = 0; i < 50 && !svr.is_running(); ++i) {
            std::this_thread::sleep_for(10ms);
        }
    }

    ~LocalManifestServer() {
        svr.stop();
        if (th.joinable()) th.join();
    }

    std::string base_url() const {
        return "http://127.0.0.1:" + std::to_string(port) + "/";
    }
};

// 用一个同名普通文件占住日志目录,让 DiagnosticLog 的 create_directories 必然失败。
std::filesystem::path blocked_log_directory(const TempDir& tmp) {
    const auto blocked = tmp.path / "logs";
    std::ofstream(blocked) << "blocks the upgrade log directory";
    return blocked;
}

} // namespace

// 场景 1:清单正文是 GBK 中文错误页(不是 JSON)。
// 触发:parse_update_manifest(kGbkErrorPage) —— nlohmann 在第一个字节 0xB8 处报
//      "invalid literal; last read: '<0xB8>'",parse_error::what() 原样回显这个字节。
// 期望:返回 nullopt;error 以 "invalid JSON: " 开头、是合法 UTF-8,
//      放进 JSON 按严格模式 dump 不抛异常。
// 回归:修复前 error 里带裸 0xB8,/api/update/status 序列化时抛
//      "[json.exception.type_error.316] invalid UTF-8 byte at index 144: 0xB8"
//      (144 = "invalid JSON: " 加 nlohmann 错误前缀的长度),接口 500。
TEST(UpdateCheckUtf8, ManifestParseErrorFromGbkBodyIsUtf8) {
    std::string error;
    const auto manifest = acecode::upgrade::parse_update_manifest(kGbkErrorPage, &error);

    EXPECT_FALSE(manifest.has_value());
    EXPECT_EQ(error.rfind("invalid JSON: ", 0), 0u) << error;
    EXPECT_TRUE(acecode::is_valid_utf8(error)) << error;
    EXPECT_NO_THROW(nlohmann::json({{"error", error}}).dump());
}

// 场景 2:清单文件被按 GBK 保存,notes 字符串里是 GBK 字节。
// 触发:前面的字段都是合法 JSON,解析到 notes 的 "重" 时 0xD6 被当作 UTF-8 首字节、
//      0xD8 不是续字节,报 "invalid string: ill-formed UTF-8 byte",
//      回显的 token 是 '"' + 0xD6 0xD8。
// 期望:error 是合法 UTF-8,严格模式 dump 不抛异常(中文 Windows 上按 GBK 解出「重」,
//      其他代码页解成别的字符或 '?',只要求合法)。
// 回归:修复前 token 里的 0xD6 0xD8 原样进入 error,序列化 500。
TEST(UpdateCheckUtf8, ManifestParseErrorFromGbkStringValueIsUtf8) {
    const std::string body =
        "{\"schema_version\":1,\"latest\":\"1.0.0\",\"releases\":[{\"version\":\"1.0.0\","
        "\"notes\":\"" + kGbkNotes + "\",\"packages\":[]}]}";

    std::string error;
    const auto manifest = acecode::upgrade::parse_update_manifest(body, &error);

    EXPECT_FALSE(manifest.has_value());
    EXPECT_EQ(error.rfind("invalid JSON: ", 0), 0u) << error;
    EXPECT_TRUE(acecode::is_valid_utf8(error)) << error;
    EXPECT_NO_THROW(nlohmann::json({{"error", error}}).dump());
}

// 场景 3:升级诊断日志目录建不出来(logs 路径被同名普通文件占住)。
// 触发:DiagnosticLog 构造时 create_directories 失败,
//      error() = "cannot create log directory: " + ec.message()。
// 期望:error() 非空、前缀不变、是合法 UTF-8;with_location() 拼上这段说明后仍是合法 UTF-8。
// 回归:中文 Windows 上 ec.message() 是 GBK(「当文件已存在时，无法创建该文件。」),
//      修复前它经 check_for_update 进 log_error / error,/api/update/status 序列化 500。
//      英文系统上该文本是 ASCII,这条用例只在中文 Windows 上能复现修复前的失败。
TEST(UpdateCheckUtf8, DiagnosticLogDirectoryFailureTextIsUtf8) {
    TempDir tmp;
    acecode::upgrade::DiagnosticLog log("check", blocked_log_directory(tmp));

    const std::string error = log.error();
    ASSERT_FALSE(error.empty());
    EXPECT_EQ(error.rfind("cannot create log directory: ", 0), 0u) << error;
    EXPECT_TRUE(acecode::is_valid_utf8(error)) << error;
    EXPECT_TRUE(acecode::is_valid_utf8(
        log.with_location("manifest request returned HTTP 404")));
    EXPECT_NO_THROW(nlohmann::json({{"log_error", error}}).dump());
}

// 场景 4:check_for_update 端到端 —— 本地清单服务器返回 GBK 正文,同时日志目录被占住。
// 触发:清单解析失败(场景 1)+ DiagnosticLog 失败(场景 3),check_for_update 用
//      with_location() 把日志失败说明追加到 error 末尾,log_error 也带同一段文本。
// 期望:status 为 ManifestInvalid;error 以 "invalid JSON: " 开头并带
//      "Upgrade diagnostics unavailable or incomplete: cannot create log directory: ";
//      error 与 log_error 都是合法 UTF-8,与 /api/update/status 一样按严格模式 dump 不抛异常。
// 回归:修复前 error 里带清单正文的 0xB8,任何平台上严格 dump 都会抛 type_error.316。
TEST(UpdateCheckUtf8, CheckForUpdateResultStaysSerializableWithGbkInputs) {
    TempDir tmp;
    LocalManifestServer server(kGbkErrorPage, "text/html; charset=gbk");
    acecode::AppConfig cfg;
    cfg.network.proxy_mode = "off";
    cfg.upgrade.base_url = server.base_url();
    acecode::upgrade::DiagnosticLog diagnostics("check", blocked_log_directory(tmp));

    const auto result = acecode::upgrade::check_for_update(cfg, "0.1.2", &diagnostics);

    EXPECT_EQ(result.status, acecode::upgrade::UpdateCheckStatus::ManifestInvalid);
    EXPECT_EQ(result.http_status, 200);
    EXPECT_EQ(result.error.rfind("invalid JSON: ", 0), 0u) << result.error;
    EXPECT_NE(result.error.find("Upgrade diagnostics unavailable or incomplete: "
                                "cannot create log directory: "),
              std::string::npos) << result.error;
    EXPECT_TRUE(acecode::is_valid_utf8(result.error)) << result.error;
    EXPECT_TRUE(acecode::is_valid_utf8(result.log_error)) << result.log_error;
    const nlohmann::json body{{"error", result.error}, {"log_error", result.log_error}};
    EXPECT_NO_THROW(body.dump());
}
