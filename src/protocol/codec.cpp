#include "elcache/protocol.hpp"
#include <cstring>
#include <stdexcept>

namespace elcache::protocol {

// Encoding helpers
void Codec::encode_header(ByteBuffer& buf, MessageType type, 
                           uint32_t payload_len, uint32_t request_id) {
    MessageHeader hdr;
    hdr.type = static_cast<uint16_t>(type);
    hdr.length = payload_len;
    hdr.request_id = request_id;
    hdr.timestamp = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            SystemClock::now().time_since_epoch()).count());
    
    buf.resize(buf.size() + MessageHeader::SIZE);
    auto* ptr = buf.data() + buf.size() - MessageHeader::SIZE;
    
    std::memcpy(ptr, &hdr.magic, 4); ptr += 4;
    std::memcpy(ptr, &hdr.version, 2); ptr += 2;
    std::memcpy(ptr, &hdr.type, 2); ptr += 2;
    std::memcpy(ptr, &hdr.length, 4); ptr += 4;
    std::memcpy(ptr, &hdr.request_id, 4); ptr += 4;
    std::memcpy(ptr, &hdr.timestamp, 8);
}

void Codec::encode_string(ByteBuffer& buf, std::string_view str) {
    encode_u32(buf, static_cast<uint32_t>(str.size()));
    buf.insert(buf.end(), str.begin(), str.end());
}

void Codec::encode_bytes(ByteBuffer& buf, ByteView data) {
    encode_u32(buf, static_cast<uint32_t>(data.size()));
    buf.insert(buf.end(), data.begin(), data.end());
}

void Codec::encode_u16(ByteBuffer& buf, uint16_t v) {
    buf.push_back(v & 0xFF);
    buf.push_back((v >> 8) & 0xFF);
}

void Codec::encode_u32(ByteBuffer& buf, uint32_t v) {
    buf.push_back(v & 0xFF);
    buf.push_back((v >> 8) & 0xFF);
    buf.push_back((v >> 16) & 0xFF);
    buf.push_back((v >> 24) & 0xFF);
}

void Codec::encode_u64(ByteBuffer& buf, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        buf.push_back((v >> (i * 8)) & 0xFF);
    }
}

// Decoding helpers
std::string Codec::decode_string(ByteView& data) {
    uint32_t len = decode_u32(data);
    if (data.size() < len) {
        throw std::runtime_error("Truncated string");
    }
    std::string result(reinterpret_cast<const char*>(data.data()), len);
    data = data.subspan(len);
    return result;
}

ByteBuffer Codec::decode_bytes(ByteView& data) {
    uint32_t len = decode_u32(data);
    if (data.size() < len) {
        throw std::runtime_error("Truncated bytes");
    }
    ByteBuffer result(data.begin(), data.begin() + len);
    data = data.subspan(len);
    return result;
}

uint16_t Codec::decode_u16(ByteView& data) {
    if (data.size() < 2) throw std::runtime_error("Truncated u16");
    uint16_t v = data[0] | (static_cast<uint16_t>(data[1]) << 8);
    data = data.subspan(2);
    return v;
}

uint32_t Codec::decode_u32(ByteView& data) {
    if (data.size() < 4) throw std::runtime_error("Truncated u32");
    uint32_t v = data[0] | 
                 (static_cast<uint32_t>(data[1]) << 8) |
                 (static_cast<uint32_t>(data[2]) << 16) |
                 (static_cast<uint32_t>(data[3]) << 24);
    data = data.subspan(4);
    return v;
}

uint64_t Codec::decode_u64(ByteView& data) {
    if (data.size() < 8) throw std::runtime_error("Truncated u64");
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<uint64_t>(data[i]) << (i * 8);
    }
    data = data.subspan(8);
    return v;
}

MessageHeader Codec::parse_header(ByteView data) {
    if (data.size() < MessageHeader::SIZE) {
        throw std::runtime_error("Truncated header");
    }
    
    MessageHeader hdr;
    const uint8_t* ptr = data.data();
    
    std::memcpy(&hdr.magic, ptr, 4); ptr += 4;
    std::memcpy(&hdr.version, ptr, 2); ptr += 2;
    std::memcpy(&hdr.type, ptr, 2); ptr += 2;
    std::memcpy(&hdr.length, ptr, 4); ptr += 4;
    std::memcpy(&hdr.request_id, ptr, 4); ptr += 4;
    std::memcpy(&hdr.timestamp, ptr, 8);
    
    if (hdr.magic != 0x454C4341) {
        throw std::runtime_error("Invalid magic number");
    }
    
    return hdr;
}

