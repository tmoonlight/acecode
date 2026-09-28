#include "hook_seeds.hpp"

namespace acecode {

const std::vector<DefaultHookSeed>& default_hook_seeds() {
    static const std::vector<DefaultHookSeed> seeds = {
        {"agent-reporting",
         "acecode:managed-hook/agent-reporting@2026-09-17.1",
         "agent-reporting",
         "90403a8d660fc4086bca9c7c979c8fb8e98321dcebc63aefd1e995addc2fab1e",
         {
             "daeb5ce4f3ff42d1717c9997b9627bc6daf00df9ec24643203253da2aae30644",
             "b731118b927bb32a5c43083f5d3279ecd4ce3d96137b351c2d105ac3548d9f2f",
             "e139bd9bc1314dfe82cd7a7c018332e7a33c42eb451c8ce1db99f58d18d6ff4e",
         }},
    };
    return seeds;
}

} // namespace acecode
