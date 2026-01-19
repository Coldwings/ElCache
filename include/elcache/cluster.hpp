#pragma once

#include "types.hpp"
#include "config.hpp"
#include "cache.hpp"
#include "protocol.hpp"
#include <elio/coro/task.hpp>
#include <elio/sync/primitives.hpp>
#include <elio/runtime/scheduler.hpp>
#include <elio/io/io_context.hpp>
#include <elio/net/tcp.hpp>
#include <elio/time/timer.hpp>
#include <memory>
#include <functional>
#include <map>
#include <set>
#include <unordered_map>

namespace elcache {

// Forward declarations
class GossipProtocol;

// Connection to a remote node using Elio TCP
class Connection : public std::enable_shared_from_this<Connection> {
public:
    Connection(elio::net::tcp_stream stream, elio::io::io_context& io_ctx);
    ~Connection();
    
    // Send a message
    elio::coro::task<Status> send(const protocol::Message& msg);
    
    // Receive a message
    elio::coro::task<std::optional<protocol::Message>> receive();
    
    // Check if connected
    bool is_connected() const { return connected_; }
    
    // Close connection
    elio::coro::task<void> close();
    
    // Get peer address
    std::string peer_address() const;
    
private:
    elio::net::tcp_stream stream_;
    elio::io::io_context& io_ctx_;
    std::atomic<bool> connected_{true};
    elio::sync::mutex send_mutex_;
    elio::sync::mutex recv_mutex_;
    
    // Buffer for receiving messages
    ByteBuffer recv_buffer_;
};

// Cluster node representation
class ClusterNode {
public:
    ClusterNode(const protocol::NodeInfo& info);
    
    NodeId id() const { return info_.id; }
    const std::string& address() const { return info_.address; }
    uint16_t port() const { return info_.port; }
    bool is_active() const { return info_.state == protocol::NodeInfo::Active; }
    
    // Resource availability
    uint64_t available_memory() const {
        return info_.memory_capacity - info_.memory_used;
    }
    uint64_t available_disk() const {
        return info_.disk_capacity - info_.disk_used;
    }
    
    // Update from gossip
    void update(const protocol::NodeInfo& info);
    
    // Connection management
    void set_connection(std::shared_ptr<Connection> conn);
    std::shared_ptr<Connection> connection() const { return connection_; }
    bool is_connected() const { return connection_ != nullptr; }
    
    // Info access
    const protocol::NodeInfo& info() const { return info_; }
    
private:
    protocol::NodeInfo info_;
    std::shared_ptr<Connection> connection_;
    mutable elio::sync::mutex mutex_;
};

// Consistent hash ring for data distribution
class HashRing {
public:
    HashRing(size_t virtual_nodes = 150);
    
    void add_node(const NodeId& id);
    void remove_node(const NodeId& id);
    
    // Get N nodes responsible for a key
    std::vector<NodeId> get_nodes(const CacheKey& key, size_t count = 1) const;
    std::vector<NodeId> get_nodes(const ChunkId& chunk, size_t count = 1) const;
    
    // Check if a node is responsible for a key
    bool is_responsible(const NodeId& node, const CacheKey& key) const;
    
    size_t node_count() const { return nodes_.size(); }
    
private:
    size_t virtual_nodes_;
    std::map<uint64_t, NodeId> ring_;
    std::set<NodeId> nodes_;
    mutable elio::sync::shared_mutex mutex_;
    
    uint64_t hash_point(const NodeId& id, size_t replica) const;
};

// Cluster management
class Cluster {
public:
    Cluster(const Config& config, elio::io::io_context& io_ctx);
    ~Cluster();
    
    // Lifecycle (requires scheduler for background tasks)
    elio::coro::task<Status> start(elio::runtime::scheduler& sched);
    elio::coro::task<void> stop();
    
    // Node access
    std::shared_ptr<ClusterNode> local_node() const { return local_node_; }
    std::shared_ptr<ClusterNode> get_node(const NodeId& id) const;
    std::vector<std::shared_ptr<ClusterNode>> all_nodes() const;
    std::vector<std::shared_ptr<ClusterNode>> active_nodes() const;
    
    // Routing
    std::vector<std::shared_ptr<ClusterNode>> nodes_for_key(
        const CacheKey& key, size_t count = 0) const;  // 0 = replication factor
    std::vector<std::shared_ptr<ClusterNode>> nodes_for_chunk(
        const ChunkId& chunk, size_t count = 0) const;
    
