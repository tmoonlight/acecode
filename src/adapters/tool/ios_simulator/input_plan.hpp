#pragma once

// Pure translation of tool-level gestures and text into the helper's
// low-level event lists. Coordinates are device points in the current
// interface orientation (what screenshots show); the helper maps them to the
// panel's native orientation.

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace acecode::ios_simulator {

struct ScreenSize {
    double width = 0;   // points, interface orientation
    double height = 0;
};

struct PathPoint {
    double x = 0;
    double y = 0;
    double dt_ms = 0;  // delay before this sample
};

struct TwoFingerPoint {
    double x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    double dt_ms = 0;
};

// Edge name ("none", "top", "left", "bottom", "right") for a gesture that
// starts on-screen within 4 points of an edge. Such gestures are delivered as
// OS edge gestures (back, Notification Center, home, Control Center).
std::string edge_for_start(double x, double y, const ScreenSize& screen);

// Throws std::invalid_argument for points outside the screen.
void require_on_screen(double x, double y, const ScreenSize& screen);

// {"type":"touch"|"touch2"|"delay", ...} sequences for the helper.
nlohmann::json plan_tap(double x, double y, double duration_s, const ScreenSize& screen);
nlohmann::json plan_swipe(double x1, double y1, double x2, double y2, double duration_s, const ScreenSize& screen);
nlohmann::json plan_touch_path(const std::vector<PathPoint>& points, const ScreenSize& screen);
nlohmann::json plan_touch2_path(const std::vector<TwoFingerPoint>& points, const ScreenSize& screen);

// Total duration of a planned sequence in milliseconds.
double planned_duration_ms(const nlohmann::json& events);

// --- Keyboard ---------------------------------------------------------------

struct KeyStroke {
    std::uint32_t usage = 0;
    bool shift = false;
};

// US-layout HID usage for one character; nullopt when it cannot be typed as
// a key press (non-ASCII text goes through the pasteboard instead).
std::optional<KeyStroke> key_for_char(char32_t character);
// Whole text as keystrokes; nullopt if any character is unmappable.
std::optional<std::vector<KeyStroke>> keys_for_text(const std::string& utf8);
// Named key or chord ("return", "escape", "cmd+a", "shift+tab", ...).
// Returns nullopt for unknown names.
std::optional<std::vector<std::uint32_t>> chord_for_name(const std::string& name);

// Helper "keys" event list: strokes with shift wrapping and a short gap.
nlohmann::json plan_keystrokes(const std::vector<KeyStroke>& strokes, double gap_ms = 8);
// Presses every usage in order, then releases them in reverse.
nlohmann::json plan_chord(const std::vector<std::uint32_t>& usages);

inline constexpr std::uint32_t kUsageLeftCommand = 0xE3;
inline constexpr std::uint32_t kUsageLeftShift = 0xE1;
inline constexpr std::uint32_t kUsageV = 0x19;

} // namespace acecode::ios_simulator
