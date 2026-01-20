#include "elcache/http_api.hpp"
#include "elcache/cache_coordinator.hpp"
#include "elcache/cluster.hpp"
#include "elcache/sparse_cache.hpp"
#include <sstream>
#include <algorithm>
#include <cstring>
#include <iostream>

namespace elcache {

// HttpRequest implementation

HttpRequest HttpRequest::from_context(elio::http::context& ctx) {
    HttpRequest req;
    
    // Map Elio method to string
    switch (ctx.req().get_method()) {
        case elio::http::method::GET:     req.method = "GET"; break;
        case elio::http::method::POST:    req.method = "POST"; break;
        case elio::http::method::PUT:     req.method = "PUT"; break;
        case elio::http::method::DELETE_: req.method = "DELETE"; break;
        case elio::http::method::HEAD:    req.method = "HEAD"; break;
        case elio::http::method::OPTIONS: req.method = "OPTIONS"; break;
        case elio::http::method::PATCH:   req.method = "PATCH"; break;
        default: req.method = "UNKNOWN"; break;
    }
    
    req.path = std::string(ctx.req().path());
    
    // Parse query string from full path if present
    auto query_pos = req.path.find('?');
    if (query_pos != std::string::npos) {
        req.query = req.path.substr(query_pos + 1);
        req.path = req.path.substr(0, query_pos);
    } else {
        req.query = std::string(ctx.req().query());
    }
    
    // Copy headers from Elio headers collection
    for (const auto& [name, value] : ctx.req().get_headers()) {
        req.headers[name] = value;
    }
    
    // Copy body
    auto body_view = ctx.req().body();
    req.body = ByteBuffer(
        reinterpret_cast<const uint8_t*>(body_view.data()),
        reinterpret_cast<const uint8_t*>(body_view.data() + body_view.size())
    );
    
    return req;
}

std::string HttpRequest::header(const std::string& name) const {
    auto it = headers.find(name);
    if (it != headers.end()) {
        return it->second;
    }
    // Case-insensitive search
    for (const auto& [k, v] : headers) {
        std::string lower_k = k;
        std::transform(lower_k.begin(), lower_k.end(), lower_k.begin(), ::tolower);
        std::string lower_name = name;
        std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);
        if (lower_k == lower_name) {
            return v;
        }
    }
    return "";
}

std::optional<uint64_t> HttpRequest::content_length() const {
    auto cl = header("Content-Length");
    if (cl.empty()) return std::nullopt;
    return std::stoull(cl);
}

bool HttpRequest::has_range_header() const {
    return !header("Range").empty();
}

Range HttpRequest::parse_range_header(uint64_t total_size) const {
    auto range_str = header("Range");
    if (range_str.empty()) {
        return {0, total_size};
    }
    
    // Parse "bytes=start-end" format
    auto eq = range_str.find('=');
    if (eq == std::string::npos) {
        return {0, total_size};
    }
    
    auto range_val = range_str.substr(eq + 1);
    auto dash = range_val.find('-');
    if (dash == std::string::npos) {
        return {0, total_size};
    }
    
    uint64_t start = 0;
    uint64_t end = total_size - 1;
    
    if (dash > 0) {
        start = std::stoull(range_val.substr(0, dash));
    }
    if (dash < range_val.size() - 1) {
        end = std::stoull(range_val.substr(dash + 1));
    }
    
    // Handle suffix range (e.g., bytes=-500)
    if (dash == 0) {
        start = total_size - end;
        end = total_size - 1;
    }
    
    return {start, end - start + 1};
}

// HttpResponse implementation

HttpResponse HttpResponse::ok(ByteBuffer body) {
    HttpResponse resp;
    resp.status_code = 200;
    resp.status_text = "OK";
    resp.body = std::move(body);
    return resp;
}

HttpResponse HttpResponse::created() {
    HttpResponse resp;
    resp.status_code = 201;
    resp.status_text = "Created";
    return resp;
}

