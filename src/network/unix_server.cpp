#include "elcache/unix_server.hpp"
#include "elcache/sparse_cache.hpp"
#include <cstring>
#include <iostream>
#include <filesystem>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>
#include <thread>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

namespace elcache {

namespace {

// Debug helper to check fd info
void debug_fd_info(int fd, const char* label) {
    struct stat st;
    if (fstat(fd, &st) == 0) {
        const char* type = "unknown";
        if (S_ISSOCK(st.st_mode)) type = "socket";
        else if (S_ISREG(st.st_mode)) type = "regular file";
        else if (S_ISFIFO(st.st_mode)) type = "fifo";
        else if (S_ISCHR(st.st_mode)) type = "char device";
        
        int flags = fcntl(fd, F_GETFL);
        bool nonblock = (flags & O_NONBLOCK) != 0;
        
        std::cerr << "[UDS DEBUG] " << label << ": fd=" << fd 
                  << " type=" << type 
                  << " nonblock=" << (nonblock ? "yes" : "no")
                  << " flags=0x" << std::hex << flags << std::dec << "\n";
    } else {
        std::cerr << "[UDS DEBUG] " << label << ": fd=" << fd 
                  << " fstat failed: " << strerror(errno) << "\n";
    }
}

// Simple synchronous in-memory cache for Unix socket server
// Uses standard mutexes instead of async ones
class SyncCache {
public:
    void put(const std::string& key, const std::vector<uint8_t>& value) {
        std::unique_lock lock(mutex_);
        data_[key] = value;
    }
    
    bool get(const std::string& key, std::vector<uint8_t>& value) {
        std::shared_lock lock(mutex_);
        auto it = data_.find(key);
        if (it == data_.end()) return false;
        value = it->second;
        return true;
    }
    
    bool remove(const std::string& key) {
        std::unique_lock lock(mutex_);
        return data_.erase(key) > 0;
    }
    
    bool exists(const std::string& key) {
        std::shared_lock lock(mutex_);
        return data_.find(key) != data_.end();
    }
    
private:
    std::shared_mutex mutex_;
    std::unordered_map<std::string, std::vector<uint8_t>> data_;
};

// Global sync cache instance for Unix socket server
static SyncCache g_sync_cache;

// Little-endian encoding/decoding helpers
inline void encode_u16(uint8_t* buf, uint16_t v) {
    buf[0] = v & 0xFF;
    buf[1] = (v >> 8) & 0xFF;
}

inline void encode_u32(uint8_t* buf, uint32_t v) {
    buf[0] = v & 0xFF;
    buf[1] = (v >> 8) & 0xFF;
    buf[2] = (v >> 16) & 0xFF;
    buf[3] = (v >> 24) & 0xFF;
}

inline void encode_u64(uint8_t* buf, uint64_t v) {
    for (int i = 0; i < 8; i++) {
        buf[i] = (v >> (i * 8)) & 0xFF;
    }
}

inline uint16_t decode_u16(const uint8_t* buf) {
    return buf[0] | (static_cast<uint16_t>(buf[1]) << 8);
}

inline uint32_t decode_u32(const uint8_t* buf) {
    return buf[0] | (static_cast<uint32_t>(buf[1]) << 8) | 
           (static_cast<uint32_t>(buf[2]) << 16) | (static_cast<uint32_t>(buf[3]) << 24);
}

inline uint64_t decode_u64(const uint8_t* buf) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v |= static_cast<uint64_t>(buf[i]) << (i * 8);
    }
    return v;
}

// Encode message header
void encode_header(uint8_t* buf, MessageType type, uint32_t payload_len, uint32_t request_id) {
    encode_u32(buf, PROTO_MAGIC);
    encode_u16(buf + 4, PROTO_VERSION);
    encode_u16(buf + 6, static_cast<uint16_t>(type));
    encode_u32(buf + 8, payload_len);
    encode_u32(buf + 12, request_id);
    encode_u64(buf + 16, static_cast<uint64_t>(time(nullptr)) * 1000);
}

// Blocking read helper
bool read_all(int fd, void* buf, size_t len) {
    uint8_t* ptr = static_cast<uint8_t*>(buf);
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t n = ::recv(fd, ptr, remaining, 0);
        if (n <= 0) return false;
        ptr += n;
        remaining -= n;
    }
    return true;
}

