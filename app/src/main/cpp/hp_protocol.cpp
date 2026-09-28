#include "hp_protocol.h"

namespace hp {

void writeVarint(std::vector<uint8_t>& out, uint64_t v) {
    while (v >= 0x80) {
        out.push_back(static_cast<uint8_t>(v) | 0x80);
        v >>= 7;
    }
    out.push_back(static_cast<uint8_t>(v));
}

bool readVarint(const uint8_t*& p, const uint8_t* end, uint64_t& v) {
    v = 0;
    int shift = 0;
    while (p < end) {
        uint8_t b = *p++;
        v |= static_cast<uint64_t>(b & 0x7F) << shift;
        if ((b & 0x80) == 0) return true;
        shift += 7;
        if (shift > 63) return false;
    }
    return false;
}

static void writeTag(std::vector<uint8_t>& out, uint32_t field, uint32_t wireType) {
    writeVarint(out, (static_cast<uint64_t>(field) << 3) | wireType);
}

static void writeStringField(std::vector<uint8_t>& out, uint32_t field, const std::string& s) {
    if (s.empty()) return;
    writeTag(out, field, 2);
    writeVarint(out, s.size());
    out.insert(out.end(), s.begin(), s.end());
}

static void writeBytesField(std::vector<uint8_t>& out, uint32_t field, const std::vector<uint8_t>& b) {
    if (b.empty()) return;
    writeTag(out, field, 2);
    writeVarint(out, b.size());
    out.insert(out.end(), b.begin(), b.end());
}

static void writeVarintField(std::vector<uint8_t>& out, uint32_t field, uint64_t v) {
    writeTag(out, field, 0);
    writeVarint(out, v);
}

static void writeMessageField(std::vector<uint8_t>& out, uint32_t field, const std::vector<uint8_t>& msg) {
    writeTag(out, field, 2);
    writeVarint(out, msg.size());
    out.insert(out.end(), msg.begin(), msg.end());
}

static bool skipField(const uint8_t*& p, const uint8_t* end, uint32_t wireType) {
    switch (wireType) {
        case 0: { uint64_t v; return readVarint(p, end, v); }
        case 1: if (end - p < 8) return false; p += 8; return true;
        case 2: {
            uint64_t len;
            if (!readVarint(p, end, len)) return false;
            if (static_cast<uint64_t>(end - p) < len) return false;
            p += len;
            return true;
        }
        case 5: if (end - p < 4) return false; p += 4; return true;
        default: return false;
    }
}

static bool readStringField(const uint8_t*& p, const uint8_t* end, std::string& out) {
    uint64_t len;
    if (!readVarint(p, end, len)) return false;
    if (static_cast<uint64_t>(end - p) < len) return false;
    out.assign(reinterpret_cast<const char*>(p), len);
    p += len;
    return true;
}

static bool readBytesField(const uint8_t*& p, const uint8_t* end, std::vector<uint8_t>& out) {
    uint64_t len;
    if (!readVarint(p, end, len)) return false;
    if (static_cast<uint64_t>(end - p) < len) return false;
    out.assign(p, p + len);
    p += len;
    return true;
}

std::vector<uint8_t> HpMessage::encode() const {
    std::vector<uint8_t> md;
    writeStringField(md, 1, metaData.key);
    writeVarintField(md, 2, static_cast<uint64_t>(metaData.type));
    writeStringField(md, 3, metaData.channelId);
    if (metaData.success) writeVarintField(md, 4, 1);
    writeStringField(md, 5, metaData.reason);

    std::vector<uint8_t> out;
    writeVarintField(out, 1, static_cast<uint64_t>(type));
    writeMessageField(out, 2, md);
    writeBytesField(out, 3, data);
    return out;
}

bool HpMessage::decode(const uint8_t* p, size_t n) {
    const uint8_t* end = p + n;
    while (p < end) {
        uint64_t tag;
        if (!readVarint(p, end, tag)) return false;
        uint32_t field = static_cast<uint32_t>(tag >> 3);
        uint32_t wt = static_cast<uint32_t>(tag & 7);
        switch (field) {
            case 1: {
                uint64_t v;
                if (!readVarint(p, end, v)) return false;
                type = static_cast<HpType>(v);
                break;
            }
            case 2: {
                uint64_t len;
                if (!readVarint(p, end, len)) return false;
                if (static_cast<uint64_t>(end - p) < len) return false;
                const uint8_t* mp = p;
                const uint8_t* mend = p + len;
                while (mp < mend) {
                    uint64_t mtag;
                    if (!readVarint(mp, mend, mtag)) return false;
                    uint32_t mf = static_cast<uint32_t>(mtag >> 3);
                    uint32_t mwt = static_cast<uint32_t>(mtag & 7);
                    switch (mf) {
                        case 1: if (!readStringField(mp, mend, metaData.key)) return false; break;
                        case 2: { uint64_t v; if (!readVarint(mp, mend, v)) return false;
                                  metaData.type = static_cast<ConnType>(v); break; }
                        case 3: if (!readStringField(mp, mend, metaData.channelId)) return false; break;
                        case 4: { uint64_t v; if (!readVarint(mp, mend, v)) return false;
                                  metaData.success = v != 0; break; }
                        case 5: if (!readStringField(mp, mend, metaData.reason)) return false; break;
                        default: if (!skipField(mp, mend, mwt)) return false; break;
                    }
                }
                p = mend;
                break;
            }
            case 3: if (!readBytesField(p, end, data)) return false; break;
            default: if (!skipField(p, end, wt)) return false; break;
        }
    }
    return true;
}

std::vector<uint8_t> CmdMessage::encode() const {
    std::vector<uint8_t> out;
    writeVarintField(out, 1, static_cast<uint64_t>(type));
    writeStringField(out, 2, key);
    writeBytesField(out, 3, data);
    writeStringField(out, 4, version);
    return out;
}

bool CmdMessage::decode(const uint8_t* p, size_t n) {
    const uint8_t* end = p + n;
    while (p < end) {
        uint64_t tag;
        if (!readVarint(p, end, tag)) return false;
        uint32_t field = static_cast<uint32_t>(tag >> 3);
        uint32_t wt = static_cast<uint32_t>(tag & 7);
        switch (field) {
            case 1: { uint64_t v; if (!readVarint(p, end, v)) return false;
                      type = static_cast<CmdType>(v); break; }
            case 2: if (!readStringField(p, end, key)) return false; break;
            case 3: if (!readBytesField(p, end, data)) return false; break;
            case 4: if (!readStringField(p, end, version)) return false; break;
            default: if (!skipField(p, end, wt)) return false; break;
        }
    }
    return true;
}

void frameMessage(std::vector<uint8_t>& out, const std::vector<uint8_t>& payload) {
    writeVarint(out, payload.size());
    out.insert(out.end(), payload.begin(), payload.end());
}

bool tryExtractFrame(std::vector<uint8_t>& buf, std::vector<uint8_t>& frame) {
    const uint8_t* p = buf.data();
    const uint8_t* end = buf.data() + buf.size();
    const uint8_t* start = p;
    uint64_t len;
    if (!readVarint(p, end, len)) return false;
    if (static_cast<uint64_t>(end - p) < len) return false;
    frame.assign(p, p + len);
    size_t consumed = static_cast<size_t>(p - start) + len;
    buf.erase(buf.begin(), buf.begin() + static_cast<long>(consumed));
    return true;
}


void frameMessage4B(std::vector<uint8_t>& out, const std::vector<uint8_t>& payload) {
    uint32_t len = static_cast<uint32_t>(payload.size());
    out.push_back(static_cast<uint8_t>(0x1a0a >> 8));
    out.push_back(static_cast<uint8_t>(0x1a0a & 0xff));
    out.push_back(static_cast<uint8_t>(len >> 24));
    out.push_back(static_cast<uint8_t>(len >> 16));
    out.push_back(static_cast<uint8_t>(len >> 8));
    out.push_back(static_cast<uint8_t>(len));
    out.insert(out.end(), payload.begin(), payload.end());
}

bool tryExtractFrame4B(std::vector<uint8_t>& buf, std::vector<uint8_t>& frame) {
    if (buf.size() < 4) return false;
    uint32_t len = (static_cast<uint32_t>(buf[0]) << 24) |
                   (static_cast<uint32_t>(buf[1]) << 16) |
                   (static_cast<uint32_t>(buf[2]) << 8) |
                   static_cast<uint32_t>(buf[3]);
    if (len == 0 || len > 4u * 1024 * 1024) return false;
    if (buf.size() < 4u + len) return false;
    frame.assign(buf.begin() + 4, buf.begin() + 4 + len);
    buf.erase(buf.begin(), buf.begin() + 4 + len);
    return true;
}

bool tryExtractFrame8B(std::vector<uint8_t>& buf, std::vector<uint8_t>& frame) {
    if (buf.size() < 8) return false;
    if (buf[0] != 0x00 || buf[1] != 0x00 || buf[2] != 0x1a || buf[3] != 0x0a) return false;
    uint32_t len = (static_cast<uint32_t>(buf[4]) << 24) |
                   (static_cast<uint32_t>(buf[5]) << 16) |
                   (static_cast<uint32_t>(buf[6]) << 8) |
                   static_cast<uint32_t>(buf[7]);
    if (len == 0 || len > 4u * 1024 * 1024) return false;
    if (buf.size() < 8u + len) return false;
    frame.assign(buf.begin() + 8, buf.begin() + 8 + len);
    buf.erase(buf.begin(), buf.begin() + 8 + len);
    return true;
}

} // namespace hp
