# ElCache C SDK Guide

The ElCache C SDK provides high-performance cache access with zero external dependencies.

## Features

- **Zero dependencies**: Only requires POSIX APIs
- **Two transport modes**: Unix sockets (reliable) and shared memory (fastest)
- **Partial read support**: Efficiently read byte ranges of large values
- **Streaming API**: Handle values larger than memory
- **Thread safety**: Each client instance is single-threaded; create one per thread

## Installation

### Building

```bash
cd build
cmake ..
make elcache_sdk

# Install
sudo make install
```

### Linking

```bash
# Dynamic linking
gcc myapp.c -lelcache_sdk -o myapp

# Static linking
gcc myapp.c -l:libelcache_sdk.a -lpthread -lrt -o myapp
```

## Quick Start

```c
#include <elcache/elcache_sdk.h>
#include <stdio.h>
#include <string.h>

int main() {
    // Create client
    elcache_client_t* client = elcache_client_create();
    if (!client) {
        fprintf(stderr, "Failed to create client\n");
        return 1;
    }
    
    // Connect
    elcache_error_t err = elcache_client_connect_unix(client, 
        "/var/run/elcache/elcache.sock");
    if (err != ELCACHE_OK) {
        fprintf(stderr, "Connect failed: %s\n", elcache_error_string(err));
        elcache_client_destroy(client);
        return 1;
    }
    
    // Store a value
    const char* key = "greeting";
    const char* value = "Hello, World!";
    err = elcache_client_put(client, key, strlen(key), 
                             value, strlen(value), NULL);
    if (err != ELCACHE_OK) {
        fprintf(stderr, "Put failed: %s\n", elcache_error_string(err));
    }
    
    // Retrieve the value
    char buffer[1024];
    size_t len;
    err = elcache_client_get(client, key, strlen(key),
                             buffer, sizeof(buffer), &len, NULL);
    if (err == ELCACHE_OK) {
        printf("Got: %.*s\n", (int)len, buffer);
    }
    
    // Cleanup
    elcache_client_destroy(client);
    return 0;
}
```

## API Reference

### Client Lifecycle

#### elcache_client_create

```c
elcache_client_t* elcache_client_create(void);
```

Create a new client instance.

**Returns:** Client pointer, or NULL on failure.

---

#### elcache_client_destroy

```c
void elcache_client_destroy(elcache_client_t* client);
```

Destroy client and free all resources.

---

#### elcache_client_connect_unix

```c
elcache_error_t elcache_client_connect_unix(
    elcache_client_t* client,
    const char* socket_path
);
```

Connect via Unix domain socket.

**Parameters:**
- `client` - Client instance
- `socket_path` - Path to Unix socket (e.g., `/var/run/elcache/elcache.sock`)

**Returns:** `ELCACHE_OK` on success, error code otherwise.

---

#### elcache_client_connect_shm

```c
elcache_error_t elcache_client_connect_shm(
    elcache_client_t* client,
    const char* shm_path
);
```

Connect via shared memory (fastest, requires same host).

**Parameters:**
- `client` - Client instance
- `shm_path` - Shared memory name (e.g., `/elcache_shm`)

**Returns:** `ELCACHE_OK` on success, error code otherwise.

---

#### elcache_client_disconnect

```c
void elcache_client_disconnect(elcache_client_t* client);
```

Disconnect from server.

---

#### elcache_client_is_connected

```c
int elcache_client_is_connected(const elcache_client_t* client);
```

Check connection status.

**Returns:** Non-zero if connected, 0 otherwise.

---

### Basic Operations

#### elcache_client_put

```c
elcache_error_t elcache_client_put(
    elcache_client_t* client,
    const void* key,
    size_t key_len,
    const void* value,
    size_t value_len,
    const elcache_put_options_t* options  // NULL for defaults
);
```

Store a value.

**Options:**
```c
typedef struct {
    int64_t ttl_seconds;     // 0 = no expiry
    uint32_t flags;          // User-defined flags
    int no_memory;           // Skip memory cache
    int no_disk;             // Skip disk cache
    int no_cluster;          // Don't replicate
} elcache_put_options_t;

// Initialize with defaults
elcache_put_options_t opts = ELCACHE_PUT_OPTIONS_INIT;
```

---

#### elcache_client_get

