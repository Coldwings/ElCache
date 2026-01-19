/*
 * ElCache C SDK
 * 
 * A zero-dependency C client library for ElCache.
 * Supports both Unix socket and shared memory transports.
 * 
 * Thread Safety:
 *   - elcache_client_t is NOT thread-safe. Create one per thread or use external locking.
 *   - Operations are synchronous but use async I/O internally.
 * 
 * Example:
 *   elcache_client_t* client = elcache_client_create();
 *   elcache_client_connect_unix(client, "/var/run/elcache/elcache.sock");
 *   
 *   const char* key = "mykey";
 *   const char* value = "myvalue";
 *   elcache_client_put(client, key, strlen(key), value, strlen(value), NULL);
 *   
 *   char buffer[1024];
 *   size_t len;
 *   if (elcache_client_get(client, key, strlen(key), buffer, sizeof(buffer), &len, NULL) == ELCACHE_OK) {
 *       printf("Got: %.*s\n", (int)len, buffer);
 *   }
 *   
 *   elcache_client_destroy(client);
 */

#ifndef ELCACHE_SDK_H
#define ELCACHE_SDK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Version */
#define ELCACHE_SDK_VERSION_MAJOR 0
#define ELCACHE_SDK_VERSION_MINOR 1
#define ELCACHE_SDK_VERSION_PATCH 0

/* Error codes */
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

/* Transport types */
typedef enum {
    ELCACHE_TRANSPORT_UNIX = 0,  /* Unix domain socket */
    ELCACHE_TRANSPORT_SHM = 1,   /* Shared memory */
} elcache_transport_t;

/* Cache result types */
typedef enum {
    ELCACHE_HIT = 0,
    ELCACHE_MISS = 1,
    ELCACHE_PARTIAL_HIT = 2,
    ELCACHE_EXPIRED = 3,
} elcache_result_t;

/* Forward declaration */
typedef struct elcache_client elcache_client_t;

/* Options for put operations */
typedef struct {
    int64_t ttl_seconds;     /* Time-to-live in seconds, 0 = no expiry */
    uint32_t flags;          /* User-defined flags */
    int no_memory;           /* Skip memory cache */
    int no_disk;             /* Skip disk cache */
    int no_cluster;          /* Don't replicate to cluster */
} elcache_put_options_t;

/* Options for get operations */
typedef struct {
    uint64_t offset;         /* Start offset for partial read */
    uint64_t length;         /* Length to read, 0 = entire value */
    int allow_partial;       /* Return partial data if available */
    int timeout_ms;          /* Timeout in milliseconds, 0 = default */
} elcache_get_options_t;

/* Value metadata */
typedef struct {
    uint64_t total_size;     /* Total value size */
    uint64_t chunk_count;    /* Number of chunks */
    int64_t created_at;      /* Unix timestamp of creation */
    int64_t expires_at;      /* Unix timestamp of expiration, 0 = never */
    uint32_t flags;          /* User-defined flags */
} elcache_metadata_t;

/* Range information for partial reads */
typedef struct {
    uint64_t offset;
    uint64_t length;
} elcache_range_t;

/* Availability information */
typedef struct {
    int exists;
    elcache_metadata_t metadata;
    elcache_range_t* available_ranges;
    size_t available_count;
    elcache_range_t* missing_ranges;
    size_t missing_count;
} elcache_availability_t;

/* Statistics */
typedef struct {
    uint64_t hits;
    uint64_t misses;
    uint64_t partial_hits;
    uint64_t bytes_read;
    uint64_t bytes_written;
    uint64_t latency_us_avg;    /* Average latency in microseconds */
    uint64_t latency_us_p99;    /* P99 latency */
} elcache_stats_t;

/*
 * Client lifecycle
 */

/* Create a new client instance */
elcache_client_t* elcache_client_create(void);

/* Destroy client and free resources */
void elcache_client_destroy(elcache_client_t* client);

/* Connect to ElCache via Unix socket */
elcache_error_t elcache_client_connect_unix(
    elcache_client_t* client,
    const char* socket_path
);

/* Connect to ElCache via shared memory (fastest) */
elcache_error_t elcache_client_connect_shm(
    elcache_client_t* client,
    const char* shm_path
);

/* Disconnect from server */
void elcache_client_disconnect(elcache_client_t* client);

/* Check if connected */
int elcache_client_is_connected(const elcache_client_t* client);

/*
 * Basic operations
 */

/* Get a value
 * 
 * Parameters:
 *   key, key_len: The cache key
 *   buffer: Output buffer for value
 *   buffer_size: Size of output buffer
 *   value_len: Output parameter for actual value length
 *   options: Optional get options (can be NULL for defaults)
 * 
 * Returns:
 *   ELCACHE_OK on success
 *   ELCACHE_ERR_NOT_FOUND if key doesn't exist
 *   ELCACHE_ERR_BUFFER_TOO_SMALL if buffer is too small (value_len will contain required size)
 *   ELCACHE_ERR_PARTIAL if partial data returned (when allow_partial is set)
 */
