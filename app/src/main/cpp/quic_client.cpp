#include "quic_client.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/ssl.h>
#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_ossl.h>

#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

#include "hp_protocol.h"

namespace hp {

namespace {

uint64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

constexpr uint32_t kMagic = 0x00001a0a;
static void fillRandom(uint8_t *p, size_t n) {
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        if (fread(p, 1, n, f) != n) {}
        fclose(f);
    } else {
        for (size_t i = 0; i < n; ++i) p[i] = (uint8_t)(rand() & 0xff);
    }
}

static int onGetNewConnectionId(ngtcp2_conn *, ngtcp2_cid *cid, uint8_t *token,
                                size_t cidlen, void *) {
    fillRandom(cid->data, cidlen);
    cid->datalen = cidlen;
    fillRandom(token, NGTCP2_STATELESS_RESET_TOKENLEN);
    return 0;
}

static void onRand(uint8_t *dest, size_t destlen, const ngtcp2_rand_ctx *) {
    fillRandom(dest, destlen);
}

constexpr size_t kMaxUdp = 65536;

struct ClientState {
    ngtcp2_conn *conn = nullptr;
    SSL_CTX *sslCtx = nullptr;
    SSL *ssl = nullptr;
    ngtcp2_crypto_ossl_ctx *osslCtx = nullptr;
    ngtcp2_crypto_conn_ref connRef{};
    int sock = -1;
    sockaddr_storage remote{};
    socklen_t remoteLen = 0;
    sockaddr_storage local{};
    socklen_t localLen = 0;
    std::string host;
    std::string deviceId;
    std::string alpn;
    int keepaliveSec = 15;
    int64_t regStream = -1;
    std::atomic<bool> running{true};

    std::mutex outMutex;
    std::vector<uint8_t> pendingOut;

    QuicEvent ev;

    void log(const std::string &s) { if (ev.onLog) ev.onLog(s); }
    void status(QState s, const std::string &msg) { if (ev.onStatus) ev.onStatus(s, msg); }
};

ngtcp2_conn *getConnCb(ngtcp2_crypto_conn_ref *ref) {
    return static_cast<ClientState *>(ref->user_data)->conn;
}

int onRecvStreamData(ngtcp2_conn *conn, uint32_t flags, int64_t stream_id,
                     uint64_t offset, const uint8_t *data, size_t datalen,
                     void *user_data, void *stream_user_data) {
    auto st = static_cast<ClientState *>(user_data);
    if (datalen > 0 && st->ev.onServerMessage) {
        std::vector<uint8_t> copy(data, data + datalen);
        st->ev.onServerMessage(copy);
    }
    return 0;
}

int onStreamClose(ngtcp2_conn *conn, uint32_t flags, int64_t stream_id,
                  uint64_t app_error_code, void *user_data,
                  void *stream_user_data) {
    auto st = static_cast<ClientState *>(user_data);
    st->log("[quic] 流关闭 id=" + std::to_string(stream_id) +
            " err=" + std::to_string(app_error_code));
    return 0;
}

int onHandshakeCompleted(ngtcp2_conn *conn, void *user_data) {
    static_cast<ClientState *>(user_data)->log("[quic] TLS 握手完成");
    return 0;
}

int onHandshakeConfirmed(ngtcp2_conn *conn, void *user_data) {
    auto st = static_cast<ClientState *>(user_data);
    st->status(QState::REGISTERING, "QUIC 通道建立");
    int64_t sid;
    if (ngtcp2_conn_open_bidi_stream(conn, &sid, nullptr) == 0) {
        st->regStream = sid;
        HpMessage reg;
        reg.type = HpType::REGISTER;
        reg.metaData.key = st->deviceId;
        auto payload = reg.encode();
        std::vector<uint8_t> frame;
        uint32_t len = static_cast<uint32_t>(payload.size());
        frame.push_back(static_cast<uint8_t>(kMagic >> 24));
        frame.push_back(static_cast<uint8_t>(kMagic >> 16));
        frame.push_back(static_cast<uint8_t>(kMagic >> 8));
        frame.push_back(static_cast<uint8_t>(kMagic));
        frame.push_back(static_cast<uint8_t>(len >> 24));
        frame.push_back(static_cast<uint8_t>(len >> 16));
        frame.push_back(static_cast<uint8_t>(len >> 8));
        frame.push_back(static_cast<uint8_t>(len));
        frame.insert(frame.end(), payload.begin(), payload.end());
        std::lock_guard<std::mutex> lk(st->outMutex);
        if (st->pendingOut.size() < 1024 * 1024)
            st->pendingOut.insert(st->pendingOut.end(), frame.begin(), frame.end());
        st->log("[quic] -> HpMessage{REGISTER, key=" + st->deviceId + "} sid=" + std::to_string(sid));
    }
    return 0;
}

int onRecvVersionNegotiation(ngtcp2_conn *conn, const ngtcp2_pkt_hd *hd,
                             const uint32_t *sv, size_t nsv, void *user_data) {
    auto st = static_cast<ClientState *>(user_data);
    st->log("[quic] 版本协商");
    return ngtcp2_crypto_version_negotiation_cb(conn, hd->version, &hd->dcid, user_data);
}

}  // namespace

