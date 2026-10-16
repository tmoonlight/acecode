#include "input_plan.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <stdexcept>

namespace acecode::ios_simulator {
namespace {

constexpr double kEdgeBand = 4.0;
constexpr double kSwipeDelta = 10.0;       // points per interpolated sample (idb default)
constexpr double kMaxPathMs = 30000.0;

nlohmann::json touch(const char* phase, double x, double y, const std::string& edge) {
    nlohmann::json event = {{"type", "touch"}, {"phase", phase}, {"x", x}, {"y", y}};
    if (edge != "none") event["edge"] = edge;
    return event;
}

nlohmann::json delay(double ms) { return {{"type", "delay"}, {"ms", std::max(0.0, ms)}}; }

// Decodes one UTF-8 code point; returns 0xFFFD on malformed input.
char32_t next_code_point(const std::string& text, std::size_t& index) {
    const auto byte = static_cast<unsigned char>(text[index++]);
    if (byte < 0x80) return byte;
    int extra = byte >= 0xF0 ? 3 : byte >= 0xE0 ? 2 : byte >= 0xC0 ? 1 : -1;
    if (extra < 0) return 0xFFFD;
    char32_t value = byte & (0x3F >> extra);
    for (int i = 0; i < extra; ++i) {
        if (index >= text.size()) return 0xFFFD;
        const auto next = static_cast<unsigned char>(text[index]);
        if ((next & 0xC0) != 0x80) return 0xFFFD;
        value = (value << 6) | (next & 0x3F);
        ++index;
    }
    return value;
}

} // namespace

std::string edge_for_start(double x, double y, const ScreenSize& screen) {
    if (screen.width <= 0 || screen.height <= 0) return "none";
    if (x < 0 || y < 0 || x > screen.width || y > screen.height) return "none";
    const double left = x;
    const double right = screen.width - x;
    const double top = y;
    const double bottom = screen.height - y;
    const double nearest = std::min({left, right, top, bottom});
    if (nearest > kEdgeBand) return "none";
    if (nearest == bottom) return "bottom";
    if (nearest == top) return "top";
    if (nearest == left) return "left";
    return "right";
}

void require_on_screen(double x, double y, const ScreenSize& screen) {
    if (!std::isfinite(x) || !std::isfinite(y) || x < 0 || y < 0 || x > screen.width || y > screen.height) {
        throw std::invalid_argument("Point (" + std::to_string(x) + ", " + std::to_string(y) +
            ") is outside the screen (" + std::to_string(screen.width) + " x " + std::to_string(screen.height) +
            " points).");
    }
}

nlohmann::json plan_tap(double x, double y, double duration_s, const ScreenSize& screen) {
    require_on_screen(x, y, screen);
    const double hold = duration_s > 0 ? std::min(duration_s, 10.0) * 1000.0 : 60.0;
    return nlohmann::json::array({touch("down", x, y, "none"), delay(hold), touch("up", x, y, "none")});
}

nlohmann::json plan_swipe(double x1, double y1, double x2, double y2, double duration_s, const ScreenSize& screen) {
    require_on_screen(x1, y1, screen);
    // The end may sit exactly on the border; clamp rather than reject it.
    x2 = std::clamp(x2, 0.0, screen.width);
    y2 = std::clamp(y2, 0.0, screen.height);
    const auto edge = edge_for_start(x1, y1, screen);
    const double duration = duration_s > 0 ? std::min(duration_s, 10.0) : 0.3;
    const double distance = std::hypot(x2 - x1, y2 - y1);
    const int steps = std::max(1, static_cast<int>(distance / kSwipeDelta));
    const double dx = (x2 - x1) / steps;
    const double dy = (y2 - y1) / steps;
    const double step_ms = duration * 1000.0 / (steps + 2);
    auto events = nlohmann::json::array();
    for (int i = 0; i <= steps; ++i) {
        events.push_back(touch("down", x1 + dx * i, y1 + dy * i, edge));
        events.push_back(delay(step_ms));
    }
    // A final stationary sample stops momentum so the swipe lands precisely.
    events.push_back(touch("down", x2, y2, edge));
    events.push_back(delay(step_ms));
    events.push_back(touch("up", x2, y2, edge));
    return events;
}

nlohmann::json plan_touch_path(const std::vector<PathPoint>& points, const ScreenSize& screen) {
    if (points.size() < 2) throw std::invalid_argument("touch_path needs at least two points.");
    require_on_screen(points.front().x, points.front().y, screen);
    const auto edge = edge_for_start(points.front().x, points.front().y, screen);
    auto events = nlohmann::json::array();
    double total = 0;
    for (std::size_t i = 0; i < points.size(); ++i) {
        const auto& point = points[i];
        const double wait = std::clamp(point.dt_ms, 0.0, 1000.0);
        total += wait;
        if (total > kMaxPathMs) throw std::invalid_argument("touch_path exceeds 30 seconds.");
        if (i > 0 && wait > 0) events.push_back(delay(wait));
        const double x = std::clamp(point.x, 0.0, screen.width);
        const double y = std::clamp(point.y, 0.0, screen.height);
        events.push_back(touch(i + 1 == points.size() ? "up" : "down", x, y, edge));
    }
    return events;
}

nlohmann::json plan_touch2_path(const std::vector<TwoFingerPoint>& points, const ScreenSize& screen) {
    if (points.size() < 2) throw std::invalid_argument("touch2_path needs at least two samples.");
    require_on_screen(points.front().x1, points.front().y1, screen);
    require_on_screen(points.front().x2, points.front().y2, screen);
    auto events = nlohmann::json::array();
    double total = 0;
    for (std::size_t i = 0; i < points.size(); ++i) {
        const auto& point = points[i];
        const double wait = std::clamp(point.dt_ms, 0.0, 1000.0);
        total += wait;
        if (total > kMaxPathMs) throw std::invalid_argument("touch2_path exceeds 30 seconds.");
        if (i > 0 && wait > 0) events.push_back(delay(wait));
        events.push_back({{"type", "touch2"}, {"phase", i + 1 == points.size() ? "up" : "down"},
                          {"x1", std::clamp(point.x1, 0.0, screen.width)},
                          {"y1", std::clamp(point.y1, 0.0, screen.height)},
                          {"x2", std::clamp(point.x2, 0.0, screen.width)},
                          {"y2", std::clamp(point.y2, 0.0, screen.height)}});
    }
    return events;
}

double planned_duration_ms(const nlohmann::json& events) {
    double total = 0;
    for (const auto& event : events) {
        if (event.value("type", std::string()) == "delay") total += event.value("ms", 0.0);
        if (event.contains("delay_ms")) total += event.value("delay_ms", 0.0);
    }
    return total;
}

std::optional<KeyStroke> key_for_char(char32_t c) {
    if (c >= 'a' && c <= 'z') return KeyStroke{0x04u + static_cast<std::uint32_t>(c - 'a'), false};
    if (c >= 'A' && c <= 'Z') return KeyStroke{0x04u + static_cast<std::uint32_t>(c - 'A'), true};
    if (c >= '1' && c <= '9') return KeyStroke{0x1Eu + static_cast<std::uint32_t>(c - '1'), false};
    switch (c) {
        case '0': return KeyStroke{0x27, false};
        case '\n': case '\r': return KeyStroke{0x28, false};
        case '\t': return KeyStroke{0x2B, false};
        case ' ': return KeyStroke{0x2C, false};
        case '-': return KeyStroke{0x2D, false};
        case '=': return KeyStroke{0x2E, false};
        case '[': return KeyStroke{0x2F, false};
        case ']': return KeyStroke{0x30, false};
        case '\\': return KeyStroke{0x31, false};
        case ';': return KeyStroke{0x33, false};
        case '\'': return KeyStroke{0x34, false};
        case '`': return KeyStroke{0x35, false};
        case ',': return KeyStroke{0x36, false};
        case '.': return KeyStroke{0x37, false};
        case '/': return KeyStroke{0x38, false};
        case '!': return KeyStroke{0x1E, true};
        case '@': return KeyStroke{0x1F, true};
        case '#': return KeyStroke{0x20, true};
        case '$': return KeyStroke{0x21, true};
        case '%': return KeyStroke{0x22, true};
        case '^': return KeyStroke{0x23, true};
        case '&': return KeyStroke{0x24, true};
        case '*': return KeyStroke{0x25, true};
        case '(': return KeyStroke{0x26, true};
        case ')': return KeyStroke{0x27, true};
        case '_': return KeyStroke{0x2D, true};
        case '+': return KeyStroke{0x2E, true};
        case '{': return KeyStroke{0x2F, true};
        case '}': return KeyStroke{0x30, true};
        case '|': return KeyStroke{0x31, true};
        case ':': return KeyStroke{0x33, true};
        case '"': return KeyStroke{0x34, true};
        case '~': return KeyStroke{0x35, true};
        case '<': return KeyStroke{0x36, true};
        case '>': return KeyStroke{0x37, true};
        case '?': return KeyStroke{0x38, true};
        default: return std::nullopt;
    }
}

std::optional<std::vector<KeyStroke>> keys_for_text(const std::string& utf8) {
    std::vector<KeyStroke> strokes;
    std::size_t index = 0;
    while (index < utf8.size()) {
        const auto code = next_code_point(utf8, index);
        if (code == '\r' && index < utf8.size() && utf8[index] == '\n') continue;
        auto stroke = key_for_char(code);
        if (!stroke) return std::nullopt;
        strokes.push_back(*stroke);
    }
    return strokes;
}

std::optional<std::vector<std::uint32_t>> chord_for_name(const std::string& raw) {
    std::string name;
    for (char c : raw) {
        if (!std::isspace(static_cast<unsigned char>(c))) name.push_back(static_cast<char>(std::tolower(c)));
    }
    std::vector<std::uint32_t> usages;
    std::size_t start = 0;
    while (start <= name.size()) {
        const auto plus = name.find('+', start);
        const auto part = name.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
        start = plus == std::string::npos ? name.size() + 1 : plus + 1;
        if (part.empty()) return std::nullopt;
        std::uint32_t usage = 0;
        if (part == "cmd" || part == "command" || part == "meta") usage = 0xE3;
        else if (part == "shift") usage = 0xE1;
        else if (part == "ctrl" || part == "control") usage = 0xE0;
        else if (part == "alt" || part == "option" || part == "opt") usage = 0xE2;
        else if (part == "return" || part == "enter") usage = 0x28;
        else if (part == "escape" || part == "esc") usage = 0x29;
        else if (part == "delete" || part == "backspace") usage = 0x2A;
        else if (part == "tab") usage = 0x2B;
        else if (part == "space") usage = 0x2C;
        else if (part == "forward_delete" || part == "forwarddelete") usage = 0x4C;
        else if (part == "right") usage = 0x4F;
        else if (part == "left") usage = 0x50;
        else if (part == "down") usage = 0x51;
        else if (part == "up") usage = 0x52;
        else if (part == "home") usage = 0x4A;
        else if (part == "end") usage = 0x4D;
        else if (part == "page_up" || part == "pageup") usage = 0x4B;
        else if (part == "page_down" || part == "pagedown") usage = 0x4E;
        else if (part.size() >= 2 && part[0] == 'f' && std::isdigit(static_cast<unsigned char>(part[1]))) {
            const int number = std::atoi(part.c_str() + 1);
            if (number < 1 || number > 12) return std::nullopt;
            usage = 0x3A + static_cast<std::uint32_t>(number - 1);
        } else if (part.size() == 1) {
            auto stroke = key_for_char(static_cast<unsigned char>(part[0]));
            if (!stroke) return std::nullopt;
            usage = stroke->usage;
        } else {
            return std::nullopt;
        }
        usages.push_back(usage);
    }
    if (usages.empty()) return std::nullopt;
    return usages;
}

nlohmann::json plan_keystrokes(const std::vector<KeyStroke>& strokes, double gap_ms) {
    auto events = nlohmann::json::array();
    for (std::size_t i = 0; i < strokes.size(); ++i) {
        const auto& stroke = strokes[i];
        if (i > 0 && gap_ms > 0) events.push_back({{"delay_ms", gap_ms}});
        if (stroke.shift) events.push_back({{"usage", kUsageLeftShift}, {"direction", "down"}});
        events.push_back({{"usage", stroke.usage}, {"direction", "down"}});
        events.push_back({{"usage", stroke.usage}, {"direction", "up"}});
        if (stroke.shift) events.push_back({{"usage", kUsageLeftShift}, {"direction", "up"}});
    }
    return events;
}

nlohmann::json plan_chord(const std::vector<std::uint32_t>& usages) {
    auto events = nlohmann::json::array();
    for (auto usage : usages) events.push_back({{"usage", usage}, {"direction", "down"}});
    for (auto it = usages.rbegin(); it != usages.rend(); ++it)
        events.push_back({{"usage", *it}, {"direction", "up"}});
    return events;
}

} // namespace acecode::ios_simulator
