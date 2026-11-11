#pragma once

#include "../../core/json_mini.hpp"

#include <string>

namespace dirlocker {

struct DaemonReply {
    bool transport_ok = false;
    bool ok = false;
    std::string message;
    JsonValue data;
};

class DaemonClient {
public:
    static constexpr const char* kDefaultSocketPath = "/run/dirlocker/dirlockerd.sock";

    static DaemonReply send(const std::string& action,
                            JsonValue params = JsonValue::object(),
                            const char* socket_path = kDefaultSocketPath,
                            int timeout_ms = 4000);
};

}  // namespace dirlocker
