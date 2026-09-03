#pragma once

// 链接防骗校验(add-tui-hyperlinks 变更 4.4)。
//
// 渲染 markdown 链接 `[label](href)` 时,label 可能被恶意文本伪装成某个知名
// 网站(如 `[google.com](https://evil.example.com)`)。本模块只比域名(host),
// 不比完整路径——非目标:仿冒域名(faceb00k.com)不归本层管,归浏览器。
//
// 决策规则(见 openspec/changes/add-tui-hyperlinks/design.md 决策 4):
//   - href 不含 "://"(本地文件路径 / 裸域名)→ 本地链接通道,label 是文件名
//     或任意文字,放行。防骗只针对网页链接:`[foo.md](docs/foo.md)` 是常见
//     写法,不能因文件名带点号被误降级。
//   - href 是远程 URL 但 host 解析失败(file:///、畸形)→ URL 形 label 降级,
//     普通文字标签保持既有行为。
//   - label 无"URL 形状"(不含点号,或含空白)→ 普通文字标签,放行。
//   - label 呈 URL 形状 → 提取 host 与 href host 比较(忽略大小写);一致放行,
//     不一致降级;畸形/非 ASCII 的 URL 按不匹配处理(降级)。
//
// 降级语义由调用方决定:不进 link_regions、去链接样式、按纯文本渲染。

#include <optional>
#include <string>

namespace acecode::markdown {

// 从 URL / 裸域名中提取 host(先切 authority,再去 userinfo/端口,支持方括号
// IPv6,去尾部句点,小写化)。本地路径、空 authority、畸形端口、含非 ASCII
// (IDN/百分号编码不做)解析失败 → nullopt。
std::optional<std::string> extract_url_host(const std::string& url);

// 防骗校验:label 与 href 组合是否安全渲染为可点击链接(见文件头决策规则)。
bool is_safe_link_label(const std::string& label, const std::string& href);

} // namespace acecode::markdown
