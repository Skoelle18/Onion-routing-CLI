#include "onion/onion/config.hpp"

#include "onion/error.hpp"
#include "onion/net/address.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace onion::config {
namespace {

namespace fs = std::filesystem;

std::string trim(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

std::vector<std::string> tokenize(const std::string& line) {
    std::vector<std::string> tokens;
    std::string current;
    char quote = '\0';
    bool in_token = false;

    for (unsigned char raw : line) {
        const char ch = static_cast<char>(raw);
        if (quote != '\0') {
            if (ch == quote) {
                quote = '\0';
            } else {
                current.push_back(ch);
            }
            continue;
        }
        if (ch == '"' || ch == '\'') {
            quote = ch;
            in_token = true;
            continue;
        }
        if (std::isspace(raw)) {
            if (in_token) {
                tokens.push_back(current);
                current.clear();
                in_token = false;
            }
            continue;
        }
        current.push_back(ch);
        in_token = true;
    }

    if (quote != '\0') {
        throw ProtocolError("unterminated quoted value in config");
    }
    if (in_token) {
        tokens.push_back(current);
    }
    return tokens;
}

fs::path resolve_key_path(const fs::path& config_dir, const std::string& value) {
    fs::path path(value);
    if (path.is_absolute()) {
        return path;
    }
    return (config_dir / path).lexically_normal();
}

std::string canonical_address(const std::string& value, const std::string& context) {
    try {
        return net::format_address(net::parse_address(value));
    } catch (const ProtocolError& ex) {
        throw ProtocolError(context + ": " + ex.what());
    }
}

}  // namespace

PathConfig load_path_config(const std::string& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("cannot open path config '" + path + "'");
    }

    const fs::path config_dir = fs::path(path).parent_path().empty()
                                    ? fs::path(".")
                                    : fs::path(path).parent_path();

    PathConfig result;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        const auto comment = line.find('#');
        if (comment != std::string::npos) {
            line.erase(comment);
        }
        line = trim(line);
        if (line.empty()) {
            continue;
        }

        const auto tokens = tokenize(line);
        const std::string context = path + ":" + std::to_string(line_number);
        if (tokens.size() != 3) {
            throw ProtocolError(context + ": expected '<relay|destination> addr keyfile'");
        }

        const std::string key_path = resolve_key_path(config_dir, tokens[2]).string();
        if (tokens[0] == "relay") {
            result.relays.push_back(RelayEntry{canonical_address(tokens[1], context), key_path});
        } else if (tokens[0] == "destination") {
            if (!result.destination.addr.empty()) {
                throw ProtocolError(context + ": duplicate destination entry");
            }
            result.destination =
                DestinationEntry{canonical_address(tokens[1], context), key_path};
        } else {
            throw ProtocolError(context + ": unknown directive '" + tokens[0] + "'");
        }
    }

    if (result.relays.empty()) {
        throw ProtocolError("path config has no relay entries: '" + path + "'");
    }
    if (result.destination.addr.empty()) {
        throw ProtocolError("path config has no destination entry: '" + path + "'");
    }
    return result;
}

}  // namespace onion::config
