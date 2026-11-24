#include "onion/cli/app.hpp"

#include "onion/onion/config.hpp"
#include "onion/crypto/keys.hpp"
#include "onion/error.hpp"
#include "onion/net/address.hpp"
#include "onion/net/framing.hpp"
#include "onion/net/socket.hpp"
#include "onion/onion/builder.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace onion::cli {
namespace {

namespace fs = std::filesystem;
namespace crypto = ::onion::crypto;
namespace net = ::onion::net;
namespace routing = ::onion::onion;
namespace config = ::onion::config;

using Clock = std::chrono::steady_clock;
constexpr auto kIoTimeout = std::chrono::milliseconds(5000);

struct Logger {
    bool verbose{false};

    void info(const std::string& message) const {
        std::cerr << "[onion] " << message << '\n';
    }

    void debug(const std::string& message) const {
        if (verbose) {
            std::cerr << "[onion:debug] " << message << '\n';
        }
    }
};

struct Options {
    std::map<std::string, std::string> values;

    bool has(const std::string& name) const { return values.count(name) != 0; }

    const std::string& at(const std::string& name) const {
        auto it = values.find(name);
        if (it == values.end()) {
            throw std::invalid_argument("missing required option " + name);
        }
        return it->second;
    }

    std::string optional(const std::string& name, const std::string& fallback) const {
        auto it = values.find(name);
        return it == values.end() ? fallback : it->second;
    }
};

Options parse_options(const std::vector<std::string>& args,
                      const std::set<std::string>& allowed) {
    Options options;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (allowed.count(arg) == 0) {
            throw std::invalid_argument("unknown option '" + arg + "'");
        }
        if (i + 1 >= args.size()) {
            throw std::invalid_argument("option " + arg + " requires a value");
        }
        if (!options.values.emplace(arg, args[++i]).second) {
            throw std::invalid_argument("duplicate option " + arg);
        }
    }
    return options;
}

std::uint16_t parse_port(const std::string& text) {
    const auto address = net::parse_address("127.0.0.1:" + text);
    return address.port;
}

