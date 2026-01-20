#pragma once

#include "types.hpp"
#include "cache_coordinator.hpp"
#include <elio/coro/task.hpp>
#include <elio/io/io_context.hpp>
#include <elio/runtime/scheduler.hpp>
#include <elio/net/uds.hpp>
#include <string>
#include <atomic>
#include <memory>

namespace elcache {

// Protocol constants matching SDK
constexpr uint32_t PROTO_MAGIC = 0x454C4341;  // "ELCA"
constexpr uint16_t PROTO_VERSION = 1;
constexpr size_t HEADER_SIZE = 24;

// Message types
enum class MessageType : uint16_t {
    Get = 0x0100,
    GetResponse = 0x0101,
    Put = 0x0102,
    PutResponse = 0x0103,
    Delete = 0x0104,
    DeleteResponse = 0x0105,
    Check = 0x0106,
    CheckResponse = 0x0107,
    // Sparse/partial write operations for parallel writes
    CreateSparse = 0x0108,
    CreateSparseResponse = 0x0109,
    WriteRange = 0x010A,
    WriteRangeResponse = 0x010B,
    Finalize = 0x010C,
    FinalizeResponse = 0x010D,
};

// Unix Socket Server for SDK clients
class UnixSocketServer {
public:
    UnixSocketServer(const std::string& socket_path, CacheCoordinator& cache, 
                     elio::io::io_context& ctx);
    ~UnixSocketServer();
    
    // Start listening and accepting connections
    elio::coro::task<Status> start(elio::runtime::scheduler& sched);
    
    // Stop the server
    void stop();
    
    // Get socket path
    const std::string& socket_path() const { return socket_path_; }
    
private:
    // Thread-based handlers using simple sync cache
    void handle_client_thread(int fd);
    bool process_request_thread(int fd);
    void handle_get_thread(int fd, const uint8_t* payload, size_t len, uint32_t request_id);
    void handle_put_thread(int fd, const uint8_t* payload, size_t len, uint32_t request_id);
    void handle_delete_thread(int fd, const uint8_t* payload, size_t len, uint32_t request_id);
    void handle_create_sparse_thread(int fd, const uint8_t* payload, size_t len, uint32_t request_id);
    void handle_write_range_thread(int fd, const uint8_t* payload, size_t len, uint32_t request_id);
    void handle_finalize_thread(int fd, const uint8_t* payload, size_t len, uint32_t request_id);
    bool send_response_sync(int fd, MessageType type, uint32_t request_id, const uint8_t* payload, size_t len);
    
    // Legacy coroutine-based handlers (kept for API compatibility)
    elio::coro::task<void> handle_client_sync(int fd, elio::runtime::scheduler& sched);
    elio::coro::task<bool> process_request_sync(int fd, elio::runtime::scheduler& sched);
    elio::coro::task<void> handle_get_sync(int fd, const uint8_t* payload, size_t len, uint32_t request_id);
    elio::coro::task<void> handle_put_sync(int fd, const uint8_t* payload, size_t len, uint32_t request_id);
    elio::coro::task<void> handle_delete_sync(int fd, const uint8_t* payload, size_t len, uint32_t request_id);
    
    elio::coro::task<void> handle_client(elio::net::uds_stream stream);
    elio::coro::task<bool> process_request(elio::net::uds_stream& stream);
    elio::coro::task<void> handle_get(elio::net::uds_stream& stream, const uint8_t* payload, size_t len, uint32_t request_id);
    elio::coro::task<void> handle_put(elio::net::uds_stream& stream, const uint8_t* payload, size_t len, uint32_t request_id);
    elio::coro::task<void> handle_delete(elio::net::uds_stream& stream, const uint8_t* payload, size_t len, uint32_t request_id);
    elio::coro::task<void> handle_create_sparse(elio::net::uds_stream& stream, const uint8_t* payload, size_t len, uint32_t request_id);
    elio::coro::task<void> handle_write_range(elio::net::uds_stream& stream, const uint8_t* payload, size_t len, uint32_t request_id);
    elio::coro::task<void> handle_finalize(elio::net::uds_stream& stream, const uint8_t* payload, size_t len, uint32_t request_id);
    elio::coro::task<bool> send_response(elio::net::uds_stream& stream, MessageType type, uint32_t request_id, const uint8_t* payload, size_t len);
    
    // Async accept loop using Elio's uds_listener
    elio::coro::task<void> accept_loop_async(elio::runtime::scheduler& sched);
    
    std::string socket_path_;
    CacheCoordinator& cache_;
    elio::io::io_context& io_ctx_;
    std::atomic<bool> running_{false};
    std::unique_ptr<elio::net::uds_listener> listener_;
};

}  // namespace elcache
