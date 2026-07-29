#include "url_builder.hpp"

#include "../utils/url_encoding.hpp"

#include <sstream>

namespace acecode::desktop {

std::string percent_encode(const std::string& raw) {
    return acecode::utils::percent_encode_query_component(raw);
}

std::string build_loopback_url(int port, const std::string& token) {
    std::ostringstream u;
    u << "http://127.0.0.1:" << port << "/";
    if (!token.empty()) {
        u << "?token=" << percent_encode(token);
    }
    return u.str();
}

} // namespace acecode::desktop