static bool runSession(ClientState *st, const QuicConfig &cfg, uint64_t &regTs) {
    uint8_t buf[kMaxUdp];
    while (st->running && st->conn) {
        uint64_t now = nowNs();

        if (ngtcp2_conn_handle_expiry(st->conn, now) != 0) {
            st->log("[quic] 连接超时/到期");
            return false;
        }

        ngtcp2_path path;
        ngtcp2_pkt_info pi{0};
        int64_t sid = -1;
        uint32_t wflags = 0;
        ngtcp2_ssize dataLen = 0;
        ngtcp2_vec datav{nullptr, 0};

        bool wantSend = false;
        {
            std::lock_guard<std::mutex> lk(st->outMutex);
            if (!st->pendingOut.empty() && st->regStream >= 0 &&
                ngtcp2_conn_get_max_data_left(st->conn) > 0) {
                wantSend = true;
                datav.base = st->pendingOut.data();
                datav.len = st->pendingOut.size();
            }
        }

        ngtcp2_ssize n = ngtcp2_conn_writev_stream(
            st->conn, &path, &pi, buf, sizeof(buf), &dataLen,
            NGTCP2_WRITE_STREAM_FLAG_NONE, sid,
            wantSend ? &datav : nullptr, wantSend ? 1 : 0, now);
        if (n < 0) {
            st->log(std::string("[quic] write 错误: ") + ngtcp2_strerror((int)n));
            return false;
        }

        if (n > 0) {
            if (wantSend && dataLen > 0) {
                std::lock_guard<std::mutex> lk(st->outMutex);
                size_t written = (size_t)dataLen;
                if (written > st->pendingOut.size()) written = st->pendingOut.size();
                st->pendingOut.erase(st->pendingOut.begin(),
                                     st->pendingOut.begin() + (long)written);
            }
            if (send(st->sock, buf, (size_t)n, 0) < 0) {
                st->log(std::string("[quic] send 错误: ") + strerror(errno));
                return false;
            }
        }

        if (st->regStream >= 0) {
            uint64_t nowMs = nowNs() / 1000000;
            if (regTs == 0 || nowMs - regTs >= (uint64_t)st->keepaliveSec * 1000) {
                HpMessage keep;
                keep.type = HpType::KEEPALIVE;
                keep.metaData.key = st->deviceId;
                auto payload = keep.encode();
                std::vector<uint8_t> frame;
                uint32_t len = (uint32_t)payload.size();
                uint8_t hdr[8] = {(uint8_t)(kMagic >> 24), (uint8_t)(kMagic >> 16),
                                  (uint8_t)(kMagic >> 8), (uint8_t)kMagic,
                                  (uint8_t)(len >> 24), (uint8_t)(len >> 16),
                                  (uint8_t)(len >> 8), (uint8_t)len};
                frame.assign(hdr, hdr + 8);
                frame.insert(frame.end(), payload.begin(), payload.end());
                std::lock_guard<std::mutex> lk(st->outMutex);
                if (st->pendingOut.size() < 1024 * 1024)
                    st->pendingOut.insert(st->pendingOut.end(), frame.begin(), frame.end());
                regTs = nowMs;
                st->log("[quic] keepalive");
            }
        }

        int64_t timeoutMs = 100;
        ngtcp2_tstamp expiry = ngtcp2_conn_get_expiry(st->conn);
        if (expiry > now) {
            uint64_t diffMs = (expiry - now) / 1000000;
            timeoutMs = (int64_t)(diffMs == 0 ? 1 : (diffMs > 500 ? 500 : diffMs));
        }

        struct pollfd pfd{};
        pfd.fd = st->sock;
        pfd.events = POLLIN;
        int pret = poll(&pfd, 1, (int)timeoutMs);
        if (pret > 0 && (pfd.revents & POLLIN)) {
            for (int i = 0; i < 16; ++i) {
                sockaddr_storage peer{};
                socklen_t peerLen = sizeof(peer);
                ssize_t rn = recvfrom(st->sock, buf, sizeof(buf), 0,
                                      (sockaddr *)&peer, &peerLen);
                if (rn <= 0) break;
                ngtcp2_path_storage ps;
                ngtcp2_path_storage_zero(&ps);
                ngtcp2_addr_copy_byte(&ps.path.local,
                                      (ngtcp2_sockaddr *)&st->local, st->localLen);
                ngtcp2_addr_copy_byte(&ps.path.remote,
                                      (ngtcp2_sockaddr *)&peer, peerLen);
                ngtcp2_pkt_info rpi{0};
                int rv = ngtcp2_conn_read_pkt(st->conn, &ps.path, &rpi, buf,
                                              (size_t)rn, nowNs());
                if (rv != 0) {
                    st->log(std::string("[quic] read 错误: ") + ngtcp2_strerror(rv));
                    return false;
                }
            }
        }
    }
    return false;
}

