#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <thread>

namespace hp {

struct QuicConfig {
    std::string host;
    uint16_t port = 6666;
    std::string deviceId;
    std::string alpn = "HP_PRO";
    int keepaliveSec = 15;
};

enum class QState : int {
    IDLE = 0, CONNECTING = 1, REGISTERING = 2, ONLINE = 3, RETRYING = 4, FAILED = 5, CLOSED = 6
};

struct QuicEvent {
    std::function<void(QState, const std::string&)> onStatus;
    std::function<void(const std::string&)> onLog;
    std::function<void(const std::vector<uint8_t>&)> onServerMessage;
};

class QuicClient {
public:
    bool start(const QuicConfig& cfg, const QuicEvent& ev);
    void stop();
    bool running() const { return running_.load(); }

    void* conn_ = nullptr;
    void* osslCtx_ = nullptr;
    int sock_ = -1;
    std::thread worker_;

private:
    void runLoop();

    QuicConfig cfg_;
    QuicEvent ev_;
    std::atomic<bool> running_{false};
};

}