HttpResponse HttpResponse::no_content() {
    HttpResponse resp;
    resp.status_code = 204;
    resp.status_text = "No Content";
    return resp;
}

HttpResponse HttpResponse::partial_content(ByteBuffer body, const Range& range, uint64_t total) {
    HttpResponse resp;
    resp.status_code = 206;
    resp.status_text = "Partial Content";
    resp.body = std::move(body);
    resp.set_range_headers(range, total);
    return resp;
}

HttpResponse HttpResponse::not_found(const std::string& msg) {
    HttpResponse resp;
    resp.status_code = 404;
    resp.status_text = "Not Found";
    resp.body = ByteBuffer(msg.begin(), msg.end());
    resp.set_content_type("text/plain");
    return resp;
}

HttpResponse HttpResponse::bad_request(const std::string& msg) {
    HttpResponse resp;
    resp.status_code = 400;
    resp.status_text = "Bad Request";
    resp.body = ByteBuffer(msg.begin(), msg.end());
    resp.set_content_type("text/plain");
    return resp;
}

HttpResponse HttpResponse::internal_error(const std::string& msg) {
    HttpResponse resp;
    resp.status_code = 500;
    resp.status_text = "Internal Server Error";
    resp.body = ByteBuffer(msg.begin(), msg.end());
    resp.set_content_type("text/plain");
    return resp;
}

HttpResponse HttpResponse::service_unavailable(const std::string& msg) {
    HttpResponse resp;
    resp.status_code = 503;
    resp.status_text = "Service Unavailable";
    resp.body = ByteBuffer(msg.begin(), msg.end());
    resp.set_content_type("text/plain");
    return resp;
}

void HttpResponse::set_content_type(const std::string& type) {
    headers["Content-Type"] = type;
}

void HttpResponse::set_content_length(uint64_t len) {
    headers["Content-Length"] = std::to_string(len);
}

void HttpResponse::set_range_headers(const Range& range, uint64_t total) {
    std::ostringstream oss;
    oss << "bytes " << range.offset << "-" << (range.offset + range.length - 1) 
        << "/" << total;
    headers["Content-Range"] = oss.str();
    headers["Accept-Ranges"] = "bytes";
}

elio::http::response HttpResponse::to_elio_response() const {
    elio::http::response resp(static_cast<elio::http::status>(status_code));
    
    // Set body
    resp.set_body(std::string(
        reinterpret_cast<const char*>(body.data()), 
        body.size()
    ));
    
    // Set headers
    for (const auto& [name, value] : headers) {
        resp.set_header(name, value);
    }
    
    return resp;
}

// HttpHandler implementation

HttpHandler::HttpHandler(CacheCoordinator& cache)
    : cache_(cache)
{}

elio::coro::task<HttpResponse> HttpHandler::handle(const HttpRequest& req) {
    // Check custom routes first
    for (const auto& [method, pattern, handler] : routes_) {
        if (req.method == method && req.path.find(pattern) == 0) {
            co_return co_await handler(req);
        }
    }
    
    // Built-in routes
    if (req.path.starts_with("/cache/")) {
        if (req.method == "GET") {
            co_return co_await handle_get(req);
        } else if (req.method == "PUT" || req.method == "POST") {
            co_return co_await handle_put(req);
        } else if (req.method == "DELETE") {
            co_return co_await handle_delete(req);
        } else if (req.method == "HEAD") {
            co_return co_await handle_head(req);
        } else if (req.method == "OPTIONS") {
            co_return co_await handle_options(req);
        }
    } else if (req.path == "/stats" && req.method == "GET") {
        co_return co_await handle_stats(req);
    } else if (req.path == "/health" && req.method == "GET") {
        co_return co_await handle_health(req);
    } else if (req.path == "/metrics" && req.method == "GET") {
        co_return co_await handle_metrics(req);
    } else if (req.path.starts_with("/cluster") && req.method == "GET") {
        co_return co_await handle_cluster(req);
    }
    
    co_return HttpResponse::not_found("Endpoint not found");
}

