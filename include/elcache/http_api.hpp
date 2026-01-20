#pragma once

#include "types.hpp"
#include "cache.hpp"
#include "config.hpp"
#include "metrics.hpp"
#include <elio/coro/task.hpp>
#include <elio/http/http_server.hpp>
#include <elio/io/io_context.hpp>
#include <elio/runtime/scheduler.hpp>
#include <functional>
#include <string>
#include <unordered_map>

namespace elcache {

// Forward declarations
class CacheCoordinator;
class MetricsCollector;

// HTTP request representation (kept for internal use)
struct HttpRequest {
    std::string method;
    std::string path;
    std::string query;
    std::unordered_map<std::string, std::string> headers;
    ByteBuffer body;
    
    // Parsed from path/query
    std::string cache_key;
    std::optional<Range> range;
    
    // Helper methods
    std::string header(const std::string& name) const;
    std::optional<uint64_t> content_length() const;
    bool has_range_header() const;
    Range parse_range_header(uint64_t total_size) const;
    
    // Build from Elio context
    static HttpRequest from_context(elio::http::context& ctx);
};

// HTTP response representation (kept for internal use)
struct HttpResponse {
    int status_code = 200;
    std::string status_text = "OK";
    std::unordered_map<std::string, std::string> headers;
    ByteBuffer body;
    
    // Common responses
    static HttpResponse ok(ByteBuffer body = {});
    static HttpResponse created();
    static HttpResponse no_content();
    static HttpResponse partial_content(ByteBuffer body, const Range& range, uint64_t total);
    static HttpResponse not_found(const std::string& msg = "Not Found");
    static HttpResponse bad_request(const std::string& msg);
    static HttpResponse internal_error(const std::string& msg);
    static HttpResponse service_unavailable(const std::string& msg);
    
    void set_content_type(const std::string& type);
    void set_content_length(uint64_t len);
    void set_range_headers(const Range& range, uint64_t total);
    
    // Convert to Elio response
    elio::http::response to_elio_response() const;
};

// HTTP API handler
class HttpHandler {
public:
    using Handler = std::function<elio::coro::task<HttpResponse>(const HttpRequest&)>;
    
    explicit HttpHandler(CacheCoordinator& cache);
    
    // Set metrics collector for /metrics endpoint
    void set_metrics_collector(MetricsCollector* collector) { metrics_ = collector; }
    
    // Route a request
    elio::coro::task<HttpResponse> handle(const HttpRequest& req);
    
    // Register custom handler
    void add_route(const std::string& method, const std::string& pattern, Handler handler);
    
    // Build Elio router from registered handlers
    elio::http::router build_router();
    
private:
    CacheCoordinator& cache_;
    MetricsCollector* metrics_ = nullptr;
    std::vector<std::tuple<std::string, std::string, Handler>> routes_;
    
    // Built-in handlers
    elio::coro::task<HttpResponse> handle_get(const HttpRequest& req);
    elio::coro::task<HttpResponse> handle_put(const HttpRequest& req);
    elio::coro::task<HttpResponse> handle_delete(const HttpRequest& req);
    elio::coro::task<HttpResponse> handle_head(const HttpRequest& req);
    elio::coro::task<HttpResponse> handle_options(const HttpRequest& req);
    
    // Sparse/partial write handlers
    elio::coro::task<HttpResponse> handle_sparse_create(const HttpRequest& req);
    elio::coro::task<HttpResponse> handle_sparse_write(const HttpRequest& req);
    elio::coro::task<HttpResponse> handle_sparse_finalize(const HttpRequest& req);
    elio::coro::task<HttpResponse> handle_sparse_status(const HttpRequest& req);
    
    // Stats/admin endpoints
    elio::coro::task<HttpResponse> handle_stats(const HttpRequest& req);
    elio::coro::task<HttpResponse> handle_health(const HttpRequest& req);
    elio::coro::task<HttpResponse> handle_cluster(const HttpRequest& req);
    elio::coro::task<HttpResponse> handle_metrics(const HttpRequest& req);
    
    // Elio handler adapters (wrap internal handlers for Elio's router)
    elio::coro::task<elio::http::response> elio_get_handler(elio::http::context& ctx);
    elio::coro::task<elio::http::response> elio_put_handler(elio::http::context& ctx);
    elio::coro::task<elio::http::response> elio_delete_handler(elio::http::context& ctx);
    elio::coro::task<elio::http::response> elio_head_handler(elio::http::context& ctx);
    elio::coro::task<elio::http::response> elio_options_handler(elio::http::context& ctx);
    elio::coro::task<elio::http::response> elio_stats_handler(elio::http::context& ctx);
    elio::coro::task<elio::http::response> elio_health_handler(elio::http::context& ctx);
    elio::coro::task<elio::http::response> elio_cluster_handler(elio::http::context& ctx);
    elio::coro::task<elio::http::response> elio_metrics_handler(elio::http::context& ctx);
    elio::coro::task<elio::http::response> elio_sparse_create_handler(elio::http::context& ctx);
    elio::coro::task<elio::http::response> elio_sparse_write_handler(elio::http::context& ctx);
    elio::coro::task<elio::http::response> elio_sparse_finalize_handler(elio::http::context& ctx);
    elio::coro::task<elio::http::response> elio_sparse_status_handler(elio::http::context& ctx);
};

// HTTP server using Elio's async server
class HttpServer {
public:
    HttpServer(const NetworkConfig& config, HttpHandler& handler);
    ~HttpServer();
    
    elio::coro::task<Status> start(elio::io::io_context& io_ctx, 
                                    elio::runtime::scheduler& sched);
    elio::coro::task<void> stop();
    
    uint16_t port() const { return config_.http_port; }
    bool is_running() const { return running_; }
    
private:
    NetworkConfig config_;
    HttpHandler& handler_;
    std::unique_ptr<elio::http::server> server_;
    std::atomic<bool> running_{false};
};

}  // namespace elcache

/*
HTTP API Design:

Endpoints:
  GET    /cache/{key}           - Get value (supports Range header)
  PUT    /cache/{key}           - Store value
  DELETE /cache/{key}           - Delete value
  HEAD   /cache/{key}           - Get metadata only
  
  GET    /cache/{key}?range=0-1023  - Partial read (alternative to Range header)
  
  # Sparse/Parallel Write API
  POST   /sparse/{key}          - Create sparse entry (X-ElCache-Size header required)
  PATCH  /sparse/{key}          - Write range (Content-Range header required)
  POST   /sparse/{key}/finalize - Finalize sparse entry
  GET    /sparse/{key}          - Get sparse entry status
  DELETE /sparse/{key}          - Abort sparse write
  
  GET    /stats                 - Cache statistics
  GET    /health                - Health check
  GET    /cluster               - Cluster info
  GET    /cluster/nodes         - Node list

Headers:
  X-ElCache-TTL: 3600           - Time-to-live in seconds
  X-ElCache-Flags: 123          - User-defined flags
  X-ElCache-No-Memory: true     - Skip memory cache
  X-ElCache-No-Disk: true       - Skip disk cache
  X-ElCache-No-Cluster: true    - Don't replicate
  X-ElCache-Size: 1048576       - Total size for sparse entry creation

Response Headers:
  X-ElCache-Hit: memory|disk|cluster|miss
  X-ElCache-Size: 12345         - Total value size
  X-ElCache-Chunks: 10          - Number of chunks
  Content-Range: bytes 0-999/5000  - For partial responses
*/
