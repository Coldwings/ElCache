#pragma once

#include "elcache/types.hpp"
#include "elcache/chunk.hpp"
#include <variant>
#include <vector>

namespace elcache::protocol {

// Protocol version
constexpr uint16_t VERSION = 1;

// Message types
enum class MessageType : uint16_t {
    // Handshake
    Hello = 0x0001,
    HelloAck = 0x0002,
    
    // Gossip
    GossipPush = 0x0010,
    GossipPull = 0x0011,
    GossipAck = 0x0012,
    
    // Cache operations
    Get = 0x0100,
    GetResponse = 0x0101,
    Put = 0x0102,
    PutResponse = 0x0103,
    Delete = 0x0104,
    DeleteResponse = 0x0105,
    Check = 0x0106,
    CheckResponse = 0x0107,
    
    // Chunk operations
    GetChunk = 0x0200,
    GetChunkResponse = 0x0201,
    PutChunk = 0x0202,
    PutChunkResponse = 0x0203,
    HasChunk = 0x0204,
    HasChunkResponse = 0x0205,
    
    // Streaming
    StreamStart = 0x0300,
    StreamData = 0x0301,
    StreamEnd = 0x0302,
    StreamAck = 0x0303,
    StreamAbort = 0x0304,
    
    // Cluster
    NodeJoin = 0x0400,
    NodeLeave = 0x0401,
    NodeStatus = 0x0402,
    
    // Error
    Error = 0xFFFF,
};

// Message header (fixed size for easy parsing)
struct MessageHeader {
    uint32_t magic = 0x454C4341;  // "ELCA"
    uint16_t version = VERSION;
    uint16_t type;
    uint32_t length;  // Payload length (not including header)
    uint32_t request_id;
    uint64_t timestamp;
    
    static constexpr size_t SIZE = 24;
};

// Node information for gossip
struct NodeInfo {
    NodeId id;
    std::string address;
    uint16_t port;
    uint64_t memory_capacity;
    uint64_t memory_used;
    uint64_t disk_capacity;
    uint64_t disk_used;
    uint64_t generation;  // Monotonic version for conflict resolution
    uint32_t state;       // NodeState enum
    Timestamp last_seen;
    
    enum NodeState : uint32_t {
        Unknown = 0,
        Joining = 1,
        Active = 2,
        Leaving = 3,
        Failed = 4,
    };
};

// Hello message (initial handshake)
struct HelloMessage {
    NodeInfo sender;
    std::string cluster_name;
    std::vector<std::string> capabilities;
};

struct HelloAckMessage {
    NodeInfo sender;
    bool accepted;
    std::string reject_reason;
};

// Gossip messages
struct GossipPushMessage {
    NodeId sender;
    std::vector<NodeInfo> nodes;  // Nodes this sender knows about
};

struct GossipPullMessage {
    NodeId sender;
    std::vector<NodeId> requested;  // Nodes we want info about
};

struct GossipAckMessage {
    NodeId sender;
    std::vector<NodeInfo> nodes;
};

// Get message
struct GetMessage {
    CacheKey key;
    bool metadata_only = false;
    std::optional<Range> range;
};

struct GetResponseMessage {
    CacheResult result;
    std::optional<CacheMetadata> metadata;
    ByteBuffer data;
    Range actual_range;
};

// Put message
struct PutMessage {
    CacheKey key;
    ByteBuffer data;
    std::optional<std::chrono::seconds> ttl;
    uint32_t flags = 0;
};

struct PutResponseMessage {
    ErrorCode code;
    std::string message;
};

// Delete message
struct DeleteMessage {
    CacheKey key;
};

struct DeleteResponseMessage {
    ErrorCode code;
    bool existed;
};

// Check message (availability query)
struct CheckMessage {
    CacheKey key;
    std::optional<Range> range;
};

struct CheckResponseMessage {
    bool exists;
    std::optional<CacheMetadata> metadata;
    std::vector<Range> available_ranges;
};

// Chunk operations
struct GetChunkMessage {
    ChunkId id;
    uint64_t offset = 0;
    uint64_t length = 0;  // 0 = full chunk
};

struct GetChunkResponseMessage {
    bool found;
    ByteBuffer data;
};

struct PutChunkMessage {
    ChunkId id;
    ByteBuffer data;
};

struct PutChunkResponseMessage {
    ErrorCode code;
};

struct HasChunkMessage {
    std::vector<ChunkId> ids;
};

struct HasChunkResponseMessage {
    std::vector<bool> exists;
};

// Stream messages (for large transfers)
struct StreamStartMessage {
    uint32_t stream_id;
    CacheKey key;
    uint64_t total_size;
    uint64_t chunk_count;
};

struct StreamDataMessage {
    uint32_t stream_id;
    uint64_t sequence;
    ByteBuffer data;
};

struct StreamEndMessage {
    uint32_t stream_id;
    Hash128 checksum;  // Full value checksum
};

struct StreamAckMessage {
    uint32_t stream_id;
    uint64_t acked_sequence;
    ErrorCode code;
};

struct StreamAbortMessage {
    uint32_t stream_id;
    ErrorCode reason;
    std::string message;
};

// Error message
struct ErrorMessage {
    ErrorCode code;
    std::string message;
    uint32_t original_request_id;
};

// Unified message type
using Message = std::variant<
    HelloMessage,
    HelloAckMessage,
    GossipPushMessage,
    GossipPullMessage,
    GossipAckMessage,
    GetMessage,
    GetResponseMessage,
    PutMessage,
    PutResponseMessage,
    DeleteMessage,
    DeleteResponseMessage,
    CheckMessage,
    CheckResponseMessage,
    GetChunkMessage,
    GetChunkResponseMessage,
    PutChunkMessage,
    PutChunkResponseMessage,
    HasChunkMessage,
    HasChunkResponseMessage,
    StreamStartMessage,
    StreamDataMessage,
    StreamEndMessage,
    StreamAckMessage,
    StreamAbortMessage,
    ErrorMessage
>;

// Codec for serialization/deserialization
class Codec {
public:
    // Serialize message to buffer
    static ByteBuffer encode(const Message& msg, uint32_t request_id = 0);
    
    // Deserialize message from buffer
    static std::pair<Message, MessageHeader> decode(ByteView data);
    
    // Parse header only (for length-prefixed reading)
    static MessageHeader parse_header(ByteView data);
    
    // Encoding helpers (public for use by helper functions)
    static void encode_header(ByteBuffer& buf, MessageType type, 
                              uint32_t payload_len, uint32_t request_id);
    static void encode_string(ByteBuffer& buf, std::string_view str);
    static void encode_bytes(ByteBuffer& buf, ByteView data);
    static void encode_u16(ByteBuffer& buf, uint16_t v);
    static void encode_u32(ByteBuffer& buf, uint32_t v);
    static void encode_u64(ByteBuffer& buf, uint64_t v);
    
    // Decoding helpers (public for use by helper functions)
    static std::string decode_string(ByteView& data);
    static ByteBuffer decode_bytes(ByteView& data);
    static uint16_t decode_u16(ByteView& data);
    static uint32_t decode_u32(ByteView& data);
    static uint64_t decode_u64(ByteView& data);
};

}  // namespace elcache::protocol
