#include "hp_client.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <cstring>

namespace hp {

static const char* kStateNames[] = {
    "IDLE", "CONNECTING", "REGISTERING", "ONLINE", "RETRYING", "FAILED", "CLOSED"
};

HpClient::HpClient() = default;

HpClient::~HpClient() { stop(); }

void HpClient::notify(State s, const std::string& msg) {
    state_.store(s);
    std::lock_guard<std::mutex> lk(cbMutex_);
    if (onStatus_) onStatus_(s, msg);
}

void HpClient::log(const std::string& line) {
    std::lock_guard<std::mutex> lk(cbMutex_);
    if (onLog_) onLog_(line);
}

bool HpClient::start(const ClientConfig& cfg, StatusFn onStatus, LogFn onLog, MessageFn onCmd) {
    if (running_.load()) return false;
    cfg_ = cfg;
    {
        std::lock_guard<std::mutex> lk(cbMutex_);
        onStatus_ = std::move(onStatus);
        onLog_ = std::move(onLog);
        onCmd_ = std::move(onCmd);
    }
    running_.store(true);
    worker_ = std::thread(&HpClient::runLoop, this);
    return true;
}

void HpClient::stop() {
    if (!running_.exchange(false)) return;
    if (sock_ >= 0) { ::shutdown(sock_, SHUT_RDWR); }
    if (worker_.joinable()) worker_.join();
    notify(State::CLOSED, "stopped");
}

bool HpClient::connectTcp(int& fd) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    std::string port = std::to_string(cfg_.serverPort);
    if (getaddrinfo(cfg_.serverHost.c_str(), port.c_str(), &hints, &res) != 0 || !res) {
        log("[net] DNS 解析失败: " + cfg_.serverHost);
        return false;
    }
    bool ok = false;
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) { ok = true; break; }
        ::close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (!ok) return false;

    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    
    timeval tv{5, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return true;
}

bool HpClient::sendAll(int fd, const std::vector<uint8_t>& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && (errno == EINTR)) continue;
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool HpClient::sendCmd(const CmdMessage& msg) {
    std::lock_guard<std::mutex> lk(sendMutex_);
    if (sock_ < 0) return false;
    std::vector<uint8_t> frame;
    frameMessage4B(frame, msg.encode());
    return sendAll(sock_, frame);
}

void HpClient::runLoop() {
    int backoff = 2;
    uint64_t seq = 0;
    while (running_.load()) {
        notify(State::CONNECTING, "连接 " + cfg_.serverHost + ":" + std::to_string(cfg_.serverPort));
        if (!connectTcp(sock_)) {
            notify(State::RETRYING, "连接失败，" + std::to_string(backoff) + "s 后重试");
            std::this_thread::sleep_for(std::chrono::seconds(backoff));
            backoff = std::min(backoff * 2, cfg_.reconnectMaxSec);
            continue;
        }
        log("[net] TCP 已连接 fd=" + std::to_string(sock_));
        backoff = 2;

        CmdMessage hello;
        hello.type = CmdType::CONNECT;
        hello.key = cfg_.deviceId;
        hello.version = cfg_.version;
        if (!sendCmd(hello)) { ::close(sock_); sock_ = -1; continue; }
        notify(State::REGISTERING, "已发送注册 key=" + cfg_.deviceId);
        log("[proto] -> CmdMessage{CONNECT, key=" + cfg_.deviceId + ", version=" + cfg_.version + "}");

        std::vector<uint8_t> buf;
        uint8_t rbuf[4096];
        auto lastKeepalive = std::chrono::steady_clock::now();
        bool alive = true;
        while (alive && running_.load()) {
            ssize_t n = ::recv(sock_, rbuf, sizeof(rbuf), 0);
            if (n > 0) {
                
                std::string hex;
                char tmp[4];
                size_t show = std::min<size_t>(static_cast<size_t>(n), 64);
                for (size_t i = 0; i < show; ++i) {
                    snprintf(tmp, sizeof(tmp), "%02x", rbuf[i]);
                    hex += tmp;
                    if ((i & 15) == 15) hex += ' ';
                }
                log("[recv] n=" + std::to_string(n) + " " + hex);
                buf.insert(buf.end(), rbuf, rbuf + n);
                std::vector<uint8_t> frame;
                bool got = true;
                while (got) {
                    if (tryExtractFrame8B(buf, frame) || tryExtractFrame4B(buf, frame)) {
                        got = true;
                    } else if (tryExtractFrame(buf, frame) && !frame.empty()) {
                        got = true;
                    } else {
                        got = false;
                    }
                    if (!got) break;
                    CmdMessage msg;
                    if (msg.decode(frame.data(), frame.size())) {
                        seq++;
                        std::string dataStr(msg.data.begin(), msg.data.end());
                        log("[proto] <- CmdMessage{type=" + std::to_string(static_cast<int>(msg.type)) +
                            " key=" + msg.key + " data=" + dataStr + " #" + std::to_string(seq) + "}");
                        if (msg.type == CmdType::TIPS && state_.load() != State::ONLINE) {
                            notify(State::ONLINE, "cmd 通道心跳正常");
                        }
                        std::lock_guard<std::mutex> lk(cbMutex_);
                        if (onCmd_) onCmd_(msg);
                    } else {
                        std::string hex;
                        char tmp[4];
                        for (size_t i = 0; i < frame.size(); ++i) {
                            snprintf(tmp, sizeof(tmp), "%02x", frame[i]);
                            hex += tmp;
                            if ((i & 15) == 15) hex += ' ';
                        }
                        log("[proto] 解码失败 帧长 " + std::to_string(frame.size()) + " hex: " + hex);
                    }
                }
            } else if (n == 0) {
                log("[net] 对端关闭");
                alive = false;
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    
                    auto now = std::chrono::steady_clock::now();
                    if (now - lastKeepalive >= std::chrono::seconds(cfg_.keepaliveSec)) {
                        CmdMessage keep;
                        keep.type = CmdType::TIPS;   
                        keep.key = cfg_.deviceId;
                        if (sendCmd(keep)) {
                            log("[keep] keepalive");
                            lastKeepalive = now;
                        } else {
                            alive = false;
                        }
                    }
                } else if (errno == EINTR) {
                    
                    continue;
                } else {
                    log(std::string("[net] recv 错误: ") + strerror(errno));
                    alive = false;
                }
            }
        }

        ::close(sock_);
        sock_ = -1;
        if (running_.load()) {
            notify(State::RETRYING, "连接断开，" + std::to_string(backoff) + "s 后重连");
            std::this_thread::sleep_for(std::chrono::seconds(backoff));
            backoff = std::min(backoff * 2, cfg_.reconnectMaxSec);
        }
    }
}

} 