```c
elcache_error_t elcache_client_get(
    elcache_client_t* client,
    const void* key,
    size_t key_len,
    void* buffer,
    size_t buffer_size,
    size_t* value_len,        // Output: actual value length
    const elcache_get_options_t* options  // NULL for defaults
);
```

Retrieve a value.

**Options:**
```c
typedef struct {
    uint64_t offset;         // Start offset (for partial read)
    uint64_t length;         // Length to read (0 = entire value)
    int allow_partial;       // Return partial data if available
    int timeout_ms;          // Timeout (0 = default)
} elcache_get_options_t;

// Initialize with defaults
elcache_get_options_t opts = ELCACHE_GET_OPTIONS_INIT;
```

**Returns:**
- `ELCACHE_OK` - Full value returned
- `ELCACHE_ERR_NOT_FOUND` - Key not found
- `ELCACHE_ERR_BUFFER_TOO_SMALL` - Buffer too small (value_len contains required size)
- `ELCACHE_ERR_PARTIAL` - Partial data returned (when allow_partial is set)

---

#### elcache_client_get_alloc

```c
elcache_error_t elcache_client_get_alloc(
    elcache_client_t* client,
    const void* key,
    size_t key_len,
    void** value,             // Output: allocated buffer
    size_t* value_len,        // Output: value length
    const elcache_get_options_t* options
);
```

Retrieve a value with automatic allocation.

**Important:** Caller must free the buffer with `elcache_free()`.

---

#### elcache_client_delete

```c
elcache_error_t elcache_client_delete(
    elcache_client_t* client,
    const void* key,
    size_t key_len
);
```

Delete a value.

---

#### elcache_client_exists

```c
elcache_error_t elcache_client_exists(
    elcache_client_t* client,
    const void* key,
    size_t key_len,
    int* exists               // Output: 1 if exists, 0 otherwise
);
```

Check if a key exists.

---

### Partial Reads

#### elcache_client_read_range

```c
elcache_error_t elcache_client_read_range(
    elcache_client_t* client,
    const void* key,
    size_t key_len,
    uint64_t offset,
    void* buffer,
    size_t buffer_size,
    size_t* bytes_read
);
```

Read a specific byte range.

**Example:**
```c
// Read bytes 1000-1999 of a large file
char buffer[1000];
size_t bytes_read;
err = elcache_client_read_range(client, "bigfile", 7, 
                                 1000,  // offset
                                 buffer, sizeof(buffer),
                                 &bytes_read);
```

---

### Streaming API

For values too large to fit in memory:

#### Write Stream

```c
elcache_write_stream_t* stream;

// Start stream
err = elcache_client_write_stream_start(client,
    key, key_len,
    total_size,
    NULL,  // options
    &stream);

// Write chunks
while (has_more_data) {
    err = elcache_write_stream_write(stream, chunk, chunk_len);
}

// Finish
err = elcache_write_stream_finish(stream);

// Or abort
elcache_write_stream_abort(stream);
```

#### Read Stream

```c
elcache_read_stream_t* stream;

// Start stream
err = elcache_client_read_stream_start(client,
    key, key_len,
    NULL,  // options
    &stream);

// Get total size
uint64_t total = elcache_read_stream_total_size(stream);

// Read chunks
char buffer[65536];
size_t bytes_read;
while (!elcache_read_stream_eof(stream)) {
    err = elcache_read_stream_read(stream, buffer, sizeof(buffer), &bytes_read);
    // process buffer...
}

// Close
elcache_read_stream_close(stream);
```

---

### Configuration

#### elcache_client_set_timeout

```c
void elcache_client_set_timeout(elcache_client_t* client, int timeout_ms);
```

Set operation timeout in milliseconds.

---

#### elcache_client_set_recv_buffer

```c
void elcache_client_set_recv_buffer(elcache_client_t* client, size_t size);
```

Set receive buffer size.

---

#### elcache_client_set_send_buffer

```c
void elcache_client_set_send_buffer(elcache_client_t* client, size_t size);
```

Set send buffer size.

---

### Statistics

#### elcache_client_stats

```c
typedef struct {
    uint64_t hits;
    uint64_t misses;
    uint64_t partial_hits;
    uint64_t bytes_read;
    uint64_t bytes_written;
    uint64_t latency_us_avg;
    uint64_t latency_us_p99;
} elcache_stats_t;

elcache_error_t elcache_client_stats(
    elcache_client_t* client,
    elcache_stats_t* stats
);
```

