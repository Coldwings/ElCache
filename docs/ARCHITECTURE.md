# ElCache Architecture

This document describes the internal architecture of ElCache.

## Overview

ElCache is a multi-level distributed cache designed for storing large binary objects (blobs) with support for partial reads. The system is built around three key principles:

1. **Chunk-based storage**: Large values are split into fixed-size chunks
2. **Multi-level hierarchy**: Memory -> Disk -> Network
3. **Decentralized clustering**: No coordinator, nodes are peers

## Core Components

### 1. Cache Coordinator

The `CacheCoordinator` is the central orchestrator for cache operations:

```
┌──────────────────────────────────────────────────────────────┐
│                     CacheCoordinator                         │
├──────────────────────────────────────────────────────────────┤
│  get(key, options)     → ReadResult                         │
│  put(key, value, opts) → Status                             │
│  remove(key, opts)     → Status                             │
│  check(key, range)     → AvailabilityResult                 │
├──────────────────────────────────────────────────────────────┤
│  - Routes to appropriate cache level                        │
│  - Assembles partial results from multiple chunks           │
│  - Handles cache promotion (disk→memory)                    │
│  - Manages cluster replication                              │
└──────────────────────────────────────────────────────────────┘
```

### 2. Chunk System

Values are divided into chunks for efficient storage and retrieval:

```
Value (12MB)
┌────────────┬────────────┬────────────┐
│  Chunk 0   │  Chunk 1   │  Chunk 2   │
│   (4MB)    │   (4MB)    │   (4MB)    │
└────────────┴────────────┴────────────┘
     │             │             │
     ▼             ▼             ▼
  ChunkId       ChunkId       ChunkId
  (hash128)    (hash128)    (hash128)
```

**Key structures:**

- `Chunk`: Raw data with content-addressed ID
- `ChunkId`: 128-bit hash of chunk contents
- `ValueDescriptor`: Maps a key to its ordered list of chunks
- `PartialValue`: Tracks which chunks are available locally

**Benefits:**

- Partial reads only fetch needed chunks
- Deduplication across keys with same content
- Fine-grained eviction (evict chunks, not entire values)
- Distributed storage (chunks can live on different nodes)

### 3. Memory Cache (ARC)

The memory cache uses the **Adaptive Replacement Cache (ARC)** algorithm:

```
┌─────────────────────────────────────────────────────────────┐
│                         ARC Cache                            │
├─────────────────────────────────────────────────────────────┤
│  T1 (Recent)          │  T2 (Frequent)                      │
│  ┌─────────────────┐  │  ┌─────────────────┐               │
│  │ Recently seen   │  │  │ Frequently seen │               │
│  │ items (LRU)     │  │  │ items (LRU)     │               │
│  └─────────────────┘  │  └─────────────────┘               │
│                       │                                     │
│  B1 (Ghost)           │  B2 (Ghost)                        │
│  ┌─────────────────┐  │  ┌─────────────────┐               │
│  │ Evicted from T1 │  │  │ Evicted from T2 │               │
│  │ (keys only)     │  │  │ (keys only)     │               │
│  └─────────────────┘  │  └─────────────────┘               │
│                       │                                     │
│  Parameter p: Target size for T1 (adapts based on hits)    │
└─────────────────────────────────────────────────────────────┘
```

ARC automatically balances between:
- **Recency**: Recently accessed items (scan-resistant)
- **Frequency**: Frequently accessed items (frequency-aware)

### 4. Disk Cache

The disk cache provides persistent storage with LRU eviction:

```
/var/cache/elcache/
├── chunks/
│   ├── 00/
│   │   ├── 00a1b2c3...chunk
│   │   └── 00d4e5f6...chunk
│   ├── 01/
│   │   └── ...
│   └── ff/
│       └── ...
└── descriptors/
    ├── abc123...desc
    └── def456...desc
```

**Features:**

- Sharded directory structure (256 subdirs) for filesystem scalability
- Optional O_DIRECT for large files
- Async I/O via io_uring when available
- LRU eviction based on access time

### 5. Cluster Layer

Nodes form a peer-to-peer cluster:

```
     ┌─────────┐
     │ Node A  │◄──────┐
     └────┬────┘       │
          │            │ Gossip
     ┌────▼────┐       │
     │ Node B  │───────┤
     └────┬────┘       │
          │            │
     ┌────▼────┐       │
     │ Node C  │───────┘
     └─────────┘
```

**Gossip Protocol:**

1. Each node periodically sends its known node list to random peers
2. Nodes merge received information with local state
3. Failed nodes detected via timeout (no gossip received)