void HttpHandler::add_route(const std::string& method, const std::string& pattern, 
                             Handler handler) {
    routes_.emplace_back(method, pattern, std::move(handler));
}

elio::coro::task<HttpResponse> HttpHandler::handle_get(const HttpRequest& req) {
    // Extract key from path: /cache/{key}
    std::string key = req.path.substr(7);  // Remove "/cache/"
    if (key.empty()) {
        co_return HttpResponse::bad_request("Missing cache key");
    }
    
    // Parse options
    ReadOptions opts;
    opts.allow_partial = true;
    
    // Check for Range header
    if (req.has_range_header()) {
        // We need metadata first to parse range properly
        auto meta = co_await cache_.metadata(CacheKey(key));
        if (meta) {
            opts.range = req.parse_range_header(meta->total_size);
        }
    }
    
    // Check query parameters
    if (!req.query.empty()) {
        // Parse range from query: ?range=offset-length
        auto range_pos = req.query.find("range=");
        if (range_pos != std::string::npos) {
            auto range_str = req.query.substr(range_pos + 6);
            auto dash = range_str.find('-');
            if (dash != std::string::npos) {
                uint64_t offset = std::stoull(range_str.substr(0, dash));
                uint64_t length = std::stoull(range_str.substr(dash + 1));
                opts.range = {offset, length};
            }
        }
    }
    
    auto result = co_await cache_.get(CacheKey(key), opts);
    
    if (result.result == CacheResult::Miss) {
        co_return HttpResponse::not_found("Key not found");
    }
    
    HttpResponse resp;
    
    if (result.result == CacheResult::PartialHit || opts.range.has_value()) {
        resp = HttpResponse::partial_content(
            std::move(result.data),
            result.actual_range,
            result.metadata ? result.metadata->total_size : result.data.size());
    } else {
        resp = HttpResponse::ok(std::move(result.data));
    }
    
    // Set custom headers
    std::string hit_source = "miss";
    if (result.result == CacheResult::Hit) hit_source = "hit";
    else if (result.result == CacheResult::PartialHit) hit_source = "partial";
    
    resp.headers["X-ElCache-Hit"] = hit_source;
    
    if (result.metadata) {
        resp.headers["X-ElCache-Size"] = std::to_string(result.metadata->total_size);
        resp.headers["X-ElCache-Chunks"] = std::to_string(result.metadata->chunk_count);
    }
    
    resp.set_content_type("application/octet-stream");
    resp.set_content_length(resp.body.size());
    
    co_return resp;
}

elio::coro::task<HttpResponse> HttpHandler::handle_put(const HttpRequest& req) {
    std::string key = req.path.substr(7);
    if (key.empty()) {
        co_return HttpResponse::bad_request("Missing cache key");
    }
    
    if (key.size() > MAX_KEY_SIZE) {
        co_return HttpResponse::bad_request("Key too large");
    }
    
    WriteOptions opts;
    
    // Parse headers for options
    auto ttl_header = req.header("X-ElCache-TTL");
    if (!ttl_header.empty()) {
        opts.ttl = std::chrono::seconds(std::stoll(ttl_header));
    }
    
    auto flags_header = req.header("X-ElCache-Flags");
    if (!flags_header.empty()) {
        opts.flags = std::stoul(flags_header);
    }
    
    opts.no_memory = req.header("X-ElCache-No-Memory") == "true";
    opts.no_disk = req.header("X-ElCache-No-Disk") == "true";
    opts.no_cluster = req.header("X-ElCache-No-Cluster") == "true";
    
    auto status = co_await cache_.put(CacheKey(key), req.body, opts);
    
    if (status) {
        co_return HttpResponse::created();
    }
    
    co_return HttpResponse::internal_error(status.message());
}

