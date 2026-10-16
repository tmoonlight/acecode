// Accessibility reads of the simulator's frontmost application through the
// host-side AccessibilityPlatformTranslation bridge.
#pragma once

#import <Foundation/Foundation.h>

#include <nlohmann/json.hpp>

#include <string>

namespace acecode::ios_simulator::helper {

struct UiQuery {
    std::string mode = "tree";  // "tree" or "point"
    double x = 0;
    double y = 0;
    int max_depth = 40;
    int max_nodes = 600;
};

// Frames are device points. Throws SimError when the bridge is unavailable.
nlohmann::json describe_ui(id device, const std::string& udid, const UiQuery& query);

} // namespace acecode::ios_simulator::helper