std::string trim(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

std::string detect_advertise_host() {
#ifdef _WIN32
    std::FILE* pipe = ::_popen("tailscale.exe ip -4 2>NUL", "r");
#else
    std::FILE* pipe = ::popen("tailscale ip -4 2>/dev/null", "r");
#endif
    if (pipe != nullptr) {
        char buffer[128]{};
        const bool has_line = std::fgets(buffer, sizeof(buffer), pipe) != nullptr;
#ifdef _WIN32
        ::_pclose(pipe);
#else
        ::pclose(pipe);
#endif
        if (has_line) {
            std::string value = trim(buffer);
            if (!value.empty()) {
                return value;
            }
        }
    }
    return "127.0.0.1";
}

fs::path public_key_path_for(const fs::path& private_path) {
    std::string name = private_path.filename().string();
    const std::string suffix = ".pem";
    if (name.size() > suffix.size() &&
        name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
        name.erase(name.size() - suffix.size());
    }
    return private_path.parent_path() / (name + ".pub.pem");
}

std::string status_message(net::StatusCode code) {
    switch (code) {
        case net::StatusCode::Ok:
            return "delivered";
        case net::StatusCode::DecryptFailed:
            return "DECRYPT_FAILED: a hop could not decrypt its layer (wrong key or tampered onion)";
        case net::StatusCode::UnreachableHop:
            return "UNREACHABLE_HOP: a relay could not reach its successor";
        case net::StatusCode::ProtocolError:
            return "PROTOCOL_ERROR: malformed onion or frame";
        case net::StatusCode::Timeout:
            return "TIMEOUT: a hop did not respond in time";
    }
    return "UNKNOWN_STATUS";
}

void print_usage(std::ostream& out) {
    out << "onion 0.1.0 - educational onion routing tool\n"
        << "usage: onion <command> [options] [--verbose]\n\n"
        << "commands:\n"
        << "  keygen  --out <dir> [--bits N] [--name NAME]\n"
        << "  relay   --listen <port> --key <privkey> [--advertise-addr <ip>]\n"
        << "  listen  --port <port> --key <privkey>\n"
        << "  send    --path <relays.conf> --dest <ip:port> --message <text>\n"
        << "  check   --path <relays.conf>\n"
        << "  build   --path <relays.conf> --dest <ip:port> --message <text> --out <file>\n"
        << "  inject  --addr <ip:port> --file <onion.bin>\n";
}

net::ConnectOptions hop_connect_options() {
    net::ConnectOptions options;
    options.connect_timeout = std::chrono::milliseconds(1500);
    options.retries = 2;
    options.backoff = std::chrono::milliseconds(200);
    options.keepalive = true;
    return options;
}

routing::Path load_path_with_keys(const std::string& path) {
    auto parsed = config::load_path_config(path);
    routing::Path result;
    for (const auto& relay : parsed.relays) {
        result.relays.push_back(routing::Hop{relay.addr,
                                             crypto::load_public_key(relay.public_key_path)});
    }
    result.destination =
        routing::Hop{parsed.destination.addr,
                     crypto::load_public_key(parsed.destination.public_key_path)};
    return result;
}

void verify_destination(const routing::Path& path, const std::string& requested) {
    const auto requested_addr = net::format_address(net::parse_address(requested));
    if (requested_addr != path.destination.addr) {
        throw std::invalid_argument("destination " + requested_addr +
                                    " does not match the destination in relays.conf (" +
                                    path.destination.addr + ")");
    }
}

net::Frame process_relay_frame(const net::Frame& incoming, EVP_PKEY* private_key,
                               const net::Socket& client, const Logger& log) {
    if (incoming.type == net::FrameType::Ping) {
        return net::Frame{net::FrameType::Pong, {}};
    }
    if (incoming.type != net::FrameType::Onion) {
        return net::make_status(net::StatusCode::ProtocolError, "expected ONION frame");
    }

    auto payload = routing::unwrap_layer(incoming.body, private_key);
    if (payload.kind != routing::InnerKind::Forward) {
        return net::make_status(net::StatusCode::ProtocolError, "relay received DELIVER layer");
    }

    const auto successor = net::parse_address(payload.next_addr);
    log.info("predecessor " + net::peer_address(client) + " -> successor " +
             net::format_address(successor) + ", ciphertext " +
             std::to_string(incoming.body.size()) + " bytes");
    log.debug("layer kind " + routing::to_string(payload.kind));

    net::Socket upstream = net::connect_tcp(successor, hop_connect_options());
    net::write_frame(upstream, net::Frame{net::FrameType::Onion, payload.inner}, kIoTimeout);
    net::Frame reply = net::read_frame(upstream, kIoTimeout);
    if (reply.type != net::FrameType::Status) {
        return net::make_status(net::StatusCode::ProtocolError, "expected STATUS reply");
    }
    log.debug("successor replied " +
              net::status_name(net::parse_status(reply).first));
    return reply;
}

void handle_relay_connection(net::Socket client, EVP_PKEY* private_key, const Logger& log) {
    const std::string predecessor = net::peer_address(client);
    net::Frame response;
    try {
        net::Frame incoming = net::read_frame(client, kIoTimeout);
        response = process_relay_frame(incoming, private_key, client, log);
    } catch (const DecryptError&) {
        log.info("decryption failure from " + predecessor + "; dropping layer");
        response = net::make_status(net::StatusCode::DecryptFailed, "decryption failure");
    } catch (const TimeoutError& ex) {
        log.info(std::string("timeout: ") + ex.what());
        response = net::make_status(net::StatusCode::Timeout, "timeout");
    } catch (const NetworkError& ex) {
        log.info(std::string("unreachable hop: ") + ex.what());
        response = net::make_status(net::StatusCode::UnreachableHop, "unreachable hop");
    } catch (const ProtocolError& ex) {
        log.info("protocol error from " + predecessor + ": " + ex.what());
        response = net::make_status(net::StatusCode::ProtocolError, "protocol error");
    } catch (const std::exception& ex) {
        log.info("error from " + predecessor + ": " + ex.what());
        response = net::make_status(net::StatusCode::ProtocolError, "relay error");
    }

    try {
        net::write_frame(client, response, kIoTimeout);
    } catch (const std::exception& ex) {
        log.debug("could not reply to " + predecessor + ": " + ex.what());
    }
}

void handle_destination_connection(net::Socket client, EVP_PKEY* private_key,
                                   const Logger& log) {
    const std::string predecessor = net::peer_address(client);
    net::Frame response;
    try {
        net::Frame incoming = net::read_frame(client, kIoTimeout);
        if (incoming.type == net::FrameType::Ping) {
            response = net::Frame{net::FrameType::Pong, {}};
        } else if (incoming.type != net::FrameType::Onion) {
            response = net::make_status(net::StatusCode::ProtocolError, "expected ONION frame");
        } else {
            auto payload = routing::unwrap_layer(incoming.body, private_key);
            if (payload.kind != routing::InnerKind::Deliver) {
                response = net::make_status(net::StatusCode::ProtocolError,
                                            "destination received FORWARD layer");
            } else {
                std::string message(payload.inner.begin(), payload.inner.end());
                log.info("received " + std::to_string(message.size()) + " bytes from " +
                         predecessor);
                std::cout << message << '\n' << std::flush;
                response = net::make_status(net::StatusCode::Ok, "delivered");
            }
        }
    } catch (const DecryptError&) {
        log.info("decryption failure from " + predecessor + "; dropping layer");
        response = net::make_status(net::StatusCode::DecryptFailed, "decryption failure");
    } catch (const TimeoutError& ex) {
        log.info(std::string("timeout: ") + ex.what());
        response = net::make_status(net::StatusCode::Timeout, "timeout");
    } catch (const NetworkError& ex) {
        log.info(std::string("network error: ") + ex.what());
        response = net::make_status(net::StatusCode::UnreachableHop, "unreachable hop");
    } catch (const ProtocolError& ex) {
        log.info("protocol error from " + predecessor + ": " + ex.what());
        response = net::make_status(net::StatusCode::ProtocolError, "protocol error");
    } catch (const std::exception& ex) {
        log.info("error from " + predecessor + ": " + ex.what());
        response = net::make_status(net::StatusCode::ProtocolError, "receiver error");
    }

    try {
        net::write_frame(client, response, kIoTimeout);
    } catch (const std::exception& ex) {
        log.debug("could not reply to " + predecessor + ": " + ex.what());
    }
}

int cmd_keygen(const Options& options, const Logger& log) {
    const fs::path out = options.at("--out");
    const std::string name = options.optional("--name", "key");
    if (name.empty() || name != fs::path(name).filename().string()) {
        throw std::invalid_argument("--name must be a simple filename");
    }

    int bits = 2048;
    const std::string bits_text = options.optional("--bits", "2048");
    try {
        bits = std::stoi(bits_text);
    } catch (const std::exception&) {
        throw std::invalid_argument("--bits must be an integer");
    }

    fs::create_directories(out);
    auto key = crypto::generate_rsa(bits);
    const fs::path private_path = out / (name + ".pem");
    const fs::path public_path = out / (name + ".pub.pem");
    crypto::write_private_key(key, private_path.string());
    crypto::write_public_key(key, public_path.string());

    log.info("generated " + std::to_string(bits) + "-bit RSA keypair");
    std::cout << "private: " << private_path.string() << '\n'
              << "public:  " << public_path.string() << '\n';
    return 0;
}

int cmd_relay(const Options& options, const Logger& log) {
    const auto port = parse_port(options.at("--listen"));
    const fs::path key_path = options.at("--key");
    auto private_key = crypto::load_private_key(key_path.string());

    std::string advertise_host =
        options.has("--advertise-addr") ? options.at("--advertise-addr")
                                        : detect_advertise_host();
    net::Address advertise;
    if (advertise_host.find(':') != std::string::npos && advertise_host.front() != '[') {
        advertise = net::parse_address(advertise_host);
    } else {
        if (advertise_host.find(':') != std::string::npos) {
            advertise_host = advertise_host.substr(1, advertise_host.size() - 2);
        }
        advertise = net::Address{advertise_host, port};
    }
    const std::string advertise_addr = net::format_address(advertise);

    const fs::path marker = key_path.parent_path() / "advertise.addr";
    std::ofstream(marker) << advertise_addr << '\n';

    auto server = net::listen_tcp(port);
    log.info("relay listening on 0.0.0.0:" + std::to_string(port) +
             ", advertise " + advertise_addr);
    log.info("config stanza: relay " + advertise_addr + " " +
             public_key_path_for(key_path).string());

    while (true) {
        auto client = net::accept_connection(server);
        handle_relay_connection(std::move(client), private_key.get(), log);
    }
}

int cmd_listen(const Options& options, const Logger& log) {
    const auto port = parse_port(options.at("--port"));
    const fs::path key_path = options.at("--key");
    auto private_key = crypto::load_private_key(key_path.string());

    auto server = net::listen_tcp(port);
    log.info("receiver listening on 0.0.0.0:" + std::to_string(port));
    while (true) {
        auto client = net::accept_connection(server);
        handle_destination_connection(std::move(client), private_key.get(), log);
    }
}

net::StatusCode report_status(const net::Frame& reply, const Logger& log) {
    if (reply.type != net::FrameType::Status) {
        throw ProtocolError("expected STATUS reply");
    }
    auto [code, text] = net::parse_status(reply);
    log.debug("status " + net::status_name(code));
    if (code != net::StatusCode::Ok) {
        if (!text.empty()) {
            throw std::runtime_error(status_message(code) + " (" + text + ")");
        }
        throw std::runtime_error(status_message(code));
    }
    return code;
}

int cmd_send(const Options& options, const Logger& log) {
    const std::string path = options.at("--path");
    const std::string destination = options.at("--dest");
    const std::string message = options.at("--message");

    auto route = load_path_with_keys(path);
    verify_destination(route, destination);

    auto onion = routing::build_onion(route, message);
    log.info("built " + std::to_string(onion.size()) + "-byte onion for " +
             std::to_string(route.relays.size()) + " relays");
    log.debug("outer layer " + route.relays.front().addr);

    auto first_hop = net::parse_address(route.relays.front().addr);
    auto socket = net::connect_tcp(first_hop, hop_connect_options());
    net::write_frame(socket, net::Frame{net::FrameType::Onion, std::move(onion)}, kIoTimeout);
    report_status(net::read_frame(socket, kIoTimeout), log);

    std::cout << "delivered via " << route.relays.size() << " relay(s)" << '\n';
    return 0;
}

int cmd_check(const Options& options, const Logger& log) {
    auto parsed = config::load_path_config(options.at("--path"));
    std::vector<std::string> addresses;
    for (const auto& relay : parsed.relays) {
        addresses.push_back(relay.addr);
    }
    addresses.push_back(parsed.destination.addr);

    net::ConnectOptions options_for_check;
    options_for_check.connect_timeout = std::chrono::milliseconds(1000);
    options_for_check.retries = 0;

    std::size_t failures = 0;
    for (const auto& address_text : addresses) {
        const auto address = net::parse_address(address_text);
        const auto start = Clock::now();
        try {
            auto socket = net::connect_tcp(address, options_for_check);
            net::write_frame(socket, net::Frame{net::FrameType::Ping, {}}, kIoTimeout);
            auto reply = net::read_frame(socket, kIoTimeout);
            if (reply.type != net::FrameType::Pong) {
                throw ProtocolError("expected PONG reply");
            }
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                Clock::now() - start);
            std::cout << "[reachable]   " << address_text << "  RTT=" << elapsed.count()
                      << "ms\n";
        } catch (const std::exception& ex) {
            ++failures;
            std::cout << "[unreachable] " << address_text << "  " << ex.what() << '\n';
        }
    }
    log.debug("checked " + std::to_string(addresses.size()) + " hops");
    return failures == 0 ? 0 : 1;
}

