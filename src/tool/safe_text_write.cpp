#include "safe_text_write.hpp"

#include "config/mcp_config.hpp"
#include "text_file_errors.hpp"
#include "utils/utf8_path.hpp"

#include <exception>
#include <filesystem>
#include <system_error>

namespace acecode {

TextSafeWriteResult safe_write_text_file(
    const std::string& path,
    const std::string& lf_text,
    const TextFileMetadata& metadata,
    const std::function<void(const std::string& path)>& before_write) {
    try {
        if (const auto servers = validate_mcp_file_edit(path, lf_text)) {
            if (before_write) before_write(path);
            // Configuration JSON is persisted as UTF-8, independently of an
            // external editor's previous encoding. Snapshot and active-file
            // publication share the same transaction as the settings APIs.
            write_validated_config_file(path, normalize_text_to_lf(lf_text), *servers);
            return {true, {}, false, false};
        }
    } catch (const std::exception& error) {
        return {false, error.what(), false, false};
    }
    auto encoded = with_text_file_tool_errors(encode_text_for_write(lf_text, metadata));
    if (!encoded.success) {
        return {false, encoded.error, false, false};
    }

    std::string pre_write_bytes;
    std::string error;
    const bool existed = std::filesystem::exists(path_from_utf8(path));
    if (existed && !read_file_bytes(path, pre_write_bytes, error)) {
        return {false, error, false, false};
    }

    if (before_write) {
        before_write(path);
    }

    if (!write_file_bytes(path, encoded.bytes, error)) {
        return {false, error, false, false};
    }

    std::string written_bytes;
    if (!read_file_bytes(path, written_bytes, error)) {
        if (existed && !write_file_bytes(path, pre_write_bytes, error)) {
            return {false,
                "[Error] Post-write verification could not read " + path +
                " and rollback failed: " + error,
                false,
                true};
        }
        return {false,
            "[Error] Post-write verification could not read " + path +
            "; edit was rolled back.",
            true,
            false};
    }

    auto round_trip = with_text_file_tool_errors(
        decode_text_file_bytes_with_metadata(written_bytes, metadata, path));
    if (round_trip.success && round_trip.buffer.text == normalize_text_to_lf(lf_text)) {
        return {true, {}, false, false};
    }

    std::string rollback_error;
    if (existed) {
        if (!write_file_bytes(path, pre_write_bytes, rollback_error)) {
            return {false,
                "[Error] Post-write round-trip verification failed for " + path +
                " and rollback failed: " + rollback_error,
                false,
                true};
        }
    } else {
        std::error_code ec;
        std::filesystem::remove(path_from_utf8(path), ec);
        if (ec) {
            return {false,
                "[Error] Post-write round-trip verification failed for " + path +
                " and rollback failed: " + ec.message(),
                false,
                true};
        }
    }

    std::string reason = round_trip.success
        ? "[Error] Post-write round-trip verification changed the intended text"
        : round_trip.error;
    return {false,
        reason + "; edit was rolled back for " + path + ".",
        true,
        false};
}

} // namespace acecode
