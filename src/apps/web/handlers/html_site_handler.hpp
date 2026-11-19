#pragma once

// HTML 文件预览的「页面资源站点」(GET /api/html-site/<id>/<相对路径>)的纯逻辑。
//
// 文件详情里打开 .html 时,前端把页面原文放进只有 allow-scripts 的 srcdoc 隔离框
// (不透明源,调不到本机 /api/*),再插一个 <base href> 指到这里,让页面里写成相对
// 路径的 CSS / 图片 / 脚本 / 字体能按原目录加载。iframe 发子资源请求带不了
// X-ACECode-Token,所以这条路由不走 require_auth,改由路径里的随机站点号鉴权:
// 站点号只能由已鉴权的 POST /api/files/html-site 登记,只对应一个根目录。
//
// 安全边界(docs/html-preview.md):
//   - 只服务根目录内、扩展名在网页资源白名单里的普通文件;.env、私钥这类文件一律 404;
//   - 页面脚本用 fetch / XHR 读文件内容不放行(不给跨源许可),只有 <script type=module>、
//     字体、样式、图片、媒体这类子资源按 Sec-Fetch-Dest 给 Access-Control-Allow-Origin: *;
//   - HTML / SVG 响应带 CSP sandbox,直接导航过去也还是不透明源。

#include <cstddef>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace acecode::web {

// 站点号 → 根目录。同一根目录重复登记返回同一个号;超过容量淘汰最久没用的。
class HtmlSiteRegistry {
public:
    explicit HtmlSiteRegistry(std::size_t capacity = 64);

    std::string register_root(const std::filesystem::path& root);
    std::optional<std::filesystem::path> lookup(const std::string& id);

private:
    struct Entry {
        std::string id;
        std::filesystem::path root;
        std::string root_key;
    };
    std::mutex mu_;
    std::vector<Entry> entries_;  // 末尾 = 最近使用
    std::size_t capacity_;
};

// 32 位小写十六进制,来自 std::random_device。
std::string generate_html_site_id();
bool is_html_site_id(std::string_view id);

// 允许服务的扩展名 → Content-Type;不在白名单里返回空。
std::optional<std::string> html_site_mime(const std::string& path);

// HTML / SVG 这类可被直接导航成文档的类型,响应要带 CSP sandbox。
bool html_site_mime_is_document(const std::string& mime);

// 只有子资源(模块脚本、字体、样式、图片、媒体)给跨源许可;fetch / XHR(empty)、
// JSON 模块、缺这个头的请求一律不给。
bool html_site_allows_cors(std::string_view sec_fetch_dest);

// "/api/html-site/<id>/<rest>" → {id, rest};前缀不对或没有 id 返回空。rest 保持原样(未解码)。
struct HtmlSiteRequestPath {
    std::string id;
    std::string rest;
};
std::optional<HtmlSiteRequestPath> split_html_site_url(std::string_view url);

// 百分号解码一段 URL 路径;非法转义、解码出 NUL 返回空。
std::optional<std::string> decode_html_site_path(std::string_view encoded);

// 把解码后的相对路径("css/a.css")解析到根目录下的文件。拒绝空段、"."、".."、反斜杠、
// 冒号(盘符 / NTFS 流)以及解析后不在根目录内的目标;不检查文件是否存在。
std::optional<std::filesystem::path> resolve_html_site_file(
    const std::filesystem::path& root, const std::string& relative);

// 登记站点时决定根目录与入口:文件在当前工作区(allowed_cwds 里的 cwd)内时以工作区为根,
// 否则以文件所在目录为根。entry 是入口文件相对根目录的路径(正斜杠、未编码)。
struct HtmlSitePlan {
    std::filesystem::path root;
    std::string entry;
};
std::optional<HtmlSitePlan> plan_html_site(const std::filesystem::path& file,
                                           const std::optional<std::filesystem::path>& workspace_root);

// 入口相对路径按段做百分号编码,前端直接拼进 <base href>。
std::string encode_html_site_path(const std::string& relative);

} // namespace acecode::web
