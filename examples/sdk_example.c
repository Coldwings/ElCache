/*
 * ElCache C SDK Example
 * 
 * This example demonstrates using the C SDK to:
 * - Connect to ElCache server
 * - Store and retrieve values
 * - Perform partial reads
 */

#include "elcache/elcache_sdk.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

void print_stats(elcache_client_t* client) {
    elcache_stats_t stats;
    if (elcache_client_stats(client, &stats) == ELCACHE_OK) {
        printf("Stats:\n");
        printf("  Hits: %lu\n", (unsigned long)stats.hits);
        printf("  Misses: %lu\n", (unsigned long)stats.misses);
        printf("  Bytes read: %lu\n", (unsigned long)stats.bytes_read);
        printf("  Bytes written: %lu\n", (unsigned long)stats.bytes_written);
        printf("  Avg latency: %lu us\n", (unsigned long)stats.latency_us_avg);
    }
}

int main(int argc, char* argv[]) {
    const char* socket_path = "/var/run/elcache/elcache.sock";
    
    if (argc > 1) {
        socket_path = argv[1];
    }
    
    printf("ElCache C SDK Example\n");
    printf("=====================\n\n");
    
    /* Create client */
    elcache_client_t* client = elcache_client_create();
    if (!client) {
        fprintf(stderr, "Failed to create client\n");
        return 1;
    }
    
    /* Configure client */
    elcache_client_set_timeout(client, 5000);  /* 5 second timeout */
    
    /* Try to connect */
    printf("Connecting to %s...\n", socket_path);
    elcache_error_t err = elcache_client_connect_unix(client, socket_path);
    
    if (err != ELCACHE_OK) {
        printf("Note: Could not connect to server (%s)\n", elcache_error_string(err));
        printf("This example requires a running ElCache server.\n");
        printf("\nDemonstrating API usage without server:\n\n");
        
        /* Show API usage even without connection */
        printf("1. Put operation:\n");
        printf("   elcache_client_put(client, \"key\", 3, \"value\", 5, NULL)\n\n");
        
        printf("2. Get operation:\n");
        printf("   char buffer[1024];\n");
        printf("   size_t len;\n");
        printf("   elcache_client_get(client, \"key\", 3, buffer, sizeof(buffer), &len, NULL)\n\n");
        
        printf("3. Partial read:\n");
        printf("   elcache_get_options_t opts = ELCACHE_GET_OPTIONS_INIT;\n");
        printf("   opts.offset = 100;\n");
        printf("   opts.length = 50;\n");
        printf("   elcache_client_get(client, \"key\", 3, buffer, sizeof(buffer), &len, &opts)\n\n");
        
        printf("4. Put with TTL:\n");
        printf("   elcache_put_options_t put_opts = ELCACHE_PUT_OPTIONS_INIT;\n");
        printf("   put_opts.ttl_seconds = 3600;  /* 1 hour */\n");
        printf("   elcache_client_put(client, \"key\", 3, \"value\", 5, &put_opts)\n\n");
        
        printf("5. Delete:\n");
        printf("   elcache_client_delete(client, \"key\", 3)\n\n");
        
        printf("6. Check existence:\n");
        printf("   int exists;\n");
        printf("   elcache_client_exists(client, \"key\", 3, &exists)\n\n");
        
        elcache_client_destroy(client);
        return 0;
    }
    
    printf("Connected!\n\n");
    
    /* Example: Store a simple value */
    printf("Storing key 'greeting'...\n");
    const char* key = "greeting";
    const char* value = "Hello, ElCache!";
    
    err = elcache_client_put(client, key, strlen(key), value, strlen(value), NULL);
    if (err != ELCACHE_OK) {
        fprintf(stderr, "Put failed: %s\n", elcache_error_string(err));
    } else {
        printf("Stored successfully!\n");
    }
    
    /* Example: Retrieve the value */
    printf("\nRetrieving key 'greeting'...\n");
    char buffer[1024];
    size_t len;
    
    err = elcache_client_get(client, key, strlen(key), buffer, sizeof(buffer), &len, NULL);
    if (err == ELCACHE_OK) {
        printf("Got: %.*s\n", (int)len, buffer);
    } else {
        fprintf(stderr, "Get failed: %s\n", elcache_error_string(err));
    }
    
    /* Example: Store a larger value */
    printf("\nStoring 1MB value...\n");
    char* large_value = malloc(1024 * 1024);
    if (large_value) {
        memset(large_value, 'X', 1024 * 1024);
        
        err = elcache_client_put(client, "large", 5, large_value, 1024 * 1024, NULL);
        if (err == ELCACHE_OK) {
            printf("Stored 1MB value!\n");
            
            /* Partial read */
            printf("\nReading bytes 500-600 of large value...\n");
            elcache_get_options_t opts = ELCACHE_GET_OPTIONS_INIT;
            opts.offset = 500;
            opts.length = 100;
            
            err = elcache_client_get(client, "large", 5, buffer, sizeof(buffer), &len, &opts);
            if (err == ELCACHE_OK || err == ELCACHE_ERR_PARTIAL) {
                printf("Read %zu bytes from offset 500\n", len);
            }
        } else {
            fprintf(stderr, "1MB put failed: %s (default buffer is 256KB)\n", elcache_error_string(err));
        }
        
        free(large_value);
    }
    
    /* Example: Delete */
    printf("\nDeleting 'greeting'...\n");
    err = elcache_client_delete(client, key, strlen(key));
    if (err == ELCACHE_OK) {
        printf("Deleted!\n");
    }
    
    /* Verify deletion */
    int exists;
    elcache_client_exists(client, key, strlen(key), &exists);
    printf("Key exists after delete: %s\n", exists ? "yes" : "no");
    
    /* Example: Position-based writes for large values
     * This demonstrates writing a large value in parts, which can be done
     * from multiple threads in parallel.
     */
    printf("\n=== Position-based Write Example ===\n");
    
    const char* sparse_key = "large_file";
    size_t total_size = 1024 * 1024;  /* 1MB total */
    size_t chunk_size = 200 * 1024;   /* 200KB chunks (leave room for protocol overhead) */
    
    printf("Creating sparse entry for %zu bytes...\n", total_size);
    err = elcache_client_create_sparse(client, sparse_key, strlen(sparse_key), 
                                        total_size, NULL);
    if (err != ELCACHE_OK) {
        fprintf(stderr, "create_sparse failed: %s\n", elcache_error_string(err));
    } else {
        printf("Sparse entry created!\n");
        
        /* Write in chunks (in real use, these could be parallel from different threads) */
        char* chunk_data = malloc(chunk_size);
        if (chunk_data) {
            int success = 1;
            for (size_t offset = 0; offset < total_size && success; offset += chunk_size) {
                size_t write_len = (offset + chunk_size > total_size) ? 
                                   (total_size - offset) : chunk_size;
                
                /* Fill chunk with pattern based on offset */
                memset(chunk_data, (char)('A' + (offset / chunk_size)), write_len);
                
                printf("Writing %zu bytes at offset %zu...\n", write_len, offset);
                err = elcache_client_write_range(client, sparse_key, strlen(sparse_key),
                                                  offset, chunk_data, write_len);
                if (err != ELCACHE_OK) {
                    fprintf(stderr, "write_range failed: %s\n", elcache_error_string(err));
                    success = 0;
                }
            }
            free(chunk_data);
            
            if (success) {
                printf("All chunks written, finalizing...\n");
                err = elcache_client_finalize(client, sparse_key, strlen(sparse_key));
                if (err == ELCACHE_OK) {
                    printf("Entry finalized successfully!\n");
                    
                    /* Verify we can read it back */
                    printf("Reading back first 100 bytes...\n");
                    char verify_buf[100];
                    size_t read_len;
                    elcache_get_options_t opts = ELCACHE_GET_OPTIONS_INIT;
                    opts.offset = 0;
                    opts.length = 100;
                    err = elcache_client_get(client, sparse_key, strlen(sparse_key),
                                             verify_buf, sizeof(verify_buf), &read_len, &opts);
                    if (err == ELCACHE_OK) {
                        printf("Read %zu bytes, first char: '%c' (expected 'A')\n", 
                               read_len, verify_buf[0]);
                    }
                    
                    /* Clean up */
                    elcache_client_delete(client, sparse_key, strlen(sparse_key));
                } else if (err == ELCACHE_ERR_PARTIAL) {
                    fprintf(stderr, "Finalize failed: not all ranges written\n");
                } else {
                    fprintf(stderr, "Finalize failed: %s\n", elcache_error_string(err));
                }
            }
        }
    }
    
    /* Print stats */
    printf("\n");
    print_stats(client);
    
    /* Cleanup */
    elcache_client_disconnect(client);
    elcache_client_destroy(client);
    
    printf("\nExample complete!\n");
    return 0;
}