elio::coro::task<HttpResponse> HttpHandler::handle_delete(const HttpRequest& req) {
    std::string key = req.path.substr(7);
    if (key.empty()) {
        co_return HttpResponse::bad_request("Missing cache key");
    }
    
    DeleteOptions opts;
    auto status = co_await cache_.remove(CacheKey(key), opts);
    
    if (status) {
        co_return HttpResponse::no_content();
    }
    
    if (status.code() == ErrorCode::NotFound) {
        co_return HttpResponse::not_found("Key not found");
    }
    
    co_return HttpResponse::internal_error(status.message());
}

elio::coro::task<HttpResponse> HttpHandler::handle_head(const HttpRequest& req) {
    std::string key = req.path.substr(7);
    if (key.empty()) {
        co_return HttpResponse::bad_request("Missing cache key");
    }
    
    auto meta = co_await cache_.metadata(CacheKey(key));
    
    if (!meta) {
        co_return HttpResponse::not_found("Key not found");
    }
    
    HttpResponse resp;
    resp.status_code = 200;
    resp.status_text = "OK";
    resp.headers["X-ElCache-Size"] = std::to_string(meta->total_size);
    resp.headers["X-ElCache-Chunks"] = std::to_string(meta->chunk_count);
    resp.headers["Accept-Ranges"] = "bytes";
    resp.set_content_length(meta->total_size);
    
    co_return resp;
}

elio::coro::task<HttpResponse> HttpHandler::handle_options(const HttpRequest& req) {
    HttpResponse resp;
    resp.status_code = 200;
    resp.status_text = "OK";
    resp.headers["Allow"] = "GET, PUT, POST, DELETE, HEAD, OPTIONS";
    resp.headers["Accept-Ranges"] = "bytes";
    co_return resp;
}

elio::coro::task<HttpResponse> HttpHandler::handle_stats(const HttpRequest& req) {
    auto stats = cache_.stats();
    
    std::ostringstream json;
    json << "{\n";
    json << "  \"hits\": " << stats.hits << ",\n";
    json << "  \"misses\": " << stats.misses << ",\n";
    json << "  \"partial_hits\": " << stats.partial_hits << ",\n";
    json << "  \"bytes_read\": " << stats.bytes_read << ",\n";
    json << "  \"bytes_written\": " << stats.bytes_written << ",\n";
    json << "  \"evictions\": " << stats.evictions << ",\n";
    json << "  \"entry_count\": " << stats.entry_count << ",\n";
    json << "  \"size_bytes\": " << stats.size_bytes << ",\n";
    json << "  \"capacity_bytes\": " << stats.capacity_bytes << ",\n";
    
    double hit_rate = (stats.hits + stats.misses > 0) 
        ? static_cast<double>(stats.hits) / (stats.hits + stats.misses) * 100 
        : 0;
    json << "  \"hit_rate_percent\": " << hit_rate << "\n";
    json << "}\n";
    
    auto body_str = json.str();
    HttpResponse resp = HttpResponse::ok(ByteBuffer(body_str.begin(), body_str.end()));
    resp.set_content_type("application/json");
    co_return resp;
}

elio::coro::task<HttpResponse> HttpHandler::handle_health(const HttpRequest& req) {
    std::string body = R"({"status": "healthy"})";
    HttpResponse resp = HttpResponse::ok(ByteBuffer(body.begin(), body.end()));
    resp.set_content_type("application/json");
    co_return resp;
}

elio::coro::task<HttpResponse> HttpHandler::handle_cluster(const HttpRequest& req) {
    auto* cluster = cache_.cluster();
    
    if (!cluster) {
        std::string body = R"({"mode": "standalone"})";
        HttpResponse resp = HttpResponse::ok(ByteBuffer(body.begin(), body.end()));
        resp.set_content_type("application/json");
        co_return resp;
    }
    
    auto stats = cluster->stats();
    
    std::ostringstream json;
    json << "{\n";
    json << "  \"mode\": \"cluster\",\n";
    json << "  \"total_nodes\": " << stats.total_nodes << ",\n";
    json << "  \"active_nodes\": " << stats.active_nodes << ",\n";
    json << "  \"total_memory_capacity\": " << stats.total_memory_capacity << ",\n";
    json << "  \"total_disk_capacity\": " << stats.total_disk_capacity << ",\n";
    json << "  \"total_memory_used\": " << stats.total_memory_used << ",\n";
    json << "  \"total_disk_used\": " << stats.total_disk_used << "\n";
    json << "}\n";
    
    auto body_str = json.str();
    HttpResponse resp = HttpResponse::ok(ByteBuffer(body_str.begin(), body_str.end()));
    resp.set_content_type("application/json");
    co_return resp;
}

