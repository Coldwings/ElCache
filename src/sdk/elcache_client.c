/*
 * ElCache C SDK Implementation
 * Zero-dependency client library
 */

#include "elcache/elcache_sdk.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

/* Protocol constants */
#define ELCACHE_PROTO_MAGIC 0x454C4341  /* "ELCA" */
#define ELCACHE_PROTO_VERSION 1
#define ELCACHE_HEADER_SIZE 24

/* Message types */
#define MSG_GET 0x0100
#define MSG_GET_RESPONSE 0x0101
#define MSG_PUT 0x0102
#define MSG_PUT_RESPONSE 0x0103
#define MSG_DELETE 0x0104
#define MSG_DELETE_RESPONSE 0x0105
#define MSG_CHECK 0x0106
#define MSG_CHECK_RESPONSE 0x0107
/* Sparse/partial write operations */
#define MSG_CREATE_SPARSE 0x0108
#define MSG_CREATE_SPARSE_RESPONSE 0x0109
#define MSG_WRITE_RANGE 0x010A
#define MSG_WRITE_RANGE_RESPONSE 0x010B
#define MSG_FINALIZE 0x010C
#define MSG_FINALIZE_RESPONSE 0x010D

/* Default buffer sizes */
#define DEFAULT_RECV_BUFFER (256 * 1024)
#define DEFAULT_SEND_BUFFER (256 * 1024)
#define DEFAULT_TIMEOUT_MS 30000

/* Internal structures */
struct elcache_client {
    int transport_type;
    int fd;
    void* shm_ptr;
    size_t shm_size;
    
    uint8_t* recv_buffer;
    size_t recv_buffer_size;
    uint8_t* send_buffer;
    size_t send_buffer_size;
    
    int timeout_ms;
    uint32_t next_request_id;
    
    char last_error[256];
    
    /* Statistics */
    uint64_t hits;
    uint64_t misses;
    uint64_t partial_hits;
    uint64_t bytes_read;
    uint64_t bytes_written;
    uint64_t total_latency_us;
    uint64_t request_count;
    uint64_t max_latency_us;
};

/* SHM ring buffer structure */
struct shm_header {
    uint32_t magic;
    uint32_t version;
    uint32_t request_offset;
    uint32_t request_size;
    uint32_t response_offset;
    uint32_t response_size;
    volatile uint32_t request_ready;
    volatile uint32_t response_ready;
};

/* Helper functions */

static uint64_t get_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void set_error(elcache_client_t* client, const char* msg) {
    strncpy(client->last_error, msg, sizeof(client->last_error) - 1);
    client->last_error[sizeof(client->last_error) - 1] = '\0';
}

static void encode_u16(uint8_t* buf, uint16_t v) {
    buf[0] = v & 0xFF;
    buf[1] = (v >> 8) & 0xFF;
}

static void encode_u32(uint8_t* buf, uint32_t v) {
    buf[0] = v & 0xFF;
    buf[1] = (v >> 8) & 0xFF;
    buf[2] = (v >> 16) & 0xFF;
    buf[3] = (v >> 24) & 0xFF;
}

static void encode_u64(uint8_t* buf, uint64_t v) {
    for (int i = 0; i < 8; i++) {
        buf[i] = (v >> (i * 8)) & 0xFF;
    }
}

static uint16_t decode_u16(const uint8_t* buf) {
    return buf[0] | ((uint16_t)buf[1] << 8);
}

static uint32_t decode_u32(const uint8_t* buf) {
    return buf[0] | ((uint32_t)buf[1] << 8) | 
           ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
}

static uint64_t decode_u64(const uint8_t* buf) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v |= (uint64_t)buf[i] << (i * 8);
    }
    return v;
}

/* Encode message header */
static void encode_header(uint8_t* buf, uint16_t type, uint32_t payload_len, 
                          uint32_t request_id) {
    encode_u32(buf, ELCACHE_PROTO_MAGIC);
    encode_u16(buf + 4, ELCACHE_PROTO_VERSION);
    encode_u16(buf + 6, type);
    encode_u32(buf + 8, payload_len);
    encode_u32(buf + 12, request_id);
    encode_u64(buf + 16, (uint64_t)time(NULL) * 1000);
}

