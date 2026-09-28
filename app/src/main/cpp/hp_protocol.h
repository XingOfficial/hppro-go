#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace hp {

enum class HpType : uint64_t {
    REGISTER = 0,         
    REGISTER_RESULT = 1,  
    CONNECTED = 2,        
    DISCONNECTED = 3,     
    DATA = 4,             
    KEEPALIVE = 5,        
};

enum class CmdType : uint64_t {
    CONNECT = 0,
    DISCONNECT = 1,
    LOCAL_INNER_WEAR = 2, 
    TIPS = 3,
};

enum class ConnType : uint64_t {
    TCP = 0,
    UDP = 1,
    TCP_UDP = 2,
};

void writeVarint(std::vector<uint8_t>& out, uint64_t v);
bool readVarint(const uint8_t*& p, const uint8_t* end, uint64_t& v);

struct MetaData {
    std::string key;                 
    ConnType type = ConnType::TCP;   
    std::string channelId;           
    bool success = false;            
    std::string reason;              
};

struct HpMessage {
    HpType type = HpType::REGISTER;  
    MetaData metaData;               
    std::vector<uint8_t> data;       

    std::vector<uint8_t> encode() const;
    
    bool decode(const uint8_t* p, size_t n);
};

struct CmdMessage {
    CmdType type = CmdType::CONNECT; 
    std::string key;                 
    std::vector<uint8_t> data;       
    std::string version;             

    std::vector<uint8_t> encode() const;
    bool decode(const uint8_t* p, size_t n);
};

void frameMessage(std::vector<uint8_t>& out, const std::vector<uint8_t>& payload);

void frameMessage4B(std::vector<uint8_t>& out, const std::vector<uint8_t>& payload);

bool tryExtractFrame(std::vector<uint8_t>& buf, std::vector<uint8_t>& frame);

bool tryExtractFrame4B(std::vector<uint8_t>& buf, std::vector<uint8_t>& frame);

bool tryExtractFrame8B(std::vector<uint8_t>& buf, std::vector<uint8_t>& frame);

} 
