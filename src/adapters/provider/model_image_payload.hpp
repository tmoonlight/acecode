#pragma once

#include "llm/llm_provider.hpp"
#include "session/attachment_store.hpp"
#include "session/image_request_budget.hpp"
#include "session/session_history_recovery.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace acecode {

// 判定一条附件是否应当作为 provider 图片 part 发送。以 MIME 为权威依据:
// SVG(矢量 XML)被排除,误标成 image 但 MIME 非图片的附件返回 false,从而
// 在序列化层兜底降级为文件句柄(route-attachments-by-capability D3)。
bool record_is_vision_image(const AttachmentRecord& record);

// 「压缩图片」开启且这张图需要压缩时发送的内容:压缩后的字节、MIME,以及放在
// 图片前面的说明文字。不需要压缩或开关关闭时返回空,调用方按原图发送。
struct CompressedModelImage {
    std::string bytes;
    std::string mime_type;
    std::string note;
};
std::optional<CompressedModelImage> compressed_model_image(const AttachmentRecord& record);

// 历史投影(recover_provider_history)之后,「压缩图片」开启且模型能看图时再套用
// 图片请求体预算:请求逼近上限时较早的内联图片换成文件引用。OpenAI 兼容与
// Anthropic 两条 build_request_body 共用。
ProviderHistoryRecoveryResult recover_provider_history_with_image_budget(
    const std::vector<ChatMessage>& messages,
    ImageRequestBudget& budget,
    std::size_t max_request_bytes,
    bool model_has_vision);

} // namespace acecode
