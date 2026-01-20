# ElCache HTTP API Reference

ElCache provides a RESTful HTTP API for cache operations.

## Base URL

```
http://localhost:8080
```

## Authentication

Currently no authentication is required. For production deployments, place ElCache behind a reverse proxy with authentication.

---

## Cache Operations

### Store Value

Store a value in the cache.

```
PUT /cache/{key}
```

**Path Parameters:**
- `key` - Cache key (max 8KB)

**Headers:**
| Header | Type | Description |
|--------|------|-------------|
| `Content-Type` | string | Content type (stored but not validated) |
| `X-ElCache-TTL` | integer | Time-to-live in seconds (optional) |
| `X-ElCache-Flags` | integer | User-defined flags (optional) |
| `X-ElCache-No-Memory` | boolean | Skip memory cache if "true" |
| `X-ElCache-No-Disk` | boolean | Skip disk cache if "true" |
| `X-ElCache-No-Cluster` | boolean | Don't replicate to cluster if "true" |

**Request Body:** Raw binary data

**Response:**
- `201 Created` - Value stored successfully
- `400 Bad Request` - Invalid key or value
- `500 Internal Server Error` - Storage failed

**Example:**

```bash
# Simple put
curl -X PUT http://localhost:8080/cache/mykey \
  -d "Hello, World!"

# With TTL (1 hour)
curl -X PUT http://localhost:8080/cache/mykey \
  -H "X-ElCache-TTL: 3600" \
  -d "Expires in 1 hour"

# Skip memory cache (disk only)
curl -X PUT http://localhost:8080/cache/large-file \
  -H "X-ElCache-No-Memory: true" \
  --data-binary @largefile.bin
```

---

### Retrieve Value

Retrieve a value from the cache.

```
GET /cache/{key}
```

**Path Parameters:**
- `key` - Cache key

**Query Parameters:**
| Parameter | Type | Description |
|-----------|------|-------------|
| `range` | string | Byte range in format `offset-length` |

**Headers:**
| Header | Type | Description |
|--------|------|-------------|
| `Range` | string | HTTP Range header (e.g., `bytes=0-1023`) |

**Response Headers:**
| Header | Description |
|--------|-------------|
| `X-ElCache-Hit` | Cache hit type: `hit`, `partial`, or `miss` |
| `X-ElCache-Size` | Total value size in bytes |
| `X-ElCache-Chunks` | Number of chunks |
| `Content-Range` | Byte range returned (for partial responses) |

**Response:**
- `200 OK` - Full value returned
- `206 Partial Content` - Partial value returned
- `404 Not Found` - Key not found

**Examples:**

```bash
# Full retrieval
curl http://localhost:8080/cache/mykey

# Partial read using Range header
curl -H "Range: bytes=0-99" http://localhost:8080/cache/mykey

# Partial read using query parameter
curl "http://localhost:8080/cache/mykey?range=0-100"

# Check response headers
curl -i http://localhost:8080/cache/mykey
# X-ElCache-Hit: hit
# X-ElCache-Size: 1048576
# X-ElCache-Chunks: 1
```

---

### Delete Value

Remove a value from the cache.

```
DELETE /cache/{key}
```

**Path Parameters:**
- `key` - Cache key

**Response:**
- `204 No Content` - Value deleted
- `404 Not Found` - Key not found

**Example:**

```bash
curl -X DELETE http://localhost:8080/cache/mykey
```

---

### Check Metadata

Get metadata for a cached value without retrieving the data.

```
HEAD /cache/{key}
```

**Path Parameters:**
- `key` - Cache key

**Response Headers:**
| Header | Description |
|--------|-------------|
| `X-ElCache-Size` | Total value size in bytes |
| `X-ElCache-Chunks` | Number of chunks |
| `Content-Length` | Same as X-ElCache-Size |
| `Accept-Ranges` | Always "bytes" |

**Response:**
- `200 OK` - Key exists
- `404 Not Found` - Key not found

**Example:**

```bash
curl -I http://localhost:8080/cache/mykey
```

