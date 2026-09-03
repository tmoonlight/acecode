#pragma once

// Serialization guard for JSON that carries strings ACECode did not author:
// file contents, directory names, frontmatter, tool output.
//
// nlohmann's default dump() *throws* type_error.316 on the first byte that is
// not valid UTF-8. In an HTTP handler that exception becomes a 500 for the
// entire response — one malformed SKILL.md in a skills directory used to take
// down the whole settings page, telling the user nothing about which file was
// at fault. Sanitising at every producer is the right primary fix, but the
// serializer is the only place that sees *all* of them, so it also has to be
// unable to fail.
//
// `error_handler_t::replace` substitutes U+FFFD for undecodable bytes instead
// of throwing, which degrades one field rather than the response.

#include <nlohmann/json.hpp>

#include <string>

namespace acecode::web {

inline std::string dump_json_lossy(const nlohmann::json& value) {
    return value.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

} // namespace acecode::web
