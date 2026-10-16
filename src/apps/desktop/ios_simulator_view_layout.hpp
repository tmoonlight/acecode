#pragma once

// Pure parts of the Desktop shell's native iOS Simulator view: the bridge
// layout contract and the layer geometry. The macOS host
// (ios_simulator_view_mac.mm) shows the simulator's IOSurface directly in a
// layer above the main web view; the web panel keeps handling input.

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace acecode::desktop {

inline constexpr std::size_t kIosSimulatorViewMaxOcclusionRects = 64;

struct IosSimulatorViewRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

struct IosSimulatorViewLayout {
    std::string udid;
    std::string developer_dir;  // selected Xcode's Contents/Developer
    // Screen area in main web view CSS pixels, top-left origin.
    IosSimulatorViewRect frame;
    bool visible = false;
    double corner_radius = 0;  // the device's rounded screen corners, CSS px
    std::uint64_t layout_revision = 0;
    // Web content drawn above the screen (menus, dialogs), relative to frame.
    std::vector<IosSimulatorViewRect> occlusion_rects;
};

// 8-4-4-4-12 hexadecimal, the CoreSimulator device UDID shape.
bool is_simulator_udid(const std::string& value);

// Parses the aceDesktop_iosSimulatorViewSetLayout argument object.
std::optional<IosSimulatorViewLayout> parse_ios_simulator_view_layout(const nlohmann::json& value,
                                                                      std::string* error);

// Occlusion rectangles clipped to the frame; empty ones dropped.
std::vector<IosSimulatorViewRect> clipped_occlusion_rects(const IosSimulatorViewLayout& layout);

// Placement of the framebuffer layer inside the frame. The framebuffer is in
// the device's native (portrait) orientation; the layer keeps those bounds
// and is turned clockwise by `rotation` around its center, aspect-fitted.
struct IosSimulatorLayerGeometry {
    double center_x = 0;
    double center_y = 0;
    double width = 0;   // unrotated bounds
    double height = 0;
    int rotation = 0;   // 0, 90, 180 or 270, clockwise
};

IosSimulatorLayerGeometry fit_simulator_layer(double frame_width, double frame_height, double surface_width,
                                              double surface_height, int rotation);

// Core Animation z-rotation for a clockwise turn: a positive angle turns
// clockwise on screen only in a flipped (top-left origin) geometry, which a
// flipped AppKit view's backing layer uses.
double simulator_layer_angle_radians(int rotation, bool flipped_geometry);

} // namespace acecode::desktop