---

### Options

Get supported methods for a cache endpoint.

```
OPTIONS /cache/{key}
```

**Response Headers:**
| Header | Value |
|--------|-------|
| `Allow` | `GET, PUT, POST, DELETE, HEAD, OPTIONS` |
| `Accept-Ranges` | `bytes` |

---

## Sparse/Parallel Write Operations

The sparse write API allows uploading large files in parts, potentially from multiple clients in parallel. This is useful for:

- Files larger than available memory
- Parallel uploads from multiple sources
- Resumable uploads
- Assembling data from distributed producers

### Workflow

1. **Create** a sparse entry with the total size
2. **Write** ranges at specific offsets (can be parallel from multiple clients)
3. **Finalize** to commit to cache (validates all bytes were written)

### Create Sparse Entry

Create a new sparse entry that will be filled with range writes.

```
POST /sparse/{key}
```

**Path Parameters:**
- `key` - Cache key (max 8KB)

**Headers:**
| Header | Type | Required | Description |
|--------|------|----------|-------------|
| `X-ElCache-Size` | integer | Yes | Total size in bytes |
| `X-ElCache-TTL` | integer | No | Time-to-live in seconds (applied on finalize) |

**Response:**
- `201 Created` - Sparse entry created
- `400 Bad Request` - Missing or invalid X-ElCache-Size
- `409 Conflict` - Sparse entry already exists for this key

**Example:**

```bash
# Create a 10MB sparse entry
curl -X POST http://localhost:8080/sparse/myfile \
  -H "X-ElCache-Size: 10485760"
```

---

### Write Range

Write data at a specific byte offset within a sparse entry.

```
PATCH /sparse/{key}
PUT /sparse/{key}
```

**Path Parameters:**
- `key` - Cache key

**Headers:**
| Header | Type | Description |
|--------|------|-------------|
| `Content-Range` | string | Byte range: `bytes start-end/total` |

**Query Parameters:**
| Parameter | Type | Description |
|-----------|------|-------------|
| `offset` | integer | Alternative to Content-Range header |

**Request Body:** Raw binary data for this range

**Response:**
- `202 Accepted` - Range written successfully
- `400 Bad Request` - Invalid range (exceeds total size)
- `404 Not Found` - Sparse entry doesn't exist

**Response Headers:**
| Header | Description |
|--------|-------------|
| `X-ElCache-Completion` | Percentage of bytes written (e.g., "75%") |

**Examples:**

```bash
# Write using Content-Range header
curl -X PATCH http://localhost:8080/sparse/myfile \
  -H "Content-Range: bytes 0-1048575/10485760" \
  --data-binary @chunk1.bin

# Write using offset query parameter
curl -X PATCH "http://localhost:8080/sparse/myfile?offset=1048576" \
  --data-binary @chunk2.bin

# Write using PUT (alternative to PATCH)
curl -X PUT "http://localhost:8080/sparse/myfile?offset=2097152" \
  --data-binary @chunk3.bin
```

---

### Get Sparse Entry Status

Get the current status of a sparse entry.

```
GET /sparse/{key}
```

**Path Parameters:**
- `key` - Cache key

**Response:**
- `200 OK` - Status returned
- `404 Not Found` - Sparse entry doesn't exist

**Response Body:**
```json
{
  "key": "myfile",
  "total_size": 10485760,
  "completion_percent": 75.5,
  "complete": false
}
```

**Example:**

```bash
curl http://localhost:8080/sparse/myfile
```

---

### Finalize Sparse Entry

Finalize a sparse entry, validating all bytes have been written and moving it to the main cache.

```
POST /sparse/{key}/finalize
```

**Path Parameters:**
- `key` - Cache key

**Headers:**
| Header | Type | Description |
|--------|------|-------------|
| `X-ElCache-TTL` | integer | Time-to-live in seconds (optional) |

**Response:**
- `201 Created` - Entry finalized and available in cache
- `404 Not Found` - Sparse entry doesn't exist
- `409 Conflict` - Not all byte ranges have been written

