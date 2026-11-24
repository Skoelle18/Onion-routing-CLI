#pragma once

#include <string>
#include <vector>

namespace onion::config {

struct RelayEntry {
    std::string addr;
    std::string public_key_path;
};

struct DestinationEntry {
    std::string addr;
    std::string public_key_path;
};

struct PathConfig {
    std::vector<RelayEntry> relays;
    DestinationEntry destination;
};

PathConfig load_path_config(const std::string& path);

}  // namespace onion::config
