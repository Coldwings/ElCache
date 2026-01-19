#include <catch2/catch_test_macros.hpp>
#include "elcache/protocol.hpp"

using namespace elcache;
using namespace elcache::protocol;

TEST_CASE("Message header parsing", "[protocol]") {
    SECTION("Valid header") {
        ByteBuffer buf(MessageHeader::SIZE);
        
        // Magic "ELCA" in little-endian = 0x454C4341
        buf[0] = 0x41; buf[1] = 0x43; buf[2] = 0x4C; buf[3] = 0x45;
        // Version = 1 (little-endian)
        buf[4] = 0x01; buf[5] = 0x00;
        // Type = Hello = 1 (little-endian)
        buf[6] = 0x01; buf[7] = 0x00;
        // Length = 16 (little-endian)
        buf[8] = 0x10; buf[9] = 0x00; buf[10] = 0x00; buf[11] = 0x00;
        // Request ID = 1 (little-endian)
        buf[12] = 0x01; buf[13] = 0x00; buf[14] = 0x00; buf[15] = 0x00;
        // Timestamp (8 bytes)
        for (int i = 16; i < 24; ++i) buf[i] = 0;
        
        auto header = Codec::parse_header(buf);
        
        REQUIRE(header.magic == 0x454C4341);
        REQUIRE(header.version == 1);
        REQUIRE(header.type == static_cast<uint16_t>(MessageType::Hello));
        REQUIRE(header.length == 16);
        REQUIRE(header.request_id == 1);
    }
    
    SECTION("Invalid magic throws") {
        ByteBuffer buf(MessageHeader::SIZE, 0);
        buf[0] = 0xFF;  // Wrong magic
        
        REQUIRE_THROWS(Codec::parse_header(buf));
    }
    
    SECTION("Truncated header throws") {
        ByteBuffer buf(10, 0);  // Too short
        
        REQUIRE_THROWS(Codec::parse_header(buf));
    }
}

TEST_CASE("GetMessage encoding/decoding", "[protocol]") {
    GetMessage original;
    original.key = CacheKey("test-key");
    original.metadata_only = false;
    original.range = Range{100, 200};
    
    // Encode
    auto encoded = Codec::encode(original, 42);
    
    REQUIRE(!encoded.empty());
    REQUIRE(encoded.size() >= MessageHeader::SIZE);
    
    // Decode
    auto [decoded_msg, header] = Codec::decode(encoded);
    
    REQUIRE(header.type == static_cast<uint16_t>(MessageType::Get));
    REQUIRE(header.request_id == 42);
    
    auto& decoded = std::get<GetMessage>(decoded_msg);
    REQUIRE(decoded.key.view() == "test-key");
    REQUIRE(decoded.metadata_only == false);
    REQUIRE(decoded.range.has_value());
    REQUIRE(decoded.range->offset == 100);
    REQUIRE(decoded.range->length == 200);
}

TEST_CASE("PutMessage encoding/decoding", "[protocol]") {
    PutMessage original;
    original.key = CacheKey("my-key");
    original.data = ByteBuffer{'H', 'e', 'l', 'l', 'o'};
    original.ttl = std::chrono::seconds(3600);
    original.flags = 123;
    
    auto encoded = Codec::encode(original, 1);
    auto [decoded_msg, header] = Codec::decode(encoded);
    
    REQUIRE(header.type == static_cast<uint16_t>(MessageType::Put));
    
    auto& decoded = std::get<PutMessage>(decoded_msg);
    REQUIRE(decoded.key.view() == "my-key");
    REQUIRE(decoded.data.size() == 5);
    REQUIRE(decoded.data[0] == 'H');
    REQUIRE(decoded.ttl.has_value());
    REQUIRE(decoded.ttl->count() == 3600);
    REQUIRE(decoded.flags == 123);
}

TEST_CASE("ErrorMessage encoding/decoding", "[protocol]") {
    ErrorMessage original;
    original.code = ErrorCode::NotFound;
    original.message = "Key not found";
    original.original_request_id = 99;
    
    auto encoded = Codec::encode(original, 100);
    auto [decoded_msg, header] = Codec::decode(encoded);
    
    REQUIRE(header.type == static_cast<uint16_t>(MessageType::Error));
    
    auto& decoded = std::get<ErrorMessage>(decoded_msg);
    REQUIRE(decoded.code == ErrorCode::NotFound);
    REQUIRE(decoded.message == "Key not found");
    REQUIRE(decoded.original_request_id == 99);
}

TEST_CASE("GossipPushMessage encoding/decoding", "[protocol]") {
    GossipPushMessage original;
    original.sender = NodeId::generate();
    
    // Add some nodes
    for (int i = 0; i < 3; ++i) {
        NodeInfo info;
        info.id = NodeId::generate();
        info.address = "192.168.1." + std::to_string(i);
        info.port = 7890 + i;
        info.memory_capacity = 1024 * 1024 * 1024;
        info.memory_used = 512 * 1024 * 1024;
        info.disk_capacity = 100ULL * 1024 * 1024 * 1024;
        info.disk_used = 50ULL * 1024 * 1024 * 1024;
        info.generation = i + 1;
        info.state = NodeInfo::Active;
        original.nodes.push_back(info);
    }
    
    auto encoded = Codec::encode(original, 1);
    auto [decoded_msg, header] = Codec::decode(encoded);
    
    REQUIRE(header.type == static_cast<uint16_t>(MessageType::GossipPush));
    
    auto& decoded = std::get<GossipPushMessage>(decoded_msg);
    REQUIRE(decoded.sender == original.sender);
    REQUIRE(decoded.nodes.size() == 3);
    
    for (size_t i = 0; i < decoded.nodes.size(); ++i) {
        REQUIRE(decoded.nodes[i].id == original.nodes[i].id);
        REQUIRE(decoded.nodes[i].port == original.nodes[i].port);
        REQUIRE(decoded.nodes[i].generation == original.nodes[i].generation);
    }
}
