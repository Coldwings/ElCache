#include <catch2/catch_test_macros.hpp>
#include "elcache/cluster.hpp"
#include "elcache/config.hpp"
#include <elio/io/io_context.hpp>

using namespace elcache;

TEST_CASE("HashRing operations", "[cluster]") {
    HashRing ring(100);  // 100 virtual nodes per physical node
    
    SECTION("Empty ring") {
        REQUIRE(ring.node_count() == 0);
        
        CacheKey key("test");
        auto nodes = ring.get_nodes(key, 1);
        REQUIRE(nodes.empty());
    }
    
    SECTION("Single node") {
        NodeId node1 = NodeId::generate();
        ring.add_node(node1);
        
        REQUIRE(ring.node_count() == 1);
        
        CacheKey key("test");
        auto nodes = ring.get_nodes(key, 1);
        REQUIRE(nodes.size() == 1);
        REQUIRE(nodes[0] == node1);
    }
    
    SECTION("Multiple nodes") {
        NodeId node1 = NodeId::generate();
        NodeId node2 = NodeId::generate();
        NodeId node3 = NodeId::generate();
        
        ring.add_node(node1);
        ring.add_node(node2);
        ring.add_node(node3);
        
        REQUIRE(ring.node_count() == 3);
        
        // Request more nodes than available
        CacheKey key("test");
        auto nodes = ring.get_nodes(key, 5);
        REQUIRE(nodes.size() == 3);
    }
    
    SECTION("Consistent mapping") {
        NodeId node1 = NodeId::generate();
        NodeId node2 = NodeId::generate();
        
        ring.add_node(node1);
        ring.add_node(node2);
        
        CacheKey key("consistent-key");
        
        // Same key should map to same node
        auto nodes1 = ring.get_nodes(key, 1);
        auto nodes2 = ring.get_nodes(key, 1);
        
        REQUIRE(nodes1[0] == nodes2[0]);
    }
    
    SECTION("Node removal") {
        NodeId node1 = NodeId::generate();
        NodeId node2 = NodeId::generate();
        
        ring.add_node(node1);
        ring.add_node(node2);
        REQUIRE(ring.node_count() == 2);
        
        ring.remove_node(node1);
        REQUIRE(ring.node_count() == 1);
        
        CacheKey key("test");
        auto nodes = ring.get_nodes(key, 1);
        REQUIRE(nodes.size() == 1);
        REQUIRE(nodes[0] == node2);
    }
    
    SECTION("Distribution balance") {
        // Add nodes
        std::vector<NodeId> node_ids;
        for (int i = 0; i < 5; ++i) {
            node_ids.push_back(NodeId::generate());
            ring.add_node(node_ids.back());
        }
        
        // Count how many keys map to each node
        std::map<NodeId, int> counts;
        for (int i = 0; i < 1000; ++i) {
            CacheKey key("key-" + std::to_string(i));
            auto nodes = ring.get_nodes(key, 1);
            counts[nodes[0]]++;
        }
        
        // Check that distribution is somewhat balanced
        // With 1000 keys and 5 nodes, expect ~200 each
        // Allow for some variance (50-350 per node)
        for (const auto& [node, count] : counts) {
            REQUIRE(count > 50);
            REQUIRE(count < 350);
        }
    }
}

TEST_CASE("ClusterNode operations", "[cluster]") {
    protocol::NodeInfo info;
    info.id = NodeId::generate();
    info.address = "192.168.1.100";
    info.port = 7890;
    info.memory_capacity = 1024 * 1024 * 1024;
    info.memory_used = 512 * 1024 * 1024;
    info.disk_capacity = 100ULL * 1024 * 1024 * 1024;
    info.disk_used = 50ULL * 1024 * 1024 * 1024;
    info.generation = 1;
    info.state = protocol::NodeInfo::Active;
    
    ClusterNode node(info);
    
    SECTION("Basic properties") {
        REQUIRE(node.id() == info.id);
        REQUIRE(node.address() == "192.168.1.100");
        REQUIRE(node.port() == 7890);
        REQUIRE(node.is_active());
    }
    
    SECTION("Available resources") {
        REQUIRE(node.available_memory() == 512 * 1024 * 1024);
        REQUIRE(node.available_disk() == 50ULL * 1024 * 1024 * 1024);
    }
    
    SECTION("Update with newer generation") {
        protocol::NodeInfo updated = info;
        updated.generation = 2;
        updated.memory_used = 768 * 1024 * 1024;
        
        node.update(updated);
        
        // Should accept update with higher generation
        REQUIRE(node.available_memory() == 256 * 1024 * 1024);
    }
    
    SECTION("Ignore update with older generation") {
        protocol::NodeInfo old = info;
        old.generation = 0;
        old.memory_used = 0;
        
        node.update(old);
        
        // Should ignore older generation
        REQUIRE(node.available_memory() == 512 * 1024 * 1024);
    }
}