// Blocking write helper
bool write_all(int fd, const void* buf, size_t len) {
    const uint8_t* ptr = static_cast<const uint8_t*>(buf);
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t n = ::send(fd, ptr, remaining, 0);
        if (n <= 0) return false;
        ptr += n;
        remaining -= n;
    }
    return true;
}

}  // anonymous namespace

UnixSocketServer::UnixSocketServer(const std::string& socket_path, 
                                   CacheCoordinator& cache,
                                   elio::io::io_context& ctx)
    : socket_path_(socket_path)
    , cache_(cache)
    , io_ctx_(ctx)
{}

UnixSocketServer::~UnixSocketServer() {
    stop();
}

elio::coro::task<Status> UnixSocketServer::start(elio::runtime::scheduler& sched) {
    std::cerr << "[UDS DEBUG] UnixSocketServer::start() called for " << socket_path_ << std::endl;
    running_ = true;
    
    // Ensure parent directory exists
    auto parent = std::filesystem::path(socket_path_).parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            co_return Status::error(ErrorCode::DiskError, 
                "Failed to create socket directory: " + ec.message());
        }
    }
    
    // Try using Elio's native async UDS first
    elio::net::unix_address addr(socket_path_);
    auto listener_opt = elio::net::uds_listener::bind(addr, io_ctx_);
    
    if (listener_opt) {
        std::cout << "[ElCache] Unix socket (async) listening on " << socket_path_ << std::endl;
        std::cerr << "[UDS DEBUG] Async listener fd=" << listener_opt->fd() << std::endl;
        
        // Store listener and run async accept loop
        listener_ = std::make_unique<elio::net::uds_listener>(std::move(*listener_opt));
        debug_fd_info(listener_->fd(), "listener socket");
        
        // Spawn async accept loop
        auto accept_task = accept_loop_async(sched);
        sched.spawn(accept_task.release());
        
        co_return Status::make_ok();
    }
    
    // Fall back to thread-based approach
    std::cerr << "[UDS DEBUG] Failed to bind async listener: " << strerror(errno) 
              << ", falling back to thread-based" << std::endl;
    
    // Unlink existing socket
    ::unlink(socket_path_.c_str());
    
    // Create socket directly
    int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        co_return Status::error(ErrorCode::NetworkError, 
            "Failed to create Unix socket: " + std::string(strerror(errno)));
    }
    
    debug_fd_info(listen_fd, "thread-based listener");
    
    // Bind
    struct sockaddr_un saddr;
    memset(&saddr, 0, sizeof(saddr));
    saddr.sun_family = AF_UNIX;
    strncpy(saddr.sun_path, socket_path_.c_str(), sizeof(saddr.sun_path) - 1);
    
    if (::bind(listen_fd, (struct sockaddr*)&saddr, sizeof(saddr)) < 0) {
        ::close(listen_fd);
        co_return Status::error(ErrorCode::NetworkError, 
            "Failed to bind Unix socket: " + std::string(strerror(errno)));
    }
    
    // Listen
    if (::listen(listen_fd, 128) < 0) {
        ::close(listen_fd);
        co_return Status::error(ErrorCode::NetworkError, 
            "Failed to listen on Unix socket: " + std::string(strerror(errno)));
    }
    
    std::cout << "[ElCache] Unix socket (threaded) listening on " << socket_path_ << std::endl;
    
    // Accept loop in separate thread
    std::thread accept_thread([this, listen_fd]() {
        while (running_) {
            struct pollfd pfd;
            pfd.fd = listen_fd;
            pfd.events = POLLIN;
            
            int ret = poll(&pfd, 1, 100);
            if (ret <= 0) continue;
            
            struct sockaddr_un client_addr;
            socklen_t addr_len = sizeof(client_addr);
            int client_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &addr_len);
            if (client_fd < 0) continue;
            
            debug_fd_info(client_fd, "accepted client (threaded)");
            
            // Handle client in a separate thread
            std::thread client_thread([this, client_fd]() {
                handle_client_thread(client_fd);
            });
            client_thread.detach();
        }
        
        ::close(listen_fd);
        ::unlink(socket_path_.c_str());
    });
    accept_thread.detach();
    
    co_return Status::make_ok();
}

