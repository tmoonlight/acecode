#pragma once

#include "attachment_store.hpp"

#include <optional>
#include <string>

namespace acecode {

std::optional<std::string> attachment_source_path(
    const AttachmentRecord& record);

std::string file_attachment_reference_text(
    const AttachmentRecord& record);

} // namespace acecode