TEST_CASE("Cluster initialization", "[cluster]") {
    Config config;
    config.network.bind_address = "127.0.0.1";
    config.network.cluster_port = 17890;  // Use non-standard port for testing
    config.memory.max_size = 64 * 1024 * 1024;
    config.disk.max_size = 0;  // Disable disk cache for test
    config.cluster.virtual_nodes = 50;
    config.cluster.replication_factor = 2;
    
    elio::io::io_context io_ctx;
    
    SECTION("Basic construction") {
        Cluster cluster(config, io_ctx);
        
        // Should have local node
        auto local = cluster.local_node();
        REQUIRE(local != nullptr);
        REQUIRE(local->is_active());
        REQUIRE(local->address() == "127.0.0.1");
        REQUIRE(local->port() == 17890);
    }
    
    SECTION("Initial stats") {
        Cluster cluster(config, io_ctx);
        
        auto stats = cluster.stats();
        REQUIRE(stats.total_nodes == 1);
        REQUIRE(stats.active_nodes == 1);
        REQUIRE(stats.total_memory_capacity == config.memory.max_size);
    }
    
    SECTION("Nodes for key routing") {
        Cluster cluster(config, io_ctx);
        
        CacheKey key("test-key");
        auto nodes = cluster.nodes_for_key(key);
        
        // With only 1 node, should return that node
        REQUIRE(nodes.size() == 1);
        REQUIRE(nodes[0] == cluster.local_node());
    }
    
    SECTION("Node event callbacks") {
        Cluster cluster(config, io_ctx);
        
        bool callback_fired = false;
        std::shared_ptr<ClusterNode> callback_node;
        bool callback_joined = false;
        
        cluster.on_node_change([&](const std::shared_ptr<ClusterNode>& node, bool joined) {
            callback_fired = true;
            callback_node = node;
            callback_joined = joined;
        });
        
        // Callbacks should be registered (but won't fire without new nodes)
        // Just verify registration doesn't crash
        REQUIRE_FALSE(callback_fired);
    }
}

TEST_CASE("Cluster node lookup", "[cluster]") {
    Config config;
    config.network.bind_address = "127.0.0.1";
    config.network.cluster_port = 17891;
    config.memory.max_size = 64 * 1024 * 1024;
    config.disk.max_size = 0;
    
    elio::io::io_context io_ctx;
    Cluster cluster(config, io_ctx);
    
    SECTION("Get local node by ID") {
        auto local = cluster.local_node();
        auto found = cluster.get_node(local->id());
        
        REQUIRE(found != nullptr);
        REQUIRE(found->id() == local->id());
    }
    
    SECTION("Get non-existent node") {
        NodeId fake_id = NodeId::generate();
        auto found = cluster.get_node(fake_id);
        
        REQUIRE(found == nullptr);
    }
    
    SECTION("All nodes list") {
        auto all = cluster.all_nodes();
        
        REQUIRE(all.size() == 1);
        REQUIRE(all[0] == cluster.local_node());
    }
    
    SECTION("Active nodes list") {
        auto active = cluster.active_nodes();
        
        REQUIRE(active.size() == 1);
        REQUIRE(active[0]->is_active());
    }
}

TEST_CASE("Protocol message encoding", "[cluster][protocol]") {
    SECTION("Hello message roundtrip") {
        protocol::HelloMessage hello;
        hello.sender.id = NodeId::generate();
        hello.sender.address = "10.0.0.1";
        hello.sender.port = 7890;
        hello.sender.memory_capacity = 1024 * 1024 * 1024;
        hello.sender.disk_capacity = 100ULL * 1024 * 1024 * 1024;
        hello.sender.generation = 1;
        hello.sender.state = protocol::NodeInfo::Active;
        hello.cluster_name = "test-cluster";
        
        auto encoded = protocol::Codec::encode(protocol::Message{hello});
        REQUIRE(!encoded.empty());
        
        auto [decoded, header] = protocol::Codec::decode(encoded);
        REQUIRE(std::holds_alternative<protocol::HelloMessage>(decoded));
        
        auto& result = std::get<protocol::HelloMessage>(decoded);
        REQUIRE(result.sender.id == hello.sender.id);
        REQUIRE(result.cluster_name == "test-cluster");
    }
    
    SECTION("Gossip push message roundtrip") {
        protocol::GossipPushMessage push;
        push.sender = NodeId::generate();
        
        protocol::NodeInfo node1;
        node1.id = NodeId::generate();
        node1.address = "192.168.1.1";
        node1.port = 7890;
        node1.generation = 5;
        node1.state = protocol::NodeInfo::Active;
        push.nodes.push_back(node1);
        
        auto encoded = protocol::Codec::encode(protocol::Message{push});
        REQUIRE(!encoded.empty());
        
        auto [decoded, header] = protocol::Codec::decode(encoded);
        REQUIRE(std::holds_alternative<protocol::GossipPushMessage>(decoded));
        
        auto& result = std::get<protocol::GossipPushMessage>(decoded);
        REQUIRE(result.sender == push.sender);
        REQUIRE(result.nodes.size() == 1);
        REQUIRE(result.nodes[0].address == "192.168.1.1");
    }
    
    SECTION("Get/Put message roundtrip") {
        protocol::GetMessage get_msg;
        get_msg.key = CacheKey("test-key");
        get_msg.metadata_only = false;
        get_msg.range = Range{100, 200};
        
        auto encoded = protocol::Codec::encode(protocol::Message{get_msg});
        auto [decoded, header] = protocol::Codec::decode(encoded);
        
        REQUIRE(std::holds_alternative<protocol::GetMessage>(decoded));
        auto& result = std::get<protocol::GetMessage>(decoded);
        REQUIRE(result.key.str() == "test-key");
        REQUIRE(result.range.has_value());
        REQUIRE(result.range->offset == 100);
        REQUIRE(result.range->length == 200);
    }
}
