#include "elcache/config.hpp"
#include <fstream>
#include <sstream>

namespace elcache {

// Simple JSON parsing (minimal implementation without external deps)
// For production, consider using a proper JSON library

namespace {

std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\n\r");
    return s.substr(start, end - start + 1);
}

// Very simple JSON value extraction (supports basic types)
class SimpleJson {
public:
    explicit SimpleJson(const std::string& json) : json_(json) {}
    
    std::string get_string(const std::string& key, const std::string& def = "") const {
        auto pos = json_.find("\"" + key + "\"");
        if (pos == std::string::npos) return def;
        
        pos = json_.find(':', pos);
        if (pos == std::string::npos) return def;
        
        pos = json_.find('"', pos);
        if (pos == std::string::npos) return def;
        
        auto end = json_.find('"', pos + 1);
        if (end == std::string::npos) return def;
        
        return json_.substr(pos + 1, end - pos - 1);
    }
    
    int64_t get_int(const std::string& key, int64_t def = 0) const {
        auto pos = json_.find("\"" + key + "\"");
        if (pos == std::string::npos) return def;
        
        pos = json_.find(':', pos);
        if (pos == std::string::npos) return def;
        
        // Skip whitespace
        ++pos;
        while (pos < json_.size() && std::isspace(json_[pos])) ++pos;
        
        // Parse number
        auto end = pos;
        while (end < json_.size() && (std::isdigit(json_[end]) || json_[end] == '-')) ++end;
        
        if (end == pos) return def;
        return std::stoll(json_.substr(pos, end - pos));
    }
    
    double get_double(const std::string& key, double def = 0.0) const {
        auto pos = json_.find("\"" + key + "\"");
        if (pos == std::string::npos) return def;
        
        pos = json_.find(':', pos);
        if (pos == std::string::npos) return def;
        
        ++pos;
        while (pos < json_.size() && std::isspace(json_[pos])) ++pos;
        
        auto end = pos;
        while (end < json_.size() && (std::isdigit(json_[end]) || json_[end] == '-' || json_[end] == '.')) ++end;
        
        if (end == pos) return def;
        return std::stod(json_.substr(pos, end - pos));
    }
    
    bool get_bool(const std::string& key, bool def = false) const {
        auto pos = json_.find("\"" + key + "\"");
        if (pos == std::string::npos) return def;
        
        pos = json_.find(':', pos);
        if (pos == std::string::npos) return def;
        
        if (json_.find("true", pos) != std::string::npos) return true;
        if (json_.find("false", pos) != std::string::npos) return false;
        return def;
    }
    
    std::vector<std::string> get_string_array(const std::string& key) const {
        std::vector<std::string> result;
        auto pos = json_.find("\"" + key + "\"");
        if (pos == std::string::npos) return result;
        
        pos = json_.find('[', pos);
        if (pos == std::string::npos) return result;
        
        auto end = json_.find(']', pos);
        if (end == std::string::npos) return result;
        
        std::string arr = json_.substr(pos + 1, end - pos - 1);
        size_t start = 0;
        while ((start = arr.find('"', start)) != std::string::npos) {
            auto str_end = arr.find('"', start + 1);
            if (str_end == std::string::npos) break;
            result.push_back(arr.substr(start + 1, str_end - start - 1));
            start = str_end + 1;
        }
        
        return result;
    }
    
    SimpleJson get_object(const std::string& key) const {
        auto pos = json_.find("\"" + key + "\"");
        if (pos == std::string::npos) return SimpleJson("{}");
        
        pos = json_.find('{', pos);
        if (pos == std::string::npos) return SimpleJson("{}");
        
        int depth = 1;
        size_t end = pos + 1;
        while (end < json_.size() && depth > 0) {
            if (json_[end] == '{') ++depth;
            else if (json_[end] == '}') --depth;
            ++end;
        }
        
        return SimpleJson(json_.substr(pos, end - pos));
    }
    
private:
    std::string json_;
};

}  // namespace

Config Config::load(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) {
        throw std::runtime_error("Failed to open config file: " + path.string());
    }
    
    std::stringstream buffer;
    buffer << file.rdbuf();
    return load_json(buffer.str());
}