/* Send data over socket */
static elcache_error_t send_all(elcache_client_t* client, const void* data, size_t len) {
    const uint8_t* ptr = data;
    size_t remaining = len;
    
    while (remaining > 0) {
        ssize_t n = send(client->fd, ptr, remaining, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            set_error(client, "Send failed");
            return ELCACHE_ERR_CONNECTION;
        }
        ptr += n;
        remaining -= n;
    }
    
    return ELCACHE_OK;
}

/* Receive data from socket */
static elcache_error_t recv_all(elcache_client_t* client, void* data, size_t len) {
    uint8_t* ptr = data;
    size_t remaining = len;
    
    while (remaining > 0) {
        ssize_t n = recv(client->fd, ptr, remaining, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            set_error(client, "Receive failed");
            return ELCACHE_ERR_CONNECTION;
        }
        if (n == 0) {
            set_error(client, "Connection closed");
            return ELCACHE_ERR_CONNECTION;
        }
        ptr += n;
        remaining -= n;
    }
    
    return ELCACHE_OK;
}

/* Client lifecycle */

elcache_client_t* elcache_client_create(void) {
    elcache_client_t* client = calloc(1, sizeof(elcache_client_t));
    if (!client) return NULL;
    
    client->fd = -1;
    client->shm_ptr = NULL;
    client->timeout_ms = DEFAULT_TIMEOUT_MS;
    client->next_request_id = 1;
    
    client->recv_buffer_size = DEFAULT_RECV_BUFFER;
    client->send_buffer_size = DEFAULT_SEND_BUFFER;
    
    client->recv_buffer = malloc(client->recv_buffer_size);
    client->send_buffer = malloc(client->send_buffer_size);
    
    if (!client->recv_buffer || !client->send_buffer) {
        free(client->recv_buffer);
        free(client->send_buffer);
        free(client);
        return NULL;
    }
    
    return client;
}

void elcache_client_destroy(elcache_client_t* client) {
    if (!client) return;
    
    elcache_client_disconnect(client);
    free(client->recv_buffer);
    free(client->send_buffer);
    free(client);
}