// Encode NodeInfo helper
static void encode_node_info(ByteBuffer& buf, const NodeInfo& info) {
    Codec::encode_u64(buf, info.id.value());
    Codec::encode_string(buf, info.address);
    Codec::encode_u16(buf, info.port);
    Codec::encode_u64(buf, info.memory_capacity);
    Codec::encode_u64(buf, info.memory_used);
    Codec::encode_u64(buf, info.disk_capacity);
    Codec::encode_u64(buf, info.disk_used);
    Codec::encode_u64(buf, info.generation);
    Codec::encode_u32(buf, info.state);
}

// Decode NodeInfo helper
static NodeInfo decode_node_info(ByteView& data) {
    NodeInfo info;
    info.id = NodeId(Codec::decode_u64(data));
    info.address = Codec::decode_string(data);
    info.port = Codec::decode_u16(data);
    info.memory_capacity = Codec::decode_u64(data);
    info.memory_used = Codec::decode_u64(data);
    info.disk_capacity = Codec::decode_u64(data);
    info.disk_used = Codec::decode_u64(data);
    info.generation = Codec::decode_u64(data);
    info.state = Codec::decode_u32(data);
    return info;
}

ByteBuffer Codec::encode(const Message& msg, uint32_t request_id) {
    ByteBuffer payload;
    MessageType type;
    
    std::visit([&](const auto& m) {
        using T = std::decay_t<decltype(m)>;
        
        if constexpr (std::is_same_v<T, HelloMessage>) {
            type = MessageType::Hello;
            encode_node_info(payload, m.sender);
            encode_string(payload, m.cluster_name);
            encode_u32(payload, static_cast<uint32_t>(m.capabilities.size()));
            for (const auto& cap : m.capabilities) {
                encode_string(payload, cap);
            }
        }
        else if constexpr (std::is_same_v<T, HelloAckMessage>) {
            type = MessageType::HelloAck;
            encode_node_info(payload, m.sender);
            payload.push_back(m.accepted ? 1 : 0);
            encode_string(payload, m.reject_reason);
        }
        else if constexpr (std::is_same_v<T, GossipPushMessage>) {
            type = MessageType::GossipPush;
            encode_u64(payload, m.sender.value());
            encode_u32(payload, static_cast<uint32_t>(m.nodes.size()));
            for (const auto& node : m.nodes) {
                encode_node_info(payload, node);
            }
        }
        else if constexpr (std::is_same_v<T, GetMessage>) {
            type = MessageType::Get;
            encode_string(payload, m.key.view());
            payload.push_back(m.metadata_only ? 1 : 0);
            payload.push_back(m.range.has_value() ? 1 : 0);
            if (m.range) {
                encode_u64(payload, m.range->offset);
                encode_u64(payload, m.range->length);
            }
        }
        else if constexpr (std::is_same_v<T, GetResponseMessage>) {
            type = MessageType::GetResponse;
            encode_u32(payload, static_cast<uint32_t>(m.result));
            payload.push_back(m.metadata.has_value() ? 1 : 0);
            if (m.metadata) {
                encode_u64(payload, m.metadata->total_size);
                encode_u64(payload, m.metadata->chunk_count);
                encode_u32(payload, m.metadata->flags);
            }
            encode_bytes(payload, m.data);
            encode_u64(payload, m.actual_range.offset);
            encode_u64(payload, m.actual_range.length);
        }
        else if constexpr (std::is_same_v<T, PutMessage>) {
            type = MessageType::Put;
            encode_string(payload, m.key.view());
            encode_bytes(payload, m.data);
            payload.push_back(m.ttl.has_value() ? 1 : 0);
            if (m.ttl) {
                encode_u64(payload, m.ttl->count());
            }
            encode_u32(payload, m.flags);
        }
        else if constexpr (std::is_same_v<T, PutResponseMessage>) {
            type = MessageType::PutResponse;
            encode_u32(payload, static_cast<uint32_t>(m.code));
            encode_string(payload, m.message);
        }
        else if constexpr (std::is_same_v<T, DeleteMessage>) {
            type = MessageType::Delete;
            encode_string(payload, m.key.view());
        }
        else if constexpr (std::is_same_v<T, DeleteResponseMessage>) {
            type = MessageType::DeleteResponse;
            encode_u32(payload, static_cast<uint32_t>(m.code));
            payload.push_back(m.existed ? 1 : 0);
        }
        else if constexpr (std::is_same_v<T, CheckMessage>) {
            type = MessageType::Check;
            encode_string(payload, m.key.view());
            payload.push_back(m.range.has_value() ? 1 : 0);
            if (m.range) {
                encode_u64(payload, m.range->offset);
                encode_u64(payload, m.range->length);
            }
        }
        else if constexpr (std::is_same_v<T, CheckResponseMessage>) {
            type = MessageType::CheckResponse;
            payload.push_back(m.exists ? 1 : 0);
            payload.push_back(m.metadata.has_value() ? 1 : 0);
            if (m.metadata) {
                encode_u64(payload, m.metadata->total_size);
                encode_u64(payload, m.metadata->chunk_count);
                encode_u32(payload, m.metadata->flags);
            }
            encode_u32(payload, static_cast<uint32_t>(m.available_ranges.size()));
            for (const auto& r : m.available_ranges) {
                encode_u64(payload, r.offset);
                encode_u64(payload, r.length);
            }
        }
        else if constexpr (std::is_same_v<T, GetChunkMessage>) {
            type = MessageType::GetChunk;
            auto hash = m.id.hash();
            encode_u64(payload, hash.low);
            encode_u64(payload, hash.high);
            encode_u64(payload, m.offset);
            encode_u64(payload, m.length);
        }
        else if constexpr (std::is_same_v<T, GetChunkResponseMessage>) {
            type = MessageType::GetChunkResponse;
            payload.push_back(m.found ? 1 : 0);
            encode_bytes(payload, m.data);
        }
        else if constexpr (std::is_same_v<T, ErrorMessage>) {
            type = MessageType::Error;
            encode_u32(payload, static_cast<uint32_t>(m.code));
            encode_string(payload, m.message);
            encode_u32(payload, m.original_request_id);
        }
        else {
            // Handle remaining message types similarly
            type = MessageType::Error;
        }
    }, msg);
    
    ByteBuffer result;
    encode_header(result, type, static_cast<uint32_t>(payload.size()), request_id);
    result.insert(result.end(), payload.begin(), payload.end());
    
    return result;
}