Config Config::load_json(const std::string& json) {
    Config config;
    SimpleJson j(json);
    
    config.node_name = j.get_string("node_name", "");
    
    // Memory config
    auto mem = j.get_object("memory");
    config.memory.max_size = mem.get_int("max_size", config.memory.max_size);
    config.memory.arc_ghost_size = mem.get_int("arc_ghost_size", config.memory.arc_ghost_size);
    config.memory.high_watermark = mem.get_double("high_watermark", 0.95);
    config.memory.low_watermark = mem.get_double("low_watermark", 0.85);
    
    // Disk config
    auto disk = j.get_object("disk");
    auto disk_path = disk.get_string("path", "");
    if (!disk_path.empty()) {
        config.disk.path = disk_path;
    }
    config.disk.max_size = disk.get_int("max_size", config.disk.max_size);
    config.disk.use_direct_io = disk.get_bool("use_direct_io", true);
    config.disk.use_fallocate = disk.get_bool("use_fallocate", true);
    config.disk.high_watermark = disk.get_double("high_watermark", 0.90);
    config.disk.low_watermark = disk.get_double("low_watermark", 0.80);
    
    // Network config
    auto net = j.get_object("network");
    config.network.bind_address = net.get_string("bind_address", "0.0.0.0");
    config.network.cluster_port = net.get_int("cluster_port", 7890);
    config.network.http_port = net.get_int("http_port", 8080);
    config.network.sdk_port = net.get_int("sdk_port", 7891);
    config.network.unix_socket_path = net.get_string("unix_socket_path", "/var/run/elcache/elcache.sock");
    config.network.shm_path = net.get_string("shm_path", "/elcache_shm");
    config.network.shm_size = net.get_int("shm_size", 256 * 1024 * 1024);
    config.network.max_connections = net.get_int("max_connections", 10000);
    
    // Cluster config
    auto cluster = j.get_object("cluster");
    config.cluster.cluster_name = cluster.get_string("cluster_name", "elcache");
    config.cluster.seed_nodes = cluster.get_string_array("seed_nodes");
    config.cluster.gossip_interval = std::chrono::milliseconds(
        cluster.get_int("gossip_interval_ms", 1000));
    config.cluster.failure_detection_timeout = std::chrono::milliseconds(
        cluster.get_int("failure_detection_timeout_ms", 10000));
    config.cluster.gossip_fanout = cluster.get_int("gossip_fanout", 3);
    config.cluster.replication_factor = cluster.get_int("replication_factor", 2);
    config.cluster.prefer_local = cluster.get_bool("prefer_local", true);
    config.cluster.virtual_nodes = cluster.get_int("virtual_nodes", 150);
    
    // Pool config
    auto pool = j.get_object("pool");
    config.pool.memory_contribution = pool.get_int("memory_contribution", 0);
    config.pool.disk_contribution = pool.get_int("disk_contribution", 0);
    config.pool.accept_remote_reads = pool.get_bool("accept_remote_reads", true);
    config.pool.accept_remote_writes = pool.get_bool("accept_remote_writes", true);
    
    // Performance config
    auto perf = j.get_object("performance");
    config.perf.io_threads = perf.get_int("io_threads", 0);
    config.perf.worker_threads = perf.get_int("worker_threads", 0);
    config.perf.max_concurrent_disk_ops = perf.get_int("max_concurrent_disk_ops", 64);
    config.perf.max_concurrent_network_ops = perf.get_int("max_concurrent_network_ops", 256);
    config.perf.prefetch_chunks = perf.get_int("prefetch_chunks", 2);
    config.perf.enable_metrics = perf.get_bool("enable_metrics", true);
    
    return config;
}

void Config::save(const std::filesystem::path& path) const {
    std::ofstream file(path);
    if (!file) {
        throw std::runtime_error("Failed to open config file for writing: " + path.string());
    }
    file << to_json();
}