**Example:**

```bash
curl -X POST http://localhost:8080/sparse/myfile/finalize
```

---

### Abort Sparse Write

Delete a sparse entry without finalizing (abort the upload).

```
DELETE /sparse/{key}
```

**Path Parameters:**
- `key` - Cache key

**Response:**
- `204 No Content` - Sparse entry deleted

**Example:**

```bash
curl -X DELETE http://localhost:8080/sparse/myfile
```

---

### Complete Sparse Write Example

```bash
#!/bin/bash
KEY="largefile"
TOTAL_SIZE=10485760  # 10MB
CHUNK_SIZE=2097152   # 2MB

# Step 1: Create sparse entry
echo "Creating sparse entry..."
curl -s -X POST "http://localhost:8080/sparse/$KEY" \
  -H "X-ElCache-Size: $TOTAL_SIZE"

# Step 2: Upload chunks in parallel
echo "Uploading chunks in parallel..."
for i in 0 1 2 3 4; do
  OFFSET=$((i * CHUNK_SIZE))
  (
    dd if=/dev/urandom bs=$CHUNK_SIZE count=1 2>/dev/null | \
    curl -s -X PATCH "http://localhost:8080/sparse/$KEY?offset=$OFFSET" \
      --data-binary @-
  ) &
done
wait

# Step 3: Check status
echo "Checking status..."
curl -s "http://localhost:8080/sparse/$KEY"

# Step 4: Finalize
echo "Finalizing..."
curl -s -X POST "http://localhost:8080/sparse/$KEY/finalize"

# Step 5: Verify data is accessible
echo "Verifying..."
curl -s -I "http://localhost:8080/cache/$KEY" | grep -E "(X-ElCache|Content-Length)"
```

### Parallel Upload from Multiple Clients

Multiple clients can write different ranges simultaneously:

```bash
# Client 1 (writes first quarter)
curl -X PATCH "http://localhost:8080/sparse/shared-file?offset=0" \
  --data-binary @part1.bin &

# Client 2 (writes second quarter)
curl -X PATCH "http://localhost:8080/sparse/shared-file?offset=262144" \
  --data-binary @part2.bin &

# Client 3 (writes third quarter)
curl -X PATCH "http://localhost:8080/sparse/shared-file?offset=524288" \
  --data-binary @part3.bin &

# Client 4 (writes fourth quarter)
curl -X PATCH "http://localhost:8080/sparse/shared-file?offset=786432" \
  --data-binary @part4.bin &

wait

# Any client can finalize
curl -X POST "http://localhost:8080/sparse/shared-file/finalize"
```

---

## Admin Endpoints

### Health Check

```
GET /health
```

**Response:**
```json
{
  "status": "healthy"
}
```

---

### Statistics

```
GET /stats
```

**Response:**
```json
{
  "hits": 12345,
  "misses": 1234,
  "partial_hits": 567,
  "bytes_read": 1073741824,
  "bytes_written": 536870912,
  "evictions": 100,
  "entry_count": 5000,
  "size_bytes": 1073741824,
  "capacity_bytes": 2147483648,
  "hit_rate_percent": 89.5
}
```

---

### Cluster Information

```
GET /cluster
```

**Response (Standalone mode):**
```json
{
  "mode": "standalone"
}
```

**Response (Cluster mode):**
```json
{
  "mode": "cluster",
  "total_nodes": 5,
  "active_nodes": 4,
  "total_memory_capacity": 10737418240,
  "total_disk_capacity": 107374182400,
  "total_memory_used": 5368709120,
  "total_disk_used": 53687091200
}
```

---

### Metrics (Prometheus)

```
GET /metrics
```

**Headers:**
| Header | Value | Description |
|--------|-------|-------------|
| `Accept` | `text/plain` | Prometheus format (default) |
| `Accept` | `application/json` | JSON format |

