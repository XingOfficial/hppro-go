#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "hp_protocol.h"

namespace hp {

struct ClientConfig {
    std::string serverHost;   
    uint16_t serverPort = 6666;
    std::string deviceId;     
    std::string version = "5.0";
    int keepaliveSec = 15;    
    int reconnectMaxSec = 60; 
};

enum class State : int {
    IDLE = 0,
    CONNECTING = 1,
    REGISTERING = 2,
    ONLINE = 3,
    RETRYING = 4,
    FAILED = 5,
    CLOSED = 6,
};

class HpClient {
public:
    using StatusFn  = std::function<void(State, const std::string& msg)>;
    using LogFn     = std::function<void(const std::string& line)>;
    using MessageFn = std::function<void(const CmdMessage&)>;

    HpClient();
    ~HpClient();

    bool start(const ClientConfig& cfg, StatusFn onStatus, LogFn onLog, MessageFn onCmd);
    void stop();

    bool running() const { return running_.load(); }
    State state() const { return state_.load(); }

    bool sendCmd(const CmdMessage& msg);

private:
    void runLoop();
    bool connectTcp(int& fd);
    void notify(State s, const std::string& msg);
    void log(const std::string& line);
    bool sendAll(int fd, const std::vector<uint8_t>& data);

    ClientConfig cfg_;
    std::atomic<bool> running_{false};
    std::atomic<State> state_{State::IDLE};
    std::thread worker_;

    int sock_ = -1;
    std::mutex sendMutex_;

    StatusFn onStatus_;
    LogFn onLog_;
    MessageFn onCmd_;
    std::mutex cbMutex_;
};

} 