**Consistent Hashing:**

```
Hash Ring with Virtual Nodes:

        0°
        │
   ╭────┼────╮
  ╱     │     ╲
 ╱      │      ╲
│   A₁  │  B₁   │  270°
├───────┼───────┤
│   C₁  │  A₂   │
 ╲      │      ╱
  ╲     │     ╱
   ╰────┼────╯
        │
       180°

- Each physical node has N virtual nodes (default: 150)
- Keys hash to a point on the ring
- Responsible node = first node clockwise from key position
- Replication: next N-1 nodes also store the key
```

## Data Flow

### Write Path

```
Client PUT /cache/mykey
           │
           ▼
┌──────────────────────┐
│ CacheCoordinator.put │
└──────────┬───────────┘
           │
           ▼
┌──────────────────────┐
│   Split into chunks  │
│   (4MB each)         │
└──────────┬───────────┘
           │
     ┌─────┴─────┐
     ▼           ▼
┌─────────┐ ┌─────────┐
│ Memory  │ │  Disk   │  (parallel)
│ Cache   │ │  Cache  │
└────┬────┘ └────┬────┘
     │           │
     └─────┬─────┘
           ▼
┌──────────────────────┐
│  Replicate to peers  │ (async, based on replication factor)
└──────────────────────┘
```

### Read Path (Partial)

```
Client GET /cache/mykey (Range: bytes=4194304-8388607)
           │
           ▼
┌──────────────────────────────┐
│ CacheCoordinator.get         │
│ - Requested: chunk 1 only    │
└──────────┬───────────────────┘
           │
           ▼
┌──────────────────────────────┐
│ Memory Cache: chunk 1?       │
│ - HIT → return               │
│ - MISS → continue            │
└──────────┬───────────────────┘
           │
           ▼
┌──────────────────────────────┐
│ Disk Cache: chunk 1?         │
│ - HIT → return + promote     │
│ - MISS → continue            │
└──────────┬───────────────────┘
           │
           ▼
┌──────────────────────────────┐
│ Cluster: find node with      │
│ chunk 1, fetch, cache locally│
└──────────────────────────────┘
```

## Protocol

### Wire Format

All messages use a fixed header:

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                         Magic (ELCA)                          |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|           Version             |             Type              |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                        Payload Length                         |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                         Request ID                            |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                                                               |
+                          Timestamp                            +
|                                                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

### Message Types

| Type | Code | Description |
|------|------|-------------|
| Hello | 0x0001 | Initial handshake |
| GossipPush | 0x0010 | Node list broadcast |
| Get | 0x0100 | Retrieve value |
| Put | 0x0102 | Store value |
| Delete | 0x0104 | Remove value |
| GetChunk | 0x0200 | Retrieve single chunk |

## Metrics

ElCache exports Prometheus-compatible metrics at `/metrics`:

### Counters
- `elcache_cache_hits_total` - Total cache hits
- `elcache_cache_misses_total` - Total cache misses
- `elcache_bytes_read_total` - Total bytes read
- `elcache_bytes_written_total` - Total bytes written

### Gauges
- `elcache_memory_cache_size_bytes` - Current memory usage
- `elcache_disk_cache_size_bytes` - Current disk usage
- `elcache_cluster_nodes_active` - Active cluster nodes

### Histograms
- `elcache_get_latency_ms` - Get operation latency
- `elcache_put_latency_ms` - Put operation latency

## Thread Model

```
┌─────────────────────────────────────────────────────────────┐
│                      Elio Scheduler                         │
│  ┌─────────┐ ┌─────────┐ ┌─────────┐ ┌─────────┐           │
│  │Worker 0 │ │Worker 1 │ │Worker 2 │ │Worker N │           │
│  └────┬────┘ └────┬────┘ └────┬────┘ └────┬────┘           │
│       │           │           │           │                 │
│       └───────────┴───────────┴───────────┘                 │
│                       │                                     │
│              Work-Stealing Deques                           │
└─────────────────────────────────────────────────────────────┘
                        │
            ┌───────────┴───────────┐
            ▼                       ▼
    ┌───────────────┐       ┌───────────────┐
    │   io_uring    │       │     epoll     │
    │  (preferred)  │       │  (fallback)   │
    └───────────────┘       └───────────────┘
```

All I/O is asynchronous:
- Network: TCP connections via Elio
- Disk: io_uring for file operations (Linux 5.1+)
- Memory cache: Lock-free ARC with shared_mutex for thread safety