Get client-side statistics.

---

### Error Handling

#### Error Codes

```c
typedef enum {
    ELCACHE_OK = 0,
    ELCACHE_ERR_INVALID_ARG = -1,
    ELCACHE_ERR_NOT_FOUND = -2,
    ELCACHE_ERR_PARTIAL = -3,
    ELCACHE_ERR_TIMEOUT = -4,
    ELCACHE_ERR_CONNECTION = -5,
    ELCACHE_ERR_PROTOCOL = -6,
    ELCACHE_ERR_BUFFER_TOO_SMALL = -7,
    ELCACHE_ERR_KEY_TOO_LARGE = -8,
    ELCACHE_ERR_VALUE_TOO_LARGE = -9,
    ELCACHE_ERR_OUT_OF_MEMORY = -10,
    ELCACHE_ERR_INTERNAL = -11,
} elcache_error_t;
```

#### elcache_error_string

```c
const char* elcache_error_string(elcache_error_t error);
```

Get human-readable error message.

#### elcache_client_last_error

```c
const char* elcache_client_last_error(const elcache_client_t* client);
```

Get detailed error message from last operation.

---

### Memory Management

#### elcache_free

```c
void elcache_free(void* ptr);
```

Free memory allocated by `elcache_client_get_alloc()`.

---

## Examples

### Caching HTTP responses

```c
#include <elcache/elcache_sdk.h>

// Cache an HTTP response
void cache_response(elcache_client_t* client, 
                    const char* url,
                    const char* body, size_t body_len,
                    int ttl_seconds) {
    elcache_put_options_t opts = ELCACHE_PUT_OPTIONS_INIT;
    opts.ttl_seconds = ttl_seconds;
    
    elcache_client_put(client, url, strlen(url), body, body_len, &opts);
}

// Get cached response
char* get_cached_response(elcache_client_t* client, const char* url) {
    void* value;
    size_t len;
    
    elcache_error_t err = elcache_client_get_alloc(client, 
        url, strlen(url), &value, &len, NULL);
    
    if (err == ELCACHE_OK) {
        return (char*)value;  // Caller must elcache_free()
    }
    return NULL;
}
```

### Reading a video file in chunks

```c
#include <elcache/elcache_sdk.h>

void stream_video(elcache_client_t* client, const char* video_id) {
    elcache_read_stream_t* stream;
    
    elcache_error_t err = elcache_client_read_stream_start(
        client, video_id, strlen(video_id), NULL, &stream);
    
    if (err != ELCACHE_OK) {
        return;
    }
    
    uint64_t total = elcache_read_stream_total_size(stream);
    printf("Streaming %lu bytes\n", total);
    
    char buffer[1024 * 1024];  // 1MB chunks
    size_t bytes_read;
    
    while (!elcache_read_stream_eof(stream)) {
        err = elcache_read_stream_read(stream, buffer, sizeof(buffer), &bytes_read);
        if (err == ELCACHE_OK) {
            // Send to video player...
            send_to_player(buffer, bytes_read);
        }
    }
    
    elcache_read_stream_close(stream);
}
```

### Thread-safe usage

```c
#include <elcache/elcache_sdk.h>
#include <pthread.h>

// Thread-local client
__thread elcache_client_t* tls_client = NULL;

elcache_client_t* get_client() {
    if (!tls_client) {
        tls_client = elcache_client_create();
        elcache_client_connect_unix(tls_client, "/var/run/elcache/elcache.sock");
    }
    return tls_client;
}

void* worker_thread(void* arg) {
    elcache_client_t* client = get_client();
    
    // Use client safely in this thread...
    
    return NULL;
}
```

## Performance Tips

1. **Use shared memory transport** when client and server are on the same host
2. **Reuse client instances** - connection setup has overhead
3. **Use partial reads** for large values when you only need a portion
4. **Batch operations** by keeping the connection open
5. **Tune buffer sizes** based on your typical value sizes

## Limits

| Limit | Value |
|-------|-------|
| Maximum key size | 8 KB |
| Maximum value size | 20 TB |
| Default timeout | 30 seconds |
| Default buffer size | 256 KB |