elio::coro::task<void> UnixSocketServer::accept_loop_async(elio::runtime::scheduler& sched) {
    while (running_ && listener_) {
        auto client_opt = co_await listener_->accept();
        
        if (!client_opt) {
            std::cerr << "[UDS DEBUG] accept failed: " << strerror(errno) << "\n";
            continue;
        }
        
        int client_fd = client_opt->fd();
        debug_fd_info(client_fd, "accepted client (async)");
        
        // Spawn handler for this client
        auto handler = handle_client(std::move(*client_opt));
        sched.spawn(handler.release());
    }
}

void UnixSocketServer::stop() {
    running_ = false;
    if (listener_) {
        listener_->close();
        listener_.reset();
    }
}

void UnixSocketServer::handle_client_thread(int fd) {
    while (running_) {
        if (!process_request_thread(fd)) break;
    }
    ::close(fd);
}

bool UnixSocketServer::process_request_thread(int fd) {
    uint8_t header[HEADER_SIZE];
    if (!read_all(fd, header, HEADER_SIZE)) {
        return false;
    }
    
    uint32_t magic = decode_u32(header);
    if (magic != PROTO_MAGIC) {
        return false;
    }
    
    uint16_t msg_type = decode_u16(header + 6);
    uint32_t payload_len = decode_u32(header + 8);
    uint32_t request_id = decode_u32(header + 12);
    
    std::vector<uint8_t> payload(payload_len);
    if (payload_len > 0 && !read_all(fd, payload.data(), payload_len)) {
        return false;
    }
    
    switch (static_cast<MessageType>(msg_type)) {
        case MessageType::Get:
            handle_get_thread(fd, payload.data(), payload.size(), request_id);
            break;
        case MessageType::Put:
            handle_put_thread(fd, payload.data(), payload.size(), request_id);
            break;
        case MessageType::Delete:
            handle_delete_thread(fd, payload.data(), payload.size(), request_id);
            break;
        case MessageType::CreateSparse:
            handle_create_sparse_thread(fd, payload.data(), payload.size(), request_id);
            break;
        case MessageType::WriteRange:
            handle_write_range_thread(fd, payload.data(), payload.size(), request_id);
            break;
        case MessageType::Finalize:
            handle_finalize_thread(fd, payload.data(), payload.size(), request_id);
            break;
        default:
            break;
    }
    
    return true;
}

void UnixSocketServer::handle_get_thread(
    int fd,
    const uint8_t* payload, size_t len,
    uint32_t request_id) 
{
    if (len < 4) return;
    
    uint32_t key_len = decode_u32(payload);
    if (len < 4 + key_len + 2) return;
    
    std::string key(reinterpret_cast<const char*>(payload + 4), key_len);
    size_t pos = 4 + key_len;
    
    // Parse metadata_only flag
    pos++;  // Skip metadata_only for now
    
    // Parse range
    uint8_t has_range = payload[pos++];
    uint64_t offset = 0;
    uint64_t length = 0;
    if (has_range && len >= pos + 16) {
        offset = decode_u64(payload + pos);
        pos += 8;
        length = decode_u64(payload + pos);
        pos += 8;
    }
    
    // Use sync cache
    std::vector<uint8_t> value;
    bool found = g_sync_cache.get(key, value);
    
    // Apply range if specified
    std::vector<uint8_t> result_data;
    if (found && has_range) {
        if (offset < value.size()) {
            size_t available = value.size() - offset;
            size_t to_copy = (length > 0 && length < available) ? length : available;
            result_data.assign(value.begin() + offset, value.begin() + offset + to_copy);
        }
    } else if (found) {
        result_data = std::move(value);
    }
    
    // Build response
    std::vector<uint8_t> response;
    response.reserve(64 + result_data.size());
    
    uint32_t result_code = found ? 0 : 1;  // 0=Hit, 1=Miss
    
    response.resize(4);
    encode_u32(response.data(), result_code);
    
    // No metadata
    response.push_back(0);
    
    // Data length and data
    size_t data_pos = response.size();
    response.resize(data_pos + 4 + result_data.size());
    encode_u32(response.data() + data_pos, static_cast<uint32_t>(result_data.size()));
    if (!result_data.empty()) {
        std::memcpy(response.data() + data_pos + 4, result_data.data(), result_data.size());
    }
    
    send_response_sync(fd, MessageType::GetResponse, request_id,
                       response.data(), response.size());
}