std::pair<Message, MessageHeader> Codec::decode(ByteView data) {
    auto header = parse_header(data);
    data = data.subspan(MessageHeader::SIZE);
    
    auto type = static_cast<MessageType>(header.type);
    Message msg;
    
    switch (type) {
        case MessageType::Hello: {
            HelloMessage m;
            m.sender = decode_node_info(data);
            m.cluster_name = decode_string(data);
            uint32_t cap_count = decode_u32(data);
            for (uint32_t i = 0; i < cap_count; ++i) {
                m.capabilities.push_back(decode_string(data));
            }
            msg = m;
            break;
        }
        
        case MessageType::HelloAck: {
            HelloAckMessage m;
            m.sender = decode_node_info(data);
            m.accepted = data[0] != 0;
            data = data.subspan(1);
            m.reject_reason = decode_string(data);
            msg = m;
            break;
        }
        
        case MessageType::GossipPush: {
            GossipPushMessage m;
            m.sender = NodeId(decode_u64(data));
            uint32_t node_count = decode_u32(data);
            for (uint32_t i = 0; i < node_count; ++i) {
                m.nodes.push_back(decode_node_info(data));
            }
            msg = m;
            break;
        }
        
        case MessageType::Get: {
            GetMessage m;
            m.key = CacheKey(decode_string(data));
            m.metadata_only = data[0] != 0;
            data = data.subspan(1);
            bool has_range = data[0] != 0;
            data = data.subspan(1);
            if (has_range) {
                m.range = Range{decode_u64(data), decode_u64(data)};
            }
            msg = m;
            break;
        }
        
        case MessageType::GetResponse: {
            GetResponseMessage m;
            m.result = static_cast<CacheResult>(decode_u32(data));
            bool has_meta = data[0] != 0;
            data = data.subspan(1);
            if (has_meta) {
                CacheMetadata meta;
                meta.total_size = decode_u64(data);
                meta.chunk_count = decode_u64(data);
                meta.flags = decode_u32(data);
                m.metadata = meta;
            }
            m.data = decode_bytes(data);
            m.actual_range.offset = decode_u64(data);
            m.actual_range.length = decode_u64(data);
            msg = m;
            break;
        }
        
        case MessageType::Put: {
            PutMessage m;
            m.key = CacheKey(decode_string(data));
            m.data = decode_bytes(data);
            bool has_ttl = data[0] != 0;
            data = data.subspan(1);
            if (has_ttl) {
                m.ttl = std::chrono::seconds(decode_u64(data));
            }
            m.flags = decode_u32(data);
            msg = m;
            break;
        }
        
        case MessageType::PutResponse: {
            PutResponseMessage m;
            m.code = static_cast<ErrorCode>(decode_u32(data));
            m.message = decode_string(data);
            msg = m;
            break;
        }
        
        case MessageType::Delete: {
            DeleteMessage m;
            m.key = CacheKey(decode_string(data));
            msg = m;
            break;
        }
        
        case MessageType::DeleteResponse: {
            DeleteResponseMessage m;
            m.code = static_cast<ErrorCode>(decode_u32(data));
            m.existed = data[0] != 0;
            msg = m;
            break;
        }
        
        case MessageType::Error: {
            ErrorMessage m;
            m.code = static_cast<ErrorCode>(decode_u32(data));
            m.message = decode_string(data);
            m.original_request_id = decode_u32(data);
            msg = m;
            break;
        }
        
        default:
            throw std::runtime_error("Unknown message type");
    }
    
    return {msg, header};
}

}  // namespace elcache::protocol