static bool setupSession(ClientState *st, const QuicConfig &cfg) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo *res = nullptr;
    char portStr[8];
    snprintf(portStr, sizeof(portStr), "%u", cfg.port);
    if (getaddrinfo(cfg.host.c_str(), portStr, &hints, &res) != 0 || !res) {
        st->log("[quic] DNS 解析失败");
        return false;
    }
    memcpy(&st->remote, res->ai_addr, res->ai_addrlen);
    st->remoteLen = res->ai_addrlen;
    freeaddrinfo(res);

    st->sock = socket(AF_INET, SOCK_DGRAM, 0);
    connect(st->sock, (sockaddr *)&st->remote, st->remoteLen);
    st->localLen = sizeof(st->local);
    getsockname(st->sock, (sockaddr *)&st->local, &st->localLen);
    fcntl(st->sock, F_SETFL, O_NONBLOCK);

    ngtcp2_crypto_ossl_init();

    st->sslCtx = SSL_CTX_new(TLS_client_method());
    ngtcp2_crypto_ossl_init();
    ngtcp2_crypto_ossl_configure_client_session(st->ssl);
    SSL_CTX_set_alpn_protos(st->sslCtx, (const unsigned char *)"\x06HP_PRO", 7);
    SSL_CTX_set_verify(st->sslCtx, SSL_VERIFY_NONE, nullptr);

    st->ssl = SSL_new(st->sslCtx);
    SSL_set_tlsext_host_name(st->ssl, cfg.host.c_str());
    SSL_set_app_data(st->ssl, &st->connRef);
    ngtcp2_crypto_ossl_configure_client_session(st->ssl);
    ngtcp2_crypto_ossl_ctx_new(&st->osslCtx, nullptr);
    ngtcp2_crypto_ossl_ctx_set_ssl(st->osslCtx, st->ssl);
    SSL_set_connect_state(st->ssl);

    ngtcp2_cid scid{}, dcid{};
    scid.datalen = 18;
    dcid.datalen = 18;
    uint64_t seed = nowNs();
    for (int i = 0; i < 18; ++i) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        scid.data[i] = (uint8_t)(seed >> 33);
        dcid.data[i] = (uint8_t)(seed >> 11);
    }

    ngtcp2_settings settings;
    ngtcp2_settings_default(&settings);
    settings.initial_ts = nowNs();

    ngtcp2_transport_params params;
    ngtcp2_transport_params_default(&params);
    params.initial_max_streams_bidi = 16;
    params.initial_max_stream_data_bidi_local = 256 * 1024;
    params.initial_max_stream_data_bidi_remote = 256 * 1024;
    params.initial_max_data = 1024 * 1024;
    params.version_info.chosen_version = NGTCP2_PROTO_VER_V1;
    params.version_info.available_versions = nullptr;
    params.version_info.available_versionslen = 0;

    ngtcp2_callbacks callbacks{};
    callbacks.encrypt = ngtcp2_crypto_encrypt_cb;
    callbacks.decrypt = ngtcp2_crypto_decrypt_cb;
    callbacks.hp_mask = ngtcp2_crypto_hp_mask_cb;
    callbacks.get_new_connection_id = onGetNewConnectionId;
    callbacks.rand = onRand;
    callbacks.client_initial = ngtcp2_crypto_client_initial_cb;
    callbacks.recv_crypto_data = ngtcp2_crypto_recv_crypto_data_cb;
    callbacks.handshake_completed = onHandshakeCompleted;
    callbacks.handshake_confirmed = onHandshakeConfirmed;
    callbacks.recv_version_negotiation = onRecvVersionNegotiation;
    callbacks.version_negotiation = ngtcp2_crypto_version_negotiation_cb;
    callbacks.recv_stream_data = onRecvStreamData;
    callbacks.stream_close = onStreamClose;
    callbacks.get_path_challenge_data = ngtcp2_crypto_get_path_challenge_data_cb;
    callbacks.update_key = ngtcp2_crypto_update_key_cb;
    callbacks.recv_retry = ngtcp2_crypto_recv_retry_cb;
    callbacks.delete_crypto_aead_ctx = ngtcp2_crypto_delete_crypto_aead_ctx_cb;
    callbacks.delete_crypto_cipher_ctx = ngtcp2_crypto_delete_crypto_cipher_ctx_cb;

    ngtcp2_path_storage ps;
    ngtcp2_path_storage_zero(&ps);
    ngtcp2_addr_copy_byte(&ps.path.local, (ngtcp2_sockaddr *)&st->local, st->localLen);
    ngtcp2_addr_copy_byte(&ps.path.remote, (ngtcp2_sockaddr *)&st->remote, st->remoteLen);

    int rv = ngtcp2_conn_client_new(&st->conn, &scid, &dcid, &ps.path,
                                    NGTCP2_PROTO_VER_V1, &callbacks, &settings,
                                    &params, nullptr, st);
    if (rv != 0) {
        st->log(std::string("[quic] conn 创建失败: ") + ngtcp2_strerror(rv));
        return false;
    }
    return true;
}

