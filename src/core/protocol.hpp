#pragma once

#include "json_mini.hpp"

#include <string>

namespace dirlocker {

// Local IPC protocol between daemon/tray/CLI/Peony plugin (line-delimited JSON).
inline std::string make_request(const std::string& action, JsonValue params = JsonValue::object()) {
    params.set("action", action);
    return params.dump(-1) + "\n";
}

inline std::string make_response(bool ok, const std::string& message = "",
                                 JsonValue data = JsonValue::object()) {
    data.set("ok", ok);
    if (!message.empty()) data.set("message", message);
    return data.dump(-1) + "\n";
}

}  // namespace dirlocker

