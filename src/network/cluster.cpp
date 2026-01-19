#include "elcache/cluster.hpp"
#include "elcache/protocol.hpp"
#include <algorithm>
#include <random>

namespace elcache {

// Connection implementation

Connection::Connection(elio::net::tcp_stream stream, elio::io::io_context& io_ctx)
    : stream_(std::move(stream))
    , io_ctx_(io_ctx)
{
    recv_buffer_.reserve(64 * 1024);  // 64KB initial buffer
}

Connection::~Connection() = default;

elio::coro::task<Status> Connection::send(const protocol::Message& msg) {
    co_await send_mutex_.lock();
    
    // Serialize message using Codec
    auto data = protocol::Codec::encode(msg);
    
    size_t sent = 0;
    while (sent < data.size()) {
        auto result = co_await stream_.write(data.data() + sent, data.size() - sent);
        if (result.result <= 0) {
            connected_ = false;
            send_mutex_.unlock();
            co_return Status::error(ErrorCode::NetworkError, "Failed to send message");
        }
        sent += result.result;
    }
    
    send_mutex_.unlock();
    co_return Status::make_ok();
}

elio::coro::task<std::optional<protocol::Message>> Connection::receive() {
    co_await recv_mutex_.lock();
    
    // Read header first (MessageHeader::SIZE = 24 bytes)
    ByteBuffer header(protocol::MessageHeader::SIZE);
    size_t header_read = 0;
    
    while (header_read < protocol::MessageHeader::SIZE) {
        auto result = co_await stream_.read(header.data() + header_read, 
                                            protocol::MessageHeader::SIZE - header_read);
        if (result.result <= 0) {
            connected_ = false;
            recv_mutex_.unlock();
            co_return std::nullopt;
        }
        header_read += result.result;
    }
    
    // Parse header to get message length
    auto msg_header = protocol::Codec::parse_header(header);
    if (msg_header.magic != 0x454C4341) {  // "ELCA"
        recv_mutex_.unlock();
        co_return std::nullopt;
    }
    
    // Read body
    ByteBuffer body(msg_header.length);
    size_t body_read = 0;
    
    while (body_read < msg_header.length) {
        auto result = co_await stream_.read(body.data() + body_read, 
                                            msg_header.length - body_read);
        if (result.result <= 0) {
            connected_ = false;
            recv_mutex_.unlock();
            co_return std::nullopt;
        }
        body_read += result.result;
    }
    
    // Decode full message
    ByteBuffer full_msg;
    full_msg.reserve(header.size() + body.size());
    full_msg.insert(full_msg.end(), header.begin(), header.end());
    full_msg.insert(full_msg.end(), body.begin(), body.end());
    
    try {
        auto [msg, hdr] = protocol::Codec::decode(full_msg);
        recv_mutex_.unlock();
        co_return msg;
    } catch (...) {
        recv_mutex_.unlock();
        co_return std::nullopt;
    }
}

elio::coro::task<void> Connection::close() {
    if (connected_) {
        connected_ = false;
        co_await stream_.close();
    }
}

std::string Connection::peer_address() const {
    auto addr = stream_.peer_address();
    if (addr) {
        return addr->to_string();
    }
    return "unknown";
}

// ClusterNode implementation

ClusterNode::ClusterNode(const protocol::NodeInfo& info)
    : info_(info)
{}

void ClusterNode::update(const protocol::NodeInfo& info) {
    while (!mutex_.try_lock()) {}
    if (info.generation > info_.generation) {
        info_ = info;
    }
    mutex_.unlock();
}

void ClusterNode::set_connection(std::shared_ptr<Connection> conn) {
    while (!mutex_.try_lock()) {}
    connection_ = std::move(conn);
    mutex_.unlock();
}

// HashRing implementation

HashRing::HashRing(size_t virtual_nodes)
    : virtual_nodes_(virtual_nodes)
{}

uint64_t HashRing::hash_point(const NodeId& id, size_t replica) const {
    // Simple hash combining node ID with replica number
    uint64_t combined = id.value() ^ (replica * 0x9E3779B97F4A7C15ULL);
    // Mix the bits
    combined ^= combined >> 33;
    combined *= 0xff51afd7ed558ccdULL;
    combined ^= combined >> 33;
    combined *= 0xc4ceb9fe1a85ec53ULL;
    combined ^= combined >> 33;
    return combined;
}

void HashRing::add_node(const NodeId& id) {
    while (!mutex_.try_lock()) {}
    
    if (nodes_.count(id)) {
        mutex_.unlock();
        return;
    }
    
    nodes_.insert(id);
    for (size_t i = 0; i < virtual_nodes_; ++i) {
        ring_[hash_point(id, i)] = id;
    }
    mutex_.unlock();
}

void HashRing::remove_node(const NodeId& id) {
    while (!mutex_.try_lock()) {}
    
    if (!nodes_.count(id)) {
        mutex_.unlock();
        return;
    }
    
    nodes_.erase(id);
    for (size_t i = 0; i < virtual_nodes_; ++i) {
        ring_.erase(hash_point(id, i));
    }
    mutex_.unlock();
}

std::vector<NodeId> HashRing::get_nodes(const CacheKey& key, size_t count) const {
    while (!mutex_.try_lock_shared()) {}
    
    if (ring_.empty()) {
        mutex_.unlock_shared();
        return {};
    }
    
    count = std::min(count, nodes_.size());
    if (count == 0) count = 1;
    
    std::vector<NodeId> result;
    std::set<NodeId> seen;
    
    // Find position in ring
    uint64_t hash = key.hash();
    auto it = ring_.lower_bound(hash);
    
    // Walk the ring to find unique nodes
    while (result.size() < count) {
        if (it == ring_.end()) {
            it = ring_.begin();
        }
        
        if (!seen.count(it->second)) {
            seen.insert(it->second);
            result.push_back(it->second);
        }
        
        ++it;
        
        // Safety: don't infinite loop if ring is smaller than requested
        if (seen.size() >= nodes_.size()) break;
    }
    
    mutex_.unlock_shared();
    return result;
}

std::vector<NodeId> HashRing::get_nodes(const ChunkId& chunk, size_t count) const {
    // Use chunk hash directly
    while (!mutex_.try_lock_shared()) {}
    
    if (ring_.empty()) {
        mutex_.unlock_shared();
        return {};
    }
    
    count = std::min(count, nodes_.size());
    if (count == 0) count = 1;
    
    std::vector<NodeId> result;
    std::set<NodeId> seen;
    
    uint64_t hash = chunk.hash().low;
    auto it = ring_.lower_bound(hash);
    
    while (result.size() < count) {
        if (it == ring_.end()) {
            it = ring_.begin();
        }
        
        if (!seen.count(it->second)) {
            seen.insert(it->second);
            result.push_back(it->second);
        }
        
        ++it;
        if (seen.size() >= nodes_.size()) break;
    }
    
    mutex_.unlock_shared();
    return result;
}

bool HashRing::is_responsible(const NodeId& node, const CacheKey& key) const {
    auto nodes = get_nodes(key, 1);
    return !nodes.empty() && nodes[0] == node;
}

// Cluster implementation

Cluster::Cluster(const Config& config, elio::io::io_context& io_ctx)
    : config_(config)
    , io_ctx_(io_ctx)
    , hash_ring_(std::make_unique<HashRing>(config.cluster.virtual_nodes))
{
    // Create local node
    protocol::NodeInfo local_info;
    local_info.id = NodeId::generate();
    local_info.address = config.network.bind_address;
    local_info.port = config.network.cluster_port;
    local_info.memory_capacity = config.memory.max_size;
    local_info.disk_capacity = config.disk.max_size;
    local_info.generation = 1;
    local_info.state = protocol::NodeInfo::Active;
    
    local_node_ = std::make_shared<ClusterNode>(local_info);
    
    // Add self to ring
    hash_ring_->add_node(local_info.id);
    nodes_[local_info.id] = local_node_;
    
    // Create gossip protocol
    gossip_ = std::make_unique<GossipProtocol>(*this, config.cluster, io_ctx);
}

Cluster::~Cluster() = default;

elio::coro::task<Status> Cluster::start(elio::runtime::scheduler& sched) {
    running_ = true;
    sched_ = &sched;
    
    // Bind TCP listener for incoming cluster connections
    elio::net::ipv4_address listen_addr(config_.network.bind_address, 
                                         config_.network.cluster_port);
    
    auto listener_result = elio::net::tcp_listener::bind(listen_addr, io_ctx_);
    if (!listener_result) {
        co_return Status::error(ErrorCode::NetworkError, 
            "Failed to bind cluster port " + std::to_string(config_.network.cluster_port));
    }
    
    listener_ = std::move(*listener_result);
    
    // Spawn accept loop
    auto accept_task = accept_loop();
    sched.spawn(accept_task.release());
    
    // Connect to seed nodes
    if (!config_.cluster.seed_nodes.empty()) {
        co_await connect_to_seeds();
    }
    
    // Start gossip with scheduler for periodic tasks
    co_await gossip_->start(sched);
    
    co_return Status::make_ok();
}

elio::coro::task<void> Cluster::stop() {
    running_ = false;
    
    // Close listener
    if (listener_) {
        listener_->close();
    }
    
    // Close all connections
    while (!nodes_mutex_.try_lock()) {}
    for (auto& [id, node] : nodes_) {
        auto conn = node->connection();
        if (conn) {
            co_await conn->close();
        }
    }
    nodes_mutex_.unlock();
    
    co_await gossip_->stop();
}

std::shared_ptr<ClusterNode> Cluster::get_node(const NodeId& id) const {
    while (!nodes_mutex_.try_lock_shared()) {}
    auto it = nodes_.find(id);
    if (it != nodes_.end()) {
        auto result = it->second;
        nodes_mutex_.unlock_shared();
        return result;
    }
    nodes_mutex_.unlock_shared();
    return nullptr;
}

std::vector<std::shared_ptr<ClusterNode>> Cluster::all_nodes() const {
    while (!nodes_mutex_.try_lock_shared()) {}
    std::vector<std::shared_ptr<ClusterNode>> result;
    result.reserve(nodes_.size());
    for (const auto& [id, node] : nodes_) {
        result.push_back(node);
    }
    nodes_mutex_.unlock_shared();
    return result;
}

std::vector<std::shared_ptr<ClusterNode>> Cluster::active_nodes() const {
    while (!nodes_mutex_.try_lock_shared()) {}
    std::vector<std::shared_ptr<ClusterNode>> result;
    for (const auto& [id, node] : nodes_) {
        if (node->is_active()) {
            result.push_back(node);
        }
    }
    nodes_mutex_.unlock_shared();
    return result;
}

std::vector<std::shared_ptr<ClusterNode>> Cluster::nodes_for_key(
    const CacheKey& key, size_t count) const
{
    if (count == 0) {
        count = config_.cluster.replication_factor;
    }
    
    auto node_ids = hash_ring_->get_nodes(key, count);
    
    std::vector<std::shared_ptr<ClusterNode>> result;
    result.reserve(node_ids.size());
    
    for (const auto& id : node_ids) {
        auto node = get_node(id);
        if (node) {
            result.push_back(node);
        }
    }
    
    return result;
}

std::vector<std::shared_ptr<ClusterNode>> Cluster::nodes_for_chunk(
    const ChunkId& chunk, size_t count) const
{
    if (count == 0) {
        count = config_.cluster.replication_factor;
    }
    
    auto node_ids = hash_ring_->get_nodes(chunk, count);
    
    std::vector<std::shared_ptr<ClusterNode>> result;
    for (const auto& id : node_ids) {
        auto node = get_node(id);
        if (node) {
            result.push_back(node);
        }
    }
    
    return result;
}

// Remote operations - full implementations using Elio networking

elio::coro::task<ReadResult> Cluster::remote_get(
    const std::shared_ptr<ClusterNode>& node,
    const CacheKey& key,
    const ReadOptions& opts)
{
    ReadResult result;
    result.result = CacheResult::Miss;
    
    auto conn = node->connection();
    if (!conn || !conn->is_connected()) {
        // Try to establish connection
        conn = co_await connect_to_node(node->address(), node->port());
        if (!conn) {
            co_return result;
        }
        node->set_connection(conn);
    }
    
    // Build and send Get message
    protocol::GetMessage get_msg;
    get_msg.key = key;
    get_msg.metadata_only = false;
    get_msg.range = opts.range;
    
    auto status = co_await conn->send(protocol::Message{get_msg});
    if (!status) {
        co_return result;
    }
    
    // Wait for response
    auto response = co_await conn->receive();
    if (!response || !std::holds_alternative<protocol::GetResponseMessage>(*response)) {
        co_return result;
    }
    
    auto& get_resp = std::get<protocol::GetResponseMessage>(*response);
    result.result = get_resp.result;
    result.metadata = get_resp.metadata;
    result.data = std::move(get_resp.data);
    result.actual_range = get_resp.actual_range;
    
    co_return result;
}

elio::coro::task<Status> Cluster::remote_put(
    const std::shared_ptr<ClusterNode>& node,
    const CacheKey& key,
    ByteView value,
    const WriteOptions& opts)
{
    auto conn = node->connection();
    if (!conn || !conn->is_connected()) {
        conn = co_await connect_to_node(node->address(), node->port());
        if (!conn) {
            co_return Status::error(ErrorCode::NetworkError, "Failed to connect");
        }
        node->set_connection(conn);
    }
    
    // Build and send Put message
    protocol::PutMessage put_msg;
    put_msg.key = key;
    put_msg.data = ByteBuffer(value.begin(), value.end());
    put_msg.ttl = opts.ttl;
    put_msg.flags = opts.flags;
    
    auto status = co_await conn->send(protocol::Message{put_msg});
    if (!status) {
        co_return status;
    }
    
    // Wait for response
    auto response = co_await conn->receive();
    if (!response || !std::holds_alternative<protocol::PutResponseMessage>(*response)) {
        co_return Status::error(ErrorCode::NetworkError, "Invalid response");
    }
    
    auto& put_resp = std::get<protocol::PutResponseMessage>(*response);
    if (put_resp.code != ErrorCode::Ok) {
        co_return Status::error(put_resp.code, put_resp.message);
    }
    
    co_return Status::make_ok();
}

elio::coro::task<Status> Cluster::remote_delete(
    const std::shared_ptr<ClusterNode>& node,
    const CacheKey& key)
{
    auto conn = node->connection();
    if (!conn || !conn->is_connected()) {
        conn = co_await connect_to_node(node->address(), node->port());
        if (!conn) {
            co_return Status::error(ErrorCode::NetworkError, "Failed to connect");
        }
        node->set_connection(conn);
    }
    
    // Build and send Delete message
    protocol::DeleteMessage del_msg;
    del_msg.key = key;
    
    auto status = co_await conn->send(protocol::Message{del_msg});
    if (!status) {
        co_return status;
    }
    
    // Wait for response
    auto response = co_await conn->receive();
    if (!response || !std::holds_alternative<protocol::DeleteResponseMessage>(*response)) {
        co_return Status::error(ErrorCode::NetworkError, "Invalid response");
    }
    
    auto& del_resp = std::get<protocol::DeleteResponseMessage>(*response);
    if (del_resp.code != ErrorCode::Ok) {
        co_return Status::error(del_resp.code, "Delete failed");
    }
    
    co_return Status::make_ok();
}

elio::coro::task<AvailabilityResult> Cluster::remote_check(
    const std::shared_ptr<ClusterNode>& node,
    const CacheKey& key,
    std::optional<Range> range)
{
    AvailabilityResult result;
    result.exists = false;
    
    auto conn = node->connection();
    if (!conn || !conn->is_connected()) {
        conn = co_await connect_to_node(node->address(), node->port());
        if (!conn) {
            co_return result;
        }
        node->set_connection(conn);
    }
    
    // Build and send Check message
    protocol::CheckMessage check_msg;
    check_msg.key = key;
    check_msg.range = range;
    
    auto status = co_await conn->send(protocol::Message{check_msg});
    if (!status) {
        co_return result;
    }
    
    // Wait for response
    auto response = co_await conn->receive();
    if (!response || !std::holds_alternative<protocol::CheckResponseMessage>(*response)) {
        co_return result;
    }
    
    auto& check_resp = std::get<protocol::CheckResponseMessage>(*response);
    result.exists = check_resp.exists;
    result.metadata = check_resp.metadata;
    result.available_ranges = check_resp.available_ranges;
    
    co_return result;
}

elio::coro::task<SharedChunk> Cluster::remote_get_chunk(
    const std::shared_ptr<ClusterNode>& node,
    const ChunkId& id)
{
    auto conn = node->connection();
    if (!conn || !conn->is_connected()) {
        conn = co_await connect_to_node(node->address(), node->port());
        if (!conn) {
            co_return nullptr;
        }
        node->set_connection(conn);
    }
    
    // Build and send GetChunk message
    protocol::GetChunkMessage msg;
    msg.id = id;
    
    auto status = co_await conn->send(protocol::Message{msg});
    if (!status) {
        co_return nullptr;
    }
    
    // Wait for response
    auto response = co_await conn->receive();
    if (!response || !std::holds_alternative<protocol::GetChunkResponseMessage>(*response)) {
        co_return nullptr;
    }
    
    auto& resp = std::get<protocol::GetChunkResponseMessage>(*response);
    if (!resp.found || resp.data.empty()) {
        co_return nullptr;
    }
    
    // Create chunk from received data
    co_return std::make_shared<Chunk>(std::move(resp.data));
}

elio::coro::task<Status> Cluster::remote_put_chunk(
    const std::shared_ptr<ClusterNode>& node,
    SharedChunk chunk)
{
    if (!chunk) {
        co_return Status::error(ErrorCode::InvalidArgument, "Null chunk");
    }
    
    auto conn = node->connection();
    if (!conn || !conn->is_connected()) {
        conn = co_await connect_to_node(node->address(), node->port());
        if (!conn) {
            co_return Status::error(ErrorCode::NetworkError, "Failed to connect");
        }
        node->set_connection(conn);
    }
    
    // Build and send PutChunk message
    protocol::PutChunkMessage msg;
    msg.id = chunk->id();
    msg.data = ByteBuffer(chunk->data().begin(), chunk->data().end());
    
    auto status = co_await conn->send(protocol::Message{msg});
    if (!status) {
        co_return status;
    }
    
    // Wait for response
    auto response = co_await conn->receive();
    if (!response || !std::holds_alternative<protocol::PutChunkResponseMessage>(*response)) {
        co_return Status::error(ErrorCode::NetworkError, "Invalid response");
    }
    
    auto& resp = std::get<protocol::PutChunkResponseMessage>(*response);
    if (resp.code != ErrorCode::Ok) {
        co_return Status::error(resp.code, "PutChunk failed");
    }
    
    co_return Status::make_ok();
}

elio::coro::task<std::vector<bool>> Cluster::remote_has_chunks(
    const std::shared_ptr<ClusterNode>& node,
    const std::vector<ChunkId>& ids)
{
    std::vector<bool> result(ids.size(), false);
    
    auto conn = node->connection();
    if (!conn || !conn->is_connected()) {
        conn = co_await connect_to_node(node->address(), node->port());
        if (!conn) {
            co_return result;
        }
        node->set_connection(conn);
    }
    
    // Build and send HasChunk message
    protocol::HasChunkMessage msg;
    msg.ids = ids;
    
    auto status = co_await conn->send(protocol::Message{msg});
    if (!status) {
        co_return result;
    }
    
    // Wait for response
    auto response = co_await conn->receive();
    if (!response || !std::holds_alternative<protocol::HasChunkResponseMessage>(*response)) {
        co_return result;
    }
    
    auto& resp = std::get<protocol::HasChunkResponseMessage>(*response);
    if (resp.exists.size() == ids.size()) {
        result = resp.exists;
    }
    
    co_return result;
}

void Cluster::on_node_change(NodeEventCallback callback) {
    while (!callback_mutex_.try_lock()) {}
    event_callbacks_.push_back(std::move(callback));
    callback_mutex_.unlock();
}

Cluster::Stats Cluster::stats() const {
    Stats s;
    
    while (!nodes_mutex_.try_lock_shared()) {}
    s.total_nodes = nodes_.size();
    
    for (const auto& [id, node] : nodes_) {
        if (node->is_active()) {
            s.active_nodes++;
        }
        const auto& info = node->info();
        s.total_memory_capacity += info.memory_capacity;
        s.total_disk_capacity += info.disk_capacity;
        s.total_memory_used += info.memory_used;
        s.total_disk_used += info.disk_used;
    }
    nodes_mutex_.unlock_shared();
    
    return s;
}

elio::coro::task<void> Cluster::connect_to_seeds() {
    for (const auto& seed : config_.cluster.seed_nodes) {
        // Parse seed address (format: host:port)
        auto colon = seed.rfind(':');
        if (colon == std::string::npos) continue;
        
        std::string host = seed.substr(0, colon);
        uint16_t port = std::stoi(seed.substr(colon + 1));
        
        // Connect to seed node
        auto conn = co_await connect_to_node(host, port);
        if (!conn) {
            continue;  // Failed to connect, try next seed
        }
        
        // Send Hello message
        protocol::HelloMessage hello;
        hello.sender = local_node_->info();
        hello.cluster_name = config_.cluster.cluster_name;
        
        auto status = co_await conn->send(protocol::Message{hello});
        if (!status) {
            co_await conn->close();
            continue;
        }
        
        // Wait for HelloAck
        auto response = co_await conn->receive();
        if (!response || !std::holds_alternative<protocol::HelloAckMessage>(*response)) {
            co_await conn->close();
            continue;
        }
        
        auto& ack = std::get<protocol::HelloAckMessage>(*response);
        
        if (!ack.accepted) {
            co_await conn->close();
            continue;
        }
        
        // Add node to cluster
        handle_node_update(ack.sender);
        
        // Set connection on the node
        auto node = get_node(ack.sender.id);
        if (node) {
            node->set_connection(conn);
            
            // Spawn connection handler
            if (sched_) {
                auto handler = handle_connection(conn);
                sched_->spawn(handler.release());
            }
        }
    }
    co_return;
}

elio::coro::task<void> Cluster::accept_loop() {
    while (running_ && listener_) {
        auto stream_result = co_await listener_->accept();
        if (!stream_result) {
            if (running_) {
                // Log error but continue accepting
            }
            continue;
        }
        
        // Create connection
        auto conn = std::make_shared<Connection>(std::move(*stream_result), io_ctx_);
        
        // Spawn handler for this connection
        if (sched_) {
            auto handler = handle_connection(conn);
            sched_->spawn(handler.release());
        }
    }
}

elio::coro::task<void> Cluster::handle_connection(std::shared_ptr<Connection> conn) {
    // First message should be Hello
    auto msg = co_await conn->receive();
    if (!msg) {
        co_await conn->close();
        co_return;
    }
    
    if (std::holds_alternative<protocol::HelloMessage>(*msg)) {
        auto& hello = std::get<protocol::HelloMessage>(*msg);
        
        // Add/update node
        handle_node_update(hello.sender);
        
        auto node = get_node(hello.sender.id);
        if (node) {
            node->set_connection(conn);
        }
        
        // Send HelloAck
        protocol::HelloAckMessage ack;
        ack.sender = local_node_->info();
        ack.accepted = true;
        
        co_await conn->send(protocol::Message{ack});
    }
    
    // Handle subsequent messages
    while (running_ && conn->is_connected()) {
        auto msg = co_await conn->receive();
        if (!msg) {
            break;
        }
        
        // Dispatch based on message type
        std::visit([this](auto&& m) {
            using T = std::decay_t<decltype(m)>;
            if constexpr (std::is_same_v<T, protocol::GossipPushMessage>) {
                gossip_->handle_push(m);
            } else if constexpr (std::is_same_v<T, protocol::GossipPullMessage>) {
                gossip_->handle_pull(m);
            } else if constexpr (std::is_same_v<T, protocol::GossipAckMessage>) {
                gossip_->handle_ack(m);
            }
            // Handle other message types...
        }, *msg);
    }
    
    co_await conn->close();
}

elio::coro::task<std::shared_ptr<Connection>> Cluster::connect_to_node(
    const std::string& address, uint16_t port)
{
    elio::net::ipv4_address addr(address, port);
    
    auto stream_result = co_await elio::net::tcp_connect(io_ctx_, addr);
    if (!stream_result) {
        co_return nullptr;
    }
    
    co_return std::make_shared<Connection>(std::move(*stream_result), io_ctx_);
}

void Cluster::handle_node_update(const protocol::NodeInfo& info) {
    while (!nodes_mutex_.try_lock()) {}
    
    auto it = nodes_.find(info.id);
    if (it != nodes_.end()) {
        it->second->update(info);
        nodes_mutex_.unlock();
    } else {
        // New node
        auto node = std::make_shared<ClusterNode>(info);
        nodes_[info.id] = node;
        hash_ring_->add_node(info.id);
        
        nodes_mutex_.unlock();
        notify_node_change(node, true);
    }
}

void Cluster::notify_node_change(const std::shared_ptr<ClusterNode>& node, bool joined) {
    while (!callback_mutex_.try_lock()) {}
    auto callbacks = event_callbacks_;  // Copy to avoid holding lock during callbacks
    callback_mutex_.unlock();
    
    for (const auto& cb : callbacks) {
        cb(node, joined);
    }
}

// GossipProtocol implementation

GossipProtocol::GossipProtocol(Cluster& cluster, const ClusterConfig& config,
                               elio::io::io_context& io_ctx)
    : cluster_(cluster)
    , config_(config)
    , io_ctx_(io_ctx)
{}

GossipProtocol::~GossipProtocol() = default;

elio::coro::task<void> GossipProtocol::start(elio::runtime::scheduler& sched) {
    running_ = true;
    sched_ = &sched;
    
    // Spawn periodic gossip loop
    auto gossip_task = gossip_loop();
    sched.spawn(gossip_task.release());
    
    // Spawn failure detection loop
    auto failure_task = failure_detection_loop();
    sched.spawn(failure_task.release());
    
    co_return;
}

elio::coro::task<void> GossipProtocol::stop() {
    running_ = false;
    co_return;
}

elio::coro::task<void> GossipProtocol::gossip_loop() {
    while (running_) {
        // Wait for gossip interval
        co_await elio::time::sleep_for(io_ctx_, config_.gossip_interval);
        
        if (!running_) break;
        
        // Perform gossip round
        co_await gossip_round();
    }
}

elio::coro::task<void> GossipProtocol::failure_detection_loop() {
    while (running_) {
        // Check every half of the failure timeout
        auto check_interval = config_.failure_detection_timeout / 2;
        co_await elio::time::sleep_for(io_ctx_, check_interval);
        
        if (!running_) break;
        
        check_failures();
    }
}

void GossipProtocol::handle_push(const protocol::GossipPushMessage& msg) {
    for (const auto& info : msg.nodes) {
        cluster_.handle_node_update(info);
        
        while (!failure_mutex_.try_lock()) {}
        last_heard_[info.id] = Clock::now();
        failure_mutex_.unlock();
    }
}

void GossipProtocol::handle_pull(const protocol::GossipPullMessage& msg) {
    // Respond with requested node info
    // TODO: Implement response
}

void GossipProtocol::handle_ack(const protocol::GossipAckMessage& msg) {
    for (const auto& info : msg.nodes) {
        cluster_.handle_node_update(info);
        
        while (!failure_mutex_.try_lock()) {}
        last_heard_[info.id] = Clock::now();
        failure_mutex_.unlock();
    }
}

elio::coro::task<void> GossipProtocol::gossip_round() {
    auto nodes = cluster_.active_nodes();
    if (nodes.size() <= 1) {
        co_return;  // Only self
    }
    
    // Select random nodes to gossip with
    std::random_device rd;
    std::mt19937 gen(rd());
    std::shuffle(nodes.begin(), nodes.end(), gen);
    
    size_t fanout = std::min(config_.gossip_fanout, nodes.size());
    
    // Prepare gossip message with all known nodes
    protocol::GossipPushMessage push;
    push.sender = cluster_.local_node()->id();
    
    for (const auto& node : cluster_.all_nodes()) {
        push.nodes.push_back(node->info());
    }
    
    // Send to selected nodes
    for (size_t i = 0; i < fanout; ++i) {
        auto& target = nodes[i];
        if (target->id() == cluster_.local_node()->id()) {
            continue;
        }
        
        auto conn = target->connection();
        if (conn && conn->is_connected()) {
            co_await conn->send(protocol::Message{push});
        }
    }
    
    co_return;
}

void GossipProtocol::check_failures() {
    auto now = Clock::now();
    auto timeout = config_.failure_detection_timeout;
    
    while (!failure_mutex_.try_lock()) {}
    
    for (auto it = last_heard_.begin(); it != last_heard_.end(); ) {
        if (now - it->second > timeout) {
            // Node may have failed
            // TODO: Mark node as suspect/failed
            it = last_heard_.erase(it);
        } else {
            ++it;
        }
    }
    failure_mutex_.unlock();
}

}  // namespace elcache