static void teardownSession(ClientState *st) {
    if (st->conn) ngtcp2_conn_del(st->conn);
    if (st->ssl) {
        SSL_set_app_data(st->ssl, nullptr);
        SSL_free(st->ssl);
    }
    if (st->osslCtx) ngtcp2_crypto_ossl_ctx_del(st->osslCtx);
    if (st->sslCtx) SSL_CTX_free(st->sslCtx);
    if (st->sock >= 0) close(st->sock);
}

bool QuicClient::start(const QuicConfig &cfg, const QuicEvent &ev) {
    if (running_.load()) return false;
    cfg_ = cfg;
    ev_ = ev;
    running_.store(true);
    worker_ = std::thread(&QuicClient::runLoop, this);
    return true;
}

void QuicClient::stop() {
    running_.store(false);
    if (worker_.joinable()) worker_.join();
}

void QuicClient::runLoop() {
    int backoff = 2;
    while (running_.load()) {
        auto st = new ClientState();
        st->host = cfg_.host;
        st->deviceId = cfg_.deviceId;
        st->alpn = cfg_.alpn;
        st->keepaliveSec = cfg_.keepaliveSec;
        st->ev = ev_;
        st->connRef.get_conn = getConnCb;
        st->connRef.user_data = st;

        if (ev_.onStatus)
            ev_.onStatus(QState::CONNECTING, "QUIC 拨号 " + cfg_.host + ":" + std::to_string(cfg_.port));

        if (setupSession(st, cfg_)) {
            conn_ = st->conn;
            uint64_t regTs = 0;
            runSession(st, cfg_, regTs);
        }

        teardownSession(st);
        conn_ = nullptr;
        delete st;

        if (running_.load()) {
            if (ev_.onStatus)
                ev_.onStatus(QState::RETRYING, "QUIC 断开，" + std::to_string(backoff) + "s 后重连");
            std::this_thread::sleep_for(std::chrono::seconds(backoff));
            backoff = backoff * 2 > 60 ? 60 : backoff * 2;
        }
    }
    if (ev_.onStatus) ev_.onStatus(QState::CLOSED, "QUIC 已停止");
}

}