int cmd_build(const Options& options, const Logger& log) {
    auto route = load_path_with_keys(options.at("--path"));
    verify_destination(route, options.at("--dest"));
    auto onion = routing::build_onion(route, options.at("--message"));

    const fs::path out = options.at("--out");
    std::ofstream file(out, std::ios::binary);
    if (!file) {
        throw std::runtime_error("cannot write onion file '" + out.string() + "'");
    }
    file.write(reinterpret_cast<const char*>(onion.data()),
               static_cast<std::streamsize>(onion.size()));
    log.info("wrote " + std::to_string(onion.size()) + "-byte onion to " + out.string());
    std::cout << out.string() << '\n';
    return 0;
}

int cmd_inject(const Options& options, const Logger& log) {
    const fs::path file_path = options.at("--file");
    std::ifstream file(file_path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("cannot read onion file '" + file_path.string() + "'");
    }
    std::vector<std::uint8_t> onion((std::istreambuf_iterator<char>(file)),
                                    std::istreambuf_iterator<char>());
    if (onion.empty()) {
        throw ProtocolError("onion file is empty");
    }

    auto address = net::parse_address(options.at("--addr"));
    auto socket = net::connect_tcp(address, hop_connect_options());
    net::write_frame(socket, net::Frame{net::FrameType::Onion, std::move(onion)}, kIoTimeout);
    report_status(net::read_frame(socket, kIoTimeout), log);
    std::cout << "delivered" << '\n';
    return 0;
}

}  // namespace

