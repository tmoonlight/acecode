#pragma once

#include "attachment_store.hpp"

#include <optional>
#include <string>

namespace acecode {

// 附件元数据里的标记:这张图片曾以内联图片发给服务端、请求被判超限后,由
// thread_repair 的 withhold_inline_images 降级成了文件引用部件。引用文本据此
// 换成「图片已撤下、别再挂回去」的说明,而不是普通文件的「需要时去读」。
inline constexpr const char* kInlineImageWithheldMetadataKey = "inline_image_withheld";

// 附件元数据里的标记:「压缩图片」开启时,请求体逼近上限,图片预算
// (image_request_budget)把这张较早的图片从请求里换成了文件引用。
inline constexpr const char* kImageBudgetWithheldMetadataKey = "image_budget_withheld";

std::optional<std::string> attachment_source_path(
    const AttachmentRecord& record);

std::string file_attachment_reference_text(
    const AttachmentRecord& record);

} // namespace acecode
