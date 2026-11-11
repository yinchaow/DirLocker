#include "daemon_client.hpp"

#include "../../core/protocol.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace dirlocker {

DaemonReply DaemonClient::send(const std::string& action, JsonValue params,
                               const char* socket_path, int timeout_ms) {
    DaemonReply reply;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        reply.message = std::string("创建通信套接字失败: ") + std::strerror(errno);
        return reply;
    }

    if (timeout_ms < 50) timeout_ms = 50;
    timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path);

    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        reply.message = "无法连接后台服务 dirlockerd（请确认已执行 sudo systemctl start dirlockerd）: " +
                        std::string(std::strerror(errno));
        close(fd);
        return reply;
    }

    std::string req = make_request(action, std::move(params));
    size_t sent = 0;
    while (sent < req.size()) {
        ssize_t n = write(fd, req.data() + sent, req.size() - sent);
        if (n < 0) {
            if (errno == EINTR) continue;
            reply.message = std::string("发送请求失败: ") + std::strerror(errno);
            close(fd);
            return reply;
        }
        sent += static_cast<size_t>(n);
    }

    std::string resp;
    resp.reserve(4096);
    char ch = 0;
    while (resp.size() < 4 * 1024 * 1024) {
        ssize_t n = read(fd, &ch, 1);
        if (n < 0) {
            if (errno == EINTR) continue;
            reply.message = std::string("接收响应失败: ") + std::strerror(errno);
            close(fd);
            return reply;
        }
        if (n == 0 || ch == '\n') break;
        resp.push_back(ch);
    }
    close(fd);

    JsonValue parsed;
    std::string perr;
    if (!JsonValue::parse(resp, parsed, &perr) || !parsed.is_object()) {
        reply.message = "解析后台服务响应失败: " + perr;
        return reply;
    }

    reply.transport_ok = true;
    reply.ok = parsed.get("ok").as_bool(false);
    reply.message = parsed.get("message").as_string();
    reply.data = std::move(parsed);
    return reply;
}

}  // namespace dirlocker