int run(const std::vector<std::string>& args) {
    try {
        bool verbose = false;
        std::vector<std::string> filtered;
        filtered.reserve(args.size());
        for (const auto& arg : args) {
            if (arg == "--verbose") {
                verbose = true;
            } else {
                filtered.push_back(arg);
            }
        }
        const Logger log{verbose};

        if (filtered.size() < 2 || filtered[1] == "--help" || filtered[1] == "-h") {
            print_usage(std::cout);
            return filtered.size() < 2 ? 2 : 0;
        }

        const std::string& command = filtered[1];
        std::vector<std::string> rest(filtered.begin() + 2, filtered.end());

        if (command == "keygen") {
            return cmd_keygen(parse_options(rest, {"--out", "--bits", "--name"}), log);
        }
        if (command == "relay") {
            return cmd_relay(parse_options(rest, {"--listen", "--key", "--advertise-addr"}), log);
        }
        if (command == "listen") {
            return cmd_listen(parse_options(rest, {"--port", "--key"}), log);
        }
        if (command == "send") {
            return cmd_send(parse_options(rest, {"--path", "--dest", "--message"}), log);
        }
        if (command == "check") {
            return cmd_check(parse_options(rest, {"--path"}), log);
        }
        if (command == "build") {
            return cmd_build(parse_options(rest, {"--path", "--dest", "--message", "--out"}), log);
        }
        if (command == "inject") {
            return cmd_inject(parse_options(rest, {"--addr", "--file"}), log);
        }

        throw std::invalid_argument("unknown command '" + command + "'");
    } catch (const TimeoutError& ex) {
        std::cerr << "onion: TIMEOUT: " << ex.what() << '\n';
        return 1;
    } catch (const NetworkError& ex) {
        std::cerr << "onion: UNREACHABLE_HOP: " << ex.what() << '\n';
        return 1;
    } catch (const DecryptError& ex) {
        std::cerr << "onion: DECRYPT_FAILED: " << ex.what() << '\n';
        return 1;
    } catch (const ProtocolError& ex) {
        std::cerr << "onion: PROTOCOL_ERROR: " << ex.what() << '\n';
        return 1;
    } catch (const std::exception& ex) {
        std::cerr << "onion: " << ex.what() << '\n';
        return 1;
    }
}

}  // namespace onion::cli