**Response (Prometheus format):**
```
# HELP elcache_cache_hits_total Total number of cache hits
# TYPE elcache_cache_hits_total counter
elcache_cache_hits_total 12345

# HELP elcache_cache_misses_total Total number of cache misses
# TYPE elcache_cache_misses_total counter
elcache_cache_misses_total 1234

# HELP elcache_get_latency_ms Get operation latency in milliseconds
# TYPE elcache_get_latency_ms histogram
elcache_get_latency_ms_bucket{le="0.1"} 1000
elcache_get_latency_ms_bucket{le="0.5"} 5000
elcache_get_latency_ms_bucket{le="1"} 8000
elcache_get_latency_ms_bucket{le="+Inf"} 12345
elcache_get_latency_ms_sum 12345.678
elcache_get_latency_ms_count 12345

# ... more metrics
```

**Available Metrics:**

| Metric | Type | Description |
|--------|------|-------------|
| `elcache_cache_hits_total` | counter | Total cache hits |
| `elcache_cache_misses_total` | counter | Total cache misses |
| `elcache_cache_partial_hits_total` | counter | Total partial hits |
| `elcache_bytes_read_total` | counter | Total bytes read |
| `elcache_bytes_written_total` | counter | Total bytes written |
| `elcache_memory_cache_size_bytes` | gauge | Memory cache usage |
| `elcache_memory_cache_capacity_bytes` | gauge | Memory cache capacity |
| `elcache_disk_cache_size_bytes` | gauge | Disk cache usage |
| `elcache_disk_cache_capacity_bytes` | gauge | Disk cache capacity |
| `elcache_get_latency_ms` | histogram | Get latency |
| `elcache_put_latency_ms` | histogram | Put latency |
| `elcache_cluster_nodes_active` | gauge | Active cluster nodes |
| `elcache_uptime_seconds` | gauge | Server uptime |

---

## Error Responses

All error responses include a plain text body with the error message.

| Status Code | Description |
|-------------|-------------|
| `400 Bad Request` | Invalid request (key too large, etc.) |
| `404 Not Found` | Key not found |
| `500 Internal Server Error` | Server error |
| `503 Service Unavailable` | Service temporarily unavailable |

---

## Limits

| Limit | Value |
|-------|-------|
| Maximum key size | 8 KB |
| Maximum value size | 20 TB |
| Maximum single PUT body | 100 MB |
| Chunk size | 4 MB |
| Sparse write chunk size | Unlimited (within total size) |

**Note:** For values larger than 100MB, use the sparse write API which allows uploading in smaller chunks.

---

## Examples

### Store and retrieve a file

```bash
# Store a file
curl -X PUT http://localhost:8080/cache/myfile.pdf \
  --data-binary @document.pdf

# Retrieve the file
curl http://localhost:8080/cache/myfile.pdf > downloaded.pdf
```

### Stream a large file with range requests

```bash
# Get first 1MB
curl -H "Range: bytes=0-1048575" \
  http://localhost:8080/cache/bigfile.bin > part1.bin

# Get next 1MB  
curl -H "Range: bytes=1048576-2097151" \
  http://localhost:8080/cache/bigfile.bin > part2.bin
```

### Upload a large file using sparse writes

```bash
# Create 100MB sparse entry
curl -X POST http://localhost:8080/sparse/bigfile.bin \
  -H "X-ElCache-Size: 104857600"

# Upload in 10MB chunks (can be parallelized)
for i in $(seq 0 9); do
  OFFSET=$((i * 10485760))
  dd if=bigfile.bin bs=10485760 skip=$i count=1 2>/dev/null | \
  curl -X PATCH "http://localhost:8080/sparse/bigfile.bin?offset=$OFFSET" \
    --data-binary @-
done

# Finalize
curl -X POST http://localhost:8080/sparse/bigfile.bin/finalize
```

### Monitor cache performance

```bash
# Watch hit rate
watch -n 1 'curl -s http://localhost:8080/stats | jq .hit_rate_percent'

# Prometheus scrape config
scrape_configs:
  - job_name: 'elcache'
    static_configs:
      - targets: ['localhost:8080']
    metrics_path: /metrics
```
