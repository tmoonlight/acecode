#pragma once

// 网页链接打开(add-tui-hyperlinks 5.1)——http/https 链接用系统默认浏览器打开。
//
// 安全性:
//   - 仅放行 http/https scheme(防 file:/data:/javascript: 等意外打开);
//   - 禁止控制字符(含 ESC 0x1B,防终端转义注入)与空白开头等异常;
//   - 默认 launcher 不经 shell(POSIX fork+execlp / Windows ShellExecuteW),
//     URL 原样作参数传递,无 shell 注入面。
//
// 可注入 launcher 便于单测(见 tests/utils/open_url_test.cpp)。

#include <functional>
#include <string>

namespace acecode {

struct OpenUrlResult {
    bool ok = false;
    std::string error;
};

using OpenUrlLauncher =
    std::function<bool(const std::string& url, std::string& error)>;

// 校验 URL 可安全交给系统浏览器:http/https scheme + 无控制字符/前导空白。
bool is_openable_http_url(const std::string& url);

// 打开 URL:校验通过后交给 launcher(默认 = 平台浏览器打开器)。
// 失败返回错误信息,绝不抛异常。
OpenUrlResult open_url_in_browser(const std::string& url,
                                  OpenUrlLauncher launcher = {});

} // namespace acecode
