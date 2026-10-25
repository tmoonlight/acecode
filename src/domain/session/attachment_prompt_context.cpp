#include "attachment_prompt_context.hpp"
#include "pasted_text_attachment.hpp"
#include "llm/tool_protocol_names.hpp"

#include "utils/utf8_path.hpp"

#include <cstdint>

#include <nlohmann/json.hpp>

namespace acecode {

namespace {

// Paste statistics recorded at upload time (metadata.pasted_text, validated by
// apply_pasted_text_upload_metadata). Only non-negative integers are copied;
// anything else in a hand-edited record is ignored rather than echoed.
struct PastedTextStats {
    nlohmann::json fields = nlohmann::json::object();
    std::uint64_t part = 0;
    std::uint64_t parts = 0;
};

bool is_non_negative_integer(const nlohmann::json& value) {
    if (value.is_number_unsigned()) return true;
    return value.is_number_integer() && value.get<std::int64_t>() >= 0;
}

PastedTextStats pasted_text_stats(const AttachmentRecord& record) {
    PastedTextStats stats;
    const auto it = record.metadata.find("pasted_text");
    if (it == record.metadata.end() || !it->is_object()) return stats;
    for (const char* key : {"lines", "chars"}) {
        if (it->contains(key) && is_non_negative_integer((*it)[key])) {
            stats.fields[key] = (*it)[key];
        }
    }
    if (it->contains("part") && is_non_negative_integer((*it)["part"]) &&
        it->contains("parts") && is_non_negative_integer((*it)["parts"])) {
        stats.part = (*it)["part"].get<std::uint64_t>();
        stats.parts = (*it)["parts"].get<std::uint64_t>();
        if (stats.part >= 1 && stats.part <= stats.parts) {
            stats.fields["part"] = stats.part;
            stats.fields["parts"] = stats.parts;
        } else {
            stats.part = stats.parts = 0;
        }
    }
    return stats;
}

} // namespace

std::optional<std::string> attachment_source_path(
    const AttachmentRecord& record) {
    if (!record.metadata.is_object()) return std::nullopt;
    const auto it = record.metadata.find("source_path");
    if (it == record.metadata.end() || !it->is_string()) {
        return std::nullopt;
    }
    const std::string path = it->get<std::string>();
    return path.empty() ? std::nullopt : std::optional<std::string>{path};
}

std::string file_attachment_reference_text(
    const AttachmentRecord& record) {
    const std::string snapshot_path = record.path.empty()
        ? std::string{}
        : path_to_utf8_generic(path_from_utf8(record.path));
    nlohmann::json reference = {
        {"attachment_id", record.id},
        {"name", record.name},
        {"mime_type", record.mime_type},
        {"size_bytes", record.size_bytes},
    };

    const auto source_path = attachment_source_path(record);
    if (source_path.has_value()) {
        reference["source_path"] = *source_path;
    }
    if (!snapshot_path.empty()) {
        reference["snapshot_path"] = snapshot_path;
    }

    const std::string read_path = source_path.value_or(snapshot_path);
    if (!read_path.empty()) {
        reference["read_path"] = read_path;
    }

    // Large pastes from the Web composer are stored as a file and reach the
    // model only through this reference. They are the user's own material,
    // not an incidental attachment, so the wording asks for a read up front.
    // Everything added here comes from the attachment record, so the text
    // stays byte-stable across requests (prompt cache prefix).
    const bool pasted_text = is_pasted_text_attachment(record);
    PastedTextStats paste_stats;
    if (pasted_text) {
        paste_stats = pasted_text_stats(record);
        reference["origin"] = kPastedTextOrigin;
        for (auto field = paste_stats.fields.begin();
             field != paste_stats.fields.end(); ++field) {
            reference[field.key()] = field.value();
        }
    }

    std::string text = "[Attached file reference]\n" + reference.dump(2);
    text += "\nThe file content is not included in this message.";
    // 被超限恢复撤下的内联图片:说清楚为什么撤、别再挂回去。否则模型看到的是
    // 「需要时去读」,转身又用 show_image 把同一张图挂回请求,再被拒、再撤,
    // 白白空转。文案只取决于附件记录,逐字节稳定(prompt cache 前缀)。
    const auto withheld = record.metadata.is_object()
        ? record.metadata.find(kInlineImageWithheldMetadataKey)
        : record.metadata.end();
    if (withheld != record.metadata.end() && withheld->is_boolean() &&
        withheld->get<bool>()) {
        text +=
            " This image was withheld: the provider rejected a request that"
            " carried it as inline image data, so the active model may not be"
            " able to read images. Do not attach it again (for example with `" +
            model_tool_name_for_native("show_image") +
            "`) unless the user asks; if its contents matter, ask the user or"
            " call `" + model_tool_name_for_native("vision_analyze") +
            "` with `model_name` set to a vision-capable saved model.";
        return text;
    }
    // 图片预算撤下的较早图片(grok-build 同款措辞):说明它已不可见、别凭记忆
    // 描述;真要再看就重新打开,预算会再从更早的图片里腾地方。
    const auto budget_withheld = record.metadata.is_object()
        ? record.metadata.find(kImageBudgetWithheldMetadataKey)
        : record.metadata.end();
    if (budget_withheld != record.metadata.end() && budget_withheld->is_boolean() &&
        budget_withheld->get<bool>()) {
        text +=
            " This earlier image was removed to keep the request within its size"
            " limit and is no longer visible. Do not describe or reason about its"
            " contents from memory; if you need to see it again, open it with `" +
            model_tool_name_for_native("show_image") +
            "` or ask the user to share it again.";
        return text;
    }
    if (read_path.empty()) {
        text += " No readable path is available.";
        return text;
    }

    const std::string read_tool = model_tool_name_for_native("file_read");
    if (pasted_text) {
        text += " This is text the user pasted into the message";
        if (paste_stats.parts > 0) {
            text += " (part " + std::to_string(paste_stats.part) + " of " +
                    std::to_string(paste_stats.parts) + ")";
        }
        text +=
            "; ACECode stored it as a file because it is large. The user's"
            " request usually depends on it: read `read_path` with `" +
            read_tool + "` before answering, in line or byte windows if it"
            " is long.";
    } else {
        text +=
            " Read `read_path` with `" + read_tool +
            "` or another suitable read-only "
            "inspection tool only when the task needs the contents.";
    }
    if (source_path.has_value() && !snapshot_path.empty()) {
        text +=
            " If `source_path` is unavailable, read `snapshot_path` instead."
            " Modify `source_path` only when the user asks to change the original"
            " file; never modify `snapshot_path`.";
    } else if (source_path.has_value()) {
        text +=
            " This attachment is a source reference; no session snapshot was"
            " created. Modify `source_path` only when the user asks to change"
            " the original file.";
    } else if (!snapshot_path.empty()) {
        text += " `snapshot_path` is the session copy; never modify it.";
    }
    return text;
}

} // namespace acecode