void UnixSocketServer::handle_put_thread(
    int fd,
    const uint8_t* payload, size_t len,
    uint32_t request_id)
{
    if (len < 8) return;
    
    uint32_t key_len = decode_u32(payload);
    if (len < 4 + key_len + 4) return;
    
    std::string key(reinterpret_cast<const char*>(payload + 4), key_len);
    size_t pos = 4 + key_len;
    
    uint32_t value_len = decode_u32(payload + pos);
    pos += 4;
    if (len < pos + value_len) return;
    
    std::vector<uint8_t> value(payload + pos, payload + pos + value_len);
    
    // Use sync cache
    g_sync_cache.put(key, value);
    
    // Build response - success
    uint8_t response[4];
    encode_u32(response, 0);  // 0 = success
    
    send_response_sync(fd, MessageType::PutResponse, request_id,
                       response, sizeof(response));
}

void UnixSocketServer::handle_delete_thread(
    int fd,
    const uint8_t* payload, size_t len,
    uint32_t request_id)
{
    if (len < 4) return;
    
    uint32_t key_len = decode_u32(payload);
    if (len < 4 + key_len) return;
    
    std::string key(reinterpret_cast<const char*>(payload + 4), key_len);
    
    // Use sync cache
    g_sync_cache.remove(key);
    
    // Build response - success
    uint8_t response[4];
    encode_u32(response, 0);
    
    send_response_sync(fd, MessageType::DeleteResponse, request_id,
                       response, sizeof(response));
}

void UnixSocketServer::handle_create_sparse_thread(
    int fd,
    const uint8_t* payload, size_t len,
    uint32_t request_id)
{
    if (len < 4) return;
    
    uint32_t key_len = decode_u32(payload);
    if (len < 4 + key_len + 8) return;
    
    std::string key(reinterpret_cast<const char*>(payload + 4), key_len);
    size_t pos = 4 + key_len;
    
    uint64_t total_size = decode_u64(payload + pos);
    
    bool created = global_sparse_cache().create(key, total_size);
    
    uint8_t response[4];
    encode_u32(response, created ? 0 : 1);
    
    send_response_sync(fd, MessageType::CreateSparseResponse, request_id,
                       response, sizeof(response));
}

void UnixSocketServer::handle_write_range_thread(
    int fd,
    const uint8_t* payload, size_t len,
    uint32_t request_id)
{
    if (len < 4) return;
    
    uint32_t key_len = decode_u32(payload);
    if (len < 4 + key_len + 8 + 4) return;
    
    std::string key(reinterpret_cast<const char*>(payload + 4), key_len);
    size_t pos = 4 + key_len;
    
    uint64_t offset = decode_u64(payload + pos);
    pos += 8;
    
    uint32_t data_len = decode_u32(payload + pos);
    pos += 4;
    
    if (len < pos + data_len) return;
    
    bool success = global_sparse_cache().write_range(key, offset, payload + pos, data_len);
    
    uint8_t response[4];
    if (!success) {
        if (!global_sparse_cache().exists(key)) {
            encode_u32(response, 1);
        } else {
            encode_u32(response, 2);
        }
    } else {
        encode_u32(response, 0);
    }
    
    send_response_sync(fd, MessageType::WriteRangeResponse, request_id,
                       response, sizeof(response));
}

void UnixSocketServer::handle_finalize_thread(
    int fd,
    const uint8_t* payload, size_t len,
    uint32_t request_id)
{
    if (len < 4) return;
    
    uint32_t key_len = decode_u32(payload);
    if (len < 4 + key_len) return;
    
    std::string key(reinterpret_cast<const char*>(payload + 4), key_len);
    
    std::vector<uint8_t> data;
    int result = global_sparse_cache().finalize(key, data);
    
    // If successful, store in sync cache
    if (result == 0) {
        g_sync_cache.put(key, data);
    }
    
    uint8_t response[4];
    encode_u32(response, result);
    
    send_response_sync(fd, MessageType::FinalizeResponse, request_id,
                       response, sizeof(response));
}

bool UnixSocketServer::send_response_sync(
    int fd,
    MessageType type, uint32_t request_id,
    const uint8_t* payload, size_t len)
{
    std::vector<uint8_t> msg(HEADER_SIZE + len);
    encode_header(msg.data(), type, static_cast<uint32_t>(len), request_id);
    if (len > 0) {
        std::memcpy(msg.data() + HEADER_SIZE, payload, len);
    }
    
    return write_all(fd, msg.data(), msg.size());
}