elio::coro::task<HttpResponse> HttpHandler::handle_metrics(const HttpRequest& req) {
    if (!metrics_) {
        co_return HttpResponse::service_unavailable("Metrics not configured");
    }
    
    // Collect latest metrics
    metrics_->collect();
    
    // Check Accept header for format preference
    auto accept = req.header("Accept");
    
    std::string body;
    std::string content_type;
    
    if (accept.find("application/json") != std::string::npos) {
        body = metrics_->export_json();
        content_type = "application/json";
    } else {
        // Default to Prometheus format
        body = metrics_->export_prometheus();
        content_type = "text/plain; version=0.0.4; charset=utf-8";
    }
    
    HttpResponse resp = HttpResponse::ok(ByteBuffer(body.begin(), body.end()));
    resp.set_content_type(content_type);
    co_return resp;
}

// Sparse/Partial Write Handlers

elio::coro::task<HttpResponse> HttpHandler::handle_sparse_create(const HttpRequest& req) {
    // Extract key from path: /sparse/{key}
    std::string key = req.path.substr(8);  // Remove "/sparse/"
    if (key.empty()) {
        co_return HttpResponse::bad_request("Missing key");
    }
    
    // Get total size from header
    auto size_header = req.header("X-ElCache-Size");
    if (size_header.empty()) {
        co_return HttpResponse::bad_request("X-ElCache-Size header required");
    }
    
    uint64_t total_size;
    try {
        total_size = std::stoull(size_header);
    } catch (...) {
        co_return HttpResponse::bad_request("Invalid X-ElCache-Size value");
    }
    
    if (total_size == 0) {
        co_return HttpResponse::bad_request("Size must be greater than 0");
    }
    
    // Create sparse entry
    bool created = global_sparse_cache().create(key, total_size);
    
    if (!created) {
        HttpResponse resp;
        resp.status_code = 409;  // Conflict
        resp.status_text = "Conflict";
        std::string msg = "Sparse entry already exists";
        resp.body = ByteBuffer(msg.begin(), msg.end());
        resp.set_content_type("text/plain");
        co_return resp;
    }
    
    HttpResponse resp = HttpResponse::created();
    resp.headers["X-ElCache-Size"] = std::to_string(total_size);
    co_return resp;
}

elio::coro::task<HttpResponse> HttpHandler::handle_sparse_write(const HttpRequest& req) {
    // Extract key from path: /sparse/{key}
    std::string key = req.path.substr(8);  // Remove "/sparse/"
    if (key.empty()) {
        co_return HttpResponse::bad_request("Missing key");
    }
    
    // Parse Content-Range header: "bytes start-end/total" or query param ?offset=
    uint64_t offset = 0;
    
    auto content_range = req.header("Content-Range");
    if (!content_range.empty()) {
        // Parse "bytes start-end/total"
        auto bytes_pos = content_range.find("bytes ");
        if (bytes_pos != std::string::npos) {
            auto range_part = content_range.substr(bytes_pos + 6);
            auto dash_pos = range_part.find('-');
            if (dash_pos != std::string::npos) {
                try {
                    offset = std::stoull(range_part.substr(0, dash_pos));
                } catch (...) {
                    co_return HttpResponse::bad_request("Invalid Content-Range");
                }
            }
        }
    } else {
        // Try query parameter ?offset=
        auto offset_pos = req.query.find("offset=");
        if (offset_pos != std::string::npos) {
            try {
                offset = std::stoull(req.query.substr(offset_pos + 7));
            } catch (...) {
                co_return HttpResponse::bad_request("Invalid offset parameter");
            }
        }
    }
    
    if (req.body.empty()) {
        co_return HttpResponse::bad_request("Request body is empty");
    }
    
    // Write to sparse entry
    bool success = global_sparse_cache().write_range(key, offset, req.body.data(), req.body.size());
    
    if (!success) {
        if (!global_sparse_cache().exists(key)) {
            co_return HttpResponse::not_found("Sparse entry not found");
        }
        co_return HttpResponse::bad_request("Invalid range (offset + size exceeds total)");
    }
    
    HttpResponse resp;
    resp.status_code = 202;  // Accepted
    resp.status_text = "Accepted";
    resp.headers["X-ElCache-Completion"] = std::to_string(global_sparse_cache().get_completion(key)) + "%";
    co_return resp;
}

