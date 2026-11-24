#pragma once

#include <stdexcept>
#include <string>

namespace onion {

class ProtocolError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class DecryptError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class NetworkError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class TimeoutError : public NetworkError {
public:
    using NetworkError::NetworkError;
};

}  // namespace onion