std::string Config::to_json() const {
    std::ostringstream oss;
    oss << "{\n";
    oss << "  \"node_name\": \"" << node_name << "\",\n";
    
    // Memory
    oss << "  \"memory\": {\n";
    oss << "    \"max_size\": " << memory.max_size << ",\n";
    oss << "    \"arc_ghost_size\": " << memory.arc_ghost_size << ",\n";
    oss << "    \"high_watermark\": " << memory.high_watermark << ",\n";
    oss << "    \"low_watermark\": " << memory.low_watermark << "\n";
    oss << "  },\n";
    
    // Disk
    oss << "  \"disk\": {\n";
    oss << "    \"path\": \"" << disk.path.string() << "\",\n";
    oss << "    \"max_size\": " << disk.max_size << ",\n";
    oss << "    \"use_direct_io\": " << (disk.use_direct_io ? "true" : "false") << ",\n";
    oss << "    \"use_fallocate\": " << (disk.use_fallocate ? "true" : "false") << ",\n";
    oss << "    \"high_watermark\": " << disk.high_watermark << ",\n";
    oss << "    \"low_watermark\": " << disk.low_watermark << "\n";
    oss << "  },\n";
    
    // Network
    oss << "  \"network\": {\n";
    oss << "    \"bind_address\": \"" << network.bind_address << "\",\n";
    oss << "    \"cluster_port\": " << network.cluster_port << ",\n";
    oss << "    \"http_port\": " << network.http_port << ",\n";
    oss << "    \"sdk_port\": " << network.sdk_port << ",\n";
    oss << "    \"unix_socket_path\": \"" << network.unix_socket_path << "\",\n";
    oss << "    \"shm_path\": \"" << network.shm_path << "\",\n";
    oss << "    \"shm_size\": " << network.shm_size << ",\n";
    oss << "    \"max_connections\": " << network.max_connections << "\n";
    oss << "  },\n";
    
    // Cluster
    oss << "  \"cluster\": {\n";
    oss << "    \"cluster_name\": \"" << cluster.cluster_name << "\",\n";
    oss << "    \"seed_nodes\": [";
    for (size_t i = 0; i < cluster.seed_nodes.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << "\"" << cluster.seed_nodes[i] << "\"";
    }
    oss << "],\n";
    oss << "    \"gossip_interval_ms\": " << cluster.gossip_interval.count() << ",\n";
    oss << "    \"failure_detection_timeout_ms\": " << cluster.failure_detection_timeout.count() << ",\n";
    oss << "    \"gossip_fanout\": " << cluster.gossip_fanout << ",\n";
    oss << "    \"replication_factor\": " << cluster.replication_factor << ",\n";
    oss << "    \"prefer_local\": " << (cluster.prefer_local ? "true" : "false") << ",\n";
    oss << "    \"virtual_nodes\": " << cluster.virtual_nodes << "\n";
    oss << "  },\n";
    
    // Pool
    oss << "  \"pool\": {\n";
    oss << "    \"memory_contribution\": " << pool.memory_contribution << ",\n";
    oss << "    \"disk_contribution\": " << pool.disk_contribution << ",\n";
    oss << "    \"accept_remote_reads\": " << (pool.accept_remote_reads ? "true" : "false") << ",\n";
    oss << "    \"accept_remote_writes\": " << (pool.accept_remote_writes ? "true" : "false") << "\n";
    oss << "  },\n";
    
    // Performance
    oss << "  \"performance\": {\n";
    oss << "    \"io_threads\": " << perf.io_threads << ",\n";
    oss << "    \"worker_threads\": " << perf.worker_threads << ",\n";
    oss << "    \"max_concurrent_disk_ops\": " << perf.max_concurrent_disk_ops << ",\n";
    oss << "    \"max_concurrent_network_ops\": " << perf.max_concurrent_network_ops << ",\n";
    oss << "    \"prefetch_chunks\": " << perf.prefetch_chunks << ",\n";
    oss << "    \"enable_metrics\": " << (perf.enable_metrics ? "true" : "false") << ",\n";
    oss << "    \"metrics_interval_s\": " << perf.metrics_interval.count() << "\n";
    oss << "  }\n";
    
    oss << "}\n";
    return oss.str();
}

Status Config::validate() const {
    if (memory.max_size == 0 && disk.max_size == 0) {
        return Status::error(ErrorCode::InvalidArgument, 
                            "At least one of memory or disk cache must be enabled");
    }
    
    if (memory.high_watermark <= memory.low_watermark) {
        return Status::error(ErrorCode::InvalidArgument,
                            "Memory high_watermark must be greater than low_watermark");
    }
    
    if (disk.high_watermark <= disk.low_watermark) {
        return Status::error(ErrorCode::InvalidArgument,
                            "Disk high_watermark must be greater than low_watermark");
    }
    
    if (network.http_port == network.cluster_port || 
        network.http_port == network.sdk_port ||
        network.cluster_port == network.sdk_port) {
        return Status::error(ErrorCode::InvalidArgument,
                            "Network ports must be unique");
    }
    
    if (cluster.replication_factor == 0) {
        return Status::error(ErrorCode::InvalidArgument,
                            "Replication factor must be at least 1");
    }
    
    return Status::make_ok();
}

}  // namespace elcache