elcache_error_t elcache_client_get(
    elcache_client_t* client,
    const void* key,
    size_t key_len,
    void* buffer,
    size_t buffer_size,
    size_t* value_len,
    const elcache_get_options_t* options
);

/* Get a value with dynamic allocation
 * 
 * Allocates buffer internally. Caller must free with elcache_free().
 */
elcache_error_t elcache_client_get_alloc(
    elcache_client_t* client,
    const void* key,
    size_t key_len,
    void** value,
    size_t* value_len,
    const elcache_get_options_t* options
);

/* Store a value */
elcache_error_t elcache_client_put(
    elcache_client_t* client,
    const void* key,
    size_t key_len,
    const void* value,
    size_t value_len,
    const elcache_put_options_t* options
);

/* Delete a value */
elcache_error_t elcache_client_delete(
    elcache_client_t* client,
    const void* key,
    size_t key_len
);

/* Check if a key exists */
elcache_error_t elcache_client_exists(
    elcache_client_t* client,
    const void* key,
    size_t key_len,
    int* exists
);

/* Get metadata for a key */
elcache_error_t elcache_client_metadata(
    elcache_client_t* client,
    const void* key,
    size_t key_len,
    elcache_metadata_t* metadata
);

/*
 * Partial read/write operations (for large values)
 */

/* Check what ranges are available for a key */
elcache_error_t elcache_client_check_availability(
    elcache_client_t* client,
    const void* key,
    size_t key_len,
    uint64_t offset,
    uint64_t length,
    elcache_availability_t* availability
);

/* Free availability result */
void elcache_availability_free(elcache_availability_t* availability);

/* Read a range of a value */
elcache_error_t elcache_client_read_range(
    elcache_client_t* client,
    const void* key,
    size_t key_len,
    uint64_t offset,
    void* buffer,
    size_t buffer_size,
    size_t* bytes_read
);

/*
 * Streaming operations (for very large values)
 */

/* Opaque stream handle */
typedef struct elcache_write_stream elcache_write_stream_t;
typedef struct elcache_read_stream elcache_read_stream_t;

/* Start a streaming write */
elcache_error_t elcache_client_write_stream_start(
    elcache_client_t* client,
    const void* key,
    size_t key_len,
    uint64_t total_size,
    const elcache_put_options_t* options,
    elcache_write_stream_t** stream
);

/* Write data to stream */
elcache_error_t elcache_write_stream_write(
    elcache_write_stream_t* stream,
    const void* data,
    size_t len
);

/* Finish streaming write */
elcache_error_t elcache_write_stream_finish(elcache_write_stream_t* stream);

/* Abort streaming write */
void elcache_write_stream_abort(elcache_write_stream_t* stream);

/* Start a streaming read */
elcache_error_t elcache_client_read_stream_start(
    elcache_client_t* client,
    const void* key,
    size_t key_len,
    const elcache_get_options_t* options,
    elcache_read_stream_t** stream
);

/* Read data from stream */
elcache_error_t elcache_read_stream_read(
    elcache_read_stream_t* stream,
    void* buffer,
    size_t buffer_size,
    size_t* bytes_read
);

/* Get total size from read stream */
uint64_t elcache_read_stream_total_size(const elcache_read_stream_t* stream);

/* Get bytes read so far */
uint64_t elcache_read_stream_bytes_read(const elcache_read_stream_t* stream);

/* Check if stream is at end */
int elcache_read_stream_eof(const elcache_read_stream_t* stream);

/* Close read stream */
void elcache_read_stream_close(elcache_read_stream_t* stream);

/*
 * Configuration
 */

/* Set connection timeout in milliseconds */
void elcache_client_set_timeout(elcache_client_t* client, int timeout_ms);

/* Set receive buffer size */
void elcache_client_set_recv_buffer(elcache_client_t* client, size_t size);

/* Set send buffer size */
void elcache_client_set_send_buffer(elcache_client_t* client, size_t size);

/*
 * Statistics and diagnostics
 */

/* Get client statistics */
elcache_error_t elcache_client_stats(
    elcache_client_t* client,
    elcache_stats_t* stats
);

/* Reset client statistics */
void elcache_client_reset_stats(elcache_client_t* client);

/* Get last error message */
const char* elcache_client_last_error(const elcache_client_t* client);

/* Get error message for error code */
const char* elcache_error_string(elcache_error_t error);

/*
 * Memory management
 */

/* Free memory allocated by elcache_client_get_alloc */
void elcache_free(void* ptr);

/*
 * Utility macros
 */

/* Initialize put options with defaults */
#define ELCACHE_PUT_OPTIONS_INIT { \
    .ttl_seconds = 0, \
    .flags = 0, \
    .no_memory = 0, \
    .no_disk = 0, \
    .no_cluster = 0 \
}

/* Initialize get options with defaults */
#define ELCACHE_GET_OPTIONS_INIT { \
    .offset = 0, \
    .length = 0, \
    .allow_partial = 1, \
    .timeout_ms = 0 \
}

#ifdef __cplusplus
}
#endif

#endif /* ELCACHE_SDK_H */
