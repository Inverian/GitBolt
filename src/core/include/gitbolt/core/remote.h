#pragma once

#include <string>
#include <vector>

namespace gitbolt::core {

struct RemoteInfo {
    std::string name;
    std::string url;
    std::string pushUrl;
    std::vector<std::string> fetchRefspecs;
};

} // namespace gitbolt::core