elio::coro::task<HttpResponse> HttpHandler::handle_sparse_finalize(const HttpRequest& req) {
    // Extract key from path: /sparse/{key}/finalize
    std::string path = req.path;
    auto finalize_pos = path.rfind("/finalize");
    if (finalize_pos == std::string::npos) {
        co_return HttpResponse::bad_request("Invalid path");
    }
    
    std::string key = path.substr(8, finalize_pos - 8);  // Remove "/sparse/" and "/finalize"
    if (key.empty()) {
        co_return HttpResponse::bad_request("Missing key");
    }
    
    // Finalize sparse entry
    std::vector<uint8_t> data;
    int result = global_sparse_cache().finalize(key, data);
    
    if (result == 1) {
        co_return HttpResponse::not_found("Sparse entry not found");
    }
    
    if (result == 2) {
        HttpResponse resp;
        resp.status_code = 409;  // Conflict
        resp.status_text = "Conflict";
        std::string msg = "Not all ranges written";
        resp.body = ByteBuffer(msg.begin(), msg.end());
        resp.set_content_type("text/plain");
        resp.headers["X-ElCache-Completion"] = std::to_string(global_sparse_cache().get_completion(key)) + "%";
        co_return resp;
    }
    
    // Store in main cache
    WriteOptions opts;
    auto ttl_header = req.header("X-ElCache-TTL");
    if (!ttl_header.empty()) {
        opts.ttl = std::chrono::seconds(std::stoll(ttl_header));
    }
    
    auto status = co_await cache_.put(CacheKey(key), ByteBuffer(data.begin(), data.end()), opts);
    
    if (!status) {
        co_return HttpResponse::internal_error("Failed to store finalized data: " + status.message());
    }
    
    co_return HttpResponse::created();
}

elio::coro::task<HttpResponse> HttpHandler::handle_sparse_status(const HttpRequest& req) {
    // Extract key from path: /sparse/{key}
    std::string key = req.path.substr(8);  // Remove "/sparse/"
    if (key.empty()) {
        co_return HttpResponse::bad_request("Missing key");
    }
    
    if (!global_sparse_cache().exists(key)) {
        co_return HttpResponse::not_found("Sparse entry not found");
    }
    
    uint64_t total_size = global_sparse_cache().get_total_size(key);
    double completion = global_sparse_cache().get_completion(key);
    
    std::ostringstream json;
    json << "{\n";
    json << "  \"key\": \"" << key << "\",\n";
    json << "  \"total_size\": " << total_size << ",\n";
    json << "  \"completion_percent\": " << completion << ",\n";
    json << "  \"complete\": " << (completion >= 100.0 ? "true" : "false") << "\n";
    json << "}\n";
    
    auto body_str = json.str();
    HttpResponse resp = HttpResponse::ok(ByteBuffer(body_str.begin(), body_str.end()));
    resp.set_content_type("application/json");
    co_return resp;
}

// Elio handler adapters - wrap internal handlers for Elio's router