// Async client handler using Elio's uds_stream
elio::coro::task<void> UnixSocketServer::handle_client(elio::net::uds_stream stream) {
    int fd = stream.fd();
    std::cerr << "[UDS DEBUG] handle_client starting for fd=" << fd << "\n";
    
    while (running_) {
        bool ok = co_await process_request(stream);
        if (!ok) {
            std::cerr << "[UDS DEBUG] process_request returned false for fd=" << fd << "\n";
            break;
        }
    }
    
    std::cerr << "[UDS DEBUG] handle_client ending for fd=" << fd << "\n";
    co_await stream.close();
}

elio::coro::task<bool> UnixSocketServer::process_request(elio::net::uds_stream& stream) {
    uint8_t header[HEADER_SIZE];
    int fd = stream.fd();
    
    std::cerr << "[UDS DEBUG] process_request starting read on fd=" << fd << "\n";
    
    // Read header
    size_t total_read = 0;
    while (total_read < HEADER_SIZE) {
        std::cerr << "[UDS DEBUG] calling stream.read(), total_read=" << total_read << "\n";
        auto result = co_await stream.read(header + total_read, HEADER_SIZE - total_read);
        std::cerr << "[UDS DEBUG] stream.read() returned: result=" << result.result 
                  << " flags=" << result.flags << "\n";
        if (result.result <= 0) {
            std::cerr << "[UDS DEBUG] read header failed: result=" << result.result 
                      << " errno=" << errno << " (" << strerror(errno) << ")\n";
            co_return false;
        }
        total_read += result.result;
    }
    
    uint32_t magic = decode_u32(header);
    if (magic != PROTO_MAGIC) {
        std::cerr << "[UDS DEBUG] bad magic: 0x" << std::hex << magic << std::dec << "\n";
        co_return false;
    }
    
    uint16_t msg_type = decode_u16(header + 6);
    uint32_t payload_len = decode_u32(header + 8);
    uint32_t request_id = decode_u32(header + 12);
    
    std::cerr << "[UDS DEBUG] header parsed: msg_type=0x" << std::hex << msg_type 
              << std::dec << " payload_len=" << payload_len 
              << " request_id=" << request_id << "\n";
    
    std::vector<uint8_t> payload(payload_len);
    if (payload_len > 0) {
        std::cerr << "[UDS DEBUG] reading payload of " << payload_len << " bytes\n";
        total_read = 0;
        while (total_read < payload_len) {
            auto result = co_await stream.read(payload.data() + total_read, payload_len - total_read);
            if (result.result <= 0) {
                std::cerr << "[UDS DEBUG] read payload failed: result=" << result.result << "\n";
                co_return false;
            }
            total_read += result.result;
            std::cerr << "[UDS DEBUG] payload read progress: " << total_read << "/" << payload_len << "\n";
        }
    }
    
    switch (static_cast<MessageType>(msg_type)) {
        case MessageType::Get:
            co_await handle_get(stream, payload.data(), payload.size(), request_id);
            break;
        case MessageType::Put:
            co_await handle_put(stream, payload.data(), payload.size(), request_id);
            break;
        case MessageType::Delete:
            co_await handle_delete(stream, payload.data(), payload.size(), request_id);
            break;
        case MessageType::CreateSparse:
            co_await handle_create_sparse(stream, payload.data(), payload.size(), request_id);
            break;
        case MessageType::WriteRange:
            co_await handle_write_range(stream, payload.data(), payload.size(), request_id);
            break;
        case MessageType::Finalize:
            co_await handle_finalize(stream, payload.data(), payload.size(), request_id);
            break;
        default:
            std::cerr << "[UDS DEBUG] unknown message type: 0x" << std::hex << msg_type << std::dec << "\n";
            break;
    }
    
    co_return true;
}