    // Remote operations
    elio::coro::task<ReadResult> remote_get(
        const std::shared_ptr<ClusterNode>& node,
        const CacheKey& key,
        const ReadOptions& opts);
    
    elio::coro::task<Status> remote_put(
        const std::shared_ptr<ClusterNode>& node,
        const CacheKey& key,
        ByteView value,
        const WriteOptions& opts);
    
    elio::coro::task<Status> remote_delete(
        const std::shared_ptr<ClusterNode>& node,
        const CacheKey& key);
    
    elio::coro::task<AvailabilityResult> remote_check(
        const std::shared_ptr<ClusterNode>& node,
        const CacheKey& key,
        std::optional<Range> range);
    
    // Chunk-level operations
    elio::coro::task<SharedChunk> remote_get_chunk(
        const std::shared_ptr<ClusterNode>& node,
        const ChunkId& id);
    
    elio::coro::task<Status> remote_put_chunk(
        const std::shared_ptr<ClusterNode>& node,
        SharedChunk chunk);
    
    elio::coro::task<std::vector<bool>> remote_has_chunks(
        const std::shared_ptr<ClusterNode>& node,
        const std::vector<ChunkId>& ids);
    
    // Events
    using NodeEventCallback = std::function<void(const std::shared_ptr<ClusterNode>&, bool joined)>;
    void on_node_change(NodeEventCallback callback);
    
    // Statistics
    struct Stats {
        size_t total_nodes = 0;
        size_t active_nodes = 0;
        uint64_t total_memory_capacity = 0;
        uint64_t total_disk_capacity = 0;
        uint64_t total_memory_used = 0;
        uint64_t total_disk_used = 0;
        uint64_t messages_sent = 0;
        uint64_t messages_received = 0;
    };
    
    Stats stats() const;
    
private:
    friend class GossipProtocol;
    
    Config config_;
    elio::io::io_context& io_ctx_;
    elio::runtime::scheduler* sched_ = nullptr;
    std::shared_ptr<ClusterNode> local_node_;
    
    // TCP listener for incoming connections
    std::optional<elio::net::tcp_listener> listener_;
    
    mutable elio::sync::shared_mutex nodes_mutex_;
    std::unordered_map<NodeId, std::shared_ptr<ClusterNode>> nodes_;
    
    std::unique_ptr<HashRing> hash_ring_;
    std::unique_ptr<GossipProtocol> gossip_;
    
    std::vector<NodeEventCallback> event_callbacks_;
    elio::sync::mutex callback_mutex_;
    
    std::atomic<bool> running_{false};
    
    // Internal helpers
    elio::coro::task<void> connect_to_seeds();
    elio::coro::task<void> accept_loop();
    elio::coro::task<void> handle_connection(std::shared_ptr<Connection> conn);
    elio::coro::task<std::shared_ptr<Connection>> connect_to_node(
        const std::string& address, uint16_t port);
    void handle_node_update(const protocol::NodeInfo& info);
    void notify_node_change(const std::shared_ptr<ClusterNode>& node, bool joined);
};

// Gossip protocol implementation
class GossipProtocol {
public:
    GossipProtocol(Cluster& cluster, const ClusterConfig& config, 
                   elio::io::io_context& io_ctx);
    ~GossipProtocol();
    
    elio::coro::task<void> start(elio::runtime::scheduler& sched);
    elio::coro::task<void> stop();
    
    // Process incoming gossip
    void handle_push(const protocol::GossipPushMessage& msg);
    void handle_pull(const protocol::GossipPullMessage& msg);
    void handle_ack(const protocol::GossipAckMessage& msg);
    
    // Trigger gossip round
    elio::coro::task<void> gossip_round();
    
private:
    Cluster& cluster_;
    ClusterConfig config_;
    elio::io::io_context& io_ctx_;
    elio::runtime::scheduler* sched_ = nullptr;
    std::atomic<bool> running_{false};
    
    // Failure detection
    std::unordered_map<NodeId, TimePoint> last_heard_;
    elio::sync::mutex failure_mutex_;
    
    elio::coro::task<void> gossip_loop();
    elio::coro::task<void> failure_detection_loop();
    void check_failures();
};

}  // namespace elcache