elio::coro::task<elio::http::response> HttpHandler::elio_get_handler(elio::http::context& ctx) {
    auto req = HttpRequest::from_context(ctx);
    auto resp = co_await handle_get(req);
    co_return resp.to_elio_response();
}

elio::coro::task<elio::http::response> HttpHandler::elio_put_handler(elio::http::context& ctx) {
    auto req = HttpRequest::from_context(ctx);
    auto resp = co_await handle_put(req);
    co_return resp.to_elio_response();
}

elio::coro::task<elio::http::response> HttpHandler::elio_delete_handler(elio::http::context& ctx) {
    auto req = HttpRequest::from_context(ctx);
    auto resp = co_await handle_delete(req);
    co_return resp.to_elio_response();
}

elio::coro::task<elio::http::response> HttpHandler::elio_head_handler(elio::http::context& ctx) {
    auto req = HttpRequest::from_context(ctx);
    auto resp = co_await handle_head(req);
    co_return resp.to_elio_response();
}

elio::coro::task<elio::http::response> HttpHandler::elio_options_handler(elio::http::context& ctx) {
    auto req = HttpRequest::from_context(ctx);
    auto resp = co_await handle_options(req);
    co_return resp.to_elio_response();
}

elio::coro::task<elio::http::response> HttpHandler::elio_stats_handler(elio::http::context& ctx) {
    auto req = HttpRequest::from_context(ctx);
    auto resp = co_await handle_stats(req);
    co_return resp.to_elio_response();
}

elio::coro::task<elio::http::response> HttpHandler::elio_health_handler(elio::http::context& ctx) {
    auto req = HttpRequest::from_context(ctx);
    auto resp = co_await handle_health(req);
    co_return resp.to_elio_response();
}

elio::coro::task<elio::http::response> HttpHandler::elio_cluster_handler(elio::http::context& ctx) {
    auto req = HttpRequest::from_context(ctx);
    auto resp = co_await handle_cluster(req);
    co_return resp.to_elio_response();
}

elio::coro::task<elio::http::response> HttpHandler::elio_metrics_handler(elio::http::context& ctx) {
    auto req = HttpRequest::from_context(ctx);
    auto resp = co_await handle_metrics(req);
    co_return resp.to_elio_response();
}

elio::coro::task<elio::http::response> HttpHandler::elio_sparse_create_handler(elio::http::context& ctx) {
    auto req = HttpRequest::from_context(ctx);
    auto resp = co_await handle_sparse_create(req);
    co_return resp.to_elio_response();
}

elio::coro::task<elio::http::response> HttpHandler::elio_sparse_write_handler(elio::http::context& ctx) {
    auto req = HttpRequest::from_context(ctx);
    auto resp = co_await handle_sparse_write(req);
    co_return resp.to_elio_response();
}

elio::coro::task<elio::http::response> HttpHandler::elio_sparse_finalize_handler(elio::http::context& ctx) {
    auto req = HttpRequest::from_context(ctx);
    auto resp = co_await handle_sparse_finalize(req);
    co_return resp.to_elio_response();
}

elio::coro::task<elio::http::response> HttpHandler::elio_sparse_status_handler(elio::http::context& ctx) {
    auto req = HttpRequest::from_context(ctx);
    auto resp = co_await handle_sparse_status(req);
    co_return resp.to_elio_response();
}