elio::coro::task<void> UnixSocketServer::handle_get(
    elio::net::uds_stream& stream,
    const uint8_t* payload, size_t len,
    uint32_t request_id)
{
    if (len < 4) co_return;
    
    uint32_t key_len = decode_u32(payload);
    if (len < 4 + key_len + 2) co_return;
    
    std::string key(reinterpret_cast<const char*>(payload + 4), key_len);
    size_t pos = 4 + key_len;
    
    // Parse metadata_only flag
    // uint8_t metadata_only = payload[pos++];
    pos++;  // Skip metadata_only for now
    
    // Parse range
    uint8_t has_range = payload[pos++];
    uint64_t offset = 0;
    uint64_t length = 0;
    if (has_range && len >= pos + 16) {
        offset = decode_u64(payload + pos);
        pos += 8;
        length = decode_u64(payload + pos);
        pos += 8;
    }
    
    // Use sync cache (thread-safe)
    std::vector<uint8_t> value;
    bool found = g_sync_cache.get(key, value);
    
    // Apply range if specified
    std::vector<uint8_t> result_data;
    if (found && has_range) {
        if (offset < value.size()) {
            size_t available = value.size() - offset;
            size_t to_copy = (length > 0 && length < available) ? length : available;
            result_data.assign(value.begin() + offset, value.begin() + offset + to_copy);
        }
        // If offset >= size, result_data stays empty
    } else if (found) {
        result_data = std::move(value);
    }
    
    // Build response
    std::vector<uint8_t> response;
    response.reserve(64 + result_data.size());
    
    uint32_t result_code = found ? 0 : 1;
    response.resize(4);
    encode_u32(response.data(), result_code);
    
    // No metadata
    response.push_back(0);
    
    // Data length and data
    size_t data_pos = response.size();
    response.resize(data_pos + 4 + result_data.size());
    encode_u32(response.data() + data_pos, static_cast<uint32_t>(result_data.size()));
    if (!result_data.empty()) {
        std::memcpy(response.data() + data_pos + 4, result_data.data(), result_data.size());
    }
    
    co_await send_response(stream, MessageType::GetResponse, request_id,
                           response.data(), response.size());
}

elio::coro::task<void> UnixSocketServer::handle_put(
    elio::net::uds_stream& stream,
    const uint8_t* payload, size_t len,
    uint32_t request_id)
{
    if (len < 8) co_return;
    
    uint32_t key_len = decode_u32(payload);
    if (len < 4 + key_len + 4) co_return;
    
    std::string key(reinterpret_cast<const char*>(payload + 4), key_len);
    size_t pos = 4 + key_len;
    
    uint32_t value_len = decode_u32(payload + pos);
    pos += 4;
    if (len < pos + value_len) co_return;
    
    std::vector<uint8_t> value(payload + pos, payload + pos + value_len);
    
    // Use sync cache
    g_sync_cache.put(key, value);
    
    // Build response - success
    uint8_t response[4];
    encode_u32(response, 0);
    
    co_await send_response(stream, MessageType::PutResponse, request_id,
                           response, sizeof(response));
}

elio::coro::task<void> UnixSocketServer::handle_delete(
    elio::net::uds_stream& stream,
    const uint8_t* payload, size_t len,
    uint32_t request_id)
{
    if (len < 4) co_return;
    
    uint32_t key_len = decode_u32(payload);
    if (len < 4 + key_len) co_return;
    
    std::string key(reinterpret_cast<const char*>(payload + 4), key_len);
    
    // Use sync cache
    g_sync_cache.remove(key);
    
    // Build response - success
    uint8_t response[4];
    encode_u32(response, 0);
    
    co_await send_response(stream, MessageType::DeleteResponse, request_id,
                           response, sizeof(response));
}

elio::coro::task<void> UnixSocketServer::handle_create_sparse(
    elio::net::uds_stream& stream,
    const uint8_t* payload, size_t len,
    uint32_t request_id)
{
    std::cerr << "[UDS DEBUG] handle_create_sparse: len=" << len << "\n";
    
    // Parse: key_len(4) + key + total_size(8) + has_ttl(1) + [ttl(8)] + flags(4)
    if (len < 4) {
        std::cerr << "[UDS DEBUG] handle_create_sparse: len < 4, returning\n";
        co_return;
    }
    
    uint32_t key_len = decode_u32(payload);
    std::cerr << "[UDS DEBUG] handle_create_sparse: key_len=" << key_len << "\n";
    
    if (len < 4 + key_len + 8) {
        std::cerr << "[UDS DEBUG] handle_create_sparse: len < 4+key_len+8, returning\n";
        co_return;
    }
    
    std::string key(reinterpret_cast<const char*>(payload + 4), key_len);
    size_t pos = 4 + key_len;
    
    uint64_t total_size = decode_u64(payload + pos);
    std::cerr << "[UDS DEBUG] handle_create_sparse: key='" << key << "' total_size=" << total_size << "\n";
    pos += 8;
    
    // Skip TTL and flags for now (just for creating the entry)
    
    // Create sparse entry
    std::cerr << "[UDS DEBUG] handle_create_sparse: calling global_sparse_cache().create\n";
    bool created = global_sparse_cache().create(key, total_size);
    std::cerr << "[UDS DEBUG] handle_create_sparse: created=" << created << "\n";
    
    // Build response
    uint8_t response[4];
    encode_u32(response, created ? 0 : 1);  // 0 = success, 1 = already exists
    
    std::cerr << "[UDS DEBUG] handle_create_sparse: sending response\n";
    co_await send_response(stream, MessageType::CreateSparseResponse, request_id,
                           response, sizeof(response));
    std::cerr << "[UDS DEBUG] handle_create_sparse: response sent\n";
}

