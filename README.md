# ElCache

A high-performance distributed blob cache system built with C++20 and the [Elio](https://github.com/Coldwings/Elio) coroutine library.

## Features

- **Multi-level caching**: Memory -> Disk -> Distributed cluster hierarchy
- **Partial read support**: Access any byte range of cached values up to 20TB
- **Decentralized architecture**: No single point of failure, nodes discover each other via gossip
- **High throughput**: Lock-free data structures, async I/O with io_uring
- **Flexible APIs**: HTTP REST API and zero-dependency C SDK
- **Prometheus metrics**: Built-in metrics export for monitoring

## Quick Start

### Building

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

### Running

```bash
# Start with default settings (1GB memory cache)
./elcached

# Custom configuration
./elcached -m 4096 -p 8080 -d /var/cache/elcache -D 100

# With config file
./elcached -c /etc/elcache/elcache.json
```

### Basic Usage (HTTP API)

```bash
# Store a value
curl -X PUT http://localhost:8080/cache/mykey -d "Hello, World!"

# Retrieve a value
curl http://localhost:8080/cache/mykey

# Partial read (bytes 0-4)
curl -H "Range: bytes=0-4" http://localhost:8080/cache/mykey

# Delete a value
curl -X DELETE http://localhost:8080/cache/mykey

# Check health
curl http://localhost:8080/health

# Get metrics (Prometheus format)
curl http://localhost:8080/metrics
```

### Basic Usage (C SDK)

```c
#include <elcache/elcache_sdk.h>

int main() {
    elcache_client_t* client = elcache_client_create();
    elcache_client_connect_unix(client, "/var/run/elcache/elcache.sock");
    
    // Store
    elcache_client_put(client, "key", 3, "value", 5, NULL);
    
    // Retrieve
    char buffer[1024];
    size_t len;
    elcache_client_get(client, "key", 3, buffer, sizeof(buffer), &len, NULL);
    
    elcache_client_destroy(client);
    return 0;
}
```

## Architecture

ElCache uses a multi-level cache architecture:

```
┌─────────────────────────────────────────────────────────────────┐
│                         Client Request                          │
└─────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│                      Cache Coordinator                          │
│   - Routes requests to appropriate cache level                  │
│   - Handles partial reads across chunks                         │
│   - Manages cache promotion/demotion                            │
└─────────────────────────────────────────────────────────────────┘
                                │
            ┌───────────────────┼───────────────────┐
            ▼                   ▼                   ▼
    ┌───────────────┐   ┌───────────────┐   ┌───────────────┐
    │ Memory Cache  │   │  Disk Cache   │   │    Cluster    │
    │   (ARC)       │   │  (LRU + Disk) │   │   (Gossip)    │
    │               │   │               │   │               │
    │ - Fast access │   │ - Large cap.  │   │ - Distributed │
    │ - ARC eviction│   │ - Persistent  │   │ - Replicated  │
    └───────────────┘   └───────────────┘   └───────────────┘
```

### Chunk-Based Storage

Large values are split into 4MB chunks for efficient partial caching:

- Content-addressed chunks (128-bit hash)
- Partial reads fetch only required chunks
- Chunks can be distributed across cluster nodes
- Fine-grained cache eviction

### Cluster Mode

Nodes form a decentralized cluster using:

- **Gossip protocol**: Node discovery and failure detection
- **Consistent hashing**: Balanced data distribution with virtual nodes
- **Replication**: Configurable replication factor for durability

## Configuration

See [config/elcache.json.example](config/elcache.json.example) for all options.

Key settings:

| Setting | Default | Description |
|---------|---------|-------------|
| `memory.max_size` | 1GB | Memory cache capacity |
| `disk.max_size` | 100GB | Disk cache capacity |
| `disk.path` | /var/cache/elcache | Disk cache directory |
| `network.http_port` | 8080 | HTTP API port |
| `network.cluster_port` | 7890 | Inter-node communication port |
| `cluster.replication_factor` | 2 | Number of replicas |

## Documentation

- [Architecture Guide](docs/ARCHITECTURE.md)
- [HTTP API Reference](docs/API.md)
- [C SDK Guide](docs/SDK.md)

## Performance

ElCache is designed for high throughput:

- **Memory cache**: Millions of ops/sec with ARC eviction
- **Disk cache**: Async I/O with io_uring (Linux 5.1+)
- **Network**: Lock-free message passing, zero-copy where possible
- **Partial reads**: Only fetch needed chunks, not entire values

## Requirements

- C++20 compiler (GCC 11+, Clang 14+)
- CMake 3.20+
- Linux (for io_uring support, optional)
- OpenSSL (for TLS)

## License

MIT License