elio::http::router HttpHandler::build_router() {
    elio::http::router router;
    
    // Cache endpoints with path parameter :key
    router.get("/cache/:key", [this](elio::http::context& ctx) {
        return elio_get_handler(ctx);
    });
    
    router.put("/cache/:key", [this](elio::http::context& ctx) {
        return elio_put_handler(ctx);
    });
    
    router.post("/cache/:key", [this](elio::http::context& ctx) {
        return elio_put_handler(ctx);
    });
    
    router.del("/cache/:key", [this](elio::http::context& ctx) {
        return elio_delete_handler(ctx);
    });
    
    router.add_route(elio::http::method::HEAD, "/cache/:key", [this](elio::http::context& ctx) {
        return elio_head_handler(ctx);
    });
    
    router.options("/cache/:key", [this](elio::http::context& ctx) {
        return elio_options_handler(ctx);
    });
    
    // Admin/stats endpoints
    router.get("/stats", [this](elio::http::context& ctx) {
        return elio_stats_handler(ctx);
    });
    
    router.get("/health", [this](elio::http::context& ctx) {
        return elio_health_handler(ctx);
    });
    
    router.get("/metrics", [this](elio::http::context& ctx) {
        return elio_metrics_handler(ctx);
    });
    
    router.get("/cluster", [this](elio::http::context& ctx) {
        return elio_cluster_handler(ctx);
    });
    
    router.get("/cluster/nodes", [this](elio::http::context& ctx) {
        return elio_cluster_handler(ctx);
    });
    
    // Sparse/Partial write endpoints
    // POST /sparse/{key} - Create sparse entry
    router.post("/sparse/:key", [this](elio::http::context& ctx) {
        return elio_sparse_create_handler(ctx);
    });
    
    // PATCH /sparse/{key} - Write range to sparse entry
    router.add_route(elio::http::method::PATCH, "/sparse/:key", [this](elio::http::context& ctx) {
        return elio_sparse_write_handler(ctx);
    });
    
    // PUT /sparse/{key} - Alternative to PATCH for range write
    router.put("/sparse/:key", [this](elio::http::context& ctx) {
        return elio_sparse_write_handler(ctx);
    });
    
    // POST /sparse/{key}/finalize - Finalize sparse entry
    router.post("/sparse/:key/finalize", [this](elio::http::context& ctx) {
        return elio_sparse_finalize_handler(ctx);
    });
    
    // GET /sparse/{key} - Get sparse entry status
    router.get("/sparse/:key", [this](elio::http::context& ctx) {
        return elio_sparse_status_handler(ctx);
    });
    
    // DELETE /sparse/{key} - Abort sparse write (use sync handler)
    router.del("/sparse/:key", [](elio::http::context& ctx) -> elio::http::response {
        auto path = std::string(ctx.req().path());
        std::string key = path.substr(8);  // Remove "/sparse/"
        global_sparse_cache().remove(key);
        return elio::http::response(elio::http::status::no_content);
    });
    
    return router;
}

// HttpServer implementation using Elio's async HTTP server

HttpServer::HttpServer(const NetworkConfig& config, HttpHandler& handler)
    : config_(config)
    , handler_(handler)
{}

HttpServer::~HttpServer() {
    if (server_) {
        server_->stop();
    }
}

elio::coro::task<Status> HttpServer::start(elio::io::io_context& io_ctx, 
                                            elio::runtime::scheduler& sched) {
    running_ = true;
    
    // Build router from handler
    auto router = handler_.build_router();
    
    // Configure server
    elio::http::server_config server_config;
    server_config.max_request_size = 100 * 1024 * 1024;  // 100MB max request for large blobs
    server_config.read_buffer_size = 64 * 1024;          // 64KB read buffer
    server_config.keep_alive_timeout = std::chrono::duration_cast<std::chrono::seconds>(config_.read_timeout);
    server_config.max_keep_alive_requests = 1000;
    server_config.enable_logging = true;
    
    // Create server
    server_ = std::make_unique<elio::http::server>(std::move(router), server_config);
    
    // Create bind address
    elio::net::ipv4_address addr(config_.bind_address, config_.http_port);
    
    // Start listening - run the accept loop directly instead of spawning
    // This ensures the accept loop runs on the scheduler properly
    auto listen_task = server_->listen(addr, io_ctx, sched);
    sched.spawn(listen_task.release());
    
    std::cout << "[ElCache] HTTP server listen task spawned\n" << std::flush;
    
    co_return Status::make_ok();
}

elio::coro::task<void> HttpServer::stop() {
    running_ = false;
    if (server_) {
        server_->stop();
    }
    co_return;
}

}  // namespace elcache