elio::coro::task<void> UnixSocketServer::handle_write_range(
    elio::net::uds_stream& stream,
    const uint8_t* payload, size_t len,
    uint32_t request_id)
{
    // Parse: key_len(4) + key + offset(8) + data_len(4) + data
    if (len < 4) co_return;
    
    uint32_t key_len = decode_u32(payload);
    if (len < 4 + key_len + 8 + 4) co_return;
    
    std::string key(reinterpret_cast<const char*>(payload + 4), key_len);
    size_t pos = 4 + key_len;
    
    uint64_t offset = decode_u64(payload + pos);
    pos += 8;
    
    uint32_t data_len = decode_u32(payload + pos);
    pos += 4;
    
    if (len < pos + data_len) co_return;
    
    // Write to sparse entry
    bool success = global_sparse_cache().write_range(key, offset, payload + pos, data_len);
    
    // Build response: 0 = success, 1 = not found, 2 = invalid range
    uint8_t response[4];
    if (!success) {
        if (!global_sparse_cache().exists(key)) {
            encode_u32(response, 1);  // Not found
        } else {
            encode_u32(response, 2);  // Invalid range
        }
    } else {
        encode_u32(response, 0);  // Success
    }
    
    co_await send_response(stream, MessageType::WriteRangeResponse, request_id,
                           response, sizeof(response));
}

elio::coro::task<void> UnixSocketServer::handle_finalize(
    elio::net::uds_stream& stream,
    const uint8_t* payload, size_t len,
    uint32_t request_id)
{
    // Parse: key_len(4) + key
    if (len < 4) co_return;
    
    uint32_t key_len = decode_u32(payload);
    if (len < 4 + key_len) co_return;
    
    std::string key(reinterpret_cast<const char*>(payload + 4), key_len);
    
    // Finalize sparse entry
    std::vector<uint8_t> data;
    int result = global_sparse_cache().finalize(key, data);
    
    // If successful, store in sync cache
    if (result == 0) {
        g_sync_cache.put(key, data);
    }
    
    // Build response: 0 = success, 1 = not found, 2 = incomplete
    uint8_t response[4];
    encode_u32(response, result);
    
    co_await send_response(stream, MessageType::FinalizeResponse, request_id,
                           response, sizeof(response));
}

elio::coro::task<bool> UnixSocketServer::send_response(
    elio::net::uds_stream& stream,
    MessageType type, uint32_t request_id,
    const uint8_t* payload, size_t len)
{
    std::vector<uint8_t> msg(HEADER_SIZE + len);
    encode_header(msg.data(), type, static_cast<uint32_t>(len), request_id);
    if (len > 0) {
        std::memcpy(msg.data() + HEADER_SIZE, payload, len);
    }
    
    size_t total_written = 0;
    while (total_written < msg.size()) {
        auto result = co_await stream.write(msg.data() + total_written, msg.size() - total_written);
        if (result.result <= 0) {
            std::cerr << "[UDS DEBUG] write failed: result=" << result.result << "\n";
            co_return false;
        }
        total_written += result.result;
    }
    
    co_return true;
}

// Legacy methods - no longer needed
elio::coro::task<void> UnixSocketServer::handle_client_sync(int, elio::runtime::scheduler&) { co_return; }
elio::coro::task<bool> UnixSocketServer::process_request_sync(int, elio::runtime::scheduler&) { co_return false; }
elio::coro::task<void> UnixSocketServer::handle_get_sync(int, const uint8_t*, size_t, uint32_t) { co_return; }
elio::coro::task<void> UnixSocketServer::handle_put_sync(int, const uint8_t*, size_t, uint32_t) { co_return; }
elio::coro::task<void> UnixSocketServer::handle_delete_sync(int, const uint8_t*, size_t, uint32_t) { co_return; }

}  // namespace elcache