elcache_error_t elcache_client_connect_unix(elcache_client_t* client, 
                                             const char* socket_path) {
    if (!client || !socket_path) {
        return ELCACHE_ERR_INVALID_ARG;
    }
    
    elcache_client_disconnect(client);
    
    client->fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (client->fd < 0) {
        set_error(client, "Failed to create socket");
        return ELCACHE_ERR_CONNECTION;
    }
    
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);
    
    if (connect(client->fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(client->fd);
        client->fd = -1;
        set_error(client, "Failed to connect");
        return ELCACHE_ERR_CONNECTION;
    }
    
    /* Set socket timeout */
    struct timeval tv;
    tv.tv_sec = client->timeout_ms / 1000;
    tv.tv_usec = (client->timeout_ms % 1000) * 1000;
    setsockopt(client->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(client->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    
    client->transport_type = ELCACHE_TRANSPORT_UNIX;
    return ELCACHE_OK;
}

elcache_error_t elcache_client_connect_shm(elcache_client_t* client,
                                           const char* shm_path) {
    if (!client || !shm_path) {
        return ELCACHE_ERR_INVALID_ARG;
    }
    
    elcache_client_disconnect(client);
    
    int fd = shm_open(shm_path, O_RDWR, 0666);
    if (fd < 0) {
        set_error(client, "Failed to open shared memory");
        return ELCACHE_ERR_CONNECTION;
    }
    
    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        set_error(client, "Failed to stat shared memory");
        return ELCACHE_ERR_CONNECTION;
    }
    
    client->shm_size = st.st_size;
    client->shm_ptr = mmap(NULL, client->shm_size, PROT_READ | PROT_WRITE,
                           MAP_SHARED, fd, 0);
    close(fd);
    
    if (client->shm_ptr == MAP_FAILED) {
        client->shm_ptr = NULL;
        set_error(client, "Failed to map shared memory");
        return ELCACHE_ERR_CONNECTION;
    }
    
    /* Verify SHM header */
    struct shm_header* hdr = client->shm_ptr;
    if (hdr->magic != ELCACHE_PROTO_MAGIC) {
        munmap(client->shm_ptr, client->shm_size);
        client->shm_ptr = NULL;
        set_error(client, "Invalid shared memory format");
        return ELCACHE_ERR_PROTOCOL;
    }
    
    client->transport_type = ELCACHE_TRANSPORT_SHM;
    return ELCACHE_OK;
}

void elcache_client_disconnect(elcache_client_t* client) {
    if (!client) return;
    
    if (client->fd >= 0) {
        close(client->fd);
        client->fd = -1;
    }
    
    if (client->shm_ptr) {
        munmap(client->shm_ptr, client->shm_size);
        client->shm_ptr = NULL;
    }
}

int elcache_client_is_connected(const elcache_client_t* client) {
    if (!client) return 0;
    return client->fd >= 0 || client->shm_ptr != NULL;
}

/* Basic operations */

elcache_error_t elcache_client_get(elcache_client_t* client,
                                    const void* key, size_t key_len,
                                    void* buffer, size_t buffer_size,
                                    size_t* value_len,
                                    const elcache_get_options_t* options) {
    if (!client || !key || !buffer || !value_len) {
        return ELCACHE_ERR_INVALID_ARG;
    }
    
    if (key_len > 8192) {
        set_error(client, "Key too large");
        return ELCACHE_ERR_KEY_TOO_LARGE;
    }
    
    if (!elcache_client_is_connected(client)) {
        set_error(client, "Not connected");
        return ELCACHE_ERR_CONNECTION;
    }
    
    uint64_t start_time = get_time_us();
    
    /* Build request */
    uint8_t* payload = client->send_buffer + ELCACHE_HEADER_SIZE;
    size_t payload_len = 0;
    
    /* Key length + key */
    encode_u32(payload + payload_len, (uint32_t)key_len);
    payload_len += 4;
    memcpy(payload + payload_len, key, key_len);
    payload_len += key_len;
    
    /* Metadata only flag */
    payload[payload_len++] = 0;
    
    /* Range */
    int has_range = options && (options->offset > 0 || options->length > 0);
    payload[payload_len++] = has_range ? 1 : 0;
    if (has_range) {
        encode_u64(payload + payload_len, options->offset);
        payload_len += 8;
        encode_u64(payload + payload_len, options->length);
        payload_len += 8;
    }
    
    /* Encode header */
    uint32_t request_id = client->next_request_id++;
    encode_header(client->send_buffer, MSG_GET, (uint32_t)payload_len, request_id);
    
    /* Send request */
    elcache_error_t err = send_all(client, client->send_buffer, 
                                    ELCACHE_HEADER_SIZE + payload_len);
    if (err != ELCACHE_OK) {
        return err;
    }
    
    /* Receive response header */
    err = recv_all(client, client->recv_buffer, ELCACHE_HEADER_SIZE);
    if (err != ELCACHE_OK) {
        return err;
    }
    
    /* Parse header */
    uint32_t magic = decode_u32(client->recv_buffer);
    if (magic != ELCACHE_PROTO_MAGIC) {
        set_error(client, "Invalid response magic");
        return ELCACHE_ERR_PROTOCOL;
    }
    
    uint16_t msg_type = decode_u16(client->recv_buffer + 6);
    uint32_t response_len = decode_u32(client->recv_buffer + 8);
    
    if (msg_type != MSG_GET_RESPONSE) {
        set_error(client, "Unexpected response type");
        return ELCACHE_ERR_PROTOCOL;
    }
    
    /* Receive response payload */
    if (response_len > client->recv_buffer_size - ELCACHE_HEADER_SIZE) {
        set_error(client, "Response too large");
        return ELCACHE_ERR_BUFFER_TOO_SMALL;
    }
    
    err = recv_all(client, client->recv_buffer + ELCACHE_HEADER_SIZE, response_len);
    if (err != ELCACHE_OK) {
        return err;
    }
    
    /* Parse response */
    uint8_t* resp = client->recv_buffer + ELCACHE_HEADER_SIZE;
    uint32_t result = decode_u32(resp);
    resp += 4;
    
    /* Skip metadata if present */
    int has_meta = resp[0];
    resp += 1;
    if (has_meta) {
        resp += 8 + 8 + 4;  /* total_size + chunk_count + flags */
    }
    
    /* Data */
    uint32_t data_len = decode_u32(resp);
    resp += 4;
    
    if (result == 1) {  /* Miss */
        client->misses++;
        set_error(client, "Key not found");
        return ELCACHE_ERR_NOT_FOUND;
    }
    
    if (data_len > buffer_size) {
        *value_len = data_len;
        set_error(client, "Buffer too small");
        return ELCACHE_ERR_BUFFER_TOO_SMALL;
    }
    
    memcpy(buffer, resp, data_len);
    *value_len = data_len;
    
    /* Update stats */
    uint64_t latency = get_time_us() - start_time;
    client->request_count++;
    client->total_latency_us += latency;
    if (latency > client->max_latency_us) {
        client->max_latency_us = latency;
    }
    client->bytes_read += data_len;
    
    if (result == 0) {  /* Hit */
        client->hits++;
        return ELCACHE_OK;
    } else if (result == 2) {  /* Partial */
        client->partial_hits++;
        return ELCACHE_ERR_PARTIAL;
    }
    
    return ELCACHE_OK;
}

elcache_error_t elcache_client_get_alloc(elcache_client_t* client,
                                          const void* key, size_t key_len,
                                          void** value, size_t* value_len,
                                          const elcache_get_options_t* options) {
    if (!client || !key || !value || !value_len) {
        return ELCACHE_ERR_INVALID_ARG;
    }
    
    /* First try with recv buffer */
    size_t len;
    elcache_error_t err = elcache_client_get(client, key, key_len,
                                              client->recv_buffer, 
                                              client->recv_buffer_size,
                                              &len, options);
    
    if (err == ELCACHE_OK || err == ELCACHE_ERR_PARTIAL) {
        *value = malloc(len);
        if (!*value) {
            return ELCACHE_ERR_OUT_OF_MEMORY;
        }
        memcpy(*value, client->recv_buffer, len);
        *value_len = len;
        return err;
    }
    
    if (err == ELCACHE_ERR_BUFFER_TOO_SMALL) {
        /* Allocate larger buffer and retry */
        void* buf = malloc(len);
        if (!buf) {
            return ELCACHE_ERR_OUT_OF_MEMORY;
        }
        
        err = elcache_client_get(client, key, key_len, buf, len, &len, options);
        if (err == ELCACHE_OK || err == ELCACHE_ERR_PARTIAL) {
            *value = buf;
            *value_len = len;
            return err;
        }
        
        free(buf);
    }
    
    return err;
}

elcache_error_t elcache_client_put(elcache_client_t* client,
                                    const void* key, size_t key_len,
                                    const void* value, size_t value_len,
                                    const elcache_put_options_t* options) {
    if (!client || !key || !value) {
        return ELCACHE_ERR_INVALID_ARG;
    }
    
    if (key_len > 8192) {
        set_error(client, "Key too large");
        return ELCACHE_ERR_KEY_TOO_LARGE;
    }
    
    if (!elcache_client_is_connected(client)) {
        set_error(client, "Not connected");
        return ELCACHE_ERR_CONNECTION;
    }
    
    uint64_t start_time = get_time_us();
    
    /* Check if payload fits in buffer */
    size_t needed = ELCACHE_HEADER_SIZE + 4 + key_len + 4 + value_len + 1 + 8 + 4;
    if (needed > client->send_buffer_size) {
        set_error(client, "Value too large for buffer");
        return ELCACHE_ERR_VALUE_TOO_LARGE;
    }
    
    /* Build request */
    uint8_t* payload = client->send_buffer + ELCACHE_HEADER_SIZE;
    size_t payload_len = 0;
    
    /* Key */
    encode_u32(payload + payload_len, (uint32_t)key_len);
    payload_len += 4;
    memcpy(payload + payload_len, key, key_len);
    payload_len += key_len;
    
    /* Value */
    encode_u32(payload + payload_len, (uint32_t)value_len);
    payload_len += 4;
    memcpy(payload + payload_len, value, value_len);
    payload_len += value_len;
    
    /* TTL */
    int has_ttl = options && options->ttl_seconds > 0;
    payload[payload_len++] = has_ttl ? 1 : 0;
    if (has_ttl) {
        encode_u64(payload + payload_len, (uint64_t)options->ttl_seconds);
        payload_len += 8;
    }
    
    /* Flags */
    uint32_t flags = options ? options->flags : 0;
    encode_u32(payload + payload_len, flags);
    payload_len += 4;
    
    /* Encode header */
    uint32_t request_id = client->next_request_id++;
    encode_header(client->send_buffer, MSG_PUT, (uint32_t)payload_len, request_id);
    
    /* Send */
    elcache_error_t err = send_all(client, client->send_buffer,
                                    ELCACHE_HEADER_SIZE + payload_len);
    if (err != ELCACHE_OK) {
        return err;
    }
    
    /* Receive response */
    err = recv_all(client, client->recv_buffer, ELCACHE_HEADER_SIZE);
    if (err != ELCACHE_OK) {
        return err;
    }
    
    uint32_t response_len = decode_u32(client->recv_buffer + 8);
    err = recv_all(client, client->recv_buffer + ELCACHE_HEADER_SIZE, response_len);
    if (err != ELCACHE_OK) {
        return err;
    }
    
    /* Parse response */
    uint8_t* resp = client->recv_buffer + ELCACHE_HEADER_SIZE;
    uint32_t code = decode_u32(resp);
    
    /* Update stats */
    uint64_t latency = get_time_us() - start_time;
    client->request_count++;
    client->total_latency_us += latency;
    if (latency > client->max_latency_us) {
        client->max_latency_us = latency;
    }
    client->bytes_written += value_len;
    
    if (code != 0) {
        set_error(client, "Put failed");
        return ELCACHE_ERR_INTERNAL;
    }
    
    return ELCACHE_OK;
}

elcache_error_t elcache_client_delete(elcache_client_t* client,
                                       const void* key, size_t key_len) {
    if (!client || !key) {
        return ELCACHE_ERR_INVALID_ARG;
    }
    
    if (!elcache_client_is_connected(client)) {
        set_error(client, "Not connected");
        return ELCACHE_ERR_CONNECTION;
    }
    
    /* Build request */
    uint8_t* payload = client->send_buffer + ELCACHE_HEADER_SIZE;
    size_t payload_len = 0;
    
    encode_u32(payload + payload_len, (uint32_t)key_len);
    payload_len += 4;
    memcpy(payload + payload_len, key, key_len);
    payload_len += key_len;
    
    uint32_t request_id = client->next_request_id++;
    encode_header(client->send_buffer, MSG_DELETE, (uint32_t)payload_len, request_id);
    
    elcache_error_t err = send_all(client, client->send_buffer,
                                    ELCACHE_HEADER_SIZE + payload_len);
    if (err != ELCACHE_OK) {
        return err;
    }
    
    err = recv_all(client, client->recv_buffer, ELCACHE_HEADER_SIZE);
    if (err != ELCACHE_OK) {
        return err;
    }
    
    uint32_t response_len = decode_u32(client->recv_buffer + 8);
    err = recv_all(client, client->recv_buffer + ELCACHE_HEADER_SIZE, response_len);
    if (err != ELCACHE_OK) {
        return err;
    }
    
    return ELCACHE_OK;
}

elcache_error_t elcache_client_exists(elcache_client_t* client,
                                       const void* key, size_t key_len,
                                       int* exists) {
    if (!client || !key || !exists) {
        return ELCACHE_ERR_INVALID_ARG;
    }
    
    elcache_metadata_t meta;
    elcache_error_t err = elcache_client_metadata(client, key, key_len, &meta);
    
    *exists = (err == ELCACHE_OK);
    return (err == ELCACHE_ERR_NOT_FOUND) ? ELCACHE_OK : err;
}

elcache_error_t elcache_client_metadata(elcache_client_t* client,
                                         const void* key, size_t key_len,
                                         elcache_metadata_t* metadata) {
    if (!client || !key || !metadata) {
        return ELCACHE_ERR_INVALID_ARG;
    }
    
    /* Use HEAD-like request (GET with metadata_only=true) */
    /* Simplified implementation - would need protocol support */
    
    return ELCACHE_ERR_INTERNAL;
}

/* Configuration */

void elcache_client_set_timeout(elcache_client_t* client, int timeout_ms) {
    if (client) {
        client->timeout_ms = timeout_ms;
    }
}

void elcache_client_set_recv_buffer(elcache_client_t* client, size_t size) {
    if (client && size > 0) {
        uint8_t* new_buf = realloc(client->recv_buffer, size);
        if (new_buf) {
            client->recv_buffer = new_buf;
            client->recv_buffer_size = size;
        }
    }
}

void elcache_client_set_send_buffer(elcache_client_t* client, size_t size) {
    if (client && size > 0) {
        uint8_t* new_buf = realloc(client->send_buffer, size);
        if (new_buf) {
            client->send_buffer = new_buf;
            client->send_buffer_size = size;
        }
    }
}

/* Statistics */

elcache_error_t elcache_client_stats(elcache_client_t* client,
                                      elcache_stats_t* stats) {
    if (!client || !stats) {
        return ELCACHE_ERR_INVALID_ARG;
    }
    
    stats->hits = client->hits;
    stats->misses = client->misses;
    stats->partial_hits = client->partial_hits;
    stats->bytes_read = client->bytes_read;
    stats->bytes_written = client->bytes_written;
    
    if (client->request_count > 0) {
        stats->latency_us_avg = client->total_latency_us / client->request_count;
    } else {
        stats->latency_us_avg = 0;
    }
    stats->latency_us_p99 = client->max_latency_us;  /* Approximation */
    
    return ELCACHE_OK;
}

void elcache_client_reset_stats(elcache_client_t* client) {
    if (client) {
        client->hits = 0;
        client->misses = 0;
        client->partial_hits = 0;
        client->bytes_read = 0;
        client->bytes_written = 0;
        client->total_latency_us = 0;
        client->request_count = 0;
        client->max_latency_us = 0;
    }
}

const char* elcache_client_last_error(const elcache_client_t* client) {
    if (!client) return "Invalid client";
    return client->last_error;
}

const char* elcache_error_string(elcache_error_t error) {
    switch (error) {
        case ELCACHE_OK: return "Success";
        case ELCACHE_ERR_INVALID_ARG: return "Invalid argument";
        case ELCACHE_ERR_NOT_FOUND: return "Not found";
        case ELCACHE_ERR_PARTIAL: return "Partial data";
        case ELCACHE_ERR_TIMEOUT: return "Timeout";
        case ELCACHE_ERR_CONNECTION: return "Connection error";
        case ELCACHE_ERR_PROTOCOL: return "Protocol error";
        case ELCACHE_ERR_BUFFER_TOO_SMALL: return "Buffer too small";
        case ELCACHE_ERR_KEY_TOO_LARGE: return "Key too large";
        case ELCACHE_ERR_VALUE_TOO_LARGE: return "Value too large";
        case ELCACHE_ERR_OUT_OF_MEMORY: return "Out of memory";
        case ELCACHE_ERR_INTERNAL: return "Internal error";
        default: return "Unknown error";
    }
}

void elcache_free(void* ptr) {
    free(ptr);
}

/*
 * Position-based write operations
 */

elcache_error_t elcache_client_create_sparse(elcache_client_t* client,
                                              const void* key, size_t key_len,
                                              uint64_t total_size,
                                              const elcache_put_options_t* options) {
    if (!client || !key) {
        return ELCACHE_ERR_INVALID_ARG;
    }
    
    if (key_len > 8192) {
        set_error(client, "Key too large");
        return ELCACHE_ERR_KEY_TOO_LARGE;
    }
    
    if (!elcache_client_is_connected(client)) {
        set_error(client, "Not connected");
        return ELCACHE_ERR_CONNECTION;
    }
    
    /* Build request: key_len(4) + key + total_size(8) + has_ttl(1) + [ttl(8)] + flags(4) */
    uint8_t* payload = client->send_buffer + ELCACHE_HEADER_SIZE;
    size_t payload_len = 0;
    
    /* Key */
    encode_u32(payload + payload_len, (uint32_t)key_len);
    payload_len += 4;
    memcpy(payload + payload_len, key, key_len);
    payload_len += key_len;
    
    /* Total size */
    encode_u64(payload + payload_len, total_size);
    payload_len += 8;
    
    /* TTL */
    int has_ttl = options && options->ttl_seconds > 0;
    payload[payload_len++] = has_ttl ? 1 : 0;
    if (has_ttl) {
        encode_u64(payload + payload_len, (uint64_t)options->ttl_seconds);
        payload_len += 8;
    }
    
    /* Flags */
    uint32_t flags = options ? options->flags : 0;
    encode_u32(payload + payload_len, flags);
    payload_len += 4;
    
    /* Encode header and send */
    uint32_t request_id = client->next_request_id++;
    encode_header(client->send_buffer, MSG_CREATE_SPARSE, (uint32_t)payload_len, request_id);
    
    elcache_error_t err = send_all(client, client->send_buffer, ELCACHE_HEADER_SIZE + payload_len);
    if (err != ELCACHE_OK) {
        return err;
    }
    
    /* Receive response */
    err = recv_all(client, client->recv_buffer, ELCACHE_HEADER_SIZE);
    if (err != ELCACHE_OK) {
        return err;
    }
    
    uint32_t response_len = decode_u32(client->recv_buffer + 8);
    err = recv_all(client, client->recv_buffer + ELCACHE_HEADER_SIZE, response_len);
    if (err != ELCACHE_OK) {
        return err;
    }
    
    /* Parse response */
    uint8_t* resp = client->recv_buffer + ELCACHE_HEADER_SIZE;
    uint32_t code = decode_u32(resp);
    
    if (code != 0) {
        set_error(client, "Create sparse failed");
        return ELCACHE_ERR_INTERNAL;
    }
    
    return ELCACHE_OK;
}

elcache_error_t elcache_client_write_range(elcache_client_t* client,
                                            const void* key, size_t key_len,
                                            uint64_t offset,
                                            const void* data, size_t data_len) {
    if (!client || !key || !data) {
        return ELCACHE_ERR_INVALID_ARG;
    }
    
    if (key_len > 8192) {
        set_error(client, "Key too large");
        return ELCACHE_ERR_KEY_TOO_LARGE;
    }
    
    if (!elcache_client_is_connected(client)) {
        set_error(client, "Not connected");
        return ELCACHE_ERR_CONNECTION;
    }
    
    /* Check if payload fits in buffer */
    size_t needed = ELCACHE_HEADER_SIZE + 4 + key_len + 8 + 4 + data_len;
    if (needed > client->send_buffer_size) {
        set_error(client, "Data too large for buffer");
        return ELCACHE_ERR_VALUE_TOO_LARGE;
    }
    
    /* Build request: key_len(4) + key + offset(8) + data_len(4) + data */
    uint8_t* payload = client->send_buffer + ELCACHE_HEADER_SIZE;
    size_t payload_len = 0;
    
    /* Key */
    encode_u32(payload + payload_len, (uint32_t)key_len);
    payload_len += 4;
    memcpy(payload + payload_len, key, key_len);
    payload_len += key_len;
    
    /* Offset */
    encode_u64(payload + payload_len, offset);
    payload_len += 8;
    
    /* Data */
    encode_u32(payload + payload_len, (uint32_t)data_len);
    payload_len += 4;
    memcpy(payload + payload_len, data, data_len);
    payload_len += data_len;
    
    /* Encode header and send */
    uint32_t request_id = client->next_request_id++;
    encode_header(client->send_buffer, MSG_WRITE_RANGE, (uint32_t)payload_len, request_id);
    
    elcache_error_t err = send_all(client, client->send_buffer, ELCACHE_HEADER_SIZE + payload_len);
    if (err != ELCACHE_OK) {
        return err;
    }
    
    /* Receive response */
    err = recv_all(client, client->recv_buffer, ELCACHE_HEADER_SIZE);
    if (err != ELCACHE_OK) {
        return err;
    }
    
    uint32_t response_len = decode_u32(client->recv_buffer + 8);
    err = recv_all(client, client->recv_buffer + ELCACHE_HEADER_SIZE, response_len);
    if (err != ELCACHE_OK) {
        return err;
    }
    
    /* Parse response */
    uint8_t* resp = client->recv_buffer + ELCACHE_HEADER_SIZE;
    uint32_t code = decode_u32(resp);
    
    if (code == 1) {
        set_error(client, "Key not found or not sparse");
        return ELCACHE_ERR_NOT_FOUND;
    } else if (code == 2) {
        set_error(client, "Invalid range");
        return ELCACHE_ERR_INVALID_ARG;
    } else if (code != 0) {
        set_error(client, "Write range failed");
        return ELCACHE_ERR_INTERNAL;
    }
    
    /* Update stats */
    client->bytes_written += data_len;
    
    return ELCACHE_OK;
}

elcache_error_t elcache_client_finalize(elcache_client_t* client,
                                         const void* key, size_t key_len) {
    if (!client || !key) {
        return ELCACHE_ERR_INVALID_ARG;
    }
    
    if (!elcache_client_is_connected(client)) {
        set_error(client, "Not connected");
        return ELCACHE_ERR_CONNECTION;
    }
    
    /* Build request: key_len(4) + key */
    uint8_t* payload = client->send_buffer + ELCACHE_HEADER_SIZE;
    size_t payload_len = 0;
    
    encode_u32(payload + payload_len, (uint32_t)key_len);
    payload_len += 4;
    memcpy(payload + payload_len, key, key_len);
    payload_len += key_len;
    
    /* Encode header and send */
    uint32_t request_id = client->next_request_id++;
    encode_header(client->send_buffer, MSG_FINALIZE, (uint32_t)payload_len, request_id);
    
    elcache_error_t err = send_all(client, client->send_buffer, ELCACHE_HEADER_SIZE + payload_len);
    if (err != ELCACHE_OK) {
        return err;
    }
    
    /* Receive response */
    err = recv_all(client, client->recv_buffer, ELCACHE_HEADER_SIZE);
    if (err != ELCACHE_OK) {
        return err;
    }
    
    uint32_t response_len = decode_u32(client->recv_buffer + 8);
    err = recv_all(client, client->recv_buffer + ELCACHE_HEADER_SIZE, response_len);
    if (err != ELCACHE_OK) {
        return err;
    }
    
    /* Parse response */
    uint8_t* resp = client->recv_buffer + ELCACHE_HEADER_SIZE;
    uint32_t code = decode_u32(resp);
    
    if (code == 1) {
        set_error(client, "Key not found");
        return ELCACHE_ERR_NOT_FOUND;
    } else if (code == 2) {
        set_error(client, "Not all ranges written");
        return ELCACHE_ERR_PARTIAL;
    } else if (code != 0) {
        set_error(client, "Finalize failed");
        return ELCACHE_ERR_INTERNAL;
    }
    
    return ELCACHE_OK;
}

/* Streaming operations stubs */

struct elcache_write_stream {
    elcache_client_t* client;
    char* key;
    size_t key_len;
    uint64_t total_size;
    uint64_t written;
};

struct elcache_read_stream {
    elcache_client_t* client;
    uint64_t total_size;
    uint64_t position;
    uint8_t* buffer;
    size_t buffer_size;
    size_t buffer_pos;
    size_t buffer_len;
};

elcache_error_t elcache_client_write_stream_start(elcache_client_t* client,
                                                   const void* key, size_t key_len,
                                                   uint64_t total_size,
                                                   const elcache_put_options_t* options,
                                                   elcache_write_stream_t** stream) {
    /* TODO: Implement streaming write */
    return ELCACHE_ERR_INTERNAL;
}

elcache_error_t elcache_write_stream_write(elcache_write_stream_t* stream,
                                            const void* data, size_t len) {
    return ELCACHE_ERR_INTERNAL;
}

elcache_error_t elcache_write_stream_finish(elcache_write_stream_t* stream) {
    return ELCACHE_ERR_INTERNAL;
}

void elcache_write_stream_abort(elcache_write_stream_t* stream) {
    if (stream) {
        free(stream->key);
        free(stream);
    }
}

elcache_error_t elcache_client_read_stream_start(elcache_client_t* client,
                                                  const void* key, size_t key_len,
                                                  const elcache_get_options_t* options,
                                                  elcache_read_stream_t** stream) {
    return ELCACHE_ERR_INTERNAL;
}

elcache_error_t elcache_read_stream_read(elcache_read_stream_t* stream,
                                          void* buffer, size_t buffer_size,
                                          size_t* bytes_read) {
    return ELCACHE_ERR_INTERNAL;
}

uint64_t elcache_read_stream_total_size(const elcache_read_stream_t* stream) {
    return stream ? stream->total_size : 0;
}

uint64_t elcache_read_stream_bytes_read(const elcache_read_stream_t* stream) {
    return stream ? stream->position : 0;
}

int elcache_read_stream_eof(const elcache_read_stream_t* stream) {
    return stream ? (stream->position >= stream->total_size) : 1;
}

void elcache_read_stream_close(elcache_read_stream_t* stream) {
    if (stream) {
        free(stream->buffer);
        free(stream);
    }
}

elcache_error_t elcache_client_check_availability(elcache_client_t* client,
                                                   const void* key, size_t key_len,
                                                   uint64_t offset, uint64_t length,
                                                   elcache_availability_t* availability) {
    return ELCACHE_ERR_INTERNAL;
}

void elcache_availability_free(elcache_availability_t* availability) {
    if (availability) {
        free(availability->available_ranges);
        free(availability->missing_ranges);
        availability->available_ranges = NULL;
        availability->missing_ranges = NULL;
        availability->available_count = 0;
        availability->missing_count = 0;
    }
}

elcache_error_t elcache_client_read_range(elcache_client_t* client,
                                           const void* key, size_t key_len,
                                           uint64_t offset,
                                           void* buffer, size_t buffer_size,
                                           size_t* bytes_read) {
    elcache_get_options_t opts = ELCACHE_GET_OPTIONS_INIT;
    opts.offset = offset;
    opts.length = buffer_size;
    
    return elcache_client_get(client, key, key_len, buffer, buffer_size, 
                              bytes_read, &opts);
}
